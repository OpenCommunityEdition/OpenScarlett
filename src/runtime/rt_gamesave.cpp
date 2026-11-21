// SPDX-License-Identifier: AGPL-3.0-or-later
//
// XGameSave over host folders: <saves>/<configId>/<container>/<blob>.blob, with the container's
// display name in <container>/.displayname. Updates are staged in memory and applied atomically
// per blob (write to .tmp, then rename) when submitted. No cloud sync: every container is in sync.
#include <windows.h>

#include <XGameSave.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "rt_async.h"
#include "runtime_classes.h"
#include "xsl/log.h"

#define CH "rt"

namespace {
std::string saves_root() {
    char buf[MAX_PATH];
    if (GetEnvironmentVariableA("XSL_SAVE_DIR", buf, sizeof buf)) return buf;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string name(exe);
    name = name.substr(name.find_last_of("\\/") + 1);
    name = name.substr(0, name.find_last_of('.'));
    std::string base = "C:\\users\\Public\\OpenScarlett";
    CreateDirectoryA(base.c_str(), nullptr);
    base += "\\" + name;
    CreateDirectoryA(base.c_str(), nullptr);
    base += "\\GameSaves";
    CreateDirectoryA(base.c_str(), nullptr);
    return base;
}

// Blob and container names may contain characters a file system rejects; encode them.
std::string encode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.') o += (char)c;
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}
std::string decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else o += s[i];
    }
    return o;
}

struct Provider { std::string dir; };
struct Container { Provider* provider; std::string name, dir; };
struct Update {
    Container* container;
    std::string displayName;
    std::map<std::string, std::vector<uint8_t>> writes;
    std::vector<std::string> deletes;
};

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = out.empty() || ReadFile(f, out.data(), (DWORD)out.size(), &rd, nullptr);
    CloseHandle(f);
    return ok;
}
bool write_file(const std::string& path, const void* data, size_t n) {
    std::string tmp = path + ".tmp";
    HANDLE f = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = n == 0 || WriteFile(f, data, (DWORD)n, &wr, nullptr);
    CloseHandle(f);
    return ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

template <class F> void for_each_entry(const std::string& dir, bool dirs, F f) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == ".." || n[0] == '.') continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir == dirs && !f(n, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, fd.ftLastWriteTime)) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

time_t to_time_t(FILETIME ft) {
    ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime};
    return (time_t)((u.QuadPart - 116444736000000000ull) / 10000000ull);
}

const char kInitIdentity[] = "XGameSaveInitializeProviderAsync";
const char kQuotaIdentity[] = "XGameSaveGetRemainingQuotaAsync";
const char kDeleteIdentity[] = "XGameSaveDeleteContainerAsync";
const char kSubmitIdentity[] = "XGameSaveSubmitUpdateAsync";
const char kReadIdentity[] = "XGameSaveReadBlobDataAsync";

HRESULT M_InitializeProvider(void*, XUserHandle, const char* config, bool, XGameSaveProviderHandle* out) {
    auto* p = new Provider;
    p->dir = saves_root() + "\\" + encode(config ? config : "default");
    CreateDirectoryA(p->dir.c_str(), nullptr);
    XSL_INFO(CH, "game saves: %s", p->dir.c_str());
    *out = reinterpret_cast<XGameSaveProviderHandle>(p);
    return S_OK;
}
HRESULT M_InitializeProviderAsync(void* self, XUserHandle u, const char* config, bool sync, XAsyncBlock* async) {
    XGameSaveProviderHandle h = nullptr;
    HRESULT hr = M_InitializeProvider(self, u, config, sync, &h);
    return xsl_async_immediate(async, kInitIdentity, kInitIdentity, hr, &h, sizeof h);
}
HRESULT M_InitializeProviderResult(void*, XAsyncBlock* async, XGameSaveProviderHandle* out) {
    return xsl_async_result(async, kInitIdentity, sizeof *out, out, nullptr);
}
void M_CloseProvider(void*, XGameSaveProviderHandle p) { delete reinterpret_cast<Provider*>(p); }
HRESULT M_GetRemainingQuota(void*, XGameSaveProviderHandle, int64_t* q) { *q = 256ll << 20; return S_OK; }
HRESULT M_GetRemainingQuotaAsync(void*, XGameSaveProviderHandle, XAsyncBlock* async) {
    int64_t q = 256ll << 20;
    return xsl_async_immediate(async, kQuotaIdentity, kQuotaIdentity, S_OK, &q, sizeof q);
}
HRESULT M_GetRemainingQuotaResult(void*, XAsyncBlock* async, int64_t* q) { return xsl_async_result(async, kQuotaIdentity, sizeof *q, q, nullptr); }

HRESULT M_DeleteContainer(void*, XGameSaveProviderHandle ph, const char* name) {
    auto* p = reinterpret_cast<Provider*>(ph);
    std::string dir = p->dir + "\\" + encode(name);
    std::vector<std::string> files;
    for_each_entry(dir, false, [&](const std::string& n, uint64_t, FILETIME) { files.push_back(n); return true; });
    files.push_back(".displayname");
    for (auto& f : files) DeleteFileA((dir + "\\" + f).c_str());
    return RemoveDirectoryA(dir.c_str()) ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
HRESULT M_DeleteContainerAsync(void* self, XGameSaveProviderHandle p, const char* name, XAsyncBlock* async) {
    return xsl_async_immediate(async, kDeleteIdentity, kDeleteIdentity, M_DeleteContainer(self, p, name), nullptr, 0);
}
HRESULT M_DeleteContainerResult(void*, XAsyncBlock* async) { return xsl_async_result(async, kDeleteIdentity, 0, nullptr, nullptr); }

bool container_info(Provider* p, const std::string& enc, XGameSaveContainerInfoCallback* cb, void* ctx) {
    std::string dir = p->dir + "\\" + enc;
    std::string name = decode(enc);
    std::vector<uint8_t> dn;
    std::string display = read_file(dir + "\\.displayname", dn) ? std::string(dn.begin(), dn.end()) : name;
    XGameSaveContainerInfo info{};
    info.name = name.c_str();
    info.displayName = display.c_str();
    FILETIME newest{};
    for_each_entry(dir, false, [&](const std::string&, uint64_t size, FILETIME ft) {
        info.blobCount++;
        info.totalSize += size;
        if (CompareFileTime(&ft, &newest) > 0) newest = ft;
        return true;
    });
    info.lastModifiedTime = newest.dwHighDateTime ? to_time_t(newest) : 0;
    info.needsSync = false;
    return cb(&info, ctx);
}
HRESULT M_GetContainerInfo(void*, XGameSaveProviderHandle ph, const char* name, void* ctx, XGameSaveContainerInfoCallback* cb) {
    auto* p = reinterpret_cast<Provider*>(ph);
    std::string enc = encode(name);
    if (GetFileAttributesA((p->dir + "\\" + enc).c_str()) == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    container_info(p, enc, cb, ctx);
    return S_OK;
}
HRESULT M_EnumerateContainerInfoByName(void*, XGameSaveProviderHandle ph, const char* prefix, void* ctx, XGameSaveContainerInfoCallback* cb) {
    auto* p = reinterpret_cast<Provider*>(ph);
    std::string pre = prefix ? prefix : "";
    for_each_entry(p->dir, true, [&](const std::string& enc, uint64_t, FILETIME) {
        if (decode(enc).compare(0, pre.size(), pre) != 0) return true;
        return container_info(p, enc, cb, ctx);
    });
    return S_OK;
}
HRESULT M_EnumerateContainerInfo(void* self, XGameSaveProviderHandle ph, void* ctx, XGameSaveContainerInfoCallback* cb) {
    return M_EnumerateContainerInfoByName(self, ph, "", ctx, cb);
}
HRESULT M_CreateContainer(void*, XGameSaveProviderHandle ph, const char* name, XGameSaveContainerHandle* out) {
    auto* p = reinterpret_cast<Provider*>(ph);
    auto* c = new Container{p, name, p->dir + "\\" + encode(name)};
    *out = reinterpret_cast<XGameSaveContainerHandle>(c);
    return S_OK;
}
void M_CloseContainer(void*, XGameSaveContainerHandle c) { delete reinterpret_cast<Container*>(c); }

HRESULT M_EnumerateBlobInfoByName(void*, XGameSaveContainerHandle ch, const char* prefix, void* ctx, XGameSaveBlobInfoCallback* cb) {
    auto* c = reinterpret_cast<Container*>(ch);
    std::string pre = prefix ? prefix : "";
    for_each_entry(c->dir, false, [&](const std::string& enc, uint64_t size, FILETIME) {
        std::string name = decode(enc);
        if (name.compare(0, pre.size(), pre) != 0) return true;
        XGameSaveBlobInfo info{name.c_str(), (uint32_t)size};
        return cb(&info, ctx);
    });
    return S_OK;
}
HRESULT M_EnumerateBlobInfo(void* self, XGameSaveContainerHandle c, void* ctx, XGameSaveBlobInfoCallback* cb) {
    return M_EnumerateBlobInfoByName(self, c, "", ctx, cb);
}

// Reads lay out XGameSaveBlob[count] followed by names and data in one caller buffer.
HRESULT read_blobs(Container* c, const char** names, uint32_t count, std::vector<uint8_t>& packed, uint32_t* found) {
    std::vector<std::string> want;
    if (count && names) for (uint32_t i = 0; i < count; i++) want.push_back(names[i]);
    else for_each_entry(c->dir, false, [&](const std::string& enc, uint64_t, FILETIME) { want.push_back(decode(enc)); return true; });
    std::vector<std::pair<std::string, std::vector<uint8_t>>> blobs;
    for (auto& n : want) {
        std::vector<uint8_t> d;
        if (!read_file(c->dir + "\\" + encode(n), d)) return E_GS_BLOB_NOT_FOUND;
        blobs.push_back({n, std::move(d)});
    }
    size_t size = blobs.size() * sizeof(XGameSaveBlob);
    for (auto& b : blobs) size += b.first.size() + 1 + b.second.size();
    packed.assign(size, 0);
    auto* hdr = reinterpret_cast<XGameSaveBlob*>(packed.data());
    size_t off = blobs.size() * sizeof(XGameSaveBlob);
    for (size_t i = 0; i < blobs.size(); i++) {
        char* nm = (char*)packed.data() + off;
        memcpy(nm, blobs[i].first.c_str(), blobs[i].first.size() + 1);
        off += blobs[i].first.size() + 1;
        uint8_t* data = packed.data() + off;
        if (!blobs[i].second.empty()) memcpy(data, blobs[i].second.data(), blobs[i].second.size());
        off += blobs[i].second.size();
        hdr[i].info.name = nm;
        hdr[i].info.size = (uint32_t)blobs[i].second.size();
        hdr[i].data = data;
    }
    *found = (uint32_t)blobs.size();
    return S_OK;
}
// Pointers inside the packed buffer are rebased when it is copied to the caller's buffer.
void rebase(std::vector<uint8_t>& packed, uint32_t n, uint8_t* to) {
    auto* hdr = reinterpret_cast<XGameSaveBlob*>(packed.data());
    for (uint32_t i = 0; i < n; i++) {
        hdr[i].info.name = (const char*)(to + ((const uint8_t*)hdr[i].info.name - packed.data()));
        hdr[i].data = to + (hdr[i].data - packed.data());
    }
}
HRESULT M_ReadBlobData(void*, XGameSaveContainerHandle ch, const char** names, uint32_t* count, size_t size, XGameSaveBlob* out) {
    std::vector<uint8_t> packed;
    uint32_t found = 0;
    HRESULT hr = read_blobs(reinterpret_cast<Container*>(ch), names, count ? *count : 0, packed, &found);
    if (FAILED(hr)) return hr;
    if (size < packed.size()) return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    rebase(packed, found, (uint8_t*)out);
    memcpy(out, packed.data(), packed.size());
    if (count) *count = found;
    return S_OK;
}
HRESULT M_ReadBlobDataAsync(void*, XGameSaveContainerHandle ch, const char** names, uint32_t count, XAsyncBlock* async) {
    std::vector<uint8_t> packed;
    uint32_t found = 0;
    HRESULT hr = read_blobs(reinterpret_cast<Container*>(ch), names, count, packed, &found);
    // Result = blob count followed by the packed (not yet rebased) buffer.
    std::vector<uint8_t> result(sizeof(uint32_t) + sizeof(uint8_t*) + packed.size());
    uint8_t* base = packed.data();
    memcpy(result.data(), &found, sizeof found);
    memcpy(result.data() + sizeof found, &base, sizeof base);
    if (!packed.empty()) memcpy(result.data() + sizeof found + sizeof base, packed.data(), packed.size());
    return xsl_async_immediate(async, kReadIdentity, kReadIdentity, hr, result.data(), result.size());
}
HRESULT M_ReadBlobDataResult(void*, XAsyncBlock* async, size_t size, XGameSaveBlob* out, uint32_t* count) {
    size_t total = 0;
    HRESULT hr = xsl_async_result_size(async, &total);
    if (FAILED(hr)) return hr;
    std::vector<uint8_t> result(total);
    hr = xsl_async_result(async, kReadIdentity, total, result.data(), nullptr);
    if (FAILED(hr)) return hr;
    uint32_t found;
    uint8_t* oldBase;
    memcpy(&found, result.data(), sizeof found);
    memcpy(&oldBase, result.data() + sizeof found, sizeof oldBase);
    size_t n = total - sizeof found - sizeof oldBase;
    if (size < n) return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    std::vector<uint8_t> packed(result.begin() + sizeof found + sizeof oldBase, result.end());
    auto* hdr = reinterpret_cast<XGameSaveBlob*>(packed.data());
    for (uint32_t i = 0; i < found; i++) {
        hdr[i].info.name = (const char*)((uint8_t*)out + ((const uint8_t*)hdr[i].info.name - oldBase));
        hdr[i].data = (uint8_t*)out + (hdr[i].data - oldBase);
    }
    memcpy(out, packed.data(), n);
    *count = found;
    return S_OK;
}

HRESULT M_CreateUpdate(void*, XGameSaveContainerHandle ch, const char* display, XGameSaveUpdateHandle* out) {
    auto* u = new Update{reinterpret_cast<Container*>(ch), display ? display : "", {}, {}};
    *out = reinterpret_cast<XGameSaveUpdateHandle>(u);
    return S_OK;
}
void M_CloseUpdate(void*, XGameSaveUpdateHandle u) { delete reinterpret_cast<Update*>(u); }
HRESULT M_SubmitBlobWrite(void*, XGameSaveUpdateHandle uh, const char* name, const uint8_t* data, size_t n) {
    auto* u = reinterpret_cast<Update*>(uh);
    u->writes[name].assign(data, data + n);
    return S_OK;
}
HRESULT M_SubmitBlobDelete(void*, XGameSaveUpdateHandle uh, const char* name) {
    reinterpret_cast<Update*>(uh)->deletes.push_back(name);
    return S_OK;
}
HRESULT M_SubmitUpdate(void*, XGameSaveUpdateHandle uh) {
    auto* u = reinterpret_cast<Update*>(uh);
    CreateDirectoryA(u->container->dir.c_str(), nullptr);
    if (!u->displayName.empty()) write_file(u->container->dir + "\\.displayname", u->displayName.data(), u->displayName.size());
    for (auto& [name, data] : u->writes)
        if (!write_file(u->container->dir + "\\" + encode(name), data.data(), data.size())) return E_FAIL;
    for (auto& name : u->deletes) DeleteFileA((u->container->dir + "\\" + encode(name)).c_str());
    return S_OK;
}
HRESULT M_SubmitUpdateAsync(void* self, XGameSaveUpdateHandle uh, XAsyncBlock* async) {
    return xsl_async_immediate(async, kSubmitIdentity, kSubmitIdentity, M_SubmitUpdate(self, uh), nullptr, 0);
}
HRESULT M_SubmitUpdateResult(void*, XAsyncBlock* async) { return xsl_async_result(async, kSubmitIdentity, 0, nullptr, nullptr); }
}  // namespace

XSL_RT_REGISTER(XGameSaveInitializeProvider, &M_InitializeProvider);
XSL_RT_REGISTER(XGameSaveInitializeProviderAsync, &M_InitializeProviderAsync);
XSL_RT_REGISTER(XGameSaveInitializeProviderResult, &M_InitializeProviderResult);
XSL_RT_REGISTER(XGameSaveCloseProvider, &M_CloseProvider);
XSL_RT_REGISTER(XGameSaveGetRemainingQuota, &M_GetRemainingQuota);
XSL_RT_REGISTER(XGameSaveGetRemainingQuotaAsync, &M_GetRemainingQuotaAsync);
XSL_RT_REGISTER(XGameSaveGetRemainingQuotaResult, &M_GetRemainingQuotaResult);
XSL_RT_REGISTER(XGameSaveDeleteContainer, &M_DeleteContainer);
XSL_RT_REGISTER(XGameSaveDeleteContainerAsync, &M_DeleteContainerAsync);
XSL_RT_REGISTER(XGameSaveDeleteContainerResult, &M_DeleteContainerResult);
XSL_RT_REGISTER(XGameSaveGetContainerInfo, &M_GetContainerInfo);
XSL_RT_REGISTER(XGameSaveEnumerateContainerInfo, &M_EnumerateContainerInfo);
XSL_RT_REGISTER(XGameSaveEnumerateContainerInfoByName, &M_EnumerateContainerInfoByName);
XSL_RT_REGISTER(XGameSaveCreateContainer, &M_CreateContainer);
XSL_RT_REGISTER(XGameSaveCloseContainer, &M_CloseContainer);
XSL_RT_REGISTER(XGameSaveEnumerateBlobInfo, &M_EnumerateBlobInfo);
XSL_RT_REGISTER(XGameSaveEnumerateBlobInfoByName, &M_EnumerateBlobInfoByName);
XSL_RT_REGISTER(XGameSaveReadBlobData, &M_ReadBlobData);
XSL_RT_REGISTER(XGameSaveReadBlobDataAsync, &M_ReadBlobDataAsync);
XSL_RT_REGISTER(XGameSaveReadBlobDataResult, &M_ReadBlobDataResult);
XSL_RT_REGISTER(XGameSaveCreateUpdate, &M_CreateUpdate);
XSL_RT_REGISTER(XGameSaveCloseUpdate, &M_CloseUpdate);
XSL_RT_REGISTER(XGameSaveSubmitBlobWrite, &M_SubmitBlobWrite);
XSL_RT_REGISTER(XGameSaveSubmitBlobDelete, &M_SubmitBlobDelete);
XSL_RT_REGISTER(XGameSaveSubmitUpdate, &M_SubmitUpdate);
XSL_RT_REGISTER(XGameSaveSubmitUpdateAsync, &M_SubmitUpdateAsync);
XSL_RT_REGISTER(XGameSaveSubmitUpdateResult, &M_SubmitUpdateResult);
