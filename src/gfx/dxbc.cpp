// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Xbox shader containers -> PC shader containers.
//
// The GDKX shader compiler emits a standard DXBC container (SFI0, ISG1, OSG1,
// PSV0, RTS0, STAT, HASH, DXIL). Precompiled native GPU code is stored inside
// the DXIL part, between the DXIL program header and the LLVM bitcode, so the
// bitcode offset is larger than the usual 16 bytes (docs/provenance.md). PC runtimes
// expect the plain layout, so this file rebuilds the container: every part is
// copied unchanged except DXIL, which is reduced to header + bitcode, and the
// container checksum is recomputed.
#include <windows.h>

#include <objbase.h>
#include <dxcapi_xs.h>

#include <cstdint>
#include <string>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bridge.h"
#include "xsl/log.h"

namespace {
constexpr uint32_t fourcc(const char* s) { return uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24; }
uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
void wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

// ---- MD5 transform (RFC 1321) ------------------------------------------------------------
struct Md5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
    void block(const uint32_t* m) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) & 15; }
            else { F = C ^ (B | ~D); g = (7 * i) & 15; }
            uint32_t t = D; D = C; C = B;
            B = B + rol(A + F + K[i] + m[g], S[i]);
            A = t;
        }
        a += A; b += B; c += C; d += D;
    }
};
// ---- Shader PDBs (MSF 7.00) ------------------------------------------------------------------
// Shaders compiled with an external PDB keep only a name (ILDN part) in the container; the full
// program is the ILDB part of a DXIL container stored in one of the PDB's streams.
std::vector<uint8_t> read_whole(const std::string& path) {
    std::vector<uint8_t> d;
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return d;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    d.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    if (!d.empty() && (!ReadFile(f, d.data(), (DWORD)d.size(), &got, nullptr) || got != d.size())) d.clear();
    CloseHandle(f);
    return d;
}
std::vector<std::vector<uint8_t>> msf_streams(const std::vector<uint8_t>& f) {
    std::vector<std::vector<uint8_t>> out;
    static const char magic[] = "Microsoft C/C++ MSF 7.00\r\n\x1a" "DS\0\0\0";
    if (f.size() < 56 || memcmp(f.data(), magic, 32) != 0) return out;
    uint32_t bs = rd32(&f[32]), numDirBytes = rd32(&f[44]), mapAddr = rd32(&f[52]);
    if (!bs || (uint64_t)mapAddr * bs + 4 > f.size()) return out;
    auto block = [&](uint32_t b) -> const uint8_t* { return (uint64_t)(b + 1) * bs <= f.size() ? &f[(size_t)b * bs] : nullptr; };
    std::vector<uint8_t> dir;
    uint32_t dirBlocks = (numDirBytes + bs - 1) / bs;
    for (uint32_t i = 0; i < dirBlocks; i++) {
        const uint8_t* b = block(rd32(&f[(size_t)mapAddr * bs + 4 * i]));
        if (!b) return out;
        dir.insert(dir.end(), b, b + bs);
    }
    if (dir.size() < 4) return out;
    uint32_t n = rd32(dir.data());
    if (4ull + 4ull * n > dir.size()) return out;
    size_t cur = 4 + 4 * (size_t)n;
    for (uint32_t s = 0; s < n; s++) {
        uint32_t size = rd32(&dir[4 + 4 * s]);
        if (size == 0xFFFFFFFF) size = 0;
        uint32_t blocks = (size + bs - 1) / bs;
        std::vector<uint8_t> st;
        for (uint32_t i = 0; i < blocks && cur + 4 <= dir.size(); i++, cur += 4) {
            const uint8_t* b = block(rd32(&dir[cur]));
            if (!b) return {};
            st.insert(st.end(), b, b + bs);
        }
        st.resize(size);
        out.push_back(std::move(st));
    }
    return out;
}
// A slim PDB (/Zs) holds the sources and arguments but no program. Recompile them with the SDK
// compiler (dxcompiler_xs.dll from the user's SDK, copied next to the title at run time), without
// the options that strip the program, and take the DXIL program from the result.
struct PdbIncludes : IDxcIncludeHandler {
    std::vector<std::pair<std::wstring, IDxcBlobEncoding*>> files;
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR name, IDxcBlob** out) override {
        std::wstring n(name);
        auto tail = [](const std::wstring& w) { return w.substr(w.find_last_of(L"/\\") + 1); };
        for (auto& [fn, blob] : files)
            if (_wcsicmp(tail(fn).c_str(), tail(n).c_str()) == 0) { blob->AddRef(); *out = blob; return S_OK; }
        *out = nullptr;
        return E_FAIL;
    }
};
std::vector<uint8_t> recompile_slim_pdb(const std::vector<uint8_t>& slim) {
    static DxcCreateInstanceProc create = [] {
        HMODULE m = LoadLibraryW(L"dxcompiler_xs.dll");
        auto p = m ? (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance") : nullptr;
        if (!p) XSL_WARN("gfx.pc", "dxcompiler_xs.dll not available: slim shader PDBs cannot be recompiled");
        return p;
    }();
    std::vector<uint8_t> out;
    if (!create) return out;
    IDxcUtils* utils = nullptr;
    IDxcBlobEncoding* blob = nullptr;
    IDxcPdbUtils* pdb = nullptr;
    IDxcCompiler3* compiler = nullptr;
    IDxcResult* result = nullptr;
    PdbIncludes inc;
    HRESULT hr = create(CLSID_DxcUtils, __uuidof(IDxcUtils), (void**)&utils);
    if (SUCCEEDED(hr)) hr = utils->CreateBlob(slim.data(), (UINT32)slim.size(), DXC_CP_ACP, &blob);
    if (SUCCEEDED(hr)) hr = create(CLSID_DxcPdbUtils, __uuidof(IDxcPdbUtils), (void**)&pdb);
    if (SUCCEEDED(hr)) hr = pdb->Load(blob);
    if (SUCCEEDED(hr)) hr = create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&compiler);
    std::wstring mainName;
    std::vector<std::wstring> args;
    if (SUCCEEDED(hr)) {
        BSTR b = nullptr;
        if (SUCCEEDED(pdb->GetMainFileName(&b)) && b) { mainName = b; SysFreeString(b); }
        UINT32 n = 0;
        pdb->GetSourceCount(&n);
        for (UINT32 i = 0; i < n; i++) {
            BSTR nm = nullptr;
            IDxcBlobEncoding* src = nullptr;
            if (SUCCEEDED(pdb->GetSourceName(i, &nm)) && SUCCEEDED(pdb->GetSource(i, &src))) inc.files.push_back({nm ? nm : L"", src});
            if (nm) SysFreeString(nm);
        }
        pdb->GetArgCount(&n);
        for (UINT32 i = 0; i < n; i++) {
            BSTR a = nullptr;
            if (FAILED(pdb->GetArg(i, &a)) || !a) continue;
            std::wstring w(a);
            SysFreeString(a);
            auto is = [&](const wchar_t* o) { return _wcsicmp(w.c_str() + ((w[0] == L'-' || w[0] == L'/') ? 1 : 0), o) == 0; };
            if ((w[0] == L'-' || w[0] == L'/') && (is(L"Zs") || is(L"Qstrip_reflect") || is(L"Qstrip_debug") || is(L"Qembed_debug") || is(L"Zi")))
                continue;
            if ((w[0] == L'-' || w[0] == L'/') && (is(L"Fd") || is(L"Fo") || is(L"Fre") || is(L"Fsh"))) { i++; continue; }  // and its value
            if (w.find(L"__XBOX_STRIP_DXIL") != std::wstring::npos) {
                if (!args.empty() && (args.back() == L"-D" || args.back() == L"/D")) args.pop_back();
                continue;
            }
            if (w[0] != L'-' && w[0] != L'/' && (args.empty() || args.back().size() > 3)) continue;  // the source file name
            args.push_back(w);
        }
    }
    IDxcBlobEncoding* mainSrc = nullptr;
    for (auto& [fn, b] : inc.files)
        if (fn == mainName || mainSrc == nullptr) mainSrc = b;
    if (SUCCEEDED(hr) && mainSrc) {
        std::vector<LPCWSTR> argv;
        for (auto& a : args) argv.push_back(a.c_str());
        DxcBuffer buf{mainSrc->GetBufferPointer(), mainSrc->GetBufferSize(), DXC_CP_ACP};
        hr = compiler->Compile(&buf, argv.data(), (UINT32)argv.size(), &inc, __uuidof(IDxcResult), (void**)&result);
        HRESULT status = E_FAIL;
        if (SUCCEEDED(hr)) result->GetStatus(&status);
        IDxcBlob* obj = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), (void**)&obj, nullptr)) && obj) {
            auto* p = (const uint8_t*)obj->GetBufferPointer();
            std::vector<uint8_t> c(p, p + obj->GetBufferSize());
            obj->Release();
            uint32_t count = c.size() >= 32 ? rd32(&c[28]) : 0;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t off = rd32(&c[32 + 4 * i]);
                if (rd32(&c[off]) != fourcc("DXIL")) continue;
                uint32_t sz = rd32(&c[off + 4]);
                out.assign(c.begin() + off + 8, c.begin() + off + 8 + sz);  // program header + (native code) + bitcode
            }
        } else {
            XSL_WARN("gfx.pc", "recompiling a slim shader PDB failed %08lx/%08lx", hr, status);
        }
    }
    for (auto& [fn, b] : inc.files) b->Release();
    if (result) result->Release();
    if (compiler) compiler->Release();
    if (pdb) pdb->Release();
    if (blob) blob->Release();
    if (utils) utils->Release();
    return out;
}
std::vector<uint8_t> ildb_from_msf(const std::vector<uint8_t>& f) {
    for (auto& st : msf_streams(f)) {
        if (st.size() < 32 || rd32(st.data()) != fourcc("DXBC")) continue;
        uint32_t count = rd32(&st[28]);
        for (uint32_t i = 0; i < count && 32 + 4 * i + 4 <= st.size(); i++) {
            uint32_t off = rd32(&st[32 + 4 * i]);
            if (off + 8 > st.size() || rd32(&st[off]) != fourcc("ILDB")) continue;
            uint32_t sz = rd32(&st[off + 4]);
            if (off + 8ull + sz <= st.size()) return std::vector<uint8_t>(st.begin() + off + 8, st.begin() + off + 8 + sz);
        }
    }
    return {};
}
// The ILDB part (program header + bitcode) of the container inside the PDB named by `name`.
std::vector<uint8_t> pdb_program(const std::string& name) {
    std::vector<std::string> tries{name};
    std::string base = name.substr(name.find_last_of("/\\") + 1);
    char exe[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
        std::string dir(exe);
        tries.push_back(dir.substr(0, dir.find_last_of("\\/") + 1) + base);
    }
    tries.push_back(base);
    for (auto& t : tries) {
        auto f = read_whole(t);
        if (f.empty()) continue;
        XSL_TRACE("gfx.pc", "PDB %s: %zu bytes, %zu streams", t.c_str(), f.size(), msf_streams(f).size());
        auto prog = ildb_from_msf(f);
        if (prog.empty()) {
            prog = recompile_slim_pdb(f);
            if (!prog.empty()) XSL_INFO("gfx.pc", "slim shader PDB %s: program recompiled from its sources", t.c_str());
        }
        if (!prog.empty()) {
            XSL_INFO("gfx.pc", "shader program from PDB %s", t.c_str());
            return prog;
        }
    }
    return {};
}
}  // namespace


// DXBC checksum: MD5 transforms over everything after the 20-byte header, with a
// non-standard final block that stores the bit count first and (bits>>2)|1 last.
void dxbc_checksum(const uint8_t* data, size_t size, uint32_t out[4]) {
    const uint8_t* p = data + 20;
    size_t n = size - 20;
    uint32_t bits = uint32_t(n * 8);
    size_t full = n & ~size_t(63), left = n - full;
    Md5 m;
    uint32_t blk[16];
    for (size_t i = 0; i < full; i += 64) { memcpy(blk, p + i, 64); m.block(blk); }
    if (left < 56) {
        memset(blk, 0, 64);
        blk[0] = bits;
        memcpy(reinterpret_cast<uint8_t*>(blk) + 4, p + full, left);
        reinterpret_cast<uint8_t*>(blk)[4 + left] = 0x80;
        blk[15] = (bits >> 2) | 1;
        m.block(blk);
    } else {
        memset(blk, 0, 64);
        memcpy(blk, p + full, left);
        reinterpret_cast<uint8_t*>(blk)[left] = 0x80;
        m.block(blk);
        memset(blk, 0, 64);
        blk[0] = bits;
        blk[15] = (bits >> 2) | 1;
        m.block(blk);
    }
    out[0] = m.a; out[1] = m.b; out[2] = m.c; out[3] = m.d;
}

namespace pc {

bool sanitize_container(const void* in, size_t inSize, void** out, size_t* outSize) {
    if (native_translate(in, inSize, out, outSize)) return true;
    auto* d = static_cast<const uint8_t*>(in);
    if (inSize < 32 || rd32(d) != fourcc("DXBC")) return false;
    uint32_t total = rd32(d + 24), count = rd32(d + 28);
    if (total > inSize || 32 + 4ull * count > total) return false;

    struct Part { uint32_t cc; std::vector<uint8_t> data; };
    std::vector<Part> parts;
    bool changed = false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = rd32(d + 32 + 4 * i);
        if (off + 8 > total) return false;
        uint32_t cc = rd32(d + off), sz = rd32(d + off + 4);
        if (off + 8ull + sz > total) return false;
        const uint8_t* body = d + off + 8;
        Part p{cc, std::vector<uint8_t>(body, body + sz)};
        if (cc == fourcc("DXIL") && sz >= 24) {
            // Program header: version, size in dwords, 'DXIL', dxil version, bitcode offset, bitcode size.
            uint32_t bcOff = rd32(body + 16), bcSize = rd32(body + 20);
            if (bcOff != 16 && 16ull + bcOff + bcSize <= sz) {
                std::vector<uint8_t> nb(24 + bcSize);
                memcpy(nb.data(), body, 24);
                memcpy(nb.data() + 24, body + 8 + bcOff, bcSize);
                wr32(nb.data() + 4, uint32_t(nb.size() / 4));
                wr32(nb.data() + 16, 16);
                p.data = std::move(nb);
                changed = true;
                XSL_TRACE("gfx.pc", "DXIL part: dropped %u bytes of native code", sz - 24 - bcSize);
            }
        }
        parts.push_back(std::move(p));
    }
    // Shaders built without reflection/IL (e.g. -Qstrip_reflect with native code only) can keep an
    // empty DXIL program while the debug part ILDB holds the full program (with debug metadata).
    // Translate that one instead.
    Part* dxil = nullptr;
    const Part* ildb = nullptr;
    const Part* ildn = nullptr;
    for (auto& p : parts) {
        if (p.cc == fourcc("DXIL")) dxil = &p;
        if (p.cc == fourcc("ILDB")) ildb = &p;
        if (p.cc == fourcc("ILDN")) ildn = &p;
    }
    Part fromPdb{fourcc("ILDB"), {}};
    if (dxil && !ildb && ildn && dxil->data.size() >= 24 && rd32(dxil->data.data() + 20) < 64 && ildn->data.size() > 4) {
        // ILDN: flags (u16), name length (u16), name.
        uint16_t len;
        memcpy(&len, ildn->data.data() + 2, 2);
        if (4u + len <= ildn->data.size()) {
            std::string name((const char*)ildn->data.data() + 4, strnlen((const char*)ildn->data.data() + 4, len));
            fromPdb.data = pdb_program(name);
            if (!fromPdb.data.empty()) ildb = &fromPdb;
            else XSL_WARN("gfx.pc", "shader has no DXIL program and its PDB %s was not found or holds none", name.c_str());
        }
    }
    if (dxil && ildb && dxil->data.size() >= 24 && ildb->data.size() > 24) {
        uint32_t bcSize = rd32(dxil->data.data() + 20);
        if (bcSize < 64) {
            const uint8_t* ib = ildb->data.data();
            uint32_t iOff = rd32(ib + 16), iSize = rd32(ib + 20);
            if (8ull + iOff + iSize <= ildb->data.size()) {
                std::vector<uint8_t> nb(24 + iSize);
                memcpy(nb.data(), ib, 24);
                memcpy(nb.data() + 24, ib + 8 + iOff, iSize);
                wr32(nb.data() + 4, uint32_t(nb.size() / 4));
                wr32(nb.data() + 16, 16);
                dxil->data = std::move(nb);
                changed = true;
                XSL_TRACE("gfx.pc", "DXIL part has no program; using the %u-byte program from ILDB", iSize);
            }
        }
    }
    if (!changed) return false;

    size_t size = 32 + 4 * parts.size();
    for (auto& p : parts) size += 8 + ((p.data.size() + 3) & ~size_t(3));
    auto* o = static_cast<uint8_t*>(malloc(size));
    memset(o, 0, size);
    memcpy(o, d, 24);  // magic, checksum (rewritten below), version
    wr32(o + 24, uint32_t(size));
    wr32(o + 28, uint32_t(parts.size()));
    size_t cur = 32 + 4 * parts.size();
    for (size_t i = 0; i < parts.size(); i++) {
        wr32(o + 32 + 4 * i, uint32_t(cur));
        wr32(o + cur, parts[i].cc);
        wr32(o + cur + 4, uint32_t((parts[i].data.size() + 3) & ~size_t(3)));
        memcpy(o + cur + 8, parts[i].data.data(), parts[i].data.size());
        cur += 8 + ((parts[i].data.size() + 3) & ~size_t(3));
    }
    uint32_t sum[4];
    dxbc_checksum(o, size, sum);
    memcpy(o + 4, sum, 16);
    *out = o;
    *outSize = size;
    return true;
}

void free_container(void* p) { free(p); }

}  // namespace pc
