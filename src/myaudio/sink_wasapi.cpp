#include "sink.hpp"

#include "wincom.hpp"

#ifdef _WIN32
#include <avrt.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace myaudio {

#ifdef _WIN32

namespace {

std::string lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

class Watcher : public IMMNotificationClient {
public:
    Watcher(std::atomic<bool>* flag, const std::wstring& id, bool anyDefault)
        : flag_(flag), id_(id), anyDefault_(anyDefault) {}

    virtual ~Watcher() = default;

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG left = InterlockedDecrement(&ref_);
        if (!left)
            delete this;
        return static_cast<ULONG>(left);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (mine(id) && state != DEVICE_STATE_ACTIVE)
            flag_->store(true);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        if (mine(id))
            flag_->store(true);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR) override {
        if (anyDefault_ && flow == eRender && role == eConsole)
            flag_->store(true);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    bool mine(LPCWSTR id) const { return id && !id_.empty() && id_ == id; }

    LONG ref_ = 1;
    std::atomic<bool>* flag_;
    std::wstring id_;
    bool anyDefault_;
};

class WasapiSink : public Sink {
public:
    WasapiSink(const SinkConfig& cfg, const LogSink& log) : cfg_(cfg), log_(log) {}

    ~WasapiSink() override { WasapiSink::close(); }

    bool open(Format& fmt, uint32_t quantumMs, Pull pull, Reformat onFormat) override {
        close();
        (void)fmt;
        quantumMs_ = quantumMs ? quantumMs : 10;
        pull_ = std::move(pull);
        reformat_ = std::move(onFormat);
        quit_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!quit_)
            return false;
        run_.store(true);

        std::promise<bool> ready;
        std::future<bool> done = ready.get_future();
        th_ = std::thread(&WasapiSink::main, this, &ready);
        bool ok = done.get();
        if (!ok) {
            run_.store(false);
            SetEvent(quit_);
            if (th_.joinable())
                th_.join();
            CloseHandle(quit_);
            quit_ = nullptr;
            return false;
        }
        fmt = fmt_;
        return true;
    }

    void close() override {
        if (run_.exchange(false)) {
            if (quit_)
                SetEvent(quit_);
            if (th_.joinable())
                th_.join();
        }
        if (quit_) {
            CloseHandle(quit_);
            quit_ = nullptr;
        }
        pull_ = nullptr;
        reformat_ = nullptr;
    }

    std::string device() const override {
        std::lock_guard<std::mutex> lk(nameMtx_);
        return name_;
    }
    uint64_t frames() const override { return frames_.load(std::memory_order_relaxed); }
    uint64_t restarts() const override { return restarts_.load(std::memory_order_relaxed); }

private:
    void say(const std::string& text) {
        if (log_)
            log_(text);
    }

    IMMDevice* pick(IMMDeviceEnumerator* en, bool& byDefault) {
        byDefault = false;
        IMMDevice* dev = nullptr;

        if (!cfg_.deviceId.empty()) {
            std::wstring wid = widen(cfg_.deviceId);
            if (SUCCEEDED(en->GetDevice(wid.c_str(), &dev)) && dev)
                return dev;
            say("wasapi device id not found: " + cfg_.deviceId);
        }

        if (!cfg_.deviceMatch.empty() || cfg_.preferVirtual) {
            IMMDeviceCollection* col = nullptr;
            if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
                UINT count = 0;
                col->GetCount(&count);
                std::string want = lower(cfg_.deviceMatch);
                for (UINT i = 0; i < count && !dev; ++i) {
                    IMMDevice* item = nullptr;
                    if (FAILED(col->Item(i, &item)) || !item)
                        continue;
                    std::string name = friendlyName(item);
                    bool hit = want.empty() ? looksLikeCable(name)
                                            : lower(name).find(want) != std::string::npos;
                    if (hit)
                        dev = item;
                    else
                        item->Release();
                }
                col->Release();
            }
            if (dev)
                return dev;
            if (!cfg_.deviceMatch.empty())
                say("wasapi no endpoint matching \"" + cfg_.deviceMatch + "\"");
        }

        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev) {
            byDefault = true;
            return dev;
        }
        return nullptr;
    }

    bool boot() {
        if (FAILED(CoCreateInstance(kClsidEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator,
                                    reinterpret_cast<void**>(&enum_))) ||
            !enum_) {
            say("wasapi enumerator unavailable");
            return false;
        }

        bool byDefault = false;
        IMMDevice* dev = pick(enum_, byDefault);
        if (!dev) {
            say("wasapi found no render endpoint");
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(nameMtx_);
            name_ = friendlyName(dev);
        }
        std::wstring wid = widen(endpointId(dev));

        watch_ = new Watcher(&changed_, wid, byDefault);
        if (FAILED(enum_->RegisterEndpointNotificationCallback(watch_))) {
            watch_->Release();
            watch_ = nullptr;
        }

        HRESULT hr = dev->Activate(kIidAudioClient, CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&client_));
        dev->Release();
        if (FAILED(hr) || !client_) {
            say("wasapi activate failed");
            return false;
        }

        WAVEFORMATEX* wf = nullptr;
        if (FAILED(client_->GetMixFormat(&wf)) || !wf) {
            say("wasapi mix format unavailable");
            return false;
        }

        float_ = false;
        bool pcm16 = false;
        if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32) {
            float_ = true;
        } else if (wf->wFormatTag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 16) {
            pcm16 = true;
        } else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
            if (ext->SubFormat == kSubtypeFloat && wf->wBitsPerSample == 32)
                float_ = true;
            else if (ext->SubFormat == kSubtypePcm && wf->wBitsPerSample == 16)
                pcm16 = true;
        }
        if (!float_ && !pcm16) {
            say("wasapi mix format unsupported: " + std::to_string(wf->wBitsPerSample) + " bit");
            CoTaskMemFree(wf);
            return false;
        }

        REFERENCE_TIME period = 0;
        REFERENCE_TIME least = 0;
        client_->GetDevicePeriod(&period, &least);
        REFERENCE_TIME want = static_cast<REFERENCE_TIME>(quantumMs_) * 10000;
        if (want < period)
            want = period;

        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 want * 2, 0, wf, nullptr);
        Format got;
        got.rate = wf->nSamplesPerSec;
        got.channels = static_cast<uint8_t>(wf->nChannels);
        CoTaskMemFree(wf);
        if (FAILED(hr)) {
            say("wasapi initialize failed 0x" + std::to_string(static_cast<unsigned>(hr)));
            return false;
        }

        evt_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!evt_ || FAILED(client_->SetEventHandle(evt_))) {
            say("wasapi event setup failed");
            return false;
        }
        if (FAILED(client_->GetBufferSize(&bufFrames_)) || !bufFrames_) {
            say("wasapi buffer size unavailable");
            return false;
        }
        if (FAILED(client_->GetService(kIidRenderClient, reinterpret_cast<void**>(&render_))) ||
            !render_) {
            say("wasapi render service unavailable");
            return false;
        }
        if (FAILED(client_->Start())) {
            say("wasapi start failed");
            return false;
        }

        fmt_ = got;
        changed_.store(false);
        return true;
    }

    void shut() {
        if (client_)
            client_->Stop();
        if (render_) {
            render_->Release();
            render_ = nullptr;
        }
        if (client_) {
            client_->Release();
            client_ = nullptr;
        }
        if (watch_ && enum_)
            enum_->UnregisterEndpointNotificationCallback(watch_);
        if (watch_) {
            watch_->Release();
            watch_ = nullptr;
        }
        if (enum_) {
            enum_->Release();
            enum_ = nullptr;
        }
        if (evt_) {
            CloseHandle(evt_);
            evt_ = nullptr;
        }
        bufFrames_ = 0;
    }

    void spin() {
        HANDLE waits[2] = {evt_, quit_};
        while (run_.load(std::memory_order_relaxed)) {
            DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 400);
            if (!run_.load(std::memory_order_relaxed) || wait == WAIT_OBJECT_0 + 1)
                return;
            if (changed_.exchange(false))
                return;
            if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT)
                return;

            UINT32 pad = 0;
            HRESULT hr = client_->GetCurrentPadding(&pad);
            if (FAILED(hr))
                return;
            if (pad > bufFrames_)
                continue;
            UINT32 avail = bufFrames_ - pad;
            if (!avail)
                continue;

            BYTE* raw = nullptr;
            hr = render_->GetBuffer(avail, &raw);
            if (FAILED(hr) || !raw)
                return;

            size_t need = static_cast<size_t>(avail) * fmt_.channels;
            if (mix_.size() < need)
                mix_.resize(need);
            std::fill(mix_.begin(), mix_.begin() + static_cast<ptrdiff_t>(need), 0.0f);
            pull_(mix_.data(), avail);

            if (float_) {
                std::copy(mix_.begin(), mix_.begin() + static_cast<ptrdiff_t>(need),
                          reinterpret_cast<float*>(raw));
            } else {
                int16_t* out = reinterpret_cast<int16_t*>(raw);
                for (size_t i = 0; i < need; ++i) {
                    float v = mix_[i];
                    if (v > 1.0f)
                        v = 1.0f;
                    if (v < -1.0f)
                        v = -1.0f;
                    out[i] = static_cast<int16_t>(v * 32767.0f);
                }
            }
            render_->ReleaseBuffer(avail, 0);
            frames_.fetch_add(avail, std::memory_order_relaxed);
        }
    }

    void main(std::promise<bool>* ready) {
        ComGuard com;
        DWORD taskIdx = 0;
        HANDLE task = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIdx);

        bool first = true;
        Format last;
        while (run_.load(std::memory_order_relaxed)) {
            if (!boot()) {
                shut();
                if (first) {
                    ready->set_value(false);
                    break;
                }
                if (WaitForSingleObject(quit_, 500) == WAIT_OBJECT_0)
                    break;
                continue;
            }
            if (first) {
                last = fmt_;
                first = false;
                ready->set_value(true);
            } else {
                restarts_.fetch_add(1, std::memory_order_relaxed);
                say("wasapi reopened on " + name_);
                if ((fmt_.rate != last.rate || fmt_.channels != last.channels) && reformat_) {
                    reformat_(fmt_);
                    last = fmt_;
                }
            }
            spin();
            shut();
        }
        shut();
        if (task)
            AvRevertMmThreadCharacteristics(task);
    }

    SinkConfig cfg_;
    LogSink log_;
    Pull pull_;
    Reformat reformat_;
    Format fmt_;
    uint32_t quantumMs_ = 10;
    std::string name_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<bool> changed_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> restarts_{0};
    std::vector<float> mix_;
    IMMDeviceEnumerator* enum_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    Watcher* watch_ = nullptr;
    HANDLE evt_ = nullptr;
    HANDLE quit_ = nullptr;
    mutable std::mutex nameMtx_;
    UINT32 bufFrames_ = 0;
    bool float_ = false;
};

}

std::unique_ptr<Sink> makeWasapiSink(const SinkConfig& cfg, const LogSink& log) {
    return std::unique_ptr<Sink>(new WasapiSink(cfg, log));
}

#else

std::unique_ptr<Sink> makeWasapiSink(const SinkConfig&, const LogSink&) {
    return nullptr;
}

#endif

}
