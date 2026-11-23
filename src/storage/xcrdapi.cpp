// SPDX-License-Identifier: AGPL-3.0-or-later
//
// XCRDAPI.dll: console DirectStorage (dstorage_xs.h) over Win32 file I/O and zlib.
//
// On the console, DirectStorage moves data from the game package or memory into title memory,
// optionally through the hardware zlib/BCPack decompressors and the texture swizzler. Here every
// destination is ordinary title memory (a CPU address or physical pages through xmem.dll), so a
// queue is a worker thread that executes its submitted commands in order:
//   request  read (file or memory) -> optional zlib inflate -> copy into the destination;
//   status   publish the batch result in a status array entry;
//   signal   ID3D12Fence::Signal on the console fence (d3d12_xs.dll);
//   event    SetEvent.
// BCPack requests decode with the SDK's BCPackDecompress (BCPack.dll, copied from the user's SDK at
// run time); swizzled requests fail with their DirectStorage error code until implemented.
#include <windows.h>

#include <d3d12_xs.h>
#include <dstorage_xs.h>
#include <dstorageerr.h>
#include <zlib.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xsl/log.h"

#define CH "storage"

namespace {
constexpr size_t kPage = 64 * 1024;

template <class... I> class ComObject : public I... {
public:
    virtual ~ComObject() = default;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --m_refs;
        if (!n) delete this;
        return n;
    }

protected:
    std::atomic<ULONG> m_refs{1};
};

// Title memory given as 64 KB physical page numbers (xmem.dll maps them on demand).
struct PageAccess {
    typedef void*(WINAPI* View)(ULONG_PTR);
    typedef void(WINAPI* Unview)(void*);
    View view = nullptr;
    Unview unview = nullptr;
    PageAccess() {
        HMODULE m = GetModuleHandleW(L"xmem.dll");
        if (m) {
            view = (View)GetProcAddress(m, "xsl_xmem_page_view");
            unview = (Unview)GetProcAddress(m, "xsl_xmem_page_unview");
        }
    }
    // Copy between a linear buffer and pages[] starting `offset` bytes into the first page.
    bool copy(const ULONG_PTR* pages, size_t offset, void* linear, size_t n, bool toPages) {
        if (!view || !unview) return false;
        size_t done = 0;
        while (done < n) {
            size_t pos = offset + done, page = pos / kPage, in = pos % kPage;
            size_t chunk = kPage - in < n - done ? kPage - in : n - done;
            auto* v = (uint8_t*)view(pages[page]);
            if (!v) return false;
            if (toPages) memcpy(v + in, (uint8_t*)linear + done, chunk);
            else memcpy((uint8_t*)linear + done, v + in, chunk);
            unview(v);
            done += chunk;
        }
        return true;
    }
};
PageAccess& pages() {
    static PageAccess p;
    return p;
}

// BCPack block decompression from the SDK's BCPack.dll.
typedef HRESULT(__cdecl* PFN_BCPackDecompress)(size_t, void*, size_t, const void*, size_t);
PFN_BCPackDecompress bcpack() {
    static PFN_BCPackDecompress f = [] {
        HMODULE m = LoadLibraryW(L"BCPack.dll");
        auto p = m ? (PFN_BCPackDecompress)GetProcAddress(m, "BCPackDecompress") : nullptr;
        if (!p) XSL_WARN(CH, "BCPack.dll not found: BCPack requests will fail");
        return p;
    }();
    return f;
}

// ---- Files ------------------------------------------------------------------------------------
class File final : public ComObject<IDStorageFileX> {
public:
    File(HANDLE h, std::wstring path) : m_h(h), m_path(std::move(path)) {}
    ~File() override { Close(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDStorageFileX)) { AddRef(); *ppv = this; return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    void Close() override {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h);
        m_h = INVALID_HANDLE_VALUE;
    }
    HANDLE GetHandle() override {
        std::lock_guard<std::mutex> g(m_mu);
        HANDLE dup = nullptr;
        if (m_h == INVALID_HANDLE_VALUE || !DuplicateHandle(GetCurrentProcess(), m_h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return INVALID_HANDLE_VALUE;
        return dup;
    }
    HRESULT read(uint64_t offset, void* out, uint32_t size) {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_h == INVALID_HANDLE_VALUE) return E_DSTORAGE_FILE_NOT_OPEN;
        OVERLAPPED ov{};
        ov.Offset = (DWORD)offset;
        ov.OffsetHigh = (DWORD)(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(m_h, out, size, &got, &ov)) {
            DWORD e = GetLastError();
            return e == ERROR_HANDLE_EOF ? E_DSTORAGE_END_OF_FILE : HRESULT_FROM_WIN32(e);
        }
        return got == size ? S_OK : E_DSTORAGE_END_OF_FILE;
    }
    const std::wstring& path() const { return m_path; }

private:
    std::mutex m_mu;
    HANDLE m_h;
    std::wstring m_path;
};

// ---- Status arrays ----------------------------------------------------------------------------
class StatusArray final : public ComObject<IDStorageStatusArrayX> {
public:
    explicit StatusArray(UINT32 n) : m_results(n) {
        for (auto& r : m_results) r = S_OK;  // entries start complete
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDStorageStatusArrayX)) { AddRef(); *ppv = this; return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    bool IsComplete(UINT32 i) override { return i < m_results.size() && m_results[i].load() != E_PENDING; }
    HRESULT GetHResult(UINT32 i) override { return i < m_results.size() ? m_results[i].load() : E_DSTORAGE_INDEX_BOUND; }
    void set(UINT32 i, HRESULT hr) { if (i < m_results.size()) m_results[i] = hr; }

private:
    std::vector<std::atomic<HRESULT>> m_results;
};

// ---- Queues -----------------------------------------------------------------------------------
struct Command {
    DSTORAGE_COMMAND_TYPE type;
    DSTORAGE_REQUEST request;  // REQUEST
    File* file = nullptr;      // REQUEST from a file (holds a reference)
    StatusArray* status = nullptr;
    UINT32 index = 0;
    ID3D12Fence* fence = nullptr;
    UINT64 value = 0;
    HANDLE event = nullptr;
};

class Queue final : public ComObject<IDStorageQueueX2> {
public:
    explicit Queue(const DSTORAGE_QUEUE_DESC1& d) : m_desc(d) {
        m_errorEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_autoSubmit = d.Options.AutoSubmitDisabled ? 0 : (UINT16)(d.Capacity / 2 ? d.Capacity / 2 : 1);
        m_thread = std::thread([this] { run(); });
    }
    ~Queue() override {
        {
            std::lock_guard<std::mutex> g(m_mu);
            m_quit = true;
        }
        m_cv.notify_all();
        m_thread.join();
        for (auto& c : m_recording) drop(c);
        CloseHandle(m_errorEvent);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDStorageQueueX) || riid == __uuidof(IDStorageQueueX1) ||
            riid == __uuidof(IDStorageQueueX2)) {
            AddRef();
            *ppv = static_cast<IDStorageQueueX2*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    void EnqueueRequest(const DSTORAGE_REQUEST* r) override {
        Command c{DSTORAGE_COMMAND_TYPE_REQUEST, *r};
        if (r->Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE && r->File) {
            c.file = static_cast<File*>(r->File);
            c.file->AddRef();
        }
        record(c, true);
    }
    void EnqueueStatus(IDStorageStatusArrayX* s, UINT32 i) override {
        Command c{DSTORAGE_COMMAND_TYPE_STATUS};
        c.status = static_cast<StatusArray*>(s);
        c.status->AddRef();
        c.index = i;
        c.status->set(i, E_PENDING);
        record(c, false);
    }
    void EnqueueSignal(ID3D12Fence* f, UINT64 v) override {
        Command c{DSTORAGE_COMMAND_TYPE_SIGNAL};
        c.fence = f;
        c.fence->AddRef();
        c.value = v;
        record(c, false);
    }
    void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE h) override {
        Command c{DSTORAGE_COMMAND_TYPE_EVENT};
        c.event = h;
        record(c, false);
    }
    void Submit() override {
        {
            std::lock_guard<std::mutex> g(m_mu);
            for (auto& c : m_recording) m_submitted.push_back(c);
            m_recording.clear();
            m_sinceSubmit = 0;
        }
        m_cv.notify_all();
    }
    void CancelRequestsWithTag(UINT64 mask, UINT64 value) override {
        std::lock_guard<std::mutex> g(m_mu);
        for (auto* list : {&m_recording, &m_submitted})
            for (auto& c : *list)
                if (c.type == DSTORAGE_COMMAND_TYPE_REQUEST && (c.request.CancellationTag & mask) == value) {
                    drop(c);
                    c.type = DSTORAGE_COMMAND_TYPE_NONE;  // cancelled: skipped, not a failure
                }
    }
    void Close() override {
        std::lock_guard<std::mutex> g(m_mu);
        m_closed = true;
    }
    HANDLE GetErrorEvent() override { return m_errorEvent; }
    void RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* r) override {
        std::lock_guard<std::mutex> g(m_mu);
        *r = m_error;
        m_error = {};
        ResetEvent(m_errorEvent);
    }
    void Query(DSTORAGE_QUEUE_INFO* info) override {
        DSTORAGE_QUEUE_INFO1 i1;
        Query1(&i1);
        info->Desc = {i1.Desc.SourceType, i1.Desc.Capacity, i1.Desc.Priority, i1.Desc.Name};
        info->EmptySlotCount = i1.EmptySlotCount;
        info->RequestCountUntilAutoSubmit = i1.RequestCountUntilAutoSubmit;
    }
    void Query1(DSTORAGE_QUEUE_INFO1* info) override {
        std::lock_guard<std::mutex> g(m_mu);
        *info = {};
        info->Desc = m_desc;
        size_t used = m_recording.size() + m_submitted.size();
        info->EmptySlotCount = (UINT16)(used < m_desc.Capacity ? m_desc.Capacity - used : 0);
        info->RequestCountUntilAutoSubmit = m_autoSubmit ? (UINT16)(m_autoSubmit - m_sinceSubmit) : 0;
    }

private:
    void record(Command& c, bool isRequest) {
        bool submit = false;
        {
            std::lock_guard<std::mutex> g(m_mu);
            if (m_closed) {
                drop(c);
                fail(c, E_DSTORAGE_QUEUE_CLOSED);
                return;
            }
            m_recording.push_back(c);
            if (isRequest && m_autoSubmit && ++m_sinceSubmit >= m_autoSubmit) submit = true;
        }
        if (submit) Submit();
    }
    static void drop(Command& c) {
        if (c.file) c.file->Release();
        if (c.status) c.status->Release();
        if (c.fence) c.fence->Release();
        c.file = nullptr;
        c.status = nullptr;
        c.fence = nullptr;
    }
    // Caller holds m_mu.
    void fail(const Command& c, HRESULT hr) {
        if (m_error.FailureCount++ == 0) {
            m_error.FirstFailure.HResult = hr;
            m_error.FirstFailure.CommandType = c.type;
            if (c.type == DSTORAGE_COMMAND_TYPE_REQUEST) {
                auto& r = m_error.FirstFailure.Request;
                r.Options = c.request.Options;
                r.Destination = c.request.Destination;
                r.DestinationSize = c.request.DestinationSize;
                if (c.file) wcsncpy(r.Filename, c.file->path().c_str(), MAX_PATH - 1);
                r.FileOffset = c.request.FileOffset;
                r.SourceSize = c.request.SourceSize;
                r.CancellationTag = c.request.CancellationTag;
                if (c.request.Name) strncpy(r.Name, c.request.Name, DSTORAGE_REQUEST_MAX_NAME - 1);
            }
        }
        SetEvent(m_errorEvent);
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 5) XSL_WARN(CH, "DirectStorage command %d failed %08lx", (int)c.type, hr);
    }

    // XSL_DSTORAGE_TRACE=1: each request (first 400): source, sizes, options, the first output
    // bytes and the result.
    static void trace_request(const Command& c, HRESULT hr) {
        // XSL_DSTORAGE_TRACE=1 (first 400 requests) or =<n> (first n)
        static long limit = [] { char b[16]; return GetEnvironmentVariableA("XSL_DSTORAGE_TRACE", b, sizeof b) ? std::max(atol(b) == 1 ? 400L : atol(b), 0L) : 0L; }();
        static volatile long n = 0;
        if (!limit || InterlockedIncrement(&n) > limit) return;
        const DSTORAGE_REQUEST& r = c.request;
        char head[40] = "";
        if (SUCCEEDED(hr) && !r.Options.DestinationIsPhysicalPages && r.Destination && r.DestinationSize)
            for (UINT32 i = 0; i < 8 && i < r.DestinationSize; i++)
                snprintf(head + i * 3, sizeof head - i * 3, "%02x ", ((const uint8_t*)r.Destination)[i]);
        XSL_INFO(CH, "dstorage: %ls @%llu src %u -> dst %u (opts %llx, intermediate %u, name %s): %08lx [%s]",
                 c.file ? c.file->path().c_str() : L"(memory)", (unsigned long long)r.FileOffset, r.SourceSize, r.DestinationSize,
                 (unsigned long long)r.Options.AsUINT64, r.IntermediateSize, r.Name ? r.Name : "-", hr, head);
    }

    HRESULT execute(const DSTORAGE_REQUEST& r, File* file) {
        const auto& o = r.Options;
        if (o.BcpackMode != DSTORAGE_BCPACK_MODE_NONE && !bcpack()) return E_DSTORAGE_INVALID_BCPACK_MODE;
        if (o.SwizzleMode != DSTORAGE_SWIZZLE_MODE_NONE) return E_DSTORAGE_INVALID_SWIZZLE_MODE;
        if (o.SourceType != m_desc.SourceType) return E_DSTORAGE_INVALID_SOURCE_TYPE;
        // Source bytes.
        std::vector<uint8_t> src;
        const uint8_t* in = nullptr;
        if (o.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
            if (!file) return E_DSTORAGE_FILE_NOT_OPEN;
            src.resize(r.SourceSize);
            HRESULT hr = file->read(r.FileOffset, src.data(), r.SourceSize);
            if (FAILED(hr)) return hr;
            in = src.data();
        } else if (o.SourceIsPhysicalPages) {
            src.resize(r.SourceSize);
            if (!pages().copy(r.SourcePageArray, r.SourcePageOffset, src.data(), r.SourceSize, false)) return E_DSTORAGE_ACCESS_VIOLATION;
            in = src.data();
        } else {
            in = (const uint8_t*)r.Source;
        }
        // Destination bytes (staged when the destination is physical pages).
        std::vector<uint8_t> staged;
        uint8_t* out = (uint8_t*)r.Destination;
        if (o.DestinationIsPhysicalPages) {
            staged.resize(r.DestinationSize);
            out = staged.data();
        }
        if (o.BcpackMode != DSTORAGE_BCPACK_MODE_NONE) {
            // zlib (optional) -> BCPack stream of IntermediateSize bytes -> BCn blocks.
            const uint8_t* packed = in;
            size_t packedSize = r.SourceSize;
            std::vector<uint8_t> mid;
            if (o.ZlibDecompress) {
                if (!r.IntermediateSize) return E_DSTORAGE_INVALID_INTERMEDIATE_SIZE;
                mid.resize(r.IntermediateSize);
                uLongf n = r.IntermediateSize;
                int z = uncompress(mid.data(), &n, in, r.SourceSize);
                if (z == Z_DATA_ERROR) return E_DSTORAGE_ZLIB_BAD_DATA;
                if (z != Z_OK || n != r.IntermediateSize) return E_DSTORAGE_DECOMPRESSION_ERROR;
                packed = mid.data();
                packedSize = n;
            }
            if (FAILED(bcpack()((size_t)o.BcpackMode, out, r.DestinationSize, packed, packedSize))) return E_DSTORAGE_BCPACK_BAD_DATA;
        } else if (o.ZlibDecompress) {
            uLongf n = r.DestinationSize;
            int z = uncompress(out, &n, in, r.SourceSize);
            if (z == Z_DATA_ERROR) return E_DSTORAGE_ZLIB_BAD_DATA;
            if (z != Z_OK) return E_DSTORAGE_DECOMPRESSION_ERROR;
            if (n != r.DestinationSize) return E_DSTORAGE_INVALID_DESTINATION_SIZE;
        } else {
            if (r.SourceSize != r.DestinationSize) return E_DSTORAGE_INVALID_DESTINATION_SIZE;
            memcpy(out, in, r.SourceSize);
        }
        if (o.DestinationIsPhysicalPages &&
            !pages().copy(r.DestinationPageArray, r.DestinationPageOffset, staged.data(), staged.size(), true))
            return E_DSTORAGE_ACCESS_VIOLATION;
        return S_OK;
    }

    void run() {
        HRESULT batch = S_OK;  // first failure since the last status command
        for (;;) {
            Command c;
            {
                std::unique_lock<std::mutex> g(m_mu);
                m_cv.wait(g, [this] { return m_quit || !m_submitted.empty(); });
                if (m_submitted.empty()) return;
                c = m_submitted.front();
                m_submitted.pop_front();
            }
            switch (c.type) {
                case DSTORAGE_COMMAND_TYPE_REQUEST: {
                    HRESULT hr = execute(c.request, c.file);
                    trace_request(c, hr);
                    if (FAILED(hr)) {
                        if (SUCCEEDED(batch)) batch = hr;
                        std::lock_guard<std::mutex> g(m_mu);
                        fail(c, hr);
                    }
                    break;
                }
                case DSTORAGE_COMMAND_TYPE_STATUS:
                    c.status->set(c.index, batch);
                    batch = S_OK;
                    break;
                case DSTORAGE_COMMAND_TYPE_SIGNAL: c.fence->Signal(c.value); break;
                case DSTORAGE_COMMAND_TYPE_EVENT: SetEvent(c.event); break;
                default: break;  // cancelled
            }
            drop(c);
        }
    }

    DSTORAGE_QUEUE_DESC1 m_desc;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Command> m_recording, m_submitted;
    UINT16 m_autoSubmit = 0, m_sinceSubmit = 0;
    bool m_closed = false, m_quit = false;
    DSTORAGE_ERROR_RECORD m_error{};
    HANDLE m_errorEvent;
    std::thread m_thread;
};

// ---- Factory ----------------------------------------------------------------------------------
class Factory final : public IDStorageFactoryX2 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDStorageFactoryX) || riid == __uuidof(IDStorageFactoryX1) ||
            riid == __uuidof(IDStorageFactoryX2)) {
            *ppv = static_cast<IDStorageFactoryX2*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }  // process-lifetime singleton
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT CreateQueue(const DSTORAGE_QUEUE_DESC* d, REFIID riid, void** ppv) override {
        DSTORAGE_QUEUE_DESC1 d1{d->SourceType, d->Capacity, d->Priority, d->Name, {}};
        return CreateQueue1(&d1, riid, ppv);
    }
    HRESULT CreateQueue1(const DSTORAGE_QUEUE_DESC1* d, REFIID riid, void** ppv) override {
        if (d->Capacity < DSTORAGE_MIN_QUEUE_CAPACITY || d->Capacity > DSTORAGE_MAX_QUEUE_CAPACITY) return E_DSTORAGE_INVALID_QUEUE_CAPACITY;
        auto* q = new Queue(*d);
        HRESULT hr = q->QueryInterface(riid, ppv);
        q->Release();
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 4)
            XSL_INFO(CH, "DirectStorage queue '%s': %s source, capacity %u", d->Name ? d->Name : "",
                     d->SourceType == DSTORAGE_REQUEST_SOURCE_FILE ? "file" : "memory", d->Capacity);
        return hr;
    }
    HRESULT OpenFile(const WCHAR* path, REFIID riid, void** ppv) override {
        // Console paths (g:/...) are mapped to host paths by d3d12_xs.dll's console drives.
        using MapFn = bool (*)(const wchar_t*, wchar_t*, size_t);
        static MapFn map = (MapFn)GetProcAddress(GetModuleHandleW(L"d3d12_xs.dll"), "xsl_map_console_path");
        WCHAR host[MAX_PATH * 2];
        const WCHAR* use = (map && map(path, host, MAX_PATH * 2)) ? host : path;
        HANDLE h = CreateFileW(use, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            XSL_WARN(CH, "OpenFile '%ls' failed (%lu)", path, e);
            *ppv = nullptr;
            return HRESULT_FROM_WIN32(e);
        }
        auto* f = new File(h, path);
        HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }
    HRESULT CreateStatusArray(UINT32 capacity, PCSTR, REFIID riid, void** ppv) override {
        auto* s = new StatusArray(capacity);
        HRESULT hr = s->QueryInterface(riid, ppv);
        s->Release();
        return hr;
    }
    HRESULT SetCpuAffinity(UINT64) override { return S_OK; }
    void SetDebugFlags(UINT32) override {}
    HRESULT SetStagingBufferSize(UINT32) override { return S_OK; }
    void GetStats(DSTORAGE_STATS* s) override { *s = {}; }
};
}  // namespace

extern "C" HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv) {
    static Factory factory;
    static bool logged = [] { XSL_INFO(CH, "DirectStorage: file and memory sources, zlib, BCPack; no swizzle yet"); return true; }();
    (void)logged;
    return factory.QueryInterface(riid, ppv);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
