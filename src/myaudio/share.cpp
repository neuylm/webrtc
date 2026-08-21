#include "myaudio/share.hpp"

#include "shm.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace myaudio {
namespace {

std::string evName(const std::string& base) {
    return base + "-ev";
}

#ifdef _WIN32
size_t viewBytes(void* view) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(view, &mbi, sizeof(mbi)))
        return 0;
    return static_cast<size_t>(mbi.RegionSize);
}
#endif

}

#ifdef _WIN32

ShareWriter::~ShareWriter() {
    close();
}

size_t ShareWriter::capacity() const {
    return cap_;
}

uint64_t ShareWriter::written() const {
    return head_ ? head_->write.load(std::memory_order_relaxed) : 0;
}

uint64_t ShareWriter::dropped() const {
    return head_ ? head_->dropped.load(std::memory_order_relaxed) : 0;
}

bool ShareWriter::open(const std::string& name, const Format& fmt, uint32_t ms) {
    close();
    if (name.empty())
        return false;

    uint8_t ch = fmt.channels ? fmt.channels : 1;
    uint32_t rate = fmt.rate ? fmt.rate : 48000;
    if (ms < 20)
        ms = 20;
    if (ms > 2000)
        ms = 2000;
    size_t cap = static_cast<size_t>(rate) * ms / 1000;
    if (cap < 256)
        cap = 256;
    size_t bytes = sizeof(ShareHead) + cap * ch * sizeof(float);

    const char* scopes[] = {"Global\\", "Local\\"};
    for (const char* scope : scopes) {
        std::string full = std::string(scope) + name;
        HANDLE file = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         static_cast<DWORD>(bytes), full.c_str());
        if (!file)
            continue;
        bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        void* view = MapViewOfFile(file, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!view) {
            CloseHandle(file);
            continue;
        }
        ShareHead* head = reinterpret_cast<ShareHead*>(view);
        size_t room = viewBytes(view);
        if (existed && head->magic == kShareMagic && head->live.load() != 0) {
            UnmapViewOfFile(view);
            CloseHandle(file);
            return false;
        }
        if (room < sizeof(ShareHead) + 256 * ch * sizeof(float)) {
            UnmapViewOfFile(view);
            CloseHandle(file);
            continue;
        }
        size_t fits = (room - sizeof(ShareHead)) / (ch * sizeof(float));
        if (cap > fits)
            cap = fits;
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, evName(full).c_str());
        if (!ev) {
            UnmapViewOfFile(view);
            CloseHandle(file);
            continue;
        }

        std::memset(view, 0, sizeof(ShareHead));
        head->magic = kShareMagic;
        head->version = kShareVersion;
        head->rate = rate;
        head->channels = ch;
        head->frames = static_cast<uint32_t>(cap);
        head->stride = sizeof(float);
        head->pid = static_cast<uint32_t>(GetCurrentProcessId());
        head->write.store(0);
        head->read.store(0);
        head->dropped.store(0);
        head->live.store(1);

        file_ = file;
        map_ = view;
        event_ = ev;
        head_ = head;
        data_ = reinterpret_cast<float*>(static_cast<char*>(view) + sizeof(ShareHead));
        std::memset(data_, 0, cap * ch * sizeof(float));
        cap_ = cap;
        ch_ = ch;
        path_ = full;
        return true;
    }
    return false;
}

void ShareWriter::put(const float* in, size_t frames) {
    if (!head_ || !frames)
        return;
    if (frames > cap_) {
        head_->dropped.fetch_add(frames - cap_);
        in += (frames - cap_) * ch_;
        frames = cap_;
    }
    uint64_t w = head_->write.load(std::memory_order_relaxed);
    size_t idx = static_cast<size_t>(w % cap_);
    size_t first = std::min(frames, cap_ - idx);
    std::memcpy(data_ + idx * ch_, in, first * ch_ * sizeof(float));
    if (first < frames)
        std::memcpy(data_, in + first * ch_, (frames - first) * ch_ * sizeof(float));
    head_->write.store(w + frames, std::memory_order_release);
    SetEvent(static_cast<HANDLE>(event_));
}

void ShareWriter::close() {
    if (head_) {
        head_->live.store(0);
        if (event_)
            SetEvent(static_cast<HANDLE>(event_));
    }
    if (map_)
        UnmapViewOfFile(map_);
    if (file_)
        CloseHandle(static_cast<HANDLE>(file_));
    if (event_)
        CloseHandle(static_cast<HANDLE>(event_));
    map_ = nullptr;
    file_ = nullptr;
    event_ = nullptr;
    head_ = nullptr;
    data_ = nullptr;
    cap_ = 0;
    ch_ = 0;
    path_.clear();
}

struct ShareReader::Impl {
    void* map = nullptr;
    void* file = nullptr;
    void* event = nullptr;
    ShareHead* head = nullptr;
    const float* data = nullptr;
    size_t cap = 0;
    uint8_t ch = 0;
    uint64_t pos = 0;
    uint64_t missed = 0;

    ~Impl() { shut(); }

    void shut() {
        if (map)
            UnmapViewOfFile(map);
        if (file)
            CloseHandle(static_cast<HANDLE>(file));
        if (event)
            CloseHandle(static_cast<HANDLE>(event));
        map = nullptr;
        file = nullptr;
        event = nullptr;
        head = nullptr;
        data = nullptr;
    }
};

ShareReader::ShareReader() : impl_(new Impl()) {}

ShareReader::~ShareReader() = default;

std::shared_ptr<ShareReader> ShareReader::open(const std::string& name) {
    if (name.empty())
        return nullptr;

    std::vector<std::string> tries;
    if (name.find('\\') != std::string::npos) {
        tries.push_back(name);
    } else {
        tries.push_back("Global\\" + name);
        tries.push_back("Local\\" + name);
    }

    for (const std::string& full : tries) {
        HANDLE file = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, full.c_str());
        if (!file)
            continue;
        void* view = MapViewOfFile(file, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!view) {
            CloseHandle(file);
            continue;
        }
        ShareHead* head = reinterpret_cast<ShareHead*>(view);
        if (head->magic != kShareMagic || head->version != kShareVersion || !head->frames ||
            !head->channels || head->channels > 8 || head->stride != sizeof(float) ||
            !head->live.load()) {
            UnmapViewOfFile(view);
            CloseHandle(file);
            continue;
        }
        size_t want = sizeof(ShareHead) +
                      static_cast<size_t>(head->frames) * head->channels * sizeof(float);
        if (viewBytes(view) < want) {
            UnmapViewOfFile(view);
            CloseHandle(file);
            continue;
        }
        std::shared_ptr<ShareReader> self(new ShareReader());
        self->impl_->file = file;
        self->impl_->map = view;
        self->impl_->event = OpenEventA(SYNCHRONIZE, FALSE, evName(full).c_str());
        self->impl_->head = head;
        self->impl_->data = reinterpret_cast<const float*>(static_cast<char*>(view) +
                                                          sizeof(ShareHead));
        self->impl_->cap = head->frames;
        self->impl_->ch = static_cast<uint8_t>(head->channels);
        self->impl_->pos = head->write.load(std::memory_order_acquire);
        return self;
    }
    return nullptr;
}

Format ShareReader::format() const {
    Format f;
    if (impl_->head) {
        f.rate = impl_->head->rate;
        f.channels = static_cast<uint8_t>(impl_->head->channels);
    }
    return f;
}

size_t ShareReader::capacity() const {
    return impl_->cap;
}

size_t ShareReader::pending() const {
    if (!impl_->head)
        return 0;
    uint64_t w = impl_->head->write.load(std::memory_order_acquire);
    if (w <= impl_->pos)
        return 0;
    uint64_t n = w - impl_->pos;
    return static_cast<size_t>(n > impl_->cap ? impl_->cap : n);
}

size_t ShareReader::read(float* out, size_t frames) {
    Impl& s = *impl_;
    if (!s.head || !frames || !out)
        return 0;
    uint64_t w = s.head->write.load(std::memory_order_acquire);
    if (w < s.pos)
        s.pos = w;
    uint64_t avail = w - s.pos;
    if (avail > s.cap) {
        s.missed += avail - s.cap;
        s.pos = w - s.cap;
        avail = s.cap;
    }
    size_t n = static_cast<size_t>(std::min<uint64_t>(frames, avail));
    if (!n)
        return 0;
    size_t idx = static_cast<size_t>(s.pos % s.cap);
    size_t first = std::min(n, s.cap - idx);
    std::memcpy(out, s.data + idx * s.ch, first * s.ch * sizeof(float));
    if (first < n)
        std::memcpy(out + first * s.ch, s.data, (n - first) * s.ch * sizeof(float));
    s.pos += n;
    s.head->read.store(s.pos, std::memory_order_relaxed);
    return n;
}

bool ShareReader::wait(uint32_t timeoutMs) {
    if (!impl_->event)
        return false;
    return WaitForSingleObject(static_cast<HANDLE>(impl_->event), timeoutMs) == WAIT_OBJECT_0;
}

uint64_t ShareReader::dropped() const {
    return impl_->missed + (impl_->head ? impl_->head->dropped.load(std::memory_order_relaxed) : 0);
}

void ShareReader::close() {
    impl_->shut();
}

#else

ShareWriter::~ShareWriter() = default;
bool ShareWriter::open(const std::string&, const Format&, uint32_t) {
    return false;
}
void ShareWriter::put(const float*, size_t) {}
void ShareWriter::close() {}
size_t ShareWriter::capacity() const {
    return cap_;
}
uint64_t ShareWriter::written() const {
    return 0;
}
uint64_t ShareWriter::dropped() const {
    return 0;
}

struct ShareReader::Impl {};

ShareReader::ShareReader() : impl_(new Impl()) {}
ShareReader::~ShareReader() = default;
std::shared_ptr<ShareReader> ShareReader::open(const std::string&) {
    return nullptr;
}
Format ShareReader::format() const {
    return Format();
}
size_t ShareReader::capacity() const {
    return 0;
}
size_t ShareReader::pending() const {
    return 0;
}
size_t ShareReader::read(float*, size_t) {
    return 0;
}
bool ShareReader::wait(uint32_t) {
    return false;
}
uint64_t ShareReader::dropped() const {
    return 0;
}
void ShareReader::close() {}

#endif

}
