#include "runtime/modules/dstorage.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/inflate.hpp"
#include "runtime/modules/kernel.hpp"
#include "runtime/modules/placement.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <variant>
#include <vector>
#include <windows.h>

#include <d3d12.h>
#include <unknwn.h>

// DirectStorage as the Series console has it (dstorage_xs.dll, and XCRDAPI.dll for the
// factory): queues of read requests, with marks in between that set a status or signal a
// fence once everything before them is done. Here a queue does its work when it is
// submitted, on the thread that submits it, in order. Reads may be inflated (zlib); the
// console's texture compression (BCPack) is not known here, and such a request fails.
namespace xv::runtime::modules {
namespace {
constexpr GUID file_iid{
    0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
constexpr GUID factory_iid{
    0xfb3b54ac, 0xbf82, 0x463a, {0xb7, 0x77, 0x73, 0x19, 0x58, 0x37, 0x86, 0x4d}};
constexpr GUID status_iid{
    0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr GUID queue_iid{
    0xa3f6649a, 0x8c2c, 0x4b3a, {0x8b, 0x6e, 0x10, 0x18, 0x6a, 0x04, 0xb9, 0xbc}};

// a later GDK's IDStorageFactoryX, asked for by newer titles; its methods start as the
// earlier one's do
constexpr GUID factory_iid_later{
    0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
// and the queue that later factory makes (its slot 10)
constexpr GUID queue_iid_later{
    0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
// A method of the later interfaces nothing here knows yet: it says so once, and fails
void unknown_method(const char *object, int slot) {
    static std::atomic<std::uint32_t> shown[2]{};
    auto &seen = shown[object[0] == 'q'];
    if (const auto bit = 1u << (slot & 31); !(seen.fetch_or(bit) & bit))
        std::fprintf(stderr, "[Runtime] DirectStorage: unimplemented %s method %d\n", object, slot);
}

template <class Interface> class Object : public Interface {
  public:
    explicit Object(const GUID &iid, const GUID &also = GUID_NULL) : iid_(iid), also_(also) {}
    virtual ~Object() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid != IID_IUnknown && iid != iid_ && (also_ == GUID_NULL || iid != also_)) {
            *object = nullptr;
            if (static std::atomic<int> shown{0}; shown++ < 8)
                std::fprintf(stderr,
                             "[Runtime] DirectStorage: interface {%08lx-%04hx-%04hx-%02x%02x-"
                             "%02x%02x%02x%02x%02x%02x} is not known\n",
                             iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1],
                             iid.Data4[2], iid.Data4[3], iid.Data4[4], iid.Data4[5], iid.Data4[6],
                             iid.Data4[7]);
            return E_NOINTERFACE;
        }
        AddRef();
        *object = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }

  private:
    GUID iid_, also_;
    std::atomic<ULONG> references_{1};
};

// the interfaces, with their methods in the order the console's header has them
struct FileInterface : IUnknown {
    virtual void Close() = 0;
    virtual HANDLE GetHandle() = 0;
};
struct StatusInterface : IUnknown {
    virtual bool IsComplete(UINT32 index) = 0;
    virtual HRESULT GetHResult(UINT32 index) = 0;
};
struct QueueDesc {
    UINT64 source_type;
    UINT16 capacity;
    INT8 priority;
    const char *name;
};
struct QueueInfo {
    QueueDesc desc;
    UINT16 empty_slots, until_submit;
};
struct Request {
    UINT64 options; // bit 0 zlib, bits 1 to 4 BCPack, bit 21 destination in pages, bit 22
                    // source in memory, bit 23 source in pages
    void *destination;
    UINT64 destination_page_offset; // 16 bits of it, in a union as large as two pointers
    UINT32 destination_size;
    void *source; // the file, or the memory
    UINT64 file_offset;
    UINT32 source_size, intermediate_size;
    UINT64 tag;
    const char *name;
};
static_assert(offsetof(Request, destination_size) == 24 && offsetof(Request, source) == 32 &&
              offsetof(Request, source_size) == 48 && offsetof(Request, tag) == 56);
struct ErrorRecord {
    UINT32 failures;
    HRESULT first;
    std::uint8_t rest[1024];
};
struct QueueInterface : IUnknown {
    virtual void EnqueueRequest(const Request *request) = 0;
    virtual void EnqueueStatus(StatusInterface *status, UINT32 index) = 0;
    virtual void EnqueueSignal(ID3D12Fence *fence, UINT64 value) = 0;
    virtual void Submit() = 0;
    virtual void CancelRequestsWithTag(UINT64 mask, UINT64 value) = 0;
    virtual void Close() = 0;
    virtual HANDLE GetErrorEvent() = 0;
    virtual void RetrieveErrorRecord(ErrorRecord *record) = 0;
    virtual void Query(QueueInfo *info) = 0;
    // the later queue's methods past the earlier one's (slots 12 on)
    virtual HRESULT Later12() { return unknown_method("queue", 12), E_NOTIMPL; }
    virtual HRESULT Later13() { return unknown_method("queue", 13), E_NOTIMPL; }
    virtual HRESULT Later14() { return unknown_method("queue", 14), E_NOTIMPL; }
    virtual HRESULT Later15() { return unknown_method("queue", 15), E_NOTIMPL; }
    virtual HRESULT Later16() { return unknown_method("queue", 16), E_NOTIMPL; }
    virtual HRESULT Later17() { return unknown_method("queue", 17), E_NOTIMPL; }
};
struct FactoryInterface : IUnknown {
    virtual HRESULT CreateQueue(const QueueDesc *desc, REFIID iid, void **queue) = 0;
    virtual HRESULT OpenFile(const WCHAR *path, REFIID iid, void **file) = 0;
    virtual HRESULT CreateStatusArray(UINT32 capacity, const char *name, REFIID iid,
                                      void **status) = 0;
    virtual HRESULT SetCpuAffinity(UINT64 affinity) = 0;
    virtual void SetDebugFlags(UINT32 flags) = 0;
    virtual HRESULT SetStagingBufferSize(UINT32 size) = 0;
    // the later factory's (slots 9 on): 10 makes the later queue from a longer description
    // (the earlier one's, then a word kit 0x20004's title sets to 1)
    virtual HRESULT Later9() { return unknown_method("factory", 9), E_NOTIMPL; }
    virtual HRESULT CreateQueueLater(const QueueDesc *desc, REFIID iid, void **queue) = 0;
    virtual HRESULT Later11() { return unknown_method("factory", 11), E_NOTIMPL; }
    virtual HRESULT Later12() { return unknown_method("factory", 12), E_NOTIMPL; }
    virtual HRESULT Later13() { return unknown_method("factory", 13), E_NOTIMPL; }
    virtual HRESULT Later14() { return unknown_method("factory", 14), E_NOTIMPL; }
};

class File final : public Object<FileInterface> {
  public:
    explicit File(HANDLE handle) : Object(file_iid), handle_(handle) {}
    ~File() override { Close(); }
    void Close() override {
        if (const auto handle = handle_.exchange(INVALID_HANDLE_VALUE);
            handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    HANDLE GetHandle() override { return handle_; }

  private:
    std::atomic<HANDLE> handle_;
};

class Status final : public Object<StatusInterface> {
  public:
    explicit Status(UINT32 capacity) : Object(status_iid), entries_(capacity) {}
    bool IsComplete(UINT32 index) override {
        const std::lock_guard lock(mutex_);
        return index < entries_.size() && entries_[index].done;
    }
    HRESULT GetHResult(UINT32 index) override {
        const std::lock_guard lock(mutex_);
        if (index >= entries_.size())
            return E_INVALIDARG;
        return entries_[index].done ? entries_[index].result : E_PENDING;
    }
    void set(UINT32 index, HRESULT result) {
        const std::lock_guard lock(mutex_);
        if (index < entries_.size())
            entries_[index] = {true, result};
    }
    void pending(UINT32 index) {
        const std::lock_guard lock(mutex_);
        if (index < entries_.size())
            entries_[index] = {};
    }

  private:
    struct Entry {
        bool done = false;
        HRESULT result = S_OK;
    };
    std::mutex mutex_;
    std::vector<Entry> entries_;
};

HRESULT read(const Request &request) {
    constexpr UINT64 zlib = 1, bcpack = 0xf << 1, destination_pages = 1ull << 21,
                     from_memory = 1ull << 22, source_pages = 1ull << 23;
    if (request.options & (bcpack | destination_pages | source_pages)) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] DirectStorage: a request with options 0x%llx (texture "
                         "compression or physical pages), which is not done\n",
                         request.options);
        return E_NOTIMPL;
    }
    if (!request.destination || !request.source)
        return E_INVALIDARG;
    const bool inflated = request.options & zlib;
    std::vector<std::uint8_t> packed;
    const std::uint8_t *data = nullptr;
    if (request.options & from_memory) {
        data = static_cast<const std::uint8_t *>(request.source);
    } else {
        // read straight to where it goes unless it has to be inflated first
        auto *const target = inflated ? (packed.resize(request.source_size), packed.data())
                                      : static_cast<std::uint8_t *>(request.destination);
        if (!inflated) {
            if (request.source_size > request.destination_size)
                return E_INVALIDARG;
            prepare_title_write(target, request.source_size);
        }
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(request.file_offset);
        at.OffsetHigh = static_cast<DWORD>(request.file_offset >> 32);
        DWORD done = 0;
        if (!ReadFile(static_cast<FileInterface *>(request.source)->GetHandle(), target,
                      request.source_size, &done, &at) ||
            done != request.source_size)
            return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_HANDLE_EOF);
        if (!inflated)
            return S_OK;
        data = packed.data();
    }
    prepare_title_write(request.destination, request.destination_size);
    if (!inflated) {
        if (request.source_size > request.destination_size)
            return E_INVALIDARG;
        std::memcpy(request.destination, data, request.source_size);
        return S_OK;
    }
    std::vector<std::uint8_t> plain;
    if (!inflate_zlib({data, request.source_size}, plain) ||
        plain.size() > request.destination_size)
        return E_FAIL;
    std::memcpy(request.destination, plain.data(), plain.size());
    return S_OK;
}

class Queue final : public Object<QueueInterface> {
  public:
    explicit Queue(const QueueDesc &desc)
        : Object(queue_iid, queue_iid_later), desc_(desc), name_(desc.name ? desc.name : ""),
          error_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        desc_.name = name_.c_str();
    }
    ~Queue() override { CloseHandle(error_); }
    void EnqueueRequest(const Request *request) override {
        if (!request)
            return;
        {
            const std::lock_guard lock(mutex_);
            // the file is held until the request is done
            if (!(request->options & 1ull << 22) && request->source)
                static_cast<IUnknown *>(request->source)->AddRef();
            pending_.emplace_back(*request);
        }
        submit_when_full();
    }
    void EnqueueStatus(StatusInterface *status, UINT32 index) override {
        if (!status)
            return;
        status->AddRef();
        static_cast<Status *>(status)->pending(index);
        {
            const std::lock_guard lock(mutex_);
            pending_.emplace_back(Mark{static_cast<Status *>(status), index});
        }
        submit_when_full();
    }
    void EnqueueSignal(ID3D12Fence *fence, UINT64 value) override {
        if (!fence)
            return;
        fence->AddRef();
        {
            const std::lock_guard lock(mutex_);
            pending_.emplace_back(Signal{fence, value});
        }
        submit_when_full();
    }
    void Submit() override {
        std::vector<Entry> work;
        {
            const std::lock_guard lock(mutex_);
            work.swap(pending_);
        }
        // one submission at a time, so that marks follow the requests before them
        const std::lock_guard running(running_);
        HRESULT since_mark = S_OK;
        for (auto &entry : work) {
            if (auto *const request = std::get_if<Request>(&entry)) {
                const auto result = read(*request);
                // XV_FILE_DEST=<hex address>: the requests whose destination holds it ("all":
                // every request)
                if (static const char *const watched = std::getenv("XV_FILE_DEST"); watched) {
                    const bool all = !std::strcmp(watched, "all");
                    const auto at = std::strtoull(watched, nullptr, 16);
                    const auto start = reinterpret_cast<std::uintptr_t>(request->destination);
                    if (all || (at >= start && at < start + request->destination_size))
                        std::fprintf(stderr,
                                     "[Runtime] DirectStorage request to %p (%u bytes) holds %llx: "
                                     "options 0x%llx, %u bytes at %llu of %p: 0x%08lx\n",
                                     request->destination, request->destination_size,
                                     static_cast<unsigned long long>(at), request->options,
                                     request->source_size, request->file_offset, request->source,
                                     static_cast<unsigned long>(result));
                }
                // XV_FILE_TRACE: each kind of request once, whatever came first
                if (std::getenv("XV_FILE_TRACE")) {
                    static std::mutex kinds_mutex;
                    static std::vector<UINT64> kinds;
                    const std::lock_guard lock(kinds_mutex);
                    if (std::find(kinds.begin(), kinds.end(), request->options) == kinds.end()) {
                        kinds.push_back(request->options);
                        std::fprintf(stderr,
                                     "[Runtime] DirectStorage: first request with options "
                                     "0x%llx (%u bytes to %u, intermediate %u): 0x%08lx\n",
                                     request->options, request->source_size,
                                     request->destination_size, request->intermediate_size,
                                     static_cast<unsigned long>(result));
                    }
                }
                // XV_FILE_TRACE: the first requests, and every one that fails
                if (static std::atomic<int> shown{0};
                    std::getenv("XV_FILE_TRACE") && (FAILED(result) || shown < 24) && shown++ < 200)
                    std::fprintf(stderr,
                                 "[Runtime] DirectStorage request: options 0x%llx, %u bytes at "
                                 "%llu to %u bytes (intermediate %u): 0x%08lx\n",
                                 request->options, request->source_size, request->file_offset,
                                 request->destination_size, request->intermediate_size,
                                 static_cast<unsigned long>(result));
                if (!(request->options & 1ull << 22) && request->source)
                    static_cast<IUnknown *>(request->source)->Release();
                if (FAILED(result)) {
                    if (SUCCEEDED(since_mark))
                        since_mark = result;
                    const std::lock_guard lock(mutex_);
                    if (!failures_++)
                        first_failure_ = result;
                    SetEvent(error_);
                }
            } else if (auto *const mark = std::get_if<Mark>(&entry)) {
                mark->status->set(mark->index, since_mark);
                mark->status->Release();
                since_mark = S_OK;
            } else if (auto *const signal = std::get_if<Signal>(&entry)) {
                signal->fence->Signal(signal->value);
                signal->fence->Release();
            }
        }
    }
    void CancelRequestsWithTag(UINT64 mask, UINT64 value) override {
        const std::lock_guard lock(mutex_);
        std::erase_if(pending_, [&](Entry &entry) {
            auto *const request = std::get_if<Request>(&entry);
            if (!request || (request->tag & mask) != value)
                return false;
            if (!(request->options & 1ull << 22) && request->source)
                static_cast<IUnknown *>(request->source)->Release();
            return true;
        });
    }
    void Close() override { Submit(); }
    HANDLE GetErrorEvent() override { return error_; }
    void RetrieveErrorRecord(ErrorRecord *record) override {
        if (!record)
            return;
        const std::lock_guard lock(mutex_);
        *record = {};
        record->failures = failures_;
        record->first = first_failure_;
        failures_ = 0;
        first_failure_ = S_OK;
        ResetEvent(error_);
    }
    void Query(QueueInfo *info) override {
        if (!info)
            return;
        const std::lock_guard lock(mutex_);
        const auto used =
            static_cast<UINT16>(std::min<std::size_t>(pending_.size(), desc_.capacity));
        *info = {desc_, static_cast<UINT16>(desc_.capacity - used),
                 static_cast<UINT16>(desc_.capacity / 2 > used ? desc_.capacity / 2 - used : 0)};
    }

  private:
    struct Mark {
        Status *status;
        UINT32 index;
    };
    struct Signal {
        ID3D12Fence *fence;
        UINT64 value;
    };
    using Entry = std::variant<Request, Mark, Signal>;
    // a queue submits itself when half of it is taken
    void submit_when_full() {
        bool full;
        {
            const std::lock_guard lock(mutex_);
            full = pending_.size() >= std::max<std::size_t>(desc_.capacity / 2, 1);
        }
        if (full)
            Submit();
    }
    QueueDesc desc_;
    std::string name_;
    HANDLE error_;
    std::mutex mutex_, running_;
    std::vector<Entry> pending_;
    UINT32 failures_ = 0;
    HRESULT first_failure_ = S_OK;
};

class Factory final : public Object<FactoryInterface> {
  public:
    Factory() : Object(factory_iid, factory_iid_later) {}
    HRESULT CreateQueue(const QueueDesc *desc, REFIID iid, void **queue) override {
        if (!desc || !queue)
            return E_POINTER;
        auto *const made = new Queue(*desc);
        const auto result = made->QueryInterface(iid, queue);
        made->Release();
        return result;
    }
    HRESULT OpenFile(const WCHAR *path, REFIID iid, void **file) override {
        if (!path || !file)
            return E_POINTER;
        *file = nullptr;
        const auto handle =
            CreateFileW(console_file_path(path).c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
                        nullptr); // a title opens the root of its package as well
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (static std::atomic<int> shown{0}; shown++ < 6)
                std::fprintf(stderr,
                             "[Runtime] DirectStorage: cannot open %ls (as %ls): error %lu\n", path,
                             console_file_path(path).c_str(), error);
            return HRESULT_FROM_WIN32(error);
        }
        if (static std::atomic<int> shown{0}; std::getenv("XV_FILE_TRACE") && shown++ < 40)
            std::fprintf(stderr, "[Runtime] DirectStorage: opened %ls (%zu characters) from %s\n",
                         path, wcslen(path), detail::guest_location(_ReturnAddress()).c_str());
        auto *const made = new File(handle);
        const auto result = made->QueryInterface(iid, file);
        made->Release();
        return result;
    }
    HRESULT CreateStatusArray(UINT32 capacity, const char *, REFIID iid, void **status) override {
        if (!status)
            return E_POINTER;
        auto *const made = new Status(capacity);
        const auto result = made->QueryInterface(iid, status);
        made->Release();
        return result;
    }
    HRESULT CreateQueueLater(const QueueDesc *desc, REFIID iid, void **queue) override {
        return CreateQueue(desc, iid, queue);
    }
    HRESULT SetCpuAffinity(UINT64) override { return S_OK; }
    void SetDebugFlags(UINT32) override {}
    HRESULT SetStagingBufferSize(UINT32) override { return S_OK; }
};

HRESULT WINAPI get_factory(const GUID *iid, void **factory) {
    if (!iid || !factory)
        return E_POINTER;
    static Factory *const one = new Factory;
    return one->QueryInterface(*iid, factory);
}
} // namespace

void install_dstorage(Registry &registry) {
    for (const auto *module : {"dstorage_xs.dll", "xcrdapi.dll"})
        registry.add(module, "DStorageGetFactory", reinterpret_cast<void *>(&get_factory));
}
} // namespace xv::runtime::modules
