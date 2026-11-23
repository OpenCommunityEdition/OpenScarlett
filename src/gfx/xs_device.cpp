// SPDX-License-Identifier: AGPL-3.0-or-later
// ID3D12Device (console), D3D12XboxCreateDevice, frame events and the DXGI shims.
#include <condition_variable>
#include <deque>
#include <thread>
#include <array>
#include <atomic>
#include <memory>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <map>
#include <vector>

#include "xs_object.h"
#include "rtso.h"
#include "xs_cbv.h"
#include "xs_detile.h"
#include "xs_unified.h"

void xsl_fixed_time_install();   // fixed_time.cpp (global namespace)

namespace xs {
namespace {
Device* g_device = nullptr;
std::mutex g_device_mu;

const IID* const kDeviceIids[] = {
    &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12Device), &__uuidof(ID3D12Device1), &__uuidof(ID3D12Device2),
    &__uuidof(ID3D12Device3), &__uuidof(ID3D12Device4), &__uuidof(ID3D12Device5), &__uuidof(ID3D12Device6), &__uuidof(ID3D12Device7),
    &__uuidof(ID3D12Device8), &__uuidof(ID3D12Device9), &__uuidof(ID3D12Device10), &__uuidof(ID3D12Device11), nullptr};

#define DPC(d) dev_extra(d)->pcDevice
#define DEV(d) ((Device*)(d))

UINT D3DAPI D_GetNodeCount(ID3D12Device*) { return 1; }
HRESULT D3DAPI D_CreateCommandQueue(ID3D12Device* d, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** out) {
    PcObj q = nullptr;
    HRESULT hr = pc::create_queue(DPC(d), desc, &q);
    if (FAILED(hr)) return hr;
    DeviceExtra* e = dev_extra(d);
    if (!e->pcQueue && desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) e->pcQueue = q;
    return make_queue(DEV(d), q, desc, riid, out);
}
HRESULT D3DAPI D_CreateCommandAllocator(ID3D12Device* d, D3D12_COMMAND_LIST_TYPE t, REFIID riid, void** out) {
    PcObj a = nullptr;
    HRESULT hr = pc::create_allocator(DPC(d), t, &a);
    if (FAILED(hr)) return hr;
    return make_allocator(DEV(d), a, t, riid, out);
}
HRESULT D3DAPI D_CreateCommandListX(ID3D12Device* d, const D3D12XBOX_COMMAND_LIST_DESC* desc, ID3D12CommandAllocator* a,
                                    ID3D12PipelineState* pso, REFIID riid, void** out) {
    PcObj l = nullptr;
    HRESULT hr = pc::create_list(DPC(d), desc->Type, P(a), P(pso), &l);
    if (FAILED(hr)) return hr;
    return make_list(DEV(d), l, desc->Type, riid, out);
}
HRESULT D3DAPI D_CreateCommandList(ID3D12Device* d, UINT, D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator* a, ID3D12PipelineState* pso,
                                   REFIID riid, void** out) {
    D3D12XBOX_COMMAND_LIST_DESC desc{};
    desc.Type = t;
    return D_CreateCommandListX(d, &desc, a, pso, riid, out);
}
HRESULT D3DAPI D_CheckFeatureSupport(ID3D12Device* d, D3D12_FEATURE f, void* data, UINT size) {
    HRESULT hr = pc::check_feature_support(DPC(d), f, data, size);
    if (hr == E_INVALIDARG && size > 8) {
        // Console feature structs may append fields to the PC layout (e.g. D3D12_OPTIONS: 64 vs 60
        // bytes). Query with the PC size, keep the shared prefix and zero the console-only tail.
        std::vector<uint8_t> tmp(data ? (uint8_t*)data : nullptr, data ? (uint8_t*)data + size : nullptr);
        for (UINT pcSize = size - 4; pcSize + 64 >= size && pcSize >= 4; pcSize -= 4) {
            if (SUCCEEDED(pc::check_feature_support(DPC(d), f, tmp.data(), pcSize))) {
                memset(tmp.data() + pcSize, 0, size - pcSize);
                memcpy(data, tmp.data(), size);
                XSL_TRACE(CH, "CheckFeatureSupport(%d): console struct %u bytes, PC %u", (int)f, size, pcSize);
                return S_OK;
            }
        }
    }
    if (FAILED(hr)) XSL_WARN(CH, "CheckFeatureSupport(feature %d, %u bytes) failed %08lx", (int)f, size, hr);
    // XSL_NO_RAYTRACING=1: report no ray tracing, so titles that check take their other paths
    // (ray-tracing pipelines and ray queries are not translated yet).
    static bool no_rt = [] { char b[4]; return GetEnvironmentVariableA("XSL_NO_RAYTRACING", b, 4) && b[0] == '1'; }();
    if (no_rt && SUCCEEDED(hr) && f == D3D12_FEATURE_D3D12_OPTIONS5 && data && size >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5)) {
        ((D3D12_FEATURE_DATA_D3D12_OPTIONS5*)data)->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
        static volatile long n = 0;
        if (InterlockedIncrement(&n) == 1) XSL_INFO(CH, "ray tracing reported as unsupported (XSL_NO_RAYTRACING)");
    }
    if (f == D3D12_FEATURE_FEATURE_LEVELS && data && size >= sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) {
        auto* fl = (D3D12_FEATURE_DATA_FEATURE_LEVELS*)data;
        std::string req;
        for (UINT i = 0; fl->pFeatureLevelsRequested && i < fl->NumFeatureLevels && i < 8; i++) {
            char b[16];
            snprintf(b, sizeof b, " %x", (unsigned)fl->pFeatureLevelsRequested[i]);
            req += b;
        }
        XSL_INFO(CH, "CheckFeatureSupport(FEATURE_LEVELS): requested%s -> max %x (%08lx)", req.c_str(), (unsigned)fl->MaxSupportedFeatureLevel, hr);
    } else if (f == D3D12_FEATURE_SHADER_MODEL && data) {
        XSL_INFO(CH, "CheckFeatureSupport(SHADER_MODEL) -> %x (%08lx)", (unsigned)((D3D12_FEATURE_DATA_SHADER_MODEL*)data)->HighestShaderModel, hr);
    }
    return hr;
}
HRESULT D3DAPI D_CreateDescriptorHeap(ID3D12Device* d, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out) {
    XSL_TRACE("gfx.cmd", "CreateDescriptorHeap type %d n %u", (int)desc->Type, desc->NumDescriptors);
    static bool unified = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_UNIFIED_DESCRIPTORS", b, 4) && b[0] == '0'); }();
    // Shader-visible heaps share the global PC heap. CPU-only (staging) heaps get PC heaps of their
    // own: titles fill shader-visible heaps up to the PC maximum, and copies out of staging heaps
    // translate each descriptor (D_CopyDescriptorsSimple).
    if (unified && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) {
        uint32_t off;
        uint64_t cpu, gpu;
        // XSL_VIEW_HEAP_CAP=<n> (experiment): heaps after the first get at most n global slots.
        static uint32_t cap = [] { char b[16]; return GetEnvironmentVariableA("XSL_VIEW_HEAP_CAP", b, sizeof b) ? (uint32_t)atoi(b) : 0u; }();
        static bool first = true;
        uint32_t want = desc->NumDescriptors;
        if (cap && !first && want > cap) want = cap;
        first = false;
        if (pc::global_view_alloc(DPC(d), want, &off, &cpu, &gpu)) {
            XSL_INFO(CH, "shader-visible view heap: %u entries (%u backed) at %u", desc->NumDescriptors, want, off);
            // Console layout: CPU handles in the layer's descriptor memory (see xs_descriptors.cpp).
            if (uint64_t shadow = desc_view_cpu_start(DEV(d), off)) { cpu = shadow; gpu = desc_view_gpu_start(off); }
            return make_descriptor_heap_range(DEV(d), desc, off, cpu, gpu, riid, out);
        }
        XSL_WARN(CH, "global descriptor heap full; %u-entry heap gets its own PC heap (raise XSL_DESCRIPTOR_HEAP_SIZE)", desc->NumDescriptors);
    }
    PcObj h = nullptr;
    HRESULT hr = pc::create_descriptor_heap(DPC(d), desc, &h);
    if (FAILED(hr)) return hr;
    if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)   // staging heap: payloads of its views (never freed: small)
        desc_staging_heap(pc::heap_cpu_start(h), desc->NumDescriptors, pc::descriptor_increment(DPC(d), desc->Type), true);
    if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
        if (uint64_t shadow = desc_rtv_dsv_heap(DEV(d), desc->Type, desc->NumDescriptors))
            return make_rtv_dsv_shadow_heap(DEV(d), h, desc, shadow, riid, out);
    return make_descriptor_heap(DEV(d), h, desc, riid, out);
}
UINT D3DAPI D_GetDescriptorHandleIncrementSize(ID3D12Device* d, D3D12_DESCRIPTOR_HEAP_TYPE t) {
    return desc_increment(t, pc::descriptor_increment(DPC(d), t));
}
// Serialized root signatures by PC object (ray-tracing pipelines lay out their argument memory from
// the global root signature).
std::mutex g_rs_blob_mu;
std::map<PcObj, std::vector<uint8_t>> g_rs_blobs;
bool root_signature_blob(PcObj rs, std::vector<uint8_t>* out) {
    std::lock_guard<std::mutex> g(g_rs_blob_mu);
    auto it = g_rs_blobs.find(rs);
    if (it == g_rs_blobs.end()) return false;
    *out = it->second;
    return true;
}
HRESULT D3DAPI D_CreateRootSignature(ID3D12Device* d, UINT, const void* blob, SIZE_T n, REFIID riid, void** out) {
    XSL_TRACE("gfx.cmd", "CreateRootSignature %zu bytes", (size_t)n);
    PcObj rs = nullptr;
    HRESULT hr = pc::create_root_signature(DPC(d), blob, n, &rs);
    if (FAILED(hr)) return hr;
    if (blob && n) {
        std::lock_guard<std::mutex> g(g_rs_blob_mu);
        g_rs_blobs[rs].assign((const uint8_t*)blob, (const uint8_t*)blob + n);
    }
    XSL_TRACE("gfx.cmd", "CreateRootSignature -> %p", rs);
    return make_simple(DEV(d), Kind::RootSignature, rs, riid, out);
}
// A pipeline whose console-only shaders cannot be translated yet gets a placeholder PSO (its draws
// and dispatches are skipped) instead of failing, which titles treat as fatal.
// XSL_PLACEHOLDER_PSO=0 disables this.
static HRESULT pso_result(Device* dev, HRESULT hr, PcObj pso, REFIID riid, void** out) {
    if (SUCCEEDED(hr)) return make_simple(dev, Kind::Pso, pso, riid, out);
    static const bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_PLACEHOLDER_PSO", b, 4) && b[0] == '0'); }();
    const bool translated = *pc::translated_keys() != 0;
    if (!on || (!pc::had_untranslated() && !translated)) return hr;
    static volatile long n = 0;
    if (InterlockedIncrement(&n) <= 8) {
        if (pc::had_untranslated()) XSL_WARN(CH, "pipeline with untranslated console shaders: placeholder PSO (its work is skipped)");
        else XSL_WARN(CH, "pipeline of translated shaders %s rejected by the PC runtime (%08lx): placeholder PSO (its work is skipped)", pc::translated_keys(), hr);
    }
    return make_simple(dev, Kind::Pso, nullptr, riid, out);
}
HRESULT D3DAPI D_CreateGraphicsPipelineState(ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    if (null_graphics()) return make_simple(DEV(d), Kind::Pso, nullptr, riid, out);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC x = *desc;
    x.pRootSignature = (ID3D12RootSignature*)P(desc->pRootSignature);
    XSL_TRACE("gfx.cmd", "CreateGraphicsPipelineState topology type %d, GS %zu bytes", (int)x.PrimitiveTopologyType, x.GS.BytecodeLength);
    for (UINT i = 0; i < x.InputLayout.NumElements; i++)
        XSL_TRACE("gfx.cmd", "  input %s%u format %d offset %u", x.InputLayout.pInputElementDescs[i].SemanticName,
                  x.InputLayout.pInputElementDescs[i].SemanticIndex, (int)x.InputLayout.pInputElementDescs[i].Format,
                  x.InputLayout.pInputElementDescs[i].AlignedByteOffset);
    // Console-only topology types: quads and rects are drawn as triangles (see the list's draw path).
    if (x.PrimitiveTopologyType == D3D12XBOX_PRIMITIVE_TOPOLOGY_TYPE_QUAD || x.PrimitiveTopologyType == D3D12XBOX_PRIMITIVE_TOPOLOGY_TYPE_RECT)
        x.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PcObj pso = nullptr;
    pc::reset_untranslated();
    HRESULT hr = pc::create_graphics_pso(DPC(d), &x, &pso);
    if (SUCCEEDED(hr)) pc::note_pso_rs(pso, x.pRootSignature, false);
    return pso_result(DEV(d), hr, pso, riid, out);
}
// Compute pipelines are created on a worker: a translated shader can take the driver minutes to
// compile (one did, on the title's submission thread, and the title stopped presenting). After
// XSL_PSO_ASYNC_MS (default 300) the title gets a placeholder (its dispatches are skipped) that
// becomes the real pipeline when the compile ends.
// A few worker threads for pipeline compiles (a thread per pipeline ran dozens of multi-GB driver
// compiles at once and creations failed with E_OUTOFMEMORY). XSL_PSO_THREADS (default 3).
static void pso_pool_run(std::function<void()> job) {
    static std::mutex mu;
    static std::condition_variable cv;
    static std::deque<std::function<void()>> q;
    static std::once_flag once;
    std::call_once(once, [] {
        char b[8];
        unsigned n = GetEnvironmentVariableA("XSL_PSO_THREADS", b, sizeof b) ? (unsigned)atoi(b) : 3u;
        for (unsigned t = 0; t < std::max(1u, n); t++)
            std::thread([] {
                for (;;) {
                    std::function<void()> j;
                    { std::unique_lock<std::mutex> u(mu); cv.wait(u, [] { return !q.empty(); }); j = std::move(q.front()); q.pop_front(); }
                    j();
                }
            }).detach();
    });
    { std::lock_guard<std::mutex> g(mu); q.push_back(std::move(job)); }
    cv.notify_one();
}
struct AsyncPso {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false, untranslated = false, late = false;
    std::string keys;
    HRESULT hr = E_FAIL;
    PcObj pso = nullptr;
    void* obj = nullptr;   // the placeholder handed out (late completion)
    PcObj rs = nullptr;
};
HRESULT D3DAPI D_CreateComputePipelineStateX(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, UINT, const D3D12XBOX_EXTENDED_COMPUTE_PIPELINE_STATE_DESC*, REFIID riid,
                                             void** out) {
    if (null_graphics()) return make_simple(DEV(d), Kind::Pso, nullptr, riid, out);
    static DWORD waitMs = [] { char b[16]; return GetEnvironmentVariableA("XSL_PSO_ASYNC_MS", b, sizeof b) ? (DWORD)atoi(b) : 300u; }();
    D3D12_COMPUTE_PIPELINE_STATE_DESC x = *desc;
    x.pRootSignature = (ID3D12RootSignature*)P(desc->pRootSignature);
    if (!waitMs) {
        PcObj pso = nullptr;
        pc::reset_untranslated();
        HRESULT hr = pc::create_compute_pso(DPC(d), &x, &pso);
        if (SUCCEEDED(hr)) pc::note_pso_rs(pso, x.pRootSignature, true);
        return pso_result(DEV(d), hr, pso, riid, out);
    }
    auto job = std::make_shared<AsyncPso>();
    auto code = std::make_shared<std::vector<uint8_t>>((const uint8_t*)x.CS.pShaderBytecode, (const uint8_t*)x.CS.pShaderBytecode + x.CS.BytecodeLength);
    job->rs = x.pRootSignature;
    if (job->rs) pc::addref(job->rs);
    PcObj dev = DPC(d);
    pso_pool_run([job, code, x, dev]() mutable {
        x.CS = {code->data(), code->size()};
        pc::reset_untranslated();
        PcObj pso = nullptr;
        HRESULT hr = pc::create_compute_pso(dev, &x, &pso);
        if (SUCCEEDED(hr)) pc::note_pso_rs(pso, x.pRootSignature, true);
        std::unique_lock<std::mutex> g(job->mu);
        job->hr = hr; job->pso = pso; job->untranslated = pc::had_untranslated(); job->keys = pc::translated_keys(); job->done = true;
        if (job->late) {   // the title holds a placeholder: it becomes this pipeline
            if (SUCCEEDED(hr) && job->obj) {
                hdr(job->obj)->pc = pso;
                XSL_INFO(CH, "compute pipeline of %s ready after the title went on (placeholder replaced)", job->keys.c_str());
            }
            if (job->obj) Release((IGraphicsUnknown*)job->obj);
        }
        if (job->rs) pc::release(job->rs);
        g.unlock();
        job->cv.notify_all();
    });
    std::unique_lock<std::mutex> g(job->mu);
    if (job->cv.wait_for(g, std::chrono::milliseconds(waitMs), [&] { return job->done; })) {
        HRESULT hr = job->hr;
        if (SUCCEEDED(hr)) return make_simple(DEV(d), Kind::Pso, job->pso, riid, out);
        if (!job->untranslated && job->keys.empty()) return hr;
        return make_simple(DEV(d), Kind::Pso, nullptr, riid, out);   // placeholder (as pso_result)
    }
    HRESULT hr = make_simple(DEV(d), Kind::Pso, nullptr, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        job->late = true;
        job->obj = *out;
        AddRef((IGraphicsUnknown*)*out);
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 10) XSL_WARN(CH, "compute pipeline still compiling after %lu ms: placeholder until it is ready", (unsigned long)waitMs);
    }
    return hr;
}
HRESULT D3DAPI D_CreateComputePipelineState(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    return D_CreateComputePipelineStateX(d, desc, 0, nullptr, riid, out);
}
// Pipeline-state streams: console subobject numbering and contents -> PC.
HRESULT D3DAPI D_CreatePipelineState(ID3D12Device2* d, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out) {
    if (null_graphics()) return make_simple(DEV(d), Kind::Pso, nullptr, riid, out);
    pc::reset_untranslated();
    XSL_TRACE("gfx.cmd", "CreatePipelineState stream %zu bytes", (size_t)desc->SizeInBytes);
    auto align = [](size_t v, size_t a) { return (v + a - 1) & ~(a - 1); };
    const uint8_t* in = (const uint8_t*)desc->pPipelineStateSubobjectStream;
    std::vector<uint8_t> pcs;
    std::vector<void*> owned;  // sanitized shader containers, freed after creation
    bool saw_rs = false, stream_cs = false;
    PcObj stream_rs = nullptr;
    pcs.reserve(desc->SizeInBytes + 64);
    size_t cur_al = 8;  // alignment of the payload being copied (d3dx12 subobject layout)
    auto emit = [&](uint32_t pcType, const void* payload, size_t len) {
        size_t start = align(pcs.size(), 8);
        size_t pay = align(start + 4, cur_al);
        pcs.resize(align(pay + len, 8), 0);
        memcpy(pcs.data() + start, &pcType, 4);
        memcpy(pcs.data() + pay, payload, len);
    };
    size_t off = 0;
    while (off + 4 <= desc->SizeInBytes) {
        uint32_t type;
        memcpy(&type, in + off, 4);
        // Payload sizes: the console stream uses the same per-subobject layout as d3dx12.
        size_t len = 0, al = 8;
        switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: len = sizeof(void*); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: len = sizeof(D3D12_SHADER_BYTECODE); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: len = sizeof(D3D12_STREAM_OUTPUT_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: len = sizeof(D3D12_BLEND_DESC); al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: len = 4; al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: len = sizeof(D3D12_RASTERIZER_DESC); al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: len = sizeof(D3D12_DEPTH_STENCIL_DESC); al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: len = sizeof(D3D12_INPUT_LAYOUT_DESC); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: len = 4; al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: len = 4; al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: len = sizeof(D3D12_RT_FORMAT_ARRAY); al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: len = 4; al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: len = sizeof(DXGI_SAMPLE_DESC); al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: len = 4; al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: len = sizeof(D3D12_CACHED_PIPELINE_STATE); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: len = 4; al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: len = sizeof(D3D12_DEPTH_STENCIL_DESC1); al = 4; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: len = sizeof(D3D12_VIEW_INSTANCING_DESC); break;
            case D3D12XBOX_PIPELINE_STATE_SUBOBJECT_TYPE_PIXEL_SHADER_OPTIONS: len = sizeof(D3D12XBOX_PIXEL_SHADER_OPTIONS_DESC); al = alignof(D3D12XBOX_PIXEL_SHADER_OPTIONS_DESC); break;
            case D3D12XBOX_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_OPTIONS: len = sizeof(D3D12XBOX_DEPTH_STENCIL_OPTIONS_DESC); al = alignof(D3D12XBOX_DEPTH_STENCIL_OPTIONS_DESC); break;
            case D3D12XBOX_PIPELINE_STATE_SUBOBJECT_TYPE_GRAPHICS_SHADER_LIMITS: len = sizeof(D3D12XBOX_GRAPHICS_SHADER_LIMITS_DESC); al = alignof(D3D12XBOX_GRAPHICS_SHADER_LIMITS_DESC); break;
            case D3D12XBOX_PIPELINE_STATE_SUBOBJECT_TYPE_TESSELLATION_PARAMETERS: len = sizeof(D3D12XBOX_TESSELLATION_PARAMETERS_DESC); al = alignof(D3D12XBOX_TESSELLATION_PARAMETERS_DESC); break;
            case D3D12XBOX_PIPELINE_STATE_SUBOBJECT_TYPE_COMPUTE_SHADER_LIMITS: len = sizeof(D3D12XBOX_COMPUTE_SHADER_LIMITS_DESC); al = alignof(D3D12XBOX_COMPUTE_SHADER_LIMITS_DESC); break;
            default:
                XSL_ERR(CH, "CreatePipelineState: unknown subobject type %u at offset %zu", type, off);
                for (void* o : owned) pc::free_container(o);
                return E_INVALIDARG;
        }
        size_t pay = align(off + 4, al);
        const uint8_t* p = in + pay;
        cur_al = al;
        if (type >= D3D12XBOX_PIPELINE_STATE_SUBOBJECT_TYPE_PIXEL_SHADER_OPTIONS) {
            // console-only tuning: dropped
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
            ID3D12RootSignature* rs;
            memcpy(&rs, p, sizeof rs);
            saw_rs = rs != nullptr;
            stream_rs = P(rs);
            PcObj prs = P(rs);
            emit(0, &prs, sizeof prs);
        } else if (type <= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS ||
                   type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
            D3D12_SHADER_BYTECODE bc;
            memcpy(&bc, p, sizeof bc);
            bool mesh = type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) stream_cs = true;
            if (mesh && bc.pShaderBytecode && !pc::caps().meshShaders) {
                static volatile long warned = 0;
                if (InterlockedIncrement(&warned) <= 3)
                    XSL_ERR(CH, "title uses mesh shaders; the host GPU has none (DirectX 12 Ultimate needed)");
                for (void* o : owned) pc::free_container(o);
                return E_NOTIMPL;
            }
            void* o; size_t n;
            if (bc.pShaderBytecode && pc::sanitize_container(bc.pShaderBytecode, bc.BytecodeLength, &o, &n)) {
                owned.push_back(o);
                bc.pShaderBytecode = o;
                bc.BytecodeLength = n;
            }
            emit(mesh ? type + 1 : type, &bc, sizeof bc);  // PC numbering: AS = 24, MS = 25
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY) {
            uint32_t t;
            memcpy(&t, p, 4);
            if (t == D3D12XBOX_PRIMITIVE_TOPOLOGY_TYPE_QUAD || t == D3D12XBOX_PRIMITIVE_TOPOLOGY_TYPE_RECT) t = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            emit(type, &t, 4);
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS) {
            D3D12_RT_FORMAT_ARRAY a;
            memcpy(&a, p, sizeof a);
            for (auto& f : a.RTFormats)
                if (int sf = pc::format_stand_in(f)) f = (DXGI_FORMAT)sf;
            emit(type, &a, sizeof a);
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS) {
            uint32_t f;
            memcpy(&f, p, 4);
            f &= D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG;
            emit(type, &f, 4);
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK) {
            // console caches and node masks have no PC meaning
        } else {
            emit(type, p, len);
        }
        off = align(pay + len, 8);
    }
    if (!saw_rs) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 5) XSL_INFO(CH, "pipeline stream without a root signature (the shaders' embedded one is used)");
    }
    PcObj pso = nullptr;
    HRESULT hr = pc::create_pso_stream(DPC(d), pcs.data(), pcs.size(), &pso);
    if (SUCCEEDED(hr)) pc::note_pso_rs(pso, stream_rs, stream_cs);
    for (void* o : owned) pc::free_container(o);
    return pso_result(DEV(d), hr, pso, riid, out);
}
HRESULT D3DAPI D_CreateCommandSignature(ID3D12Device* d, const D3D12_COMMAND_SIGNATURE_DESC* desc, ID3D12RootSignature* rs, REFIID riid, void** out) {
    if (desc) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 40) {
            char t[160] = "";
            for (UINT i = 0; i < desc->NumArgumentDescs && i < 8; i++)
                snprintf(t + strlen(t), sizeof t - strlen(t), " %d", (int)desc->pArgumentDescs[i].Type);
            XSL_INFO(CH, "CreateCommandSignature: stride %u, %u arguments (types%s), root signature %p", desc->ByteStride, desc->NumArgumentDescs, t, (void*)rs);
        }
    }
    PcObj sig = nullptr;
    HRESULT hr = pc::create_command_signature(DPC(d), desc, P(rs), &sig);
    if (FAILED(hr)) { XSL_ERR(CH, "CreateCommandSignature failed %08lx", hr); return hr; }
    return make_simple(DEV(d), Kind::CommandSignature, sig, riid, out);
}
HRESULT D3DAPI D_CreateQueryHeap(ID3D12Device* d, const D3D12_QUERY_HEAP_DESC* desc, REFIID riid, void** out) {
    uint32_t t = desc->Type;
    switch (desc->Type) {  // console-only heap types -> the PC type with the same results
        case D3D12XBOX_QUERY_HEAP_TYPE_OCCLUSION_NO_SYNC: t = D3D12_QUERY_HEAP_TYPE_OCCLUSION; break;
        case D3D12XBOX_QUERY_HEAP_TYPE_TIMESTAMP_NO_SYNC:
        case D3D12XBOX_QUERY_HEAP_TYPE_DMA_TIMESTAMP:
        case D3D12XBOX_QUERY_HEAP_TYPE_DMA_TIMESTAMP_NO_SYNC: t = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; break;
        default: break;
    }
    PcObj h = nullptr;
    HRESULT hr = pc::create_query_heap(DPC(d), t, desc->Count, &h);
    if (FAILED(hr)) return hr;
    return make_simple(DEV(d), Kind::QueryHeap, h, riid, out);
}
// Offline-serialized (native ISA) pipelines need a native-shader recompiler (not implemented).
// Under null graphics a placeholder PSO lets the title run on.
HRESULT D3DAPI D_DeserializeGraphicsPipelineStateX(ID3D12Device* d, const D3D12XBOX_DESERIALIZE_GRAPHICS_PIPELINE_STATE*, REFIID riid, void** out) {
    // A placeholder PSO: draws/dispatches using it are skipped, the rest of the title runs on.
    static volatile long n = 0;
    if (!null_graphics() && InterlockedIncrement(&n) <= 3)
        XSL_WARN(CH, "DeserializeGraphicsPipelineStateX: serialized native pipeline (needs a shader recompiler); its work is skipped");
    return make_simple(DEV(d), Kind::Pso, nullptr, riid, out);
}
HRESULT D3DAPI D_DeserializeComputePipelineStateX(ID3D12Device* d, const D3D12XBOX_DESERIALIZE_COMPUTE_PIPELINE_STATE*, REFIID riid, void** out) {
    // A placeholder PSO: draws/dispatches using it are skipped, the rest of the title runs on.
    static volatile long n = 0;
    if (!null_graphics() && InterlockedIncrement(&n) <= 3)
        XSL_WARN(CH, "DeserializeComputePipelineStateX: serialized native pipeline (needs a shader recompiler); its work is skipped");
    return make_simple(DEV(d), Kind::Pso, nullptr, riid, out);
}
HRESULT D3DAPI D_CreateCommandQueueX(ID3D12Device* d, const D3D12XBOX_COMMAND_QUEUE_DESC* xd, REFIID riid, void** out) {
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = xd->Type;
    return D_CreateCommandQueue(d, &qd, riid, out);
}

HRESULT D3DAPI D_CreateCommittedResource(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* rd,
                                         D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    PcObj r = nullptr;
    XSL_TRACE("gfx.cmd", "CreateCommittedResource dim %d %llux%u fmt %d", (int)rd->Dimension, (unsigned long long)rd->Width, rd->Height, (int)rd->Format);
    HRESULT hr = pc::create_committed_resource(DPC(d), hp, hf, rd, st, cv, &r);
    if (FAILED(hr)) return hr;
    if (!out) { pc::release(r); return S_FALSE; }
    return make_resource(DEV(d), r, riid, out, hp->Type == D3D12_HEAP_TYPE_UPLOAD);
}
// The console resource description is the PC one plus DCC options (no PC meaning).
HRESULT D3DAPI D_CreateCommittedResourceX(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12XBOX_RESOURCE_DESC* xd,
                                          D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    if (out) *out = nullptr;
    if (!xd) return E_INVALIDARG;
    D3D12_RESOURCE_DESC rd{};
    // Same fields up to Flags; DccOptions sits where the PC struct has tail padding.
    static_assert(offsetof(D3D12XBOX_RESOURCE_DESC, DccOptions) == offsetof(D3D12_RESOURCE_DESC, Flags) + sizeof(D3D12_RESOURCE_FLAGS),
                  "console desc extends the PC one");
    memcpy(&rd, xd, offsetof(D3D12XBOX_RESOURCE_DESC, DccOptions));
    return D_CreateCommittedResource(d, hp, hf, &rd, st, cv, riid, out);
}
HRESULT D3DAPI D_CreateHeap(ID3D12Device* d, const D3D12_HEAP_DESC* desc, REFIID riid, void** out) {
    PcObj h = nullptr;
    HRESULT hr = pc::create_heap(DPC(d), desc, &h);
    if (FAILED(hr)) return hr;
    return make_heap(DEV(d), h, desc, riid, out);
}
HRESULT D3DAPI D_CreatePlacedResource(ID3D12Device* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st,
                                      const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    if (getenv("XSL_TRACE_VIDEO_PLANES") && rd && (rd->Format == DXGI_FORMAT_R8_UNORM || rd->Format == DXGI_FORMAT_R8G8_UNORM) && rd->Width >= 1024)
        XSL_INFO("gfx", "placed fmt %d %llux%u in heap %p at %llu", (int)rd->Format, (unsigned long long)rd->Width, rd->Height, (void*)heap, (unsigned long long)off);
    PcObj r = nullptr;
    HRESULT hr = pc::create_placed_resource(DPC(d), P(heap), off, rd, st, cv, &r);
    if (FAILED(hr)) return hr;
    return make_resource(DEV(d), r, riid, out);
}
// ---- Console-style payloads of view slots (desc_set_payload) ----
static uint32_t dxgi_class(DXGI_FORMAT f) {   // 0 float/norm, 1 uint, 2 sint
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R10G10B10A2_UINT: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R8_UINT: return 1;
        case DXGI_FORMAT_R32G32B32A32_SINT: case DXGI_FORMAT_R32G32B32_SINT: case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8_SINT: return 2;
        default: return 0;
    }
}
struct BufFmt { unsigned id; const char* name; };
static const BufFmt kBufFmts[] = {
#include "../gpucode/buf_formats.inc"
};
static uint32_t gfx10_buffer_format(DXGI_FORMAT f, uint32_t* bytes) {   // GFX10 unified buffer format id (0: unknown)
    struct M { DXGI_FORMAT f; const char* n; uint32_t b; };
    static const M m[] = {
        {DXGI_FORMAT_R32G32B32A32_FLOAT, "32_32_32_32_FLOAT", 16}, {DXGI_FORMAT_R32G32B32A32_UINT, "32_32_32_32_UINT", 16},
        {DXGI_FORMAT_R32G32B32A32_SINT, "32_32_32_32_SINT", 16}, {DXGI_FORMAT_R32G32B32_FLOAT, "32_32_32_FLOAT", 12},
        {DXGI_FORMAT_R32G32B32_UINT, "32_32_32_UINT", 12}, {DXGI_FORMAT_R16G16B16A16_FLOAT, "16_16_16_16_FLOAT", 8},
        {DXGI_FORMAT_R16G16B16A16_UNORM, "16_16_16_16_UNORM", 8}, {DXGI_FORMAT_R16G16B16A16_UINT, "16_16_16_16_UINT", 8},
        {DXGI_FORMAT_R16G16B16A16_SNORM, "16_16_16_16_SNORM", 8}, {DXGI_FORMAT_R32G32_FLOAT, "32_32_FLOAT", 8},
        {DXGI_FORMAT_R32G32_UINT, "32_32_UINT", 8}, {DXGI_FORMAT_R8G8B8A8_UNORM, "8_8_8_8_UNORM", 4}, {DXGI_FORMAT_R8G8B8A8_UINT, "8_8_8_8_UINT", 4},
        {DXGI_FORMAT_R8G8B8A8_SNORM, "8_8_8_8_SNORM", 4}, {DXGI_FORMAT_R16G16_FLOAT, "16_16_FLOAT", 4}, {DXGI_FORMAT_R16G16_UNORM, "16_16_UNORM", 4},
        {DXGI_FORMAT_R16G16_UINT, "16_16_UINT", 4}, {DXGI_FORMAT_R32_FLOAT, "32_FLOAT", 4}, {DXGI_FORMAT_R32_UINT, "32_UINT", 4},
        {DXGI_FORMAT_R32_SINT, "32_SINT", 4}, {DXGI_FORMAT_R16_FLOAT, "16_FLOAT", 2}, {DXGI_FORMAT_R16_UNORM, "16_UNORM", 2},
        {DXGI_FORMAT_R16_UINT, "16_UINT", 2}, {DXGI_FORMAT_R8_UNORM, "8_UNORM", 1}, {DXGI_FORMAT_R8_UINT, "8_UINT", 1},
        {DXGI_FORMAT_R8G8_UNORM, "8_8_UNORM", 2}, {DXGI_FORMAT_R10G10B10A2_UNORM, "10_10_10_2_UNORM", 4}};
    for (auto& e : m)
        if (e.f == f)
            for (auto& b : kBufFmts)
                if (!strcmp(b.name, e.n)) { if (bytes) *bytes = e.b; return b.id; }
    if (bytes) *bytes = 4;
    return 0;
}
// Buffer views: {base, base[47:32] | stride << 16, records, format << 12}.
static void buffer_payload(PcObj res, DXGI_FORMAT f, uint64_t first, uint32_t count, uint32_t stride, bool raw) {
    uint32_t eb = 4;
    uint32_t fmt = raw || stride ? 20u /* 32_UINT */ : gfx10_buffer_format(f, &eb);
    if (stride) eb = stride;
    uint64_t base = pc::res_gpu_va(res) + first * eb;
    uint32_t w[4] = {(uint32_t)base, (uint32_t)((base >> 32) & 0xffff) | ((stride & 0x3fff) << 16), raw ? count * 4 : count, fmt << 12};
    desc_set_payload(w);
}
static void texture_payload(DXGI_FORMAT f, bool uav) {
    uint32_t w[4] = {0, kDescTexture | (uav ? 1u : 0u) | (dxgi_class(f) << 4), 0, 0};
    desc_set_payload(w);
}
// View creation writes a PC descriptor into a record and publishes it to the console-layout slot.
template <class F> void write_view(uint64_t h, D3D12_DESCRIPTOR_HEAP_TYPE type, F create) {
    DescTag tag;
    if (uint64_t rec = desc_begin_write(h, type, &tag)) {
        create(rec);
        desc_commit(h, type, tag);
    } else {
        create(h);
        if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) desc_staging_store(h);   // staging heap: keep the payload
    }
}
void D3DAPI D_CreateRTV(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* v, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    XSL_TRACE("gfx.cmd", "CreateRTV res %p (pc %p) desc %p -> %llx", (void*)r, P(r), (void*)v, (unsigned long long)h.ptr);
    write_view(h.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, [&](uint64_t ph) { pc::create_rtv(DPC(d), P(r), v, ph); });
}
void D3DAPI D_CreateDSV(ID3D12Device* d, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* v, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    write_view(h.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, [&](uint64_t ph) { pc::create_dsv(DPC(d), P(r), v, ph); });
}
void D3DAPI D_CreateSRV(ID3D12Device* d, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* v, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (r) { D3D12_RESOURCE_DESC rd; pc::res_desc(P(r), &rd); XSL_TRACE("gfx.desc", "SRV handle %llx fmt %d %llux%u viewdesc %p", (unsigned long long)h.ptr, (int)rd.Format, (unsigned long long)rd.Width, rd.Height, (void*)v); }
    if (v && v->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE) {
        uint64_t a = v->RaytracingAccelerationStructure.Location;
        uint32_t w[4] = {(uint32_t)a, (uint32_t)(a >> 32), 0, 0};
        desc_set_payload(w);
    } else if (r) {
        D3D12_RESOURCE_DESC rd;
        pc::res_desc(P_or_pc(r), &rd);
        DXGI_FORMAT f = v && v->Format != DXGI_FORMAT_UNKNOWN ? v->Format : rd.Format;
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            if (v && v->ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
                buffer_payload(P_or_pc(r), f, v->Buffer.FirstElement, v->Buffer.NumElements, v->Buffer.StructureByteStride, (v->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0);
            else buffer_payload(P_or_pc(r), DXGI_FORMAT_UNKNOWN, 0, (uint32_t)(rd.Width / 4), 0, true);
        } else texture_payload(f, false);
    }
    write_view(h.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, [&](uint64_t ph) { pc::create_srv(DPC(d), P_or_pc(r), v, ph); });   // video frames: PC resources
    if (r && is_ours(r)) detile_view_created(P(r));   // CPU-written console texture: convert before the next submission
}
void D3DAPI D_CreateUAV(ID3D12Device* d, ID3D12Resource* r, ID3D12Resource* c, const D3D12_UNORDERED_ACCESS_VIEW_DESC* v, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (r) {
        D3D12_RESOURCE_DESC rd;
        pc::res_desc(P(r), &rd);
        DXGI_FORMAT f = v && v->Format != DXGI_FORMAT_UNKNOWN ? v->Format : rd.Format;
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            if (v && v->ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
                buffer_payload(P(r), f, v->Buffer.FirstElement, v->Buffer.NumElements, v->Buffer.StructureByteStride, (v->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0);
            else buffer_payload(P(r), DXGI_FORMAT_UNKNOWN, 0, (uint32_t)(rd.Width / 4), 0, true);
        } else texture_payload(f, true);
    }
    write_view(h.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, [&](uint64_t ph) { pc::create_uav(DPC(d), P(r), P(c), v, ph); });
}
void D3DAPI D_CreateCBV(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* v, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    // Console CBVs need 16-byte alignment, PC ones 256: unaligned views point at an aligned copy.
    D3D12_CONSTANT_BUFFER_VIEW_DESC x;
    if (v && v->BufferLocation) {
        x = *v;
        x.BufferLocation = gpu_va(DEV(d), v->BufferLocation);
        uint64_t a = x.BufferLocation;   // payload: the title's range (translated code reads it through global memory)
        uint32_t w[4] = {(uint32_t)a, (uint32_t)((a >> 32) & 0xffff), v->SizeInBytes, 20u << 12};
        desc_set_payload(w);
        if (x.BufferLocation & 255) x.BufferLocation = cbv_fix_view(DEV(d), v->BufferLocation, x.BufferLocation, v->SizeInBytes);
        v = &x;
    }
    write_view(h.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, [&](uint64_t ph) { pc::create_cbv(DPC(d), v, ph); });
}
void D3DAPI D_CreateSampler(ID3D12Device* d, const D3D12_SAMPLER_DESC* v, D3D12_CPU_DESCRIPTOR_HANDLE h) { pc::create_sampler(DPC(d), v, h.ptr); }
void D3DAPI D_CopyDescriptorsSimple(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE t) {
    if (!n || desc_copy(n, dst.ptr, src.ptr, t)) return;  // console-layout slots
    // One side in console-layout slots and the other in a PC heap: translate each descriptor
    // (the title steps both handles by its increment).
    uint64_t pd = desc_pc_cpu(dst.ptr, t), ps = desc_pc_cpu(src.ptr, t);
    const uint64_t inc = D_GetDescriptorHandleIncrementSize(d, t);
    const bool views = t == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    if (pd == dst.ptr && ps == src.ptr) {
        pc::copy_descriptors_simple(DPC(d), n, dst.ptr, src.ptr, t);
        if (views) for (UINT k = 0; k < n; k++) desc_payload_copy(dst.ptr + k * inc, src.ptr + k * inc);
        return;
    }
    for (UINT k = 0; k < n; k++) {
        uint64_t a = desc_pc_cpu(dst.ptr + k * inc, t), b = desc_pc_cpu(src.ptr + k * inc, t);
        if (a && b) {
            pc::copy_descriptors_simple(DPC(d), 1, a, b, t);
            if (views && a != dst.ptr + k * inc) desc_publish(dst.ptr + k * inc);
            if (views) desc_payload_copy(dst.ptr + k * inc, src.ptr + k * inc);
        }
    }
}
// The general form, expressed as simple copies: each destination/source range pair is walked in step.
void D3DAPI D_CopyDescriptors(ID3D12Device* d, UINT nDst, const D3D12_CPU_DESCRIPTOR_HANDLE* dst, const UINT* dstSizes, UINT nSrc,
                              const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    const uint64_t inc = D_GetDescriptorHandleIncrementSize(d, type);  // the title's handle spacing
    UINT di = 0, si = 0, dOff = 0, sOff = 0;
    while (di < nDst && si < nSrc) {
        UINT dLeft = (dstSizes ? dstSizes[di] : 1) - dOff, sLeft = (srcSizes ? srcSizes[si] : 1) - sOff;
        UINT n = dLeft < sLeft ? dLeft : sLeft;
        if (n) D_CopyDescriptorsSimple(d, n, {(SIZE_T)(dst[di].ptr + dOff * inc)}, {(SIZE_T)(src[si].ptr + sOff * inc)}, type);
        dOff += n;
        sOff += n;
        if (dOff == (dstSizes ? dstSizes[di] : 1)) { di++; dOff = 0; }
        if (sOff == (srcSizes ? srcSizes[si] : 1)) { si++; sOff = 0; }
    }
}
void D3DAPI D_GetCopyableFootprints(ID3D12Device* d, const D3D12_RESOURCE_DESC* rd, UINT first, UINT num, UINT64 base,
                                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows, UINT64* rowSizes, UINT64* total) {
    pc::get_copyable_footprints(DPC(d), rd, first, num, base, layouts, rows, rowSizes, total);
}
D3D12_RESOURCE_ALLOCATION_INFO D3DAPI D_GetResourceAllocationInfo(ID3D12Device* d, UINT mask, UINT n, const D3D12_RESOURCE_DESC* rd) {
    D3D12_RESOURCE_ALLOCATION_INFO i{};
    pc::get_resource_allocation_info(DPC(d), mask, n, rd, &i);
    return i;
}
HRESULT D3DAPI D_CreateFence(ID3D12Device* d, UINT64 init, D3D12_FENCE_FLAGS f, REFIID riid, void** out) {
    PcObj fence = nullptr;
    HRESULT hr = pc::create_fence(DPC(d), init, f, &fence);
    if (FAILED(hr)) return hr;
    return make_simple(DEV(d), Kind::Fence, fence, riid, out);
}
HRESULT D3DAPI D_GetDeviceRemovedReason(ID3D12Device* d) { return pc::device_removed_reason(DPC(d)); }
// Frame statistics, per frame-pipeline token (QPC times). Origin events come from WaitFrameEventX,
// present times from PresentX, GPU completion from the frame-latency fence (observed on query, so it
// can trail the real completion slightly); display times equal GPU completion (no scan-out feedback).
int64_t qpc_now();
struct FrameRecord {
    uint64_t token = 0;
    int64_t waitCall = 0, signal = 0, waitReturn = 0;
    int64_t presentCall = 0, presentReturn = 0, gpuDone = 0;
    uint64_t fenceValue = 0;
    uint32_t intervalUs = 0, period = 0;
    bool presented = false;
};
constexpr uint32_t kFrameRecords = 64;
std::mutex g_frames_mu;
FrameRecord g_frames[kFrameRecords];
FrameRecord& frame_record(uint64_t token) { return g_frames[token % kFrameRecords]; }

HRESULT D3DAPI D_GetFrameStatisticsX(ID3D12Device* d, D3D12XBOX_FRAME_PIPELINE_TOKEN token, D3D12XBOX_FRAME_STATISTICS_TYPE types,
                                     UINT* count, D3D12XBOX_FRAME_STATISTICS* out) {
    if (!count) return E_INVALIDARG;
    XSL_TRACE("gfx.stats", "GetFrameStatisticsX token %llu types %x count %u", (unsigned long long)token, (unsigned)types, *count);
    DeviceExtra* e = dev_extra(d);
    FrameRecord r;
    {
        std::lock_guard<std::mutex> g(g_frames_mu);
        FrameRecord& fr = frame_record(token);
        if (fr.token != token || !fr.presented || !e->latencyFence) {
            XSL_TRACE("gfx.stats", "token %llu: record %llu presented %d", (unsigned long long)token, (unsigned long long)fr.token, fr.presented);
            *count = 0;
            return S_FALSE;
        }
        if (!fr.gpuDone) { *count = 0; return S_FALSE; }  // set by the completion thread
        r = fr;
    }
    UINT n = 0, cap = *count;
    auto add = [&](D3D12XBOX_FRAME_STATISTICS_TYPE t) -> D3D12XBOX_FRAME_STATISTICS* {
        if (!(types & t) || n >= cap || !out) return nullptr;
        D3D12XBOX_FRAME_STATISTICS* s = &out[n++];
        memset(s, 0, sizeof *s);
        s->Type = t;
        return s;
    };
    if (auto* s = add(D3D12XBOX_FRAME_STATISTICS_TYPE_FRAME_INTERVAL)) {
        s->Interval.LengthInMicroseconds = r.intervalUs;
        s->Interval.PeriodInIntervals = r.period;
    }
    if (auto* s = add(D3D12XBOX_FRAME_STATISTICS_TYPE_FRAME_EVENT)) {
        s->Event.EventType = D3D12XBOX_FRAME_EVENT_ORIGIN;
        s->Event.WaitCallTime = r.waitCall;
        s->Event.SignalTime = r.signal;
        s->Event.WaitReturnTime = r.waitReturn;
    }
    if (auto* s = add(D3D12XBOX_FRAME_STATISTICS_TYPE_RENDER)) {
        s->Render.GPUWriteCompleteTime = r.gpuDone;
        s->Render.GPUBusyDuration = r.gpuDone > r.presentCall ? r.gpuDone - r.presentCall : 0;  // upper bound
    }
    if (auto* s = add(D3D12XBOX_FRAME_STATISTICS_TYPE_PRESENT)) {
        s->Present.PlaneCount = 1;
        s->Present.ViewCount = 1;
        s->Present.PresentCallTime = r.presentCall;
        s->Present.PresentQueueTime = r.presentCall;
        s->Present.PresentReturnTime = r.presentReturn;
        s->Present.GPUProcessTime = r.gpuDone;
        s->Present.PlaneSwapMask = 1;
    }
    if (auto* s = add(D3D12XBOX_FRAME_STATISTICS_TYPE_DISPLAY)) {
        s->Display.VSyncTime = r.gpuDone;
        s->Display.FlipTime = r.gpuDone;
    }
    if (auto* s = add(D3D12XBOX_FRAME_STATISTICS_TYPE_POWERSCALING)) {
        s->PowerScaling.GPUBusyDurationScaled = r.gpuDone > r.presentCall ? r.gpuDone - r.presentCall : 0;
        s->PowerScaling.ScaleFactor = 1.0;
    }
    *count = n;
    return S_OK;
}
D3D12XBOX_DEBUG_FLAGS g_debugFlags = (D3D12XBOX_DEBUG_FLAGS)0;
void D3DAPI D_SetDebugFlagsX(ID3D12Device*, D3D12XBOX_DEBUG_FLAGS f) { g_debugFlags = f; }
D3D12XBOX_DEBUG_FLAGS D3DAPI D_GetDebugFlagsX(ID3D12Device*) { return g_debugFlags; }

// ---- Frame pipeline: SetFrameIntervalX / ScheduleFrameEventX / WaitFrameEventX ------------
// The console raises an "origin" event once per frame interval. Here a QPC-based clock
// produces the same cadence; tokens count frames.
int64_t qpc_now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
int64_t qpc_freq() { static int64_t f = [] { LARGE_INTEGER t; QueryPerformanceFrequency(&t); return t.QuadPart; }(); return f; }

HRESULT D3DAPI D_SetFrameIntervalX(ID3D12Device* d, IDXGIOutput*, UINT lengthUs, UINT period, D3D12XBOX_FRAME_INTERVAL_FLAGS) {
    dev_extra(d)->frameLengthUs = lengthUs ? lengthUs : 16667;
    dev_extra(d)->maxLatency = period ? period : 1;
    XSL_INFO(CH, "frame interval %u us, latency %u frame(s)", dev_extra(d)->frameLengthUs, dev_extra(d)->maxLatency);
    return S_OK;
}
HRESULT D3DAPI D_ScheduleFrameEventX(ID3D12Device* d, D3D12XBOX_FRAME_EVENT_TYPE, UINT, D3D12XBOX_SCHEDULE_FRAME_OBJECT_LIST*,
                                     D3D12XBOX_SCHEDULE_FRAME_EVENT_FLAGS) {
    dev_extra(d)->nextOriginQpc = 0;
    return S_OK;
}
HRESULT D3DAPI D_WaitFrameEventX(ID3D12Device* d, D3D12XBOX_FRAME_EVENT_TYPE, UINT timeoutMs, D3D12XBOX_WAIT_FRAME_OBJECT_LIST*,
                                 D3D12XBOX_WAIT_FRAME_EVENT_FLAGS, D3D12XBOX_FRAME_PIPELINE_TOKEN* token) {
    DeviceExtra* e = dev_extra(d);
    int64_t period = qpc_freq() * e->frameLengthUs / 1000000;
    int64_t now = qpc_now();
    const int64_t waitCall = now;
    if (e->nextOriginQpc == 0 || now - e->nextOriginQpc > 4 * period) e->nextOriginQpc = now;  // (re)start or resync after a stall
    while ((now = qpc_now()) < e->nextOriginQpc) {
        int64_t ms = (e->nextOriginQpc - now) * 1000 / qpc_freq();
        Sleep(ms > 1 ? (DWORD)(ms - 1) : 0);
    }
    int64_t signal = e->nextOriginQpc;
    e->nextOriginQpc += period;
    uint64_t t = ++e->frameToken;
    if (token) *token = t;
    std::lock_guard<std::mutex> g(g_frames_mu);
    FrameRecord& r = frame_record(t);
    r = FrameRecord{};
    r.token = t;
    r.waitCall = waitCall;
    r.signal = signal;
    r.waitReturn = qpc_now();
    r.intervalUs = e->frameLengthUs;
    r.period = e->maxLatency;
    return S_OK;
}

D3D12Device11_Function_Table g_dev_tbl;
std::once_flag g_dev_once;
// Precompiled console state objects (ray-tracing pipelines serialized by the console driver):
// export names, serialized root signatures and console GPU code, no DXIL. Until ray tracing is
// translated (plan step R1), the layer returns an inert state object (step R0): shader
// identifiers are distinct per export, stack sizes are 0, and DispatchRays does nothing, so the
// title's raster and compute work runs. XSL_DUMP_STATE_OBJECTS=<dir> saves the blobs for study.
namespace {
D3D12StateObject_Function_Table g_so_tbl;
D3D12StateObjectProperties_Function_Table g_sop_tbl;
std::once_flag g_so_once;
struct SoExtra {};
struct SopExtra {
    std::mutex lock;
    std::vector<std::pair<std::wstring, std::array<uint8_t, 64>>> ids;
};
// The console ray-tracing pipeline's compute PSO: an inert PSO (no PC pipeline) created on first use.
void D3DAPI SO_GetPipelineStateX(ID3D12StateObject* so, ID3D12PipelineState** out) {
    if (!out) return;
    *out = nullptr;
    auto*& p = *(ID3D12PipelineState**)&hdr(so)->user[1];
    if (!p) make_simple(hdr(so)->device, Kind::Pso, nullptr, __uuidof(ID3D12PipelineState), (void**)&p);
    if (p) { AddRef((IGraphicsUnknown*)p); *out = p; }   // the state object keeps its reference
}
}  // namespace
// ---- Console ray-tracing state (literal translation, gpucode/rtso.h) ----
// Collections keep their serialized blob; pipelines link their collections into one program that is
// translated and compiled on a worker thread (minutes; cached afterwards). DispatchRays runs it
// once it is ready and is skipped until then.
// One collection compiled as a parking program (gpucode::RtSpace::program), shared by every pipeline.
struct RtProg {
    std::atomic<PcObj> pso{nullptr};
    std::atomic<int> state{0};   // 0 building, 1 ready, -1 failed
};
// A pipeline: its collections, a discovery region and the collections its rays were seen to reach.
struct RtPipe {
    std::vector<int> colls;
    int start = -1;                 // collection whose program starts the rays (round 0)
    std::vector<uint64_t> traversal;   // traversal entry addresses (the starting collection's)
    uint32_t region = 0;
    std::mutex mu;
    std::set<int> active;           // collections reached (discovery bitmap), run in rounds >= 1
    uint32_t scans = 0;
    bool skipLogged = false, peekLogged = false;
    std::vector<gpucode::RtLinked::Part> parts;   // diagnostics
};
// Every collection's code in one space (gpucode::RtSpace): identifiers are global code addresses.
std::mutex g_space_mu;
gpucode::RtSpace g_space;
static int space_add(const std::vector<uint8_t>& blob) { std::lock_guard<std::mutex> g(g_space_mu); return g_space.add(blob); }
struct RtState {
    std::vector<uint8_t> blob;                 // a collection's serialized blob
    int coll = -1;                             // its id in g_space
    std::vector<int> colls;                    // a pipeline's collection ids
    std::vector<std::vector<uint8_t>> parts;   // a pipeline's collections (AddToStateObject extends them)
    std::shared_ptr<RtPipe> pipe;
};
std::mutex g_rt_mu;
std::map<const void*, RtState> g_rt;   // console ID3D12StateObject -> state
static const void* state_object_of(void* iface, REFIID riid) {
    if (!iface) return nullptr;
    if (riid == __uuidof(ID3D12StateObjectProperties)) return (const void*)hdr(iface)->user[0];
    return iface;
}
static std::shared_ptr<RtPipe> rt_pipe_of(const void* so) {
    std::lock_guard<std::mutex> g(g_rt_mu);
    auto it = g_rt.find(so);
    return it == g_rt.end() ? nullptr : it->second.pipe;
}
// Builds run on a few worker threads (each compile takes seconds to minutes and much memory),
// smallest program first. XSL_RT_BUILD_THREADS=<n> (default 1, idle priority; at most 4).
static void rt_queue_build(size_t size, std::function<void()> job) {
    static std::mutex mu;
    static std::condition_variable cv;
    static std::multimap<size_t, std::function<void()>> q;
    static bool started = false;
    std::lock_guard<std::mutex> g(mu);
    q.emplace(size, std::move(job));
    if (!started) {
        started = true;
        char b[8];
        unsigned n = GetEnvironmentVariableA("XSL_RT_BUILD_THREADS", b, sizeof b) ? (unsigned)atoi(b) : 1u;
        n = std::max(1u, std::min(n, 12u));
        for (unsigned t = 0; t < n; t++) std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);   // behind the title's own pipeline compiles
            for (;;) {
                std::function<void()> j;
                {
                    std::unique_lock<std::mutex> u(mu);
                    cv.wait(u, [] { return !q.empty(); });
                    j = std::move(q.begin()->second);
                    q.erase(q.begin());
                }
                j();
            }
        }).detach();
    }
    cv.notify_one();
}
// Program of a collection: built once (worker threads, cached on disk), shared by all pipelines.
std::mutex g_prog_mu;
std::map<int, std::shared_ptr<RtProg>> g_progs;
PcObj g_rt_pcdev = nullptr;
// first: programs that start a pipeline's rays (every dispatch of the pipeline waits for them) are
// built before programs found by discovery, each group smallest first.
static std::shared_ptr<RtProg> rt_program(int coll, bool first = false) {
    std::shared_ptr<RtProg> prog;
    {
        std::lock_guard<std::mutex> g(g_prog_mu);
        auto& slot = g_progs[coll];
        if (slot) return slot;
        slot = prog = std::make_shared<RtProg>();
    }
    auto link = std::make_shared<gpucode::RtLinked>();
    std::string err;
    bool ok;
    { std::lock_guard<std::mutex> g(g_space_mu); ok = g_space.program(coll, *link, &err); }
    if (!ok) { XSL_WARN(CH, "ray-tracing collection %d: no program (%s)", coll, err.c_str()); prog->state = -1; return prog; }
    {   // XSL_RT_DEBUG=1: debug counters; XSL_RT_GUARD=<n>: dispatcher steps per invocation (other cache keys)
        static bool dbg = [] { char b[4]; return GetEnvironmentVariableA("XSL_RT_DEBUG", b, 4) && b[0] == '1'; }();
        static unsigned guard = [] { char b[16]; return GetEnvironmentVariableA("XSL_RT_GUARD", b, sizeof b) ? (unsigned)strtoul(b, nullptr, 0) : 0u; }();
        link->stats = dbg;
        link->loop_guard = guard;
    }
    size_t size = link->image.size() * 4;
    XSL_INFO(CH, "ray-tracing collection %d: program rt_%016llx queued (%zu KiB code)", coll,
             (unsigned long long)gpucode::rt_cache_key(*link, "gpucode-35-O3"), size / 1024);
    rt_queue_build((first ? 0 : (size_t)1 << 40) + size, [prog, link, coll, size] {
        DWORD t0 = GetTickCount();
        std::vector<uint8_t> dxil;
        std::string e;
        if (!pc::rt_build_pipeline(*link, dxil, e)) { XSL_WARN(CH, "ray-tracing collection %d not built: %s", coll, e.c_str()); prog->state = -1; return; }
        PcObj pso = pc::rt_create_pso(g_rt_pcdev, dxil);
        if (!pso) { prog->state = -1; return; }
        prog->pso = pso;
        prog->state = 1;
        XSL_INFO(CH, "ray-tracing collection %d ready (%zu KiB code) after %lu s", coll, size / 1024, (unsigned long)((GetTickCount() - t0) / 1000));
    });
    return prog;
}
static void rt_start_pipeline(Device* dev, const void* so, std::vector<std::vector<uint8_t>> blobs) {
    auto pipe = std::make_shared<RtPipe>();
    g_rt_pcdev = dev_extra(dev)->pcDevice;
    for (auto& b : blobs) pipe->colls.push_back(space_add(b));
    {
        std::lock_guard<std::mutex> g(g_space_mu);
        for (int c : pipe->colls)   // rays start in the first collection with a ray-generation shader
            if (c >= 0 && g_space.has_raygen(c)) { pipe->start = c; break; }
        if (pipe->start < 0)
            for (int c : pipe->colls)
                if (c >= 0) { pipe->start = c; break; }
        if (pipe->start < 0) { XSL_WARN(CH, "ray-tracing pipeline (%zu collections): no collection understood", blobs.size()); return; }
        pipe->traversal = g_space.traversal(pipe->start);
    }
    static std::atomic<uint32_t> regions{0};
    pipe->region = std::min<uint32_t>(regions++, 63);
    { std::lock_guard<std::mutex> g(g_rt_mu); g_rt[so].pipe = pipe; g_rt[so].parts = blobs; g_rt[so].colls = pipe->colls; }
    rt_program(pipe->start, true);   // the others once rays are seen to reach them (the driver takes 15 s to 2 min per program the first time)
    XSL_INFO(CH, "ray-tracing pipeline %p: %zu collections, rays start in collection %d", so, blobs.size(), pipe->start);
}
// Collections the pipeline's rays parked on (discovery bitmap), as program candidates.
static void rt_scan(RtPipe& p) {
    const uint8_t* m = pc::rt_discovery(p.region);
    if (!m) return;
    std::vector<uint32_t> units;
    const uint32_t* w = (const uint32_t*)m;
    for (uint32_t k = 0; k < (128u << 10) / 4; k++)
        if (w[k])
            for (uint32_t b = 0; b < 32; b++)
                if (w[k] >> b & 1) units.push_back(k * 32 + b);
    if (units.empty()) return;
    std::vector<int> found;
    {
        std::lock_guard<std::mutex> g(g_space_mu);
        for (uint32_t u : units) found.push_back(g_space.collection_in_slot(u + uint32_t(gpucode::RtSpace::kBase >> gpucode::RtSpace::kSlotShift)));
    }
    std::lock_guard<std::mutex> g(p.mu);
    for (int c : found)
        if (c >= 0 && p.active.insert(c).second) {
            XSL_INFO(CH, "ray-tracing pipeline (region %u): rays reach collection %d", p.region, c);
            rt_program(c);
        }
}
// DispatchRays on translated programs: the runtime table its scheduler reads (layout inferred from
// the code: docs/gpucode.md), round 0 in the starting collection, then XSL_RT_ROUNDS - 1 rounds
// (default 6) over the collections rays were seen to reach. Returns false when nothing ran.
bool rt_dispatch_impl(PcObj list, const void* so, const uint64_t* tables, const D3D12_DISPATCH_RAYS_DESC* d, uint64_t argsVa, PcObj samplerHeap) {
    auto pipe = rt_pipe_of(so);
    std::shared_ptr<RtProg> start = pipe && pipe->start >= 0 ? rt_program(pipe->start) : nullptr;
    if (!pipe || !start || start->state.load() != 1 || (!d && !argsVa)) {
        if (d) pc::note_rays_skipped();   // indirect ones are counted where they are skipped
        if (pipe && start && !pipe->skipLogged) {   // once per pipeline: which program it waits for
            pipe->skipLogged = true;
            XSL_INFO(CH, "ray dispatches of pipeline (region %u) wait for collection %d (program %s)", pipe->region, pipe->start,
                     start->state.load() == 0 ? "building" : "failed");
        }
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 12) {
            std::lock_guard<std::mutex> g(g_rt_mu);
            auto it = g_rt.find(so);
            XSL_INFO(CH, "ray dispatch skipped: state object %p %s, start program %s", so, it == g_rt.end() ? "unknown" : it->second.blob.empty() ? "known" : "is a collection",
                     !start ? "none" : start->state.load() == 0 ? "building" : "failed");
        }
        return false;
    }
    if (d && !d->Width) return true;
    if (pipe->scans++ % 16 == 0) rt_scan(*pipe);
    constexpr uint32_t kStackPerLane = 1024;   // bytes; the scheduler's per-lane stack window
    uint64_t stack = pc::rt_stack_buffer((uint64_t)(kStackPerLane * 32 + 0x200) * gpucode::kRtStackSlots);
    if (!stack) return false;
    pc::RtRaysArgs a{};
    for (int k = 0; k < 4; k++) a.tables[k] = (uint32_t)desc_memory_address(tables[k]);   // global RS: SRV, CBV, sampler, UAV tables
    a.tables[2] = (uint32_t)pc::sampler_memory_address(samplerHeap, tables[2]);
    a.samplerHeap = samplerHeap;
    a.discRegion = pipe->region;
    a.stack = stack;
    a.stackPerLane = kStackPerLane;
    for (int k = 0; k < 3; k++) {
        size_t n = pipe->traversal.size();
        a.traversal[k] = n ? (uint32_t)(pipe->traversal[std::min<size_t>(k, n - 1)] >> 8) : 0;
    }
    a.argsVa = argsVa;
    if (d) {
        a.width = d->Width; a.height = d->Height; a.depth = d->Depth;
        a.raygen = d->RayGenerationShaderRecord.StartAddress;
        a.miss = d->MissShaderTable.StartAddress; a.missStride = (uint32_t)d->MissShaderTable.StrideInBytes;
        a.hit = d->HitGroupTable.StartAddress; a.hitStride = (uint32_t)d->HitGroupTable.StrideInBytes;
        a.callable = d->CallableShaderTable.StartAddress;
    }
    static uint32_t rounds = [] { char b[8]; return GetEnvironmentVariableA("XSL_RT_ROUNDS", b, sizeof b) ? std::max(1u, (uint32_t)atoi(b)) : 6u; }();
    std::vector<pc::RtRun> runs{{start->pso.load(), 0}};
    std::vector<PcObj> others;
    {
        std::lock_guard<std::mutex> g(pipe->mu);
        for (int c : pipe->active) {
            auto pr = rt_program(c);
            if (pr->state.load() == 1) others.push_back(pr->pso.load());
        }
    }
    for (uint32_t r = 1; r < rounds && !others.empty(); r++)
        for (PcObj p : others) runs.push_back({p, r});
    static volatile long n = 0;
    if (InterlockedIncrement(&n) <= 5)
        XSL_INFO(CH, "DispatchRays %s%ux%ux%u: %zu program runs (tables %x %x %x %x)", argsVa ? "(indirect) " : "", a.width, a.height, a.depth, runs.size(),
                 a.tables[0], a.tables[1], a.tables[2], a.tables[3]);
    if (!pipe->peekLogged) {   // diagnostics, once per pipeline: the descriptor words the programs will read
        pipe->peekLogged = true;
        std::string s;
        const char* nm[4] = {"SRV", "CBV", "sampler", "UAV"};
        for (int k = 0; k < 4; k++) {
            char b[200];
            snprintf(b, sizeof b, " | %s handle %llx -> %x:", nm[k], (unsigned long long)tables[k], a.tables[k]);
            s += b;
            for (uint32_t j = 0; j < 6; j++) {
                uint32_t w[8] = {};
                if (!desc_peek(tables[k], j, w)) { s += " (not view memory)"; break; }
                snprintf(b, sizeof b, " [%u] %x %x %x %x tag %x %x %x %x", j, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                s += b;
            }
        }
        XSL_INFO(CH, "ray pipeline (region %u) tables:%s", pipe->region, s.c_str());
    }
    return pc::list_rays(list, runs, a);
}
namespace {
const void* D3DAPI SOP_GetShaderIdentifier(ID3D12StateObjectProperties* self, LPCWSTR name) {
    auto* e = ex<ID3D12StateObjectProperties, SopExtra>(self);
    std::lock_guard<std::mutex> g(e->lock);
    std::wstring n = name ? name : L"";
    for (auto& [k, v] : e->ids)
        if (k == n) return v.data();
    std::array<uint8_t, 64> id{};
    {
        static volatile long logged = 0;   // which pipelines titles ask for which shaders (diagnostics)
        auto pipe = rt_pipe_of((const void*)hdr(self)->user[0]);
        if (InterlockedIncrement(&logged) <= 400) {
            std::string u8(n.begin(), n.end());
            XSL_INFO(CH, "GetShaderIdentifier(%s) on %p (%s)", u8.c_str(), (void*)hdr(self)->user[0],
                     pipe ? (std::to_string(pipe->parts.size()) + " parts").c_str() : "no pipeline");
        }
    }
    {   // a collection's or pipeline's export or hit group: global code addresses >> 8
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> g(g_rt_mu);
            auto it = g_rt.find((const void*)hdr(self)->user[0]);
            if (it != g_rt.end()) {
                if (it->second.coll >= 0) ids.push_back(it->second.coll);
                ids.insert(ids.end(), it->second.colls.begin(), it->second.colls.end());
            }
        }
        std::string u8(n.begin(), n.end());
        uint32_t w[3];
        bool found = false;
        {
            std::lock_guard<std::mutex> g(g_space_mu);
            for (int c : ids)
                if ((found = g_space.identifier(c, u8, w))) break;
        }
        if (found) {
            memcpy(id.data(), w, sizeof w);
            e->ids.push_back({n, id});
            return e->ids.back().second.data();
        }
        static volatile long miss = 0;
        if (!ids.empty() && InterlockedIncrement(&miss) <= 8) XSL_WARN(CH, "GetShaderIdentifier(%s): not an export or hit group of the state object", u8.c_str());
    }
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : n) h = (h ^ uint64_t(c)) * 1099511628211ull;
    memcpy(id.data(), &h, 8);
    id[8] = uint8_t(e->ids.size() + 1);
    e->ids.push_back({n, id});
    return e->ids.back().second.data();   // std::vector growth: entries are reserved below
}
UINT64 D3DAPI SOP_GetShaderStackSize(ID3D12StateObjectProperties*, LPCWSTR) { return 0; }
UINT64 D3DAPI SOP_GetPipelineStackSize(ID3D12StateObjectProperties*) { return 0; }
void D3DAPI SOP_SetPipelineStackSize(ID3D12StateObjectProperties*, UINT64) {}
void D3DAPI SOP_GetInformationX(ID3D12StateObjectProperties*, UINT64 flags, D3D12XBOX_RAYTRACING_INFORMATION* info) {
    if (!info) return;
    memset(info, 0, sizeof *info);
    if (flags == D3D12XBOX_RAYTRACING_INFORMATION_FLAG_GET_EXPORT_TABLE) {
        // No exports are reported until state objects are translated (plan step R1).
        D3D12XBOX_RAYTRACING_INFORMATION_EXPORT_TABLE t{};
        info->GenericData = (ID3DBlob*)make_pc_blob(pc::create_blob(&t, sizeof t));
    }
}
void destroy_props(IGraphicsUnknown* self) { ex<ID3D12StateObjectProperties, SopExtra>((ID3D12StateObjectProperties*)self)->~SopExtra(); }
}  // namespace

HRESULT make_inert_state_object(Device* dev, REFIID riid, void** out) {
    std::call_once(g_so_once, [] {
        xsl_fill_default(&g_so_tbl);
        install_device_child_base(&g_so_tbl);
        g_so_tbl.GetPipelineStateX = SO_GetPipelineStateX;
        xsl_fill_default(&g_sop_tbl);
        install_object_base(&g_sop_tbl);
        g_sop_tbl.GetShaderIdentifier = SOP_GetShaderIdentifier;
        g_sop_tbl.GetShaderStackSize = SOP_GetShaderStackSize;
        g_sop_tbl.GetPipelineStackSize = SOP_GetPipelineStackSize;
        g_sop_tbl.SetPipelineStackSize = SOP_SetPipelineStackSize;
        g_sop_tbl.GetInformationX = SOP_GetInformationX;
    });
    static const IID* so_iids[] = {&__uuidof(ID3D12StateObject), &__uuidof(ID3D12Pageable), &__uuidof(ID3D12DeviceChild),
                                   &__uuidof(ID3D12Object), &__uuidof(IGraphicsUnknown), nullptr};
    static const IID* sop_iids[] = {&__uuidof(ID3D12StateObjectProperties), &__uuidof(IGraphicsUnknown), nullptr};
    auto* so = make<ID3D12StateObject, SoExtra>(Kind::StateObject, &g_so_tbl, nullptr, dev, so_iids, nullptr);
    auto* sop = make<ID3D12StateObjectProperties, SopExtra>(Kind::StateObjectProperties, &g_sop_tbl, nullptr, dev, sop_iids, destroy_props);
    ex<ID3D12StateObjectProperties, SopExtra>(sop)->ids.reserve(65536);
    hdr(so)->user[0] = (uint64_t)sop;
    hdr(sop)->user[0] = (uint64_t)so;
    HRESULT hr = QueryInterface((IGraphicsUnknown*)so, riid, out);
    Release((IGraphicsUnknown*)so);
    return hr;
}

// Acceleration-structure sizes for the inert ray-tracing path: modest fixed sizes, so titles
// allocate buffers they will never fill (builds are no-ops until ray tracing is translated).
// Sizes come from the PC driver (it builds the structures); a console structure is never smaller.
void D3DAPI D_GetRaytracingAccelerationStructurePrebuildInfo(ID3D12Device5* d, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* in,
                                                             D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* info) {
    if (!info) return;
    *info = {};
    if (in && d && !null_graphics()) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS x;
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geo;
        rt_pc_inputs(nullptr, in, &x, &geo);
        pc::rt_prebuild_info(DPC((ID3D12Device*)d), &x, info);
    }
    if (!info->ResultDataMaxSizeInBytes) {   // no PC answer: generous fixed sizes
        info->ResultDataMaxSizeInBytes = 64 * 1024;
        info->ScratchDataSizeInBytes = 64 * 1024;
        info->UpdateScratchDataSizeInBytes = 64 * 1024;
    }
    if (in && in->Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {   // room for the console layout (pc::list_rt_build_blas)
        UINT64 tris = 0;
        for (UINT i = 0; i < in->NumDescs; i++) {
            const D3D12_RAYTRACING_GEOMETRY_DESC* gd = in->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS ? in->ppGeometryDescs[i] : &in->pGeometryDescs[i];
            if (gd && gd->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES)
                tris += (gd->Triangles.IndexFormat != DXGI_FORMAT_UNKNOWN ? gd->Triangles.IndexCount : gd->Triangles.VertexCount) / 3;
        }
        UINT64 need = (pc::blas_console_size(tris) + 255) & ~255ull, scr = (pc::blas_scratch_size(tris) + 255) & ~255ull;
        if (info->ResultDataMaxSizeInBytes < need) info->ResultDataMaxSizeInBytes = need;
        if (info->ScratchDataSizeInBytes < scr) info->ScratchDataSizeInBytes = scr;
        if (info->UpdateScratchDataSizeInBytes < scr) info->UpdateScratchDataSizeInBytes = scr;
    }
    if (in && in->Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {   // room for the console layout (pc::list_rt_build_tlas)
        UINT64 need = (pc::tlas_console_size(in->NumDescs) + 255) & ~255ull, scr = (pc::tlas_scratch_size(in->NumDescs) + 255) & ~255ull;
        if (info->ResultDataMaxSizeInBytes < need) info->ResultDataMaxSizeInBytes = need;
        if (info->ScratchDataSizeInBytes < scr) info->ScratchDataSizeInBytes = scr;
        if (info->UpdateScratchDataSizeInBytes < scr) info->UpdateScratchDataSizeInBytes = scr;
    }
}

void D3DAPI D_GetRaytracingAccelerationStructurePrebuildInfoX(ID3D12Device8* d, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* in,
                                                              const D3D12XBOX_XDXR_STREAM_DESC*, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* info) {
    D_GetRaytracingAccelerationStructurePrebuildInfo((ID3D12Device5*)d, in, info);
}

// DXR libraries compiled to DXIL become console code only when the pipeline is created; until
// that is translated (plan step R1), CreateStateObject also returns an inert state object.
// The serialized blobs of a state-object description's existing collections.
static void collection_blobs(const D3D12_STATE_OBJECT_DESC* desc, std::vector<std::vector<uint8_t>>& blobs) {
    std::lock_guard<std::mutex> g(g_rt_mu);
    for (UINT i = 0; i < desc->NumSubobjects; i++)
        if (desc->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) {
            auto* c = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(desc->pSubobjects[i].pDesc);
            auto it = c ? g_rt.find((const void*)c->pExistingCollection) : g_rt.end();
            if (it != g_rt.end() && !it->second.blob.empty()) blobs.push_back(it->second.blob);
        }
}
// XSL_RT_PIPELINES=0: keep ray-tracing pipelines inert (no translation, DispatchRays does nothing).
static bool gpu_rt_enabled() { static bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_RT_PIPELINES", b, 4) && b[0] == '0'); }(); return on; }
HRESULT D3DAPI D_CreateStateObject(ID3D12Device5* d, const D3D12_STATE_OBJECT_DESC* desc, REFIID riid, void** out) {
    if (out) *out = nullptr;
    char dir[MAX_PATH];
    if (desc && GetEnvironmentVariableA("XSL_DUMP_STATE_OBJECTS", dir, sizeof dir)) {   // subobject types and DXIL libraries
        static volatile long k = 0;
        long so = InterlockedIncrement(&k);
        std::string types;
        for (UINT i = 0; i < desc->NumSubobjects; i++) {
            const D3D12_STATE_SUBOBJECT& sub = desc->pSubobjects[i];
            char t[16];
            snprintf(t, sizeof t, "%d ", (int)sub.Type);
            types += t;
            if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE && so <= 16) {
                auto* g = static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE*>(sub.pDesc);
                std::vector<uint8_t> rb;
                if (g && g->pGlobalRootSignature && root_signature_blob(P(g->pGlobalRootSignature), &rb)) {
                    char path[MAX_PATH + 48];
                    snprintf(path, sizeof path, "%s\\so%02ld_global_rs.bin", dir, so);
                    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                    if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, rb.data(), (DWORD)rb.size(), &w, nullptr); CloseHandle(f); }
                }
            }
            if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY && so <= 16) {
                auto* lib = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(sub.pDesc);
                char path[MAX_PATH + 48];
                snprintf(path, sizeof path, "%s\\so%02ld_lib%03u.cso", dir, so, i);
                HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, lib->DXILLibrary.pShaderBytecode, (DWORD)lib->DXILLibrary.BytecodeLength, &w, nullptr); CloseHandle(f); }
                XSL_INFO(CH, "state object %ld library %u: %zu bytes, %u exports", so, i, lib->DXILLibrary.BytecodeLength, lib->NumExports);
            }
        }
        XSL_INFO(CH, "state object %ld (type %d): subobject types %s", so, (int)desc->Type, types.c_str());
    }
    static volatile long n = 0;
    if (InterlockedIncrement(&n) <= 3)
        XSL_WARN(CH, "CreateStateObject (type %d, %u subobjects): ray tracing not translated yet; inert state object returned",
                 desc ? (int)desc->Type : -1, desc ? desc->NumSubobjects : 0);
    if (!out) return S_FALSE;
    HRESULT hr = make_inert_state_object(DEV(d), riid, out);
    if (SUCCEEDED(hr) && desc && desc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) {
        std::vector<std::vector<uint8_t>> blobs;
        collection_blobs(desc, blobs);
        if (!blobs.empty() && gpu_rt_enabled()) rt_start_pipeline(DEV(d), state_object_of(*out, riid), std::move(blobs));
    }
    return hr;
}
// AddToStateObject: a new pipeline of the existing one's collections plus the added ones.
HRESULT D3DAPI D_AddToStateObject(ID3D12Device7* d, const D3D12_STATE_OBJECT_DESC* desc, ID3D12StateObject* existing, REFIID riid, void** out) {
    if (out) *out = nullptr;
    if (!out) return S_FALSE;
    HRESULT hr = make_inert_state_object(DEV(d), riid, out);
    if (SUCCEEDED(hr) && desc && gpu_rt_enabled()) {
        std::vector<std::vector<uint8_t>> blobs;
        {
            std::lock_guard<std::mutex> g(g_rt_mu);
            auto it = g_rt.find((const void*)existing);
            if (it != g_rt.end()) blobs = it->second.parts;
        }
        size_t base = blobs.size();
        collection_blobs(desc, blobs);
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 8)
            XSL_INFO(CH, "AddToStateObject: %zu existing + %zu added collections", base, blobs.size() - base);
        if (!blobs.empty()) rt_start_pipeline(DEV(d), state_object_of(*out, riid), std::move(blobs));
    }
    return hr;
}

HRESULT D3DAPI D_DeserializeStateObjectX(ID3D12Device8* d, const void* data, UINT size, ID3D12RootSignature*, UINT flags, REFIID riid, void** out) {
    if (out) *out = nullptr;
    // XSL_DUMP_STATE_OBJECTS=<dir>: write each serialized state object there (format study).
    char dir[MAX_PATH];
    if (data && GetEnvironmentVariableA("XSL_DUMP_STATE_OBJECTS", dir, sizeof dir)) {
        static volatile long k = 0;
        long i = InterlockedIncrement(&k);
        if (i <= 4096) {
            char path[MAX_PATH + 32];
            snprintf(path, sizeof path, "%s\\stateobj_%04ld_flags%x.bin", dir, i, flags);
            HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, data, size, &w, nullptr); CloseHandle(f); }
        }
    }
    static volatile long n = 0;
    if (InterlockedIncrement(&n) <= 3)
        XSL_WARN(CH, "DeserializeStateObjectX: console ray-tracing pipeline (%u bytes, flags %x) not translated yet; inert state object returned (ray tracing disabled)", size, flags);
    if (!out) return S_FALSE;
    HRESULT hr = make_inert_state_object(DEV(d), riid, out);
    if (SUCCEEDED(hr) && data && size) {   // a collection: its blob is linked into pipelines later
        std::lock_guard<std::mutex> g(g_rt_mu);
        auto& st = g_rt[state_object_of(*out, riid)];
        st.blob.assign((const uint8_t*)data, (const uint8_t*)data + size);
        st.coll = space_add(st.blob);   // code placed now: identifiers are asked of collections
    }
    return hr;
}

void init_device_table() {
    std::call_once(g_dev_once, [] {
        xsl_fill_default(&g_dev_tbl);
        install_object_base(&g_dev_tbl);
        auto* t = (D3D12Device_Function_Table*)&g_dev_tbl;
        t->GetNodeCount = D_GetNodeCount;
        t->CreateCommandQueue = D_CreateCommandQueue;
        t->CreateCommandAllocator = D_CreateCommandAllocator;
        t->CreateCommandListX = D_CreateCommandListX;
        t->CreateCommandList = D_CreateCommandList;
        t->CheckFeatureSupport = D_CheckFeatureSupport;
        t->CreateDescriptorHeap = D_CreateDescriptorHeap;
        t->GetDescriptorHandleIncrementSize = D_GetDescriptorHandleIncrementSize;
        t->CreateRootSignature = D_CreateRootSignature;
        t->CreateGraphicsPipelineState = D_CreateGraphicsPipelineState;
        t->CreateComputePipelineState = D_CreateComputePipelineState;
        t->CreateComputePipelineStateX = D_CreateComputePipelineStateX;
        t->CreateCommittedResource = D_CreateCommittedResource;
        t->CreateHeap = D_CreateHeap;
        t->CreatePlacedResource = D_CreatePlacedResource;
        t->CreateRenderTargetView = D_CreateRTV;
        t->CreateDepthStencilView = D_CreateDSV;
        t->CreateShaderResourceView = D_CreateSRV;
        t->CreateUnorderedAccessView = D_CreateUAV;
        t->CreateConstantBufferView = D_CreateCBV;
        t->CreateSampler = D_CreateSampler;
        t->CopyDescriptorsSimple = D_CopyDescriptorsSimple;
        t->GetCopyableFootprints = D_GetCopyableFootprints;
        t->GetResourceAllocationInfo = D_GetResourceAllocationInfo;
        t->CreateFence = D_CreateFence;
        t->CreateCommandSignature = D_CreateCommandSignature;
        t->CreateQueryHeap = D_CreateQueryHeap;
        t->DeserializeGraphicsPipelineStateX = D_DeserializeGraphicsPipelineStateX;
        t->DeserializeComputePipelineStateX = D_DeserializeComputePipelineStateX;
        t->CopyDescriptors = D_CopyDescriptors;
        t->CreatePlacedResourceX = D_CreatePlacedResourceX;
        t->CreateCommittedResourceX = D_CreateCommittedResourceX;
        t->RegisterPagePoolX = D_RegisterPagePoolX;
        t->UnregisterPagePoolX = D_UnregisterPagePoolX;
        t->CreateCommandQueueX = D_CreateCommandQueueX;
        ((D3D12Device2_Function_Table*)&g_dev_tbl)->CreatePipelineState = D_CreatePipelineState;
        t->GetDeviceRemovedReason = D_GetDeviceRemovedReason;
        t->SetDebugFlagsX = D_SetDebugFlagsX;
        t->GetFrameStatisticsX = D_GetFrameStatisticsX;
        t->GetDebugFlagsX = D_GetDebugFlagsX;
        t->SetFrameIntervalX = D_SetFrameIntervalX;
        t->ScheduleFrameEventX = D_ScheduleFrameEventX;
        t->WaitFrameEventX = D_WaitFrameEventX;
        ((D3D12Device8_Function_Table*)&g_dev_tbl)->DeserializeStateObjectX = D_DeserializeStateObjectX;
        ((D3D12Device5_Function_Table*)&g_dev_tbl)->CreateStateObject = D_CreateStateObject;
        ((D3D12Device7_Function_Table*)&g_dev_tbl)->AddToStateObject = D_AddToStateObject;
        ((D3D12Device5_Function_Table*)&g_dev_tbl)->GetRaytracingAccelerationStructurePrebuildInfo = D_GetRaytracingAccelerationStructurePrebuildInfo;
        ((D3D12Device8_Function_Table*)&g_dev_tbl)->GetRaytracingAccelerationStructurePrebuildInfoX = D_GetRaytracingAccelerationStructurePrebuildInfoX;
    });
}
}  // namespace

DeviceExtra* dev_extra(const void* device) { return extra_of<DeviceExtra>((void*)device, sizeof(XDevice)); }

// GPU completion times: a waiter thread timestamps each presented frame's latency-fence value as
// it completes, so statistics do not depend on when the title asks for them.
namespace {
std::mutex g_done_mu;
std::condition_variable g_done_cv;
std::deque<std::pair<uint64_t, uint64_t>> g_done_queue;  // (fence value, token)
void completion_thread(PcObj fence) {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (;;) {
        std::pair<uint64_t, uint64_t> item;
        {
            std::unique_lock<std::mutex> g(g_done_mu);
            g_done_cv.wait(g, [] { return !g_done_queue.empty(); });
            item = g_done_queue.front();
            g_done_queue.pop_front();
        }
        if (pc::fence_completed(fence) < item.first) {
            pc::fence_set_event(fence, item.first, ev);
            WaitForSingleObject(ev, 5000);
        }
        int64_t now = qpc_now();
        std::lock_guard<std::mutex> g(g_frames_mu);
        FrameRecord& r = frame_record(item.second);
        if (r.token == item.second && !r.gpuDone) r.gpuDone = now;
    }
}
}  // namespace

void frame_stats_present(Device* dev, uint64_t token, int64_t callQpc, int64_t returnQpc, uint64_t fenceValue) {
    if (!token) return;
    {
        std::lock_guard<std::mutex> g(g_frames_mu);
        FrameRecord& r = frame_record(token);
        if (r.token != token) return;
        r.presentCall = callQpc;
        r.presentReturn = returnQpc;
        r.fenceValue = fenceValue;
        r.presented = true;
    }
    static std::once_flag once;
    PcObj fence = dev_extra(dev)->latencyFence;
    std::call_once(once, [fence] { std::thread(completion_thread, fence).detach(); });
    {
        std::lock_guard<std::mutex> g(g_done_mu);
        g_done_queue.push_back({fenceValue, token});
    }
    g_done_cv.notify_one();
}

HRESULT create_device(const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params, REFIID riid, void** out) {
    std::lock_guard<std::mutex> g(g_device_mu);
    if (params && params->Version != D3D12_SDK_VERSION)
        XSL_WARN(CH, "title built against D3D12.X %x, layer pinned to %x", params->Version, D3D12_SDK_VERSION);
    if (!g_device) {
        init_device_table();
        PcObj pcdev = nullptr;
        HRESULT hr = pc::create_device(&pcdev);
        if (FAILED(hr)) return hr;
        auto* d = make<XDevice, DeviceExtra>(Kind::Device, &g_dev_tbl, pcdev, nullptr, kDeviceIids, nullptr);
        // Force the out-of-line path for inline device methods (titles built without D3DCOMPILE_NO_DEBUG).
        d->m_OutOfLineFlags = ID3D12Device::OutOfLineFlags_Common;
        dev_extra(d)->pcDevice = pcdev;
        // The header object owns the PC device; keep one extra ref for the process lifetime.
        g_device = (Device*)d;
        XSL_INFO(CH, "console device created (D3D12.X %x) on %s", D3D12_SDK_VERSION, pc::adapter_name());
    }
    if (!out) return S_FALSE;
    return QueryInterface((IGraphicsUnknown*)g_device, riid, out);
}

// ============================================================================================
// DXGI: device -> adapter -> output, and the adapter's parent factory. Titles use the output to
// name what frame intervals are locked to; engines also read the adapter's description and walk
// adapter -> factory (Unreal Engine).
// ============================================================================================
namespace {
struct DxgiExtra { IGraphicsUnknown* parent; };
const IID* const kDxgiDeviceIids[] = {&__uuidof(IGraphicsUnknown), &__uuidof(IDXGIObject), &__uuidof(IDXGIDevice), &__uuidof(IDXGIDevice1), &__uuidof(IDXGIDevice2), nullptr};
const IID* const kDxgiAdapterIids[] = {&__uuidof(IGraphicsUnknown), &__uuidof(IDXGIObject), &__uuidof(IDXGIAdapter), &__uuidof(IDXGIAdapter1), &__uuidof(IDXGIAdapter2), nullptr};
const IID* const kDxgiOutputIids[] = {&__uuidof(IGraphicsUnknown), &__uuidof(IDXGIObject), &__uuidof(IDXGIOutput), nullptr};
const IID* const kDxgiFactoryIids[] = {&__uuidof(IGraphicsUnknown), &__uuidof(IDXGIObject), &__uuidof(IDXGIFactory), &__uuidof(IDXGIFactory1), &__uuidof(IDXGIFactory2), nullptr};
DXGIDevice2_Function_Table g_dxgidev_tbl;
DXGIAdapter2_Function_Table g_adapter_tbl;
DXGIOutput_Function_Table g_output_tbl;
DXGIFactory2_Function_Table g_factory_tbl;
std::once_flag g_dxgi_once;
IGraphicsUnknown* g_adapter = nullptr;
IGraphicsUnknown* g_output = nullptr;
IGraphicsUnknown* g_factory = nullptr;

void dxgi_unknown(void* t) {
    auto* u = (GraphicsUnknown_Function_Table*)t;
    u->QueryInterface = QueryInterface;
    u->AddRef = AddRef;
    u->Release = Release;
}
HRESULT D3DAPI Dxgi_GetAdapter(IDXGIDevice*, IDXGIAdapter** out) {
    AddRef(g_adapter);
    *out = (IDXGIAdapter*)g_adapter;
    return S_OK;
}
// The adapter as a console reports it: AMD's vendor ID and the Series X's memory split (10 GB
// GPU-optimal of 16 GB); the LUID is the PC adapter's, which is what it identifies.
template <class D> void fill_adapter_desc(D* d) {
    *d = {};
    wcscpy_s(d->Description, L"AMD Custom GPU (OpenScarlett)");
    d->VendorId = 0x1002;
    d->DedicatedVideoMemory = 10ull << 30;
    d->SharedSystemMemory = 6ull << 30;
    uint64_t luid = pc::adapter_luid();
    d->AdapterLuid.LowPart = (DWORD)luid;
    d->AdapterLuid.HighPart = (LONG)(luid >> 32);
}
HRESULT D3DAPI Dxgi_AdapterGetDesc(IDXGIAdapter*, DXGI_ADAPTER_DESC* d) { if (!d) return E_INVALIDARG; fill_adapter_desc(d); return S_OK; }
HRESULT D3DAPI Dxgi_AdapterGetDesc1(IDXGIAdapter1*, DXGI_ADAPTER_DESC1* d) { if (!d) return E_INVALIDARG; fill_adapter_desc(d); return S_OK; }
HRESULT D3DAPI Dxgi_AdapterGetDesc2(IDXGIAdapter2*, DXGI_ADAPTER_DESC2* d) { if (!d) return E_INVALIDARG; fill_adapter_desc(d); return S_OK; }
HRESULT D3DAPI Dxgi_AdapterGetParent(IDXGIObject*, REFIID riid, void** out) {
    if (!out) return E_INVALIDARG;
    return QueryInterface(g_factory, riid, out);
}
// The DXGI device's and the output's parent: the adapter.
HRESULT D3DAPI Dxgi_DeviceGetParent(IDXGIObject*, REFIID riid, void** out) {
    if (!out) return E_INVALIDARG;
    return QueryInterface(g_adapter, riid, out);
}
HRESULT D3DAPI Dxgi_NoParent(IDXGIObject*, REFIID, void** out) {
    if (out) *out = nullptr;
    return E_NOINTERFACE;
}
HRESULT D3DAPI Dxgi_EnumAdapters(IDXGIFactory*, UINT i, IDXGIAdapter** out) {
    if (!out) return E_INVALIDARG;
    if (i > 0) { *out = nullptr; return DXGI_ERROR_NOT_FOUND; }
    AddRef(g_adapter);
    *out = (IDXGIAdapter*)g_adapter;
    return S_OK;
}
HRESULT D3DAPI Dxgi_EnumAdapters1(IDXGIFactory1*, UINT i, IDXGIAdapter1** out) { return Dxgi_EnumAdapters(nullptr, i, (IDXGIAdapter**)out); }
BOOL D3DAPI Dxgi_IsCurrent(IDXGIFactory1*) { return TRUE; }
HRESULT D3DAPI Dxgi_MakeWindowAssociation(IDXGIFactory*, HWND, UINT) { return S_OK; }
HRESULT D3DAPI Dxgi_EnumOutputs(IDXGIAdapter*, UINT i, IDXGIOutput** out) {
    if (i > 0) { *out = nullptr; return DXGI_ERROR_NOT_FOUND; }
    AddRef(g_output);
    *out = (IDXGIOutput*)g_output;
    return S_OK;
}
void init_dxgi() {
    std::call_once(g_dxgi_once, [] {
        xsl_fill_default(&g_dxgidev_tbl);
        dxgi_unknown(&g_dxgidev_tbl);
        ((DXGIDevice_Function_Table*)&g_dxgidev_tbl)->GetAdapter = Dxgi_GetAdapter;
        ((DXGIObject_Function_Table*)&g_dxgidev_tbl)->GetParent = Dxgi_DeviceGetParent;
        xsl_fill_default(&g_adapter_tbl);
        dxgi_unknown(&g_adapter_tbl);
        ((DXGIObject_Function_Table*)&g_adapter_tbl)->GetParent = Dxgi_AdapterGetParent;
        ((DXGIAdapter_Function_Table*)&g_adapter_tbl)->EnumOutputs = Dxgi_EnumOutputs;
        ((DXGIAdapter_Function_Table*)&g_adapter_tbl)->GetDesc = Dxgi_AdapterGetDesc;
        ((DXGIAdapter1_Function_Table*)&g_adapter_tbl)->GetDesc1 = Dxgi_AdapterGetDesc1;
        g_adapter_tbl.GetDesc2 = Dxgi_AdapterGetDesc2;
        xsl_fill_default(&g_output_tbl);
        dxgi_unknown(&g_output_tbl);
        ((DXGIObject_Function_Table*)&g_output_tbl)->GetParent = Dxgi_DeviceGetParent;  // the adapter, as for the device
        xsl_fill_default(&g_factory_tbl);
        dxgi_unknown(&g_factory_tbl);
        ((DXGIObject_Function_Table*)&g_factory_tbl)->GetParent = Dxgi_NoParent;
        ((DXGIFactory_Function_Table*)&g_factory_tbl)->EnumAdapters = Dxgi_EnumAdapters;
        ((DXGIFactory_Function_Table*)&g_factory_tbl)->MakeWindowAssociation = Dxgi_MakeWindowAssociation;
        ((DXGIFactory1_Function_Table*)&g_factory_tbl)->EnumAdapters1 = Dxgi_EnumAdapters1;
        ((DXGIFactory1_Function_Table*)&g_factory_tbl)->IsCurrent = Dxgi_IsCurrent;
        g_factory = make<IDXGIFactory2, DxgiExtra>(Kind::DxgiFactory, &g_factory_tbl, nullptr, nullptr, kDxgiFactoryIids, nullptr);
        g_adapter = make<IDXGIAdapter2, DxgiExtra>(Kind::DxgiAdapter, &g_adapter_tbl, nullptr, nullptr, kDxgiAdapterIids, nullptr);
        g_output = make<IDXGIOutput, DxgiExtra>(Kind::DxgiOutput, &g_output_tbl, nullptr, nullptr, kDxgiOutputIids, nullptr);
    });
}
}  // namespace

bool rt_dispatch(PcObj list, const void* so, const uint64_t* tables, const D3D12_DISPATCH_RAYS_DESC* d, uint64_t argsVa, PcObj samplerHeap) {
    return rt_dispatch_impl(list, so, tables, d, argsVa, samplerHeap);
}

HRESULT create_dxgi_device(Device* dev, void** out) {
    init_dxgi();
    DeviceExtra* e = dev_extra(dev);
    if (!e->dxgiDevice) e->dxgiDevice = make<IDXGIDevice2, DxgiExtra>(Kind::DxgiDevice, &g_dxgidev_tbl, nullptr, dev, kDxgiDeviceIids, nullptr);
    AddRef(e->dxgiDevice);
    *out = e->dxgiDevice;
    return S_OK;
}

}  // namespace xs

// ============================================================================================
// DLL exports
// ============================================================================================
extern "C" HRESULT D3DAPI D3D12XboxCreateDevice(IGraphicsUnknown*, const D3D12XBOX_CREATE_DEVICE_PARAMETERS* p, REFIID riid, void** out) {
    xsl_fixed_time_install();
    return xs::create_device(p, riid, out);
}
extern "C" void D3DAPI D3D12XboxUnloadDriverModule() {}
