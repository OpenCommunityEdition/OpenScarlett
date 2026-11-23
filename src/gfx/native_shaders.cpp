// SPDX-License-Identifier: AGPL-3.0-or-later
// Shaders shipped as console GPU machine code only: translate the code to HLSL (src/gpucode),
// compile it with the PC shader compiler (dxcompiler.dll from Microsoft's DirectXShaderCompiler
// release, next to the layer DLLs) and give PC Direct3D 12 the resulting DXIL container.
//
// XSL_NATIVE_SHADERS=0 never translates; =1 translates every shader that has precompiled code
// (A/B testing against the DXIL path); default: only shaders without DXIL.
// XSL_SHADER_CACHE=<dir> overrides the cache directory (%LOCALAPPDATA%\OpenScarlett\shadercache).
// XSL_DUMP_SHADERS=<dir> writes each translated container and its HLSL (XSL_DUMP_KEYS=<keys>: these
// are translated again even when cached, so they are dumped too).
// XSL_NATIVE_STAGES=VS,PS,... limits translation to those stages (testing).
// XSL_SKIP_SHADERS=<key,...> treats those shaders as untranslatable (debugging GPU hangs).
// XSL_LOOP_GUARD=<n> ends a shader invocation after n loop iterations (default 4096 steps, 4M with barriers; runaway loops).
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <objbase.h>
#include <dxcapi_xs.h>

#include "bridge.h"
#include "container.h"
#include "meta.h"
#include <algorithm>
#include "translate.h"
#include "rtso.h"
#include "xsl/log.h"

void dxbc_checksum(const uint8_t* data, size_t size, uint32_t out[4]);   // dxbc.cpp

namespace pc {
namespace {
constexpr const char* CH = "gfx.native";

// Rebuilds a DXBC container from parts and recomputes its checksum.
std::vector<uint8_t> build_container(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& parts, const uint8_t* header24) {
    size_t size = 32 + 4 * parts.size();
    for (auto& p : parts) size += 8 + ((p.second.size() + 3) & ~size_t(3));
    std::vector<uint8_t> o(size, 0);
    memcpy(o.data(), header24, 24);
    auto w32 = [&](size_t at, uint32_t v) { memcpy(&o[at], &v, 4); };
    w32(24, uint32_t(size));
    w32(28, uint32_t(parts.size()));
    size_t cur = 32 + 4 * parts.size();
    for (size_t i = 0; i < parts.size(); i++) {
        w32(32 + 4 * i, uint32_t(cur));
        w32(cur, parts[i].first);
        w32(cur + 4, uint32_t((parts[i].second.size() + 3) & ~size_t(3)));
        memcpy(&o[cur + 8], parts[i].second.data(), parts[i].second.size());
        cur += 8 + ((parts[i].second.size() + 3) & ~size_t(3));
    }
    uint32_t sum[4];
    dxbc_checksum(o.data(), o.size(), sum);
    memcpy(&o[4], sum, 16);
    return o;
}
// The compiled container plus the title's root signature part (titles create root signatures
// from shader blobs).
std::vector<uint8_t> with_root_signature(const std::vector<uint8_t>& dxil, const gpucode::Container& orig) {
    gpucode::Container d;
    const auto* rs = orig.part("RTS0");
    if (!rs || !gpucode::read_container(dxil.data(), dxil.size(), d) || d.part("RTS0")) return dxil;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> parts;
    // Keep the compiler's part order (std::map in Container is sorted; re-read the order from the blob).
    uint32_t count;
    memcpy(&count, &dxil[28], 4);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off, cc, sz;
        memcpy(&off, &dxil[32 + 4 * i], 4);
        memcpy(&cc, &dxil[off], 4);
        memcpy(&sz, &dxil[off + 4], 4);
        parts.push_back({cc, std::vector<uint8_t>(dxil.begin() + off + 8, dxil.begin() + off + 8 + sz)});
    }
    parts.push_back({gpucode::fourcc("RTS0"), *rs});
    return build_container(parts, dxil.data());
}
constexpr const char* kVersion = "gpucode-36";
// Ray-tracing programs have their own cache version (rt_cache_key also tags their translation):
// they are built offline (xsl-gpucode rtcache) and should not be invalidated by unrelated changes.
constexpr const char* kRtVersion = "gpucode-35";

int mode() {
    static int m = [] {
        const char* e = getenv("XSL_NATIVE_SHADERS");
        return e && *e == '0' ? 0 : e && *e == '1' ? 1 : 2;
    }();
    return m;
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

std::wstring module_dir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&module_dir, &self);
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, p, MAX_PATH);
    std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

std::wstring cache_dir() {
    static std::wstring d = [] {
        wchar_t b[MAX_PATH];
        std::wstring r;
        if (GetEnvironmentVariableW(L"XSL_SHADER_CACHE", b, MAX_PATH)) r = b;
        else if (GetEnvironmentVariableW(L"LOCALAPPDATA", b, MAX_PATH)) r = std::wstring(b) + L"\\OpenScarlett\\shadercache";
        if (!r.empty()) {
            for (size_t i = 1; i <= r.size(); i++)
                if (i == r.size() || r[i] == L'\\' || r[i] == L'/') CreateDirectoryW(r.substr(0, i).c_str(), nullptr);
            r += L"\\";
        }
        return r;
    }();
    return d;
}

bool read_file(const std::wstring& p, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    bool ok = n > 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}
void write_file(const std::wstring& p, const void* d, size_t n) {
    std::wstring tmp = p + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    bool ok = fwrite(d, 1, n, f) == n;
    fclose(f);
    if (ok) MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}

DxcCreateInstanceProc dxc() {
    static DxcCreateInstanceProc p = [] {
        std::wstring path = module_dir() + L"dxcompiler.dll";
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        auto f = m ? (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance") : nullptr;
        if (!f) XSL_ERR(CH, "PC shader compiler not found (%ls): shaders without DXIL cannot be translated", path.c_str());
        return f;
    }();
    return p;
}

bool compile(const std::string& hlsl, const std::string& profile, std::vector<uint8_t>& out, std::string& log, const wchar_t* optOverride = nullptr) {
    auto create = dxc();
    if (!create) return false;
    IDxcCompiler3* compiler = nullptr;
    IDxcResult* result = nullptr;
    HRESULT hr = create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&compiler);
    std::wstring prof(profile.begin(), profile.end());
    // XSL_DXC_OPT=Od|O0|O1|O2|O3 (diagnostics): optimization level of compiles (cached shaders keep theirs).
    static std::wstring opt = [] {
        char b[8];
        std::wstring w = L"-O3";
        if (GetEnvironmentVariableA("XSL_DXC_OPT", b, sizeof b) && b[0] == 'O') { w = L"-"; for (char* c = b; *c; c++) w += wchar_t(*c); }
        return w;
    }();
    LPCWSTR args[] = {L"-T", prof.c_str(), L"-E", L"main", L"-HV", L"2021", optOverride ? optOverride : opt.c_str(), L"-Qstrip_debug", L"-Qstrip_reflect"};
    DxcBuffer buf{hlsl.data(), hlsl.size(), DXC_CP_UTF8};
    if (SUCCEEDED(hr)) hr = compiler->Compile(&buf, args, UINT32(sizeof args / sizeof *args), nullptr, __uuidof(IDxcResult), (void**)&result);
    HRESULT status = E_FAIL;
    if (SUCCEEDED(hr)) result->GetStatus(&status);
    if (result) {
        IDxcBlobUtf8* errs = nullptr;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), (void**)&errs, nullptr)) && errs) {
            log.assign(errs->GetStringPointer(), errs->GetStringLength());
            errs->Release();
        }
    }
    IDxcBlob* obj = nullptr;
    bool ok = SUCCEEDED(hr) && SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), (void**)&obj, nullptr)) && obj;
    if (ok) {
        auto* p = (const uint8_t*)obj->GetBufferPointer();
        out.assign(p, p + obj->GetBufferSize());
        obj->Release();
    }
    if (result) result->Release();
    if (compiler) compiler->Release();
    return ok;
}

void dump(uint64_t key, const void* in, size_t n, const std::string& hlsl, const std::string& note) {
    static const char* d = getenv("XSL_DUMP_SHADERS");
    if (!d || !*d) return;
    char base[600];
    snprintf(base, sizeof base, "%s\\%016llx", d, (unsigned long long)key);
    std::wstring w(base, base + strlen(base));
    write_file(w + L".cso", in, n);
    std::string t = hlsl.empty() ? "// " + note + "\n" : hlsl;
    write_file(w + L".hlsl", t.data(), t.size());
}

std::mutex g_lock;
std::set<uint64_t> g_failed;
thread_local bool t_untranslated = false;   // a shader without DXIL could not be translated
thread_local std::string t_keys;             // translated shaders since reset (pipeline names)
}  // namespace

void reset_untranslated() { t_untranslated = false; t_keys.clear(); }
bool had_untranslated() { return t_untranslated; }
static unsigned g_host_wave_min = 32, g_host_wave_max = 32;
static bool g_host_sm66 = false;
void set_host_waves(unsigned min_lanes, unsigned max_lanes, bool sm66) {
    g_host_wave_min = min_lanes;
    g_host_wave_max = max_lanes;
    g_host_sm66 = sm66;
}
const char* translated_keys() { return t_keys.c_str(); }

// The root signature of the pipeline being created, for shaders whose container carries none (the
// translator rebuilds bindings from it). Set by pipeline creation for the duration of the call.
thread_local std::vector<uint8_t> t_fallback_rs;
void set_fallback_root_signature(const void* blob, size_t n) {
    t_fallback_rs.clear();
    if (!blob || !n) return;
    const uint8_t* d = (const uint8_t*)blob;
    // A serialized root signature is either the RTS0 data itself or a DXBC container holding it.
    if (n >= 32 && !memcmp(d, "DXBC", 4)) {
        uint32_t count, total;
        memcpy(&total, d + 24, 4);
        memcpy(&count, d + 28, 4);
        for (uint32_t i = 0; i < count && 32 + 4ull * (i + 1) <= n; i++) {
            uint32_t off, cc, sz;
            memcpy(&off, d + 32 + 4 * i, 4);
            if (off + 8ull > n) break;
            memcpy(&cc, d + off, 4);
            memcpy(&sz, d + off + 4, 4);
            if (!memcmp(&cc, "RTS0", 4) && off + 8ull + sz <= n) { t_fallback_rs.assign(d + off + 8, d + off + 8 + sz); return; }
        }
        return;
    }
    t_fallback_rs.assign(d, d + n);
}
bool native_translate(const void* in, size_t n, void** out, size_t* outSize) {
    if (mode() == 0) return false;
    gpucode::Container c;
    if (!gpucode::read_container(in, n, c)) return false;
    if (!c.has_native) {
        if (!c.has_bitcode && c.part("DXIL")) {
            static volatile long w = 0;
            if (InterlockedIncrement(&w) <= 5)
                XSL_WARN(CH, "shader (%s) has neither DXIL nor precompiled code the translator can read (%zu bytes)", gpucode::kind_name(c.kind), n);
        }
        return false;
    }
    if (c.has_bitcode && mode() != 1) return false;
    if (const char* st = getenv("XSL_NATIVE_STAGES"); st && *st && !strstr(st, gpucode::kind_name(c.kind))) {
        t_untranslated = !c.has_bitcode;   // excluded stage: like an untranslatable shader (placeholder pipeline)
        static volatile long w = 0;
        if (InterlockedIncrement(&w) <= 3) XSL_INFO(CH, "%s shader excluded by XSL_NATIVE_STAGES (DXIL program: %d)", gpucode::kind_name(c.kind), (int)c.has_bitcode);
        return false;
    }
    uint64_t key = fnv(kVersion, strlen(kVersion), fnv(in, n));
    const bool fallbackRs = !c.part("RTS0") && !t_fallback_rs.empty();
    if (fallbackRs) key = fnv(t_fallback_rs.data(), t_fallback_rs.size(), key);   // translated against the pipeline's root signature
    if (const char* g = getenv("XSL_LOOP_GUARD"); g && *g) key = fnv(g, strlen(g), key);   // changes the output
    if (getenv("XSL_NO_BARRIER_SYNC")) key = fnv("nb", 2, key);
    if (const char* sk = getenv("XSL_SKIP_SHADERS"); sk && *sk) {   // debugging: these keys become placeholders
        char k[20];
        snprintf(k, sizeof k, "%016llx", (unsigned long long)key);
        if (strstr(sk, k)) {
            XSL_INFO(CH, "shader %s skipped (XSL_SKIP_SHADERS)", k);
            t_untranslated = !c.has_bitcode;
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_failed.count(key)) { t_untranslated = !c.has_bitcode; return false; }
    }
    wchar_t name[32];
    swprintf(name, 32, L"%016llx.dxil", (unsigned long long)key);
    std::wstring cpath = cache_dir().empty() ? L"" : cache_dir() + name;
    std::vector<uint8_t> dxil;
    // XSL_SHADER_OVERRIDE=<dir>: <dir>\<key>.hlsl, when present, is compiled instead of the
    // translation and never cached (debugging: edit a dumped translation and run it in place).
    std::vector<uint8_t> ovr;
    if (const char* od = getenv("XSL_SHADER_OVERRIDE"); od && *od) {
        char p2[600];
        snprintf(p2, sizeof p2, "%s\\%016llx.hlsl", od, (unsigned long long)key);
        if (read_file(std::wstring(p2, p2 + strlen(p2)), ovr)) {
            XSL_INFO(CH, "shader %016llx: HLSL override %s", (unsigned long long)key, p2);
            cpath.clear();
        }
    }
    if (const char* dk = getenv("XSL_DUMP_KEYS"); dk && *dk) {   // these keys are translated again (and dumped)
        char k[20];
        snprintf(k, sizeof k, "%016llx", (unsigned long long)key);
        if (strstr(dk, k)) cpath.clear();
    }
    bool cached = !cpath.empty() && read_file(cpath, dxil);
    if (cached) XSL_INFO(CH, "shader %016llx (%s): from cache", (unsigned long long)key, gpucode::kind_name(c.kind));
    if (!cached) {
        const auto* rsp = c.part("RTS0");
        gpucode::RootSig rs;
        std::string err;
        const uint8_t* rsData = rsp ? (const uint8_t*)rsp->data() : fallbackRs ? t_fallback_rs.data() : nullptr;
        size_t rsSize = rsp ? rsp->size() : fallbackRs ? t_fallback_rs.size() : 0;
        if (!rsData || !gpucode::parse_root_signature(rsData, rsSize, rs, &err)) {
            char head[100] = "";
            for (size_t k = 0; rsData && k < std::min<size_t>(rsSize, 40); k += 4) snprintf(head + strlen(head), sizeof head - strlen(head), "%08x ", *(const uint32_t*)(rsData + k));
            XSL_WARN(CH, "shader %016llx: no usable root signature in the container (%s; %s, %zu bytes: %s)", (unsigned long long)key, err.c_str(),
                     rsp ? "own" : fallbackRs ? "pipeline's" : "none", rsSize, head);
            std::lock_guard<std::mutex> g(g_lock);
            g_failed.insert(key);
            t_untranslated = !c.has_bitcode;
            return false;
        }
        gpucode::TranslateOptions o;
        // 32-lane PC waves run console wave64 code two lanes per thread; without shader model 6.6
        // the wave size cannot be requested, so that needs a GPU whose waves are always 32 lanes.
        o.wave_size_attr = g_host_sm66;
        if (const char* g = getenv("XSL_LOOP_GUARD"); g && *g) o.loop_guard = o.loop_guard_barrier = (unsigned)strtoul(g, nullptr, 0);
        if (getenv("XSL_NO_BARRIER_SYNC")) o.barrier_sync = false;
        {   // global memory follows the backend's switches (pc::gm_active)
            const char* g = getenv("XSL_GLOBAL_MEMORY");
            o.global_memory = !(g && g[0] == '0') && !getenv("XSL_GM_NO_RS");
        }
        o.wave64_dual = g_host_wave_min <= 32 && (g_host_wave_max <= 32 || g_host_sm66);
        gpucode::TranslateResult r = gpucode::translate(c, rs, o);
        if (r.ok && !ovr.empty()) r.hlsl.assign(ovr.begin(), ovr.end());
        if (!r.ok) {
            XSL_WARN(CH, "shader %016llx (%s): not translated: %s", (unsigned long long)key, gpucode::kind_name(c.kind), r.error.c_str());
            dump(key, in, n, "", r.error);
            std::lock_guard<std::mutex> g(g_lock);
            g_failed.insert(key);
            t_untranslated = !c.has_bitcode;
            return false;
        }
        std::string log;
        if (!compile(r.hlsl, r.profile, dxil, log)) {
            XSL_ERR(CH, "shader %016llx: translated HLSL does not compile:\n%s", (unsigned long long)key, log.c_str());
            dump(key, in, n, r.hlsl, "");
            std::lock_guard<std::mutex> g(g_lock);
            g_failed.insert(key);
            t_untranslated = !c.has_bitcode;
            return false;
        }
        dxil = with_root_signature(dxil, c);
        dump(key, in, n, r.hlsl, "");
        if (!cpath.empty()) write_file(cpath, dxil.data(), dxil.size());
        XSL_INFO(CH, "shader %016llx (%s, wave%u): translated to %s", (unsigned long long)key, gpucode::kind_name(c.kind), r.wave_size, r.profile.c_str());
    }
    char kb[24];
    snprintf(kb, sizeof kb, "%s%016llx", t_keys.empty() ? "" : " ", (unsigned long long)key);
    t_keys += kb;
    void* o = malloc(dxil.size());
    memcpy(o, dxil.data(), dxil.size());
    *out = o;
    *outSize = dxil.size();
    return true;
}

// Linked console ray-tracing pipelines (gpucode/rtso.h): translated and compiled to DXIL, cached
// by the image's hash (compiling one takes minutes: callers run this off the title's threads).
// XSL_RT_DXC_OPT=Od|O1|O2|O3: optimization level of these programs (default O3: slower in DXC,
// but the driver, which compiles each program again on every new machine, is faster with it).
std::wstring rt_cache_dir() { return cache_dir(); }
static const std::wstring& rt_opt() {
    static std::wstring opt = [] {
        char b[8];
        std::wstring w = L"-O3";   // the driver compiles O3 DXIL of these programs about 30% faster
        if (GetEnvironmentVariableA("XSL_RT_DXC_OPT", b, sizeof b) && b[0] == 'O') { w = L"-"; for (char* c = b; *c; c++) w += wchar_t(*c); }
        return w;
    }();
    return opt;
}
static std::wstring rt_cache_path(const gpucode::RtLinked& l, const std::wstring& opt) {
    uint64_t key = gpucode::rt_cache_key(l, std::string(kRtVersion) + std::string(opt.begin(), opt.end()));   // same key as `xsl-gpucode rtcache`
    wchar_t name[64];
    swprintf(name, 64, L"\\rt_%016llx.dxil", (unsigned long long)key);
    return cache_dir() + name;
}
// Whether the program's DXIL is in the cache (its pipeline is then quick to create, or at least
// needs no translation and DXC run).
bool rt_program_cached(const gpucode::RtLinked& l) {
    return GetFileAttributesW(rt_cache_path(l, rt_opt()).c_str()) != INVALID_FILE_ATTRIBUTES;
}
bool rt_build_pipeline(const gpucode::RtLinked& l, std::vector<uint8_t>& dxil, std::string& err) {
    const std::wstring& opt = rt_opt();
    std::wstring path = rt_cache_path(l, opt);
    uint64_t key = gpucode::rt_cache_key(l, std::string(kRtVersion) + std::string(opt.begin(), opt.end()));
    if (read_file(path, dxil) && !dxil.empty()) return true;
    XSL_INFO(CH, "ray-tracing program rt_%016llx: not cached, compiling (image %zu bytes, %zu segments, main 0x%x, %zu entries)", (unsigned long long)key,
             l.image.size() * 4, l.segs.size(), l.main, l.entries.size());
    gpucode::Container c;
    c.kind = gpucode::ProgramKind::CS;
    gpucode::RootSig rs;
    gpucode::RsParam p;
    p.type = gpucode::ParamType::Constants;
    p.num32 = gpucode::kRtRootConstants;
    rs.params.push_back(p);
    gpucode::TranslateResult r = gpucode::translate(c, rs, gpucode::rt_translate_options(l));
    if (!r.ok) { err = "translation: " + r.error; return false; }
    std::string log;
    if (!compile(r.hlsl, "cs_6_0", dxil, log, opt.c_str())) { err = "compile: " + log.substr(0, 400); return false; }
    write_file(path, dxil.data(), dxil.size());
    return true;
}
// The layer's own shaders (HLSL written here, not translated) through the same PC compiler.
bool compile_layer_shader(const std::string& hlsl, const char* profile, std::vector<uint8_t>& out, std::string& log) {
    return compile(hlsl, profile, out, log);
}
}  // namespace pc
