// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Neutral interface between the two halves of d3d12_xs.dll.
//
// The Xbox half (xs_*.cpp) includes the SDK's d3d12_xs.h; the PC half
// (pc_*.cpp) includes the Windows SDK's d3d12.h. The two headers define the
// same names differently, so they never meet in one translation unit. Every
// PC object crosses this boundary as an opaque void*. Structs whose layout is
// identical in both headers (checked by tools/abi/probe_*.cpp)
// cross as const void*; any Xbox object pointer inside such a struct is
// replaced by the matching PC pointer on the Xbox side before the call.
#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>

typedef long HRESULT_T;
typedef void* PcObj;

namespace pc {

// Device and adapter
HRESULT_T create_device(PcObj* outDevice);
const char* adapter_name();
uint64_t adapter_luid();  // the PC adapter's LUID (HighPart << 32 | LowPart)
// Host capability probe. Filled at device creation.
struct Caps { bool meshShaders; bool raytracing; bool vrs; bool samplerFeedback; int featureLevel; unsigned waveMin, waveMax; };
const Caps& caps();
// PC wave widths and shader model 6.6 support (shader translation picks the lane mapping).
void set_host_waves(unsigned min_lanes, unsigned max_lanes, bool sm66);
// PC stand-in for a console format that PC GPUs cannot render to (0 = none needed).
int format_stand_in(int format);
void release(PcObj o);
void addref(PcObj o);
// Console descriptor model: every CBV/SRV/UAV descriptor heap is GPU-visible and descriptor tables may
// point into any of them. The layer keeps one shader-visible PC heap and gives each title heap a range.
PcObj global_view_heap(PcObj dev);                 // created on first use; null if creation failed
PcObj global_view_heap_if_created();
bool global_view_alloc(PcObj dev, uint32_t count, uint32_t* offset, uint64_t* cpu, uint64_t* gpu);
void global_view_free(uint32_t offset, uint32_t count);
void add_ref(PcObj o);
void set_name(PcObj o, const wchar_t* name);

// Creation (descs are layout-identical structs)
HRESULT_T create_queue(PcObj dev, const void* desc, PcObj* out);
HRESULT_T create_allocator(PcObj dev, int type, PcObj* out);
HRESULT_T create_list(PcObj dev, int type, PcObj alloc, PcObj pso, PcObj* out);
HRESULT_T create_descriptor_heap(PcObj dev, const void* desc, PcObj* out);
uint32_t descriptor_increment(PcObj dev, int heapType);
uint64_t heap_cpu_start(PcObj heap);
void heap_desc(PcObj heap, void* desc);  // D3D12_DESCRIPTOR_HEAP_DESC
uint64_t heap_gpu_start(PcObj heap);
HRESULT_T create_root_signature(PcObj dev, const void* blob, size_t size, PcObj* out);
// desc: D3D12_GRAPHICS_PIPELINE_STATE_DESC with pRootSignature already a PC pointer.
HRESULT_T create_graphics_pso(PcObj dev, const void* desc, PcObj* out);
HRESULT_T create_compute_pso(PcObj dev, const void* desc, PcObj* out);
// stream: a PC D3D12_PIPELINE_STATE_STREAM (PC subobject numbering, shaders already sanitized).
HRESULT_T create_pso_stream(PcObj dev, void* stream, size_t size, PcObj* out);
HRESULT_T create_command_signature(PcObj dev, const void* desc, PcObj rootSig, PcObj* out);
HRESULT_T create_committed_resource(PcObj dev, const void* heapProps, uint32_t heapFlags, const void* desc,
                                    uint32_t initialState, const void* clearValue, PcObj* out);
HRESULT_T create_heap(PcObj dev, const void* desc, PcObj* out);
// Reserved (tiled) resources standing in for console resources placed on GPU virtual memory whose
// pages the title maps itself (RegisterPagePoolX / CopyPageMappingsX).
struct TileCoord { uint32_t x, y, z, subresource; };
HRESULT_T create_reserved_resource(PcObj dev, const void* desc, uint32_t state, const void* clear, PcObj* out);
std::vector<TileCoord> resource_tiles(PcObj dev, PcObj res);  // every 64 KB tile, in a fixed linear order
HRESULT_T create_page_pool_heap(PcObj dev, uint64_t bytes, PcObj* out);
// Map tiles[i] of `res` to page heapPages[i] of `heap`, or unmap them all when heap is null.
void queue_map_tiles(PcObj queue, PcObj res, uint32_t n, const TileCoord* tiles, PcObj heap, const uint32_t* heapPages);
HRESULT_T create_placed_resource(PcObj dev, PcObj heap, uint64_t offset, const void* desc, uint32_t state,
                                 const void* clearValue, PcObj* out);
void create_rtv(PcObj dev, PcObj res, const void* desc, uint64_t cpuHandle);
void create_dsv(PcObj dev, PcObj res, const void* desc, uint64_t cpuHandle);
void create_srv(PcObj dev, PcObj res, const void* desc, uint64_t cpuHandle);
void create_uav(PcObj dev, PcObj res, PcObj counter, const void* desc, uint64_t cpuHandle);
void create_cbv(PcObj dev, const void* desc, uint64_t cpuHandle);
void create_sampler(PcObj dev, const void* desc, uint64_t cpuHandle);
void copy_descriptors_simple(PcObj dev, uint32_t n, uint64_t dst, uint64_t src, int heapType);
void get_copyable_footprints(PcObj dev, const void* desc, uint32_t first, uint32_t num, uint64_t base,
                             void* layouts, uint32_t* rows, uint64_t* rowSizes, uint64_t* total);
void get_resource_allocation_info(PcObj dev, uint32_t mask, uint32_t n, const void* descs, void* out);
HRESULT_T check_feature_support(PcObj dev, int feature, void* data, uint32_t size);
HRESULT_T create_fence(PcObj dev, uint64_t initial, uint32_t flags, PcObj* out);

// Fence
uint64_t fence_completed(PcObj f);
HRESULT_T fence_set_event(PcObj f, uint64_t value, void* event);
HRESULT_T fence_signal(PcObj f, uint64_t value);

// Resource
HRESULT_T res_map(PcObj r, uint32_t sub, const void* range, void** out);
void res_unmap(PcObj r, uint32_t sub, const void* range);
void res_desc(PcObj r, void* outDesc);
uint64_t res_gpu_va(PcObj r);

// Allocator and command list
HRESULT_T alloc_reset(PcObj a);
HRESULT_T list_close(PcObj l);
HRESULT_T list_reset(PcObj l, PcObj alloc, PcObj pso);
void list_barriers(PcObj l, uint32_t n, const void* barriers);
void list_om_set_render_targets(PcObj l, uint32_t n, const uint64_t* rtvs, int single, const uint64_t* dsv);
void list_clear_rtv(PcObj l, uint64_t rtv, const float* rgba, uint32_t nRects, const void* rects);
void list_clear_dsv(PcObj l, uint64_t dsv, uint32_t flags, float depth, uint8_t stencil, uint32_t nRects, const void* rects);
void list_viewports(PcObj l, uint32_t n, const void* vps);
void list_scissors(PcObj l, uint32_t n, const void* rects);
void list_set_gfx_root_signature(PcObj l, PcObj rs);
void list_set_compute_root_signature(PcObj l, PcObj rs);
void list_set_pso(PcObj l, PcObj pso);
void list_ia_topology(PcObj l, int topo);
void list_ia_vbs(PcObj l, uint32_t start, uint32_t n, const void* views);
void list_ia_ib(PcObj l, const void* view);
void list_draw(PcObj l, uint32_t vpi, uint32_t inst, uint32_t sv, uint32_t si);
void list_draw_indexed(PcObj l, uint32_t ipi, uint32_t inst, uint32_t si, int32_t bv, uint32_t sinst);
void list_dispatch(PcObj l, uint32_t x, uint32_t y, uint32_t z);
void list_dispatch_mesh(PcObj l, uint32_t x, uint32_t y, uint32_t z);
void list_execute_indirect(PcObj l, PcObj sig, uint32_t max, PcObj args, uint64_t argOff, PcObj count, uint64_t countOff);
// XSL_D3D12_DRED: copies the first argument bytes (and the count) of an ExecuteIndirect to memory the
// device-removed report prints; the buffers must be in a state that allows copying from them.
bool dred_on();
// Diagnostics (XSL_D3D12_DEBUG): the root signature each pipeline was made with, checked against
// the list's bound one at ExecuteIndirect.
void note_pso_rs(PcObj pso, PcObj rs, bool compute);
// Ray-tracing acceleration structures. Descriptions are PC layouts with PC addresses (the
// geometry array layout, never pointers).
void rt_prebuild_info(PcObj dev, const void* inputs, void* info);
void list_rt_build(PcObj l, const void* desc, uint32_t npost, const void* post);
void list_rt_postbuild(PcObj l, const void* desc, uint32_t n, const uint64_t* src);
void list_rt_copy(PcObj l, uint64_t dst, uint64_t src, uint32_t mode);
void list_depth_bounds(PcObj l, float lo, float hi);
// Root signature deserializers (PC objects; descriptions have the PC layout).
HRESULT_T create_rs_deserializer(const void* data, size_t size, bool versioned, PcObj* out);
const void* rs_deserializer_desc(PcObj d);
HRESULT_T rs_vdeserializer_desc(PcObj d, int version, const void** out);
const void* rs_vdeserializer_unconverted(PcObj d);
void list_write_immediate(PcObj l, uint32_t n, const uint64_t* addrs, const uint32_t* values);
void list_record_indirect(PcObj l, PcObj args, uint64_t argOff, PcObj count, uint64_t countOff);
void list_clear_uav_uint(PcObj l, uint64_t gpu, uint64_t cpu, PcObj res, const uint32_t* v, uint32_t n, const void* rects);
void list_clear_uav_float(PcObj l, uint64_t gpu, uint64_t cpu, PcObj res, const float* v, uint32_t n, const void* rects);
// Writes a 32-bit value to a GPU VA once prior work completes (MARKER_OUT) or immediately (MARKER_IN).
void list_write_immediate(PcObj l, uint64_t va, uint32_t value, bool bottomOfPipe);
void list_discard(PcObj l, PcObj res);
void list_set_heaps(PcObj l, uint32_t n, PcObj* heaps);
void list_gfx_root_table(PcObj l, uint32_t idx, uint64_t gpuHandle);
void list_compute_root_table(PcObj l, uint32_t idx, uint64_t gpuHandle);
void list_gfx_root_cbv(PcObj l, uint32_t idx, uint64_t va);
void list_compute_root_cbv(PcObj l, uint32_t idx, uint64_t va);
void list_gfx_root_srv(PcObj l, uint32_t idx, uint64_t va);
void list_compute_root_srv(PcObj l, uint32_t idx, uint64_t va);
void list_gfx_root_uav(PcObj l, uint32_t idx, uint64_t va);
void list_compute_root_uav(PcObj l, uint32_t idx, uint64_t va);
void list_gfx_root_constants(PcObj l, uint32_t idx, uint32_t n, const void* data, uint32_t off);
void list_compute_root_constants(PcObj l, uint32_t idx, uint32_t n, const void* data, uint32_t off);
void list_copy_buffer_region(PcObj l, PcObj dst, uint64_t dstOff, PcObj src, uint64_t srcOff, uint64_t n);
// locations: D3D12_TEXTURE_COPY_LOCATION with pResource already a PC pointer.
void list_copy_texture_region(PcObj l, const void* dst, uint32_t x, uint32_t y, uint32_t z, const void* src, const void* box);
void list_copy_resource(PcObj l, PcObj dst, PcObj src);
void list_resolve_subresource(PcObj l, PcObj dst, uint32_t dstSub, PcObj src, uint32_t srcSub, int format);
void list_blend_factor(PcObj l, const float* f);
void list_stencil_ref(PcObj l, uint32_t ref);

// Queue
void queue_execute(PcObj q, uint32_t n, PcObj* lists);
HRESULT_T queue_signal(PcObj q, PcObj f, uint64_t v);
HRESULT_T queue_wait(PcObj q, PcObj f, uint64_t v);

// Presentation: copy `res` (current state: PRESENT) to the window's swap chain.
// colorSpace: the plane's DXGI_COLOR_SPACE_TYPE (console values; HDR planes are mapped to SDR scan-out).
HRESULT_T present(PcObj dev, PcObj queue, PcObj res, void* hwnd, uint32_t syncInterval, uint32_t colorSpace);
HRESULT_T null_present(void* hwnd);  // null graphics: no scan-out, the FPS title still updates

// Unified memory: wrap a host allocation (VirtualAlloc'd, 64 KB aligned) as a GPU buffer without
// copying (OpenExistingHeapFromAddress + placed buffer). Returns the buffer and its GPU VA.
HRESULT_T import_host_memory(PcObj dev, void* base, uint64_t size, PcObj* res, uint64_t* gpuVa);
// Write the GPU timestamp counter into `res` at `offset` once prior work completes.
void list_write_timestamp(PcObj dev, PcObj l, PcObj res, uint64_t offset);
// Query heaps: types are PC D3D12_QUERY_HEAP_TYPE / D3D12_QUERY_TYPE values.
HRESULT_T create_query_heap(PcObj dev, uint32_t type, uint32_t count, PcObj* out);
void list_begin_query(PcObj l, PcObj heap, uint32_t type, uint32_t index);
void list_end_query(PcObj l, PcObj heap, uint32_t type, uint32_t index);
void list_resolve_query(PcObj l, PcObj heap, uint32_t type, uint32_t start, uint32_t count, PcObj dst, uint64_t offset);
HRESULT_T queue_timestamp_frequency(PcObj q, uint64_t* f);
HRESULT_T queue_clock_calibration(PcObj q, uint64_t* gpu, uint64_t* cpu);

// Root-signature serialization. Blobs are PC ID3DBlob objects.
HRESULT_T serialize_root_signature(const void* desc, int version, PcObj* blob, PcObj* error);
HRESULT_T serialize_versioned_root_signature(const void* desc, PcObj* blob, PcObj* error);
void* blob_ptr(PcObj b);
size_t blob_size(PcObj b);
HRESULT_T pso_cached_blob(PcObj pso, PcObj* blob);
// Persistent CPU mapping of a buffer if its heap allows one; no logging (probing).
void* buffer_try_map(PcObj res);

// Shader containers: rewrite an Xbox-compiled DXBC container into a form the
// PC runtime accepts (see dxbc.cpp). Returns false if the blob is not DXBC.
// A PC ID3DBlob holding a copy of `data` (refcount 1).
PcObj create_blob(const void* data, size_t size);
// The PC device's removal reason (S_OK while alive); logs DRED data on the first failure.
HRESULT_T device_removed_reason(PcObj dev);
bool sanitize_container(const void* in, size_t inSize, void** out, size_t* outSize);
// Console machine code -> HLSL -> DXIL for shaders without (usable) DXIL; see native_shaders.cpp.
bool native_translate(const void* in, size_t inSize, void** out, size_t* outSize);
// Whether a shader without DXIL failed translation on this thread since reset_untranslated().
void reset_untranslated();
const char* translated_keys();   // keys of shaders translated since reset_untranslated()
bool had_untranslated();
void free_container(void* p);

}  // namespace pc
