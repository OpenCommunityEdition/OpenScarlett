// SPDX-License-Identifier: AGPL-3.0-or-later
// ID3D12Device (console), D3D12XboxCreateDevice, frame events and the DXGI shims.
#include <condition_variable>
#include <deque>
#include <thread>
#include <mutex>
#include <vector>

#include "xs_object.h"

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
    return hr;
}
HRESULT D3DAPI D_CreateDescriptorHeap(ID3D12Device* d, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out) {
    XSL_TRACE("gfx.cmd", "CreateDescriptorHeap type %d n %u", (int)desc->Type, desc->NumDescriptors);
    static bool unified = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_UNIFIED_DESCRIPTORS", b, 4) && b[0] == '0'); }();
    if (unified && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        uint32_t off;
        uint64_t cpu, gpu;
        if (pc::global_view_alloc(DPC(d), desc->NumDescriptors, &off, &cpu, &gpu)) {
            // Console layout: CPU handles in the layer's descriptor memory (see xs_descriptors.cpp).
            if (uint64_t shadow = desc_view_cpu_start(DEV(d), off)) { cpu = shadow; gpu = desc_view_gpu_start(off); }
            return make_descriptor_heap_range(DEV(d), desc, off, cpu, gpu, riid, out);
        }
        XSL_WARN(CH, "global descriptor heap full; %u-entry heap gets its own PC heap (raise XSL_DESCRIPTOR_HEAP_SIZE)", desc->NumDescriptors);
    }
    PcObj h = nullptr;
    HRESULT hr = pc::create_descriptor_heap(DPC(d), desc, &h);
    if (FAILED(hr)) return hr;
    if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
        if (uint64_t shadow = desc_rtv_dsv_heap(DEV(d), desc->Type, desc->NumDescriptors))
            return make_rtv_dsv_shadow_heap(DEV(d), h, desc, shadow, riid, out);
    return make_descriptor_heap(DEV(d), h, desc, riid, out);
}
UINT D3DAPI D_GetDescriptorHandleIncrementSize(ID3D12Device* d, D3D12_DESCRIPTOR_HEAP_TYPE t) {
    return desc_increment(t, pc::descriptor_increment(DPC(d), t));
}
HRESULT D3DAPI D_CreateRootSignature(ID3D12Device* d, UINT, const void* blob, SIZE_T n, REFIID riid, void** out) {
    XSL_TRACE("gfx.cmd", "CreateRootSignature %zu bytes", (size_t)n);
    PcObj rs = nullptr;
    HRESULT hr = pc::create_root_signature(DPC(d), blob, n, &rs);
    if (FAILED(hr)) return hr;
    XSL_TRACE("gfx.cmd", "CreateRootSignature -> %p", rs);
    return make_simple(DEV(d), Kind::RootSignature, rs, riid, out);
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
    HRESULT hr = pc::create_graphics_pso(DPC(d), &x, &pso);
    if (FAILED(hr)) return hr;
    return make_simple(DEV(d), Kind::Pso, pso, riid, out);
}
HRESULT D3DAPI D_CreateComputePipelineStateX(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, UINT, const D3D12XBOX_EXTENDED_COMPUTE_PIPELINE_STATE_DESC*, REFIID riid,
                                             void** out) {
    if (null_graphics()) return make_simple(DEV(d), Kind::Pso, nullptr, riid, out);
    D3D12_COMPUTE_PIPELINE_STATE_DESC x = *desc;
    x.pRootSignature = (ID3D12RootSignature*)P(desc->pRootSignature);
    PcObj pso = nullptr;
    HRESULT hr = pc::create_compute_pso(DPC(d), &x, &pso);
    if (FAILED(hr)) return hr;
    return make_simple(DEV(d), Kind::Pso, pso, riid, out);
}
HRESULT D3DAPI D_CreateComputePipelineState(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    return D_CreateComputePipelineStateX(d, desc, 0, nullptr, riid, out);
}
// Pipeline-state streams: console subobject numbering and contents -> PC.
HRESULT D3DAPI D_CreatePipelineState(ID3D12Device2* d, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out) {
    if (null_graphics()) return make_simple(DEV(d), Kind::Pso, nullptr, riid, out);
    XSL_TRACE("gfx.cmd", "CreatePipelineState stream %zu bytes", (size_t)desc->SizeInBytes);
    auto align = [](size_t v, size_t a) { return (v + a - 1) & ~(a - 1); };
    const uint8_t* in = (const uint8_t*)desc->pPipelineStateSubobjectStream;
    std::vector<uint8_t> pcs;
    std::vector<void*> owned;  // sanitized shader containers, freed after creation
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
            PcObj prs = P(rs);
            emit(0, &prs, sizeof prs);
        } else if (type <= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS ||
                   type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
            D3D12_SHADER_BYTECODE bc;
            memcpy(&bc, p, sizeof bc);
            bool mesh = type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
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
    PcObj pso = nullptr;
    HRESULT hr = pc::create_pso_stream(DPC(d), pcs.data(), pcs.size(), &pso);
    for (void* o : owned) pc::free_container(o);
    if (FAILED(hr)) return hr;
    return make_simple(DEV(d), Kind::Pso, pso, riid, out);
}
HRESULT D3DAPI D_CreateCommandSignature(ID3D12Device* d, const D3D12_COMMAND_SIGNATURE_DESC* desc, ID3D12RootSignature* rs, REFIID riid, void** out) {
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
HRESULT D3DAPI D_CreateHeap(ID3D12Device* d, const D3D12_HEAP_DESC* desc, REFIID riid, void** out) {
    PcObj h = nullptr;
    HRESULT hr = pc::create_heap(DPC(d), desc, &h);
    if (FAILED(hr)) return hr;
    return make_simple(DEV(d), Kind::Heap, h, riid, out);
}
HRESULT D3DAPI D_CreatePlacedResource(ID3D12Device* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st,
                                      const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    PcObj r = nullptr;
    HRESULT hr = pc::create_placed_resource(DPC(d), P(heap), off, rd, st, cv, &r);
    if (FAILED(hr)) return hr;
    return make_resource(DEV(d), r, riid, out);
}
// View creation writes a PC descriptor into a record and publishes it to the console-layout slot.
template <class F> void write_view(uint64_t h, D3D12_DESCRIPTOR_HEAP_TYPE type, F create) {
    DescTag tag;
    if (uint64_t rec = desc_begin_write(h, type, &tag)) {
        create(rec);
        desc_commit(h, type, tag);
    } else {
        create(h);
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
    write_view(h.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, [&](uint64_t ph) { pc::create_srv(DPC(d), P(r), v, ph); });
}
void D3DAPI D_CreateUAV(ID3D12Device* d, ID3D12Resource* r, ID3D12Resource* c, const D3D12_UNORDERED_ACCESS_VIEW_DESC* v, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    write_view(h.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, [&](uint64_t ph) { pc::create_uav(DPC(d), P(r), P(c), v, ph); });
}
void D3DAPI D_CreateCBV(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* v, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    write_view(h.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, [&](uint64_t ph) { pc::create_cbv(DPC(d), v, ph); });
}
void D3DAPI D_CreateSampler(ID3D12Device* d, const D3D12_SAMPLER_DESC* v, D3D12_CPU_DESCRIPTOR_HANDLE h) { pc::create_sampler(DPC(d), v, h.ptr); }
void D3DAPI D_CopyDescriptorsSimple(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE t) {
    if (!n || desc_copy(n, dst.ptr, src.ptr, t)) return;  // console-layout slots
    pc::copy_descriptors_simple(DPC(d), n, dst.ptr, src.ptr, t);
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
HRESULT D3DAPI D_GetDeviceRemovedReason(ID3D12Device*) { return S_OK; }
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
// DXGI: device -> adapter -> output. Titles use these only to name the output that frame
// intervals are locked to.
// ============================================================================================
namespace {
struct DxgiExtra { IGraphicsUnknown* parent; };
const IID* const kDxgiDeviceIids[] = {&__uuidof(IGraphicsUnknown), &__uuidof(IDXGIObject), &__uuidof(IDXGIDevice), &__uuidof(IDXGIDevice1), &__uuidof(IDXGIDevice2), nullptr};
const IID* const kDxgiAdapterIids[] = {&__uuidof(IGraphicsUnknown), &__uuidof(IDXGIObject), &__uuidof(IDXGIAdapter), &__uuidof(IDXGIAdapter1), &__uuidof(IDXGIAdapter2), nullptr};
const IID* const kDxgiOutputIids[] = {&__uuidof(IGraphicsUnknown), &__uuidof(IDXGIObject), &__uuidof(IDXGIOutput), nullptr};
DXGIDevice2_Function_Table g_dxgidev_tbl;
DXGIAdapter2_Function_Table g_adapter_tbl;
DXGIOutput_Function_Table g_output_tbl;
std::once_flag g_dxgi_once;
IGraphicsUnknown* g_adapter = nullptr;
IGraphicsUnknown* g_output = nullptr;

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
        xsl_fill_default(&g_adapter_tbl);
        dxgi_unknown(&g_adapter_tbl);
        ((DXGIAdapter_Function_Table*)&g_adapter_tbl)->EnumOutputs = Dxgi_EnumOutputs;
        xsl_fill_default(&g_output_tbl);
        dxgi_unknown(&g_output_tbl);
        g_adapter = make<IDXGIAdapter2, DxgiExtra>(Kind::DxgiAdapter, &g_adapter_tbl, nullptr, nullptr, kDxgiAdapterIids, nullptr);
        g_output = make<IDXGIOutput, DxgiExtra>(Kind::DxgiOutput, &g_output_tbl, nullptr, nullptr, kDxgiOutputIids, nullptr);
    });
}
}  // namespace

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
    return xs::create_device(p, riid, out);
}
extern "C" void D3DAPI D3D12XboxUnloadDriverModule() {}
