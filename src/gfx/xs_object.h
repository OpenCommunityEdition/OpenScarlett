// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Xbox half of d3d12_xs.dll: the console object model.
//
// Console Direct3D objects are plain structs whose first field is a pointer
// to a function table (d3d12_xs.h). Title code calls the inline methods in
// the header, which either write GPU packets themselves (when the runtime has
// handed out command-buffer space) or call through the table. This layer
// never hands out command-buffer space: every m_Putter limit stays null, so
// every recording call reaches the table (verified, docs/provenance.md).
//
// Each object is allocated as [ObjHeader][SDK interface struct][kind extra].
// The pointer handed to the title is the interface struct; the header sits
// immediately before it.
#pragma once
#include <vector>
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <d3d12_xs.h>

#include <atomic>
#include <cstdint>
#include <new>

#include "bridge.h"
#include "xs_tables.h"  // generated
#include "xs_abi.h"
#include "xsl/log.h"

#define CH "gfx"

namespace xs {

enum class Kind : uint32_t {
    Device = 1, Queue, Allocator, List, Fence, Resource, Heap, DescriptorHeap, RootSignature, Pso,
    DxgiDevice, DxgiAdapter, DxgiOutput, QueryHeap, CommandSignature, DxgiFactory,
    StateObject, StateObjectProperties,
};

struct Device;

struct alignas(16) ObjHeader {
    uint32_t magic;          // 'XSLO'
    Kind kind;
    PcObj pc;                // the PC object this console object fronts (may be null)
    Device* device;          // owning device (null for the device itself)
    void (*destroy)(IGraphicsUnknown* self);
    const IID* const* iids;  // null-terminated list of IIDs this object answers to
    uint64_t user[2];
};
static_assert(sizeof(ObjHeader) == 64);
constexpr uint32_t kMagic = 0x4F4C5358;

inline ObjHeader* hdr(const void* iface) { return (ObjHeader*)((char*)iface - sizeof(ObjHeader)); }
inline bool is_ours(const void* iface) { return iface && hdr(iface)->magic == kMagic; }
inline PcObj pc_of(const void* iface) { return iface ? hdr(iface)->pc : nullptr; }

template <class Extra> Extra* extra_of(void* iface, size_t ifaceSize) { return (Extra*)((char*)iface + ifaceSize); }
template <class I, class Extra> Extra* ex(I* iface) { return extra_of<Extra>(iface, sizeof(I)); }

// Allocate an object: zeroed header + interface + Extra, function table installed, refcount 1.
XSL_ABI_TABLE(D3D12Device11)
XSL_ABI_TABLE(D3D12CommandQueue)
XSL_ABI_TABLE(D3D12GraphicsCommandList8)
XSL_ABI_TABLE(D3D12CommandAllocator)
XSL_ABI_TABLE(D3D12DescriptorHeap)
XSL_ABI_TABLE(D3D12Resource2)
XSL_ABI_TABLE(D3D12Fence1)
XSL_ABI_TABLE(D3D12RootSignature)
XSL_ABI_TABLE(D3D12PipelineState)
XSL_ABI_TABLE(D3D12Heap1)
XSL_ABI_TABLE(D3D12CommandSignature)
XSL_ABI_TABLE(D3D12QueryHeap)
XSL_ABI_TABLE(D3D12StateObject)
XSL_ABI_TABLE(D3D12StateObjectProperties)

// `table` is the pinned SDK's layout; titles of older GDK editions get it rearranged (xs_abi.h).
template <class I, class Extra, class Table>
I* make(Kind kind, Table* table, PcObj pc, Device* dev, const IID* const* iids, void (*destroy)(IGraphicsUnknown*)) {
    size_t size = sizeof(ObjHeader) + sizeof(I) + sizeof(Extra);
    char* mem = (char*)_aligned_malloc(size, 16);
    memset(mem, 0, size);
    auto* h = (ObjHeader*)mem;
    h->magic = kMagic;
    h->kind = kind;
    h->pc = pc;
    h->device = dev;
    h->destroy = destroy;
    h->iids = iids;
    I* obj = (I*)(mem + sizeof(ObjHeader));
    obj->m_pFunction = (GraphicsUnknown_Function_Table*)abi::select((void*)table, abi::TableName<Table>::value, sizeof(Table));
    obj->m_RefCount = 1;
    new (extra_of<Extra>(obj, sizeof(I))) Extra();
    return obj;
}

// Shared IGraphicsUnknown / ID3D12Object / ID3D12DeviceChild implementations.
HRESULT D3DAPI QueryInterface(IGraphicsUnknown* self, REFIID riid, void** out);
ULONG D3DAPI AddRef(IGraphicsUnknown* self);
ULONG D3DAPI Release(IGraphicsUnknown* self);
void free_object(IGraphicsUnknown* self);
void install_object_base(void* table);       // Unknown + Object entries
void install_device_child_base(void* table);  // + GetDevice

const char* iid_name(REFIID riid);

// Logs an object the layer did not create: its vtable's module and the caller (xs_objects.cpp).
void report_foreign(void* obj, void* caller, const char* fn);
ID3D10Blob* make_pc_blob(PcObj pc);

// Translate a console object pointer to its PC object (null-safe, checks ownership).
template <class T> PcObj P(T* o, const char* fn = __builtin_FUNCTION()) {
    if (!o) return nullptr;
    if (!is_ours(o)) { report_foreign((void*)o, __builtin_return_address(0), fn); return nullptr; }
    return hdr(o)->pc;
}

// Objects PC components may hand the title's queues (e.g. Media Foundation's fences through
// IMFD3D12SynchronizationObjectCommands): a layer object becomes its PC object, anything else is
// already a PC object.
// A PC object recorded into a list stays alive until the GPU is done with it (keep_foreign holds a
// reference for a few frames): the component that made it may release it right after recording.
void keep_foreign(void* pcObject);
template <class T> PcObj P_or_pc(T* o) {
    if (!o) return nullptr;
    if (is_ours(o)) return hdr(o)->pc;
    keep_foreign((void*)o);
    return (PcObj)o;
}

// ---- Device ----------------------------------------------------------------------------
using XDevice = ID3D12Device11;
struct DeviceExtra {
    PcObj pcDevice;
    PcObj pcQueue;  // first direct queue, used for presentation
    HWND window;
    uint32_t frameLengthUs = 16667;
    uint64_t frameToken = 0;
    int64_t nextOriginQpc = 0;
    IGraphicsUnknown* dxgiDevice = nullptr;
    // Frame latency: the console lets the GPU trail the CPU by at most `maxLatency` presented
    // frames (SetFrameIntervalX's period argument; titles rely on it before reusing allocators).
    uint32_t maxLatency = 1;
    PcObj latencyFence = nullptr;
    uint64_t presentCount = 0;
    HANDLE latencyEvent = nullptr;
    // PresentX on a compute or copy queue: the swap chain lives on a layer-owned direct queue
    // (never one of the title's: an inserted wait there could deadlock against the title's own
    // cross-queue fences), which waits for the title's queue through this fence.
    PcObj presentQueue = nullptr;
    PcObj presentSyncFence = nullptr;
    uint64_t presentSyncValue = 0;
};
struct Device : XDevice {};
DeviceExtra* dev_extra(const void* device);

HRESULT create_device(const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params, REFIID riid, void** out);
HRESULT create_dxgi_device(Device* dev, void** out);

// Factories for device children (implemented in xs_objects.cpp).
HRESULT make_queue(Device* dev, PcObj pcq, const D3D12_COMMAND_QUEUE_DESC* d, REFIID riid, void** out);
HRESULT make_allocator(Device* dev, PcObj pca, D3D12_COMMAND_LIST_TYPE t, REFIID riid, void** out);
HRESULT make_list(Device* dev, PcObj pcl, D3D12_COMMAND_LIST_TYPE t, REFIID riid, void** out);
HRESULT make_simple(Device* dev, Kind kind, PcObj pc, REFIID riid, void** out);
HRESULT make_heap(Device* dev, PcObj pc, const D3D12_HEAP_DESC* desc, REFIID riid, void** out);
HRESULT make_resource(Device* dev, PcObj pcr, REFIID riid, void** out, bool upload = false);
HRESULT make_descriptor_heap(Device* dev, PcObj pch, const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID riid, void** out);
// A title CBV/SRV/UAV heap living in the layer's global shader-visible heap (pc::global_view_alloc).
HRESULT make_descriptor_heap_range(Device* dev, const D3D12_DESCRIPTOR_HEAP_DESC* d, uint32_t offset, uint64_t cpu, uint64_t gpu,
                                   REFIID riid, void** out);
void list_bind_global_heap(void* list, PcObj pcList);
// Frame statistics (xs_device.cpp): PresentX reports each token's present times and latency-fence value.
void frame_stats_present(Device* dev, uint64_t token, int64_t callQpc, int64_t returnQpc, uint64_t fenceValue);
// Null graphics (XSL_NULL_GRAPHICS=1, xsl-run --backend null): objects and memory are real, but no
// shader is translated and no GPU work runs; PresentX only paces. For running a title's frame loop
// when its graphics cannot be translated yet.
inline bool null_graphics() {
    static bool on = [] { char b[4]; return GetEnvironmentVariableA("XSL_NULL_GRAPHICS", b, 4) && b[0] == '1'; }();
    return on;
}

// Console-layout descriptor memory and its decoder (xs_descriptors.cpp).
struct DescTag { uint32_t magic, type, record, gen; };
bool desc_shadow_enabled();
uint64_t desc_view_cpu_start(Device* dev, uint32_t offset);  // 0: shadowing unavailable
uint64_t desc_view_gpu_start(uint32_t offset);
uint64_t desc_rtv_dsv_heap(Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count);
void desc_rtv_dsv_heap_free(uint64_t base);
uint32_t desc_increment(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t pcInc);
uint64_t desc_begin_write(uint64_t h, D3D12_DESCRIPTOR_HEAP_TYPE type, DescTag* tag);  // PC handle to write, or 0
void desc_commit(uint64_t h, D3D12_DESCRIPTOR_HEAP_TYPE type, const DescTag& tag);
bool desc_copy(uint32_t n, uint64_t dst, uint64_t src, D3D12_DESCRIPTOR_HEAP_TYPE type);
uint64_t desc_pc_cpu(uint64_t h, D3D12_DESCRIPTOR_HEAP_TYPE type);
void desc_publish(uint64_t h);   // after writing a PC descriptor to desc_pc_cpu(h) of a view slot
uint64_t desc_pc_gpu(uint64_t h);
void desc_sync();
HRESULT make_rtv_dsv_shadow_heap(Device* dev, PcObj pch, const D3D12_DESCRIPTOR_HEAP_DESC* d, uint64_t shadow, REFIID riid, void** out);

// GPU page mapping (xs_pagemap.cpp).
void resource_set_placed(void* resource, uint64_t va);  // CreatePlacedResourceX on reserved address space
void pagemap_forget(uint64_t va, PcObj res);
HRESULT D3DAPI D_RegisterPagePoolX(ID3D12Device* d, D3D12_GPU_VIRTUAL_ADDRESS va, UINT pageCount, HANDLE* out);
HRESULT D3DAPI D_UnregisterPagePoolX(ID3D12Device* d, HANDLE h);
HRESULT D3DAPI D_CreatePlacedResourceX(ID3D12Device* d, D3D12_GPU_VIRTUAL_ADDRESS va, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st,
                                       const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out);
HRESULT D3DAPI Q_CopyPageMappingsBatchX(ID3D12CommandQueue* q, UINT batchCount, const D3D12XBOX_PAGE_MAPPING_BATCH* batches,
                                        D3D12_GPU_VIRTUAL_ADDRESS poolVa, UINT poolPages, D3D12XBOX_PAGE_MAPPING_FLAGS flags);
HRESULT D3DAPI Q_CopyPageMappingsX(ID3D12CommandQueue* q, D3D12_GPU_VIRTUAL_ADDRESS dest, UINT rangeCount, const D3D12XBOX_PAGE_MAPPING_RANGE* ranges,
                                   D3D12_GPU_VIRTUAL_ADDRESS poolVa, UINT poolPages, D3D12XBOX_PAGE_MAPPING_FLAGS flags);
HRESULT D3DAPI Q_ClearPageMappingsX(ID3D12CommandQueue* q, D3D12_GPU_VIRTUAL_ADDRESS dest, UINT pageCount, D3D12XBOX_PAGE_MAPPING_FLAGS flags);

HWND find_title_window();

// Ray-tracing build inputs in PC form: console-only flags cleared, pointer-array geometry layouts
// turned into arrays (kept in `geo`), addresses translated when `dev` is given.
void rt_pc_inputs(Device* dev, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* in,
                  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* out, std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>* geo);
}  // namespace xs
