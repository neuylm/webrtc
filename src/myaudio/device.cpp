#include "myaudio/device.hpp"

#include "wincom.hpp"

#include <algorithm>
#include <cctype>

namespace myaudio {

#ifdef _WIN32

namespace {

const char* kRoot = "Software\\myaudio\\VirtualMic";

std::string lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool regString(HKEY key, const char* name, std::string& out) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExA(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_SZ)
        return false;
    std::string buf(size + 1, '\0');
    if (RegQueryValueExA(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&buf[0]), &size) !=
        ERROR_SUCCESS)
        return false;
    buf.resize(size ? size - 1 : 0);
    out = buf.c_str();
    return true;
}

bool regDword(HKEY key, const char* name, uint32_t& out) {
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegQueryValueExA(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) !=
            ERROR_SUCCESS ||
        type != REG_DWORD)
        return false;
    out = value;
    return true;
}

void setString(HKEY key, const char* name, const std::string& value) {
    RegSetValueExA(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>(value.size() + 1));
}

void setDword(HKEY key, const char* name, uint32_t value) {
    DWORD v = value;
    RegSetValueExA(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
}

bool ownerAlive(uint32_t pid) {
    if (!pid)
        return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    bool live = GetExitCodeProcess(proc, &code) && code == STILL_ACTIVE;
    CloseHandle(proc);
    return live;
}

bool readRecord(HKEY parent, const std::string& name, MicRecord& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExA(parent, name.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    out = MicRecord();
    out.name = name;
    regString(key, "Share", out.share);
    regString(key, "Sink", out.sink);
    regString(key, "Device", out.device);
    uint32_t rate = 0;
    uint32_t ch = 0;
    if (regDword(key, "Rate", rate))
        out.format.rate = rate;
    if (regDword(key, "Channels", ch))
        out.format.channels = static_cast<uint8_t>(ch);
    regDword(key, "Pid", out.pid);
    RegCloseKey(key);
    return true;
}

}

ComGuard::ComGuard() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owned_ = SUCCEEDED(hr);
}

ComGuard::~ComGuard() {
    if (owned_)
        CoUninitialize();
}

std::string narrow(const wchar_t* text) {
    if (!text)
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& text) {
    if (text.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 1)
        return std::wstring();
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], n);
    return out;
}

std::string friendlyName(IMMDevice* dev) {
    if (!dev)
        return std::string();
    IPropertyStore* store = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &store)) || !store)
        return std::string();
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string name;
    if (SUCCEEDED(store->GetValue(kPkeyFriendly, &pv)) && pv.vt == VT_LPWSTR)
        name = narrow(pv.pwszVal);
    PropVariantClear(&pv);
    store->Release();
    return name;
}

std::string endpointId(IMMDevice* dev) {
    if (!dev)
        return std::string();
    LPWSTR id = nullptr;
    if (FAILED(dev->GetId(&id)) || !id)
        return std::string();
    std::string out = narrow(id);
    CoTaskMemFree(id);
    return out;
}

bool looksLikeCable(const std::string& name) {
    static const char* marks[] = {"cable",         "vb-audio", "voicemeeter", "virtual audio",
                                  "virtual cable", "mymic",    "virtual mic"};
    std::string low = lower(name);
    for (const char* m : marks) {
        if (low.find(m) != std::string::npos)
            return true;
    }
    return false;
}

namespace {

void describe(IMMDevice* dev, DeviceInfo& info) {
    info.id = endpointId(dev);
    info.name = friendlyName(dev);
    info.cable = looksLikeCable(info.name);

    IAudioClient* client = nullptr;
    if (SUCCEEDED(dev->Activate(kIidAudioClient, CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&client))) &&
        client) {
        WAVEFORMATEX* wf = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&wf)) && wf) {
            info.format.rate = wf->nSamplesPerSec;
            info.format.channels = static_cast<uint8_t>(wf->nChannels);
            CoTaskMemFree(wf);
        }
        client->Release();
    }
}

}

std::vector<DeviceInfo> devices(bool capture) {
    std::vector<DeviceInfo> out;
    ComGuard com;

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(kClsidEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator,
                                reinterpret_cast<void**>(&en))) ||
        !en)
        return out;

    EDataFlow flow = capture ? eCapture : eRender;
    std::string defId;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &def)) && def) {
        defId = endpointId(def);
        def->Release();
    }

    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev)) || !dev)
                continue;
            DeviceInfo info;
            info.capture = capture;
            describe(dev, info);
            info.isDefault = !info.id.empty() && info.id == defId;
            dev->Release();
            if (!info.id.empty())
                out.push_back(info);
        }
        col->Release();
    }
    en->Release();
    return out;
}

bool defaultDevice(bool capture, DeviceInfo& out) {
    ComGuard com;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(kClsidEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator,
                                reinterpret_cast<void**>(&en))) ||
        !en)
        return false;

    IMMDevice* dev = nullptr;
    bool ok = false;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &dev)) &&
        dev) {
        out = DeviceInfo();
        out.capture = capture;
        describe(dev, out);
        out.isDefault = true;
        ok = !out.id.empty();
        dev->Release();
    }
    en->Release();
    return ok;
}

bool findCable(DeviceInfo& render, DeviceInfo& capture) {
    std::vector<DeviceInfo> outs = devices(false);
    std::vector<DeviceInfo> ins = devices(true);

    for (const DeviceInfo& o : outs) {
        if (!o.cable)
            continue;
        render = o;
        for (const DeviceInfo& i : ins) {
            if (i.cable) {
                capture = i;
                return true;
            }
        }
        capture = DeviceInfo();
        return true;
    }
    return false;
}

bool registerMic(const MicRecord& rec) {
    if (rec.name.empty())
        return false;
    std::string path = std::string(kRoot) + "\\" + rec.name;
    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    setString(key, "Share", rec.share);
    setString(key, "Sink", rec.sink);
    setString(key, "Device", rec.device);
    setDword(key, "Rate", rec.format.rate);
    setDword(key, "Channels", rec.format.channels);
    setDword(key, "Pid", rec.pid ? rec.pid : GetCurrentProcessId());
    RegCloseKey(key);
    return true;
}

bool unregisterMic(const std::string& name) {
    if (name.empty())
        return false;
    std::string path = std::string(kRoot) + "\\" + name;
    return RegDeleteKeyA(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS;
}

bool queryMic(const std::string& name, MicRecord& out) {
    HKEY root = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRoot, 0, KEY_READ, &root) != ERROR_SUCCESS)
        return false;
    bool ok = readRecord(root, name, out);
    RegCloseKey(root);
    if (ok && !ownerAlive(out.pid)) {
        unregisterMic(name);
        return false;
    }
    return ok;
}

std::vector<MicRecord> registeredMics() {
    std::vector<MicRecord> out;
    std::vector<std::string> names;
    std::vector<std::string> stale;
    HKEY root = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRoot, 0, KEY_READ, &root) != ERROR_SUCCESS)
        return out;

    for (DWORD i = 0;; ++i) {
        char name[256];
        DWORD len = sizeof(name);
        if (RegEnumKeyExA(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        names.push_back(name);
    }

    for (const std::string& name : names) {
        MicRecord rec;
        if (!readRecord(root, name, rec))
            continue;
        if (ownerAlive(rec.pid))
            out.push_back(rec);
        else
            stale.push_back(name);
    }
    RegCloseKey(root);

    for (const std::string& name : stale)
        unregisterMic(name);
    return out;
}

#else

std::vector<DeviceInfo> devices(bool) {
    return std::vector<DeviceInfo>();
}

bool defaultDevice(bool, DeviceInfo&) {
    return false;
}

bool findCable(DeviceInfo&, DeviceInfo&) {
    return false;
}

bool registerMic(const MicRecord&) {
    return false;
}

bool unregisterMic(const std::string&) {
    return false;
}

bool queryMic(const std::string&, MicRecord&) {
    return false;
}

std::vector<MicRecord> registeredMics() {
    return std::vector<MicRecord>();
}

#endif

}
