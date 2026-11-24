// SPDX-License-Identifier: MIT
// xsl-shaderwarm <folder or file>... [--jobs N] [--shard K/N] [--check] [--report FILE]
//
// Fills the layer's shader cache (XSL_SHADER_CACHE, or the layer's default folder) from console
// shader containers on disk, before a title runs: every container is translated and compiled with
// the same code and the same cache key the layer uses when a pipeline is created, so a later run
// finds its shaders in the cache. Containers already in the cache are skipped, so a stopped run
// continues where it left off, and a new translator version only costs the time here instead of
// hitches in the title.
//
//   --jobs N       compiles at a time in this process (default: half the logical processors)
//   --shard K/N    only every N-th file, starting at the K-th (K from 0): for several processes
//   --check        only count what the cache has and lacks; nothing is compiled
//   --report FILE  one line per shader that did not translate (with --check: that the cache lacks):
//                  file name, tab, reason
//
// The shader compiler does not scale inside one process: a dozen compiles at a time use about three
// processors. tools/xsl-warm therefore starts one process per processor, each with --jobs 1 and a
// --shard of its own, and adds up what they report.
//
// A shader that describes its own root signature is translated against that one. A shader that
// needs the root signature of the pipeline it is used in cannot be translated here and is counted
// apart. The translation follows the GPU's capabilities, so run this on the machine and GPU the
// title will run on; dxcompiler.dll, dxil.dll and (on Windows 10) the D3D12\ runtime folder must
// sit next to this program, as they do next to a title started with xsl-run (tools/xsl-warm
// arranges that).
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bridge.h"

namespace {
struct Item { std::wstring path; uint64_t size; };
std::vector<Item> g_items;
std::atomic<size_t> g_next{0}, g_done{0}, g_cached{0}, g_made{0}, g_failed{0}, g_needs{0}, g_other{0};
std::mutex g_mu;
std::map<std::string, size_t> g_reasons;          // why shaders did not translate, with counts
std::vector<std::pair<std::wstring, std::string>> g_report;
struct StageCount { size_t cached = 0, made = 0, failed = 0, missing = 0; };
std::map<std::string, StageCount> g_stages;       // the same counts per shader stage
bool g_check = false;
PcObj g_dev = nullptr;

void add_path(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) { fprintf(stderr, "not found: %ls\n", p.c_str()); return; }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d);
        g_items.push_back({p, ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow});
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring q = p + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) add_path(q);
        else g_items.push_back({q, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool read_all(const std::wstring& p, std::vector<uint8_t>& d) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    d.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = d.empty() || (ReadFile(f, d.data(), (DWORD)d.size(), &got, nullptr) && got == d.size());
    CloseHandle(f);
    return ok;
}

void fail(const std::wstring& path, const std::string& why) {
    std::lock_guard<std::mutex> g(g_mu);
    g_reasons[why]++;
    g_report.emplace_back(path, why);
}

void worker() {
    std::vector<uint8_t> d, blob;
    for (;;) {
        size_t i = g_next++;
        if (i >= g_items.size()) return;
        const Item& it = g_items[i];
        if (!read_all(it.path, d) || d.size() < 32 || memcmp(d.data(), "DXBC", 4)) { g_other++; g_done++; continue; }
        // The root signature a pipeline of this shader would be created with: the one the shader
        // describes itself (none: the shader depends on its pipeline's).
        const void* code = d.data();
        const size_t size = d.size();
        blob.clear();
        const bool own = pc::own_root_signature(g_dev, &code, &size, 1, &blob) != nullptr && !blob.empty();
        pc::set_fallback_root_signature(own ? blob.data() : nullptr, own ? blob.size() : 0);
        char stage[8];
        int st = pc::shader_cache_state(code, size, stage);
        auto count = [&](size_t StageCount::*what) { std::lock_guard<std::mutex> g(g_mu); g_stages[stage].*what += 1; };
        if (st < 0) g_other++;
        else if (st == 1) { g_cached++; count(&StageCount::cached); }
        else if (g_check) {
            g_needs++;
            count(&StageCount::missing);
            std::lock_guard<std::mutex> g(g_mu);
            g_report.emplace_back(it.path, std::string(stage) + ": not in the cache");
        }
        else {
            pc::reset_untranslated();
            void* out = nullptr;
            size_t outSize = 0;
            if (pc::sanitize_container(code, size, &out, &outSize) && out) free(out);
            if (pc::shader_cache_state(code, size, nullptr) == 1) { g_made++; count(&StageCount::made); }
            else {
                g_failed++;
                count(&StageCount::failed);
                std::string why = pc::untranslated_reason();
                if (why.empty()) why = std::string(stage) + ": not translated";
                fail(it.path, why);
            }
        }
        g_done++;
    }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency() / 2);
    unsigned shard = 0, shards = 1;
    std::wstring report;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--jobs") && i + 1 < argc) jobs = std::max(1, _wtoi(argv[++i]));
        else if (!wcscmp(argv[i], L"--shard") && i + 1 < argc) { if (swscanf(argv[++i], L"%u/%u", &shard, &shards) != 2 || !shards || shard >= shards) { fprintf(stderr, "--shard K/N with K < N\n"); return 2; } }
        else if (!wcscmp(argv[i], L"--check")) g_check = true;
        else if (!wcscmp(argv[i], L"--report") && i + 1 < argc) report = argv[++i];
        else add_path(argv[i]);
    }
    if (g_items.empty()) {
        fprintf(stderr, "usage: xsl-shaderwarm <folder or file>... [--jobs N] [--shard K/N] [--check] [--report FILE]\n");
        return 2;
    }
    // Largest first: the long compiles start early instead of leaving one thread busy at the end.
    // (Path as the tie-break: every shard must see the same order.)
    std::sort(g_items.begin(), g_items.end(), [](const Item& a, const Item& b) { return a.size != b.size ? a.size > b.size : a.path < b.path; });
    if (shards > 1) {
        std::vector<Item> mine;
        for (size_t i = shard; i < g_items.size(); i += shards) mine.push_back(std::move(g_items[i]));
        g_items.swap(mine);
    }
    if (pc::create_device(&g_dev) < 0 || !g_dev) {
        fprintf(stderr, "no Direct3D 12 device: the translation follows the GPU, so one is needed\n");
        return 1;
    }
    printf("%zu files, %u at a time%s\n", g_items.size(), jobs, g_check ? " (check only)" : "");
    fflush(stdout);
    const ULONGLONG t0 = GetTickCount64();
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < jobs; k++) pool.emplace_back(worker);
    auto progress = [&] {
        printf("%zu / %zu: %zu already cached, %zu compiled, %zu not translated, %zu not cached, %zu other (%llu s)\n", g_done.load(), g_items.size(),
               g_cached.load(), g_made.load(), g_failed.load(), g_needs.load(), g_other.load(), (unsigned long long)((GetTickCount64() - t0) / 1000));
        fflush(stdout);
    };
    for (unsigned waited = 0; g_done.load() < g_items.size(); waited += 250) {
        Sleep(250);
        if (waited % 5000 == 4750) progress();
    }
    for (auto& t : pool) t.join();
    progress();
    printf("\ndone in %llu s: %zu files\n  %zu already in the cache\n  %zu compiled now\n  %zu not translated\n  %zu not console shader code\n",
           (unsigned long long)((GetTickCount64() - t0) / 1000), g_items.size(), g_cached.load(), g_made.load(), g_failed.load(), g_other.load());
    if (g_check) printf("  %zu not in the cache (not compiled: --check)\n", g_needs.load());
    printf("\nby stage:   cached  compiled  not translated%s\n", g_check ? "  not in the cache" : "");
    for (auto& kv : g_stages) {
        printf("  %-4s %11zu %9zu %15zu", kv.first.c_str(), kv.second.cached, kv.second.made, kv.second.failed);
        if (g_check) printf(" %17zu", kv.second.missing);
        printf("\n");
    }
    if (!g_reasons.empty()) {
        std::vector<std::pair<size_t, std::string>> r;
        for (auto& kv : g_reasons) r.emplace_back(kv.second, kv.first);
        std::sort(r.rbegin(), r.rend());
        printf("\nwhy shaders did not translate:\n");
        for (auto& kv : r) printf("  %6zu  %s\n", kv.first, kv.second.c_str());
    }
    if (!report.empty()) {
        if (FILE* f = _wfopen(report.c_str(), L"w")) {
            std::sort(g_report.begin(), g_report.end());
            for (auto& kv : g_report) fprintf(f, "%ls\t%s\n", kv.first.c_str(), kv.second.c_str());
            fclose(f);
        }
    }
    return 0;
}
