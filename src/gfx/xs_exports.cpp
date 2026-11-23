// SPDX-License-Identifier: AGPL-3.0-or-later
// Remaining d3d12_xs.dll exports: present in the import library, not yet implemented.
// Each logs its name; IIDs are defined by INITGUID in xs_object.h and exported as DATA.
#include "xs_object.h"

#define XSL_EXPORT_STUB(name) \
    extern "C" HRESULT xsl_exp_##name() { XSL_WARN(CH, "unimplemented export %s", #name); return E_NOTIMPL; }
// D3D12ConfigureVirtualMemoryX (d3d12_xs.h): sizes the console GPU page tables before any graphics
// memory exists, and reports the graphics virtual-address window. Graphics memory here is ordinary
// process memory at CPU addresses, so there is nothing to configure; the window reported is the
// console's (lower bound 2 GB, 1 GB with the legacy flag; 1 TB upper bound).
struct XslVmConfig { UINT Flags, PageTableMemory4MBPageCount, FrameBufferPreserveMemorySizeBytes; uint64_t MinimumValidGraphicsAddress, MaximumValidGraphicsAddress; };
extern "C" HRESULT WINAPI xsl_exp_D3D12ConfigureVirtualMemoryX(XslVmConfig* c) {
    if (!c) return E_INVALIDARG;
    c->MinimumValidGraphicsAddress = (c->Flags & 1) ? 0x40000000ull : 0x80000000ull;
    c->MaximumValidGraphicsAddress = 0x10000000000ull;
    XSL_INFO(CH, "D3D12ConfigureVirtualMemoryX(flags %#x, page tables %u x 4 MB, frame-buffer preserve %u bytes): window %llx..%llx", c->Flags,
             c->PageTableMemory4MBPageCount, c->FrameBufferPreserveMemorySizeBytes, (unsigned long long)c->MinimumValidGraphicsAddress,
             (unsigned long long)c->MaximumValidGraphicsAddress);
    return S_OK;
}
XSL_EXPORT_STUB(D3D12CreateDevice)
XSL_EXPORT_STUB(D3D12GetDebugInterface)
XSL_EXPORT_STUB(D3D12UploadCustomMicrocodeX)
XSL_EXPORT_STUB(D3D12XboxEnableMemoryProfiling)
XSL_EXPORT_STUB(DXGIXGetFrameStatistics)
XSL_EXPORT_STUB(DXGIXPresentArray)
XSL_EXPORT_STUB(DXGIXSetFrameNotification)
XSL_EXPORT_STUB(DXGIXSetVLineNotification)
XSL_EXPORT_STUB(InternalDriverCallX)
XSL_EXPORT_STUB(PixGetPixieEntryPoints)
// ---- Blobs ------------------------------------------------------------------------------
// Console ID3D10Blob is a function-table object; it fronts either a PC ID3DBlob or memory we own.
namespace {
struct BlobExtra { void* data; size_t size; bool owned; };
LPVOID D3DAPI Blob_Ptr(ID3D10Blob* b) {
    auto* e = xs::ex<ID3D10Blob, BlobExtra>(b);
    return e->data;
}
SIZE_T D3DAPI Blob_Size(ID3D10Blob* b) { return xs::ex<ID3D10Blob, BlobExtra>(b)->size; }
void Blob_Destroy(IGraphicsUnknown* self) {
    auto* e = xs::ex<ID3D10Blob, BlobExtra>((ID3D10Blob*)self);
    if (e->owned) free(e->data);
}
const IID* const kBlobIids[] = {&__uuidof(IGraphicsUnknown), &__uuidof(ID3D10Blob), nullptr};
D3D10Blob_Function_Table g_blob_tbl = [] {
    D3D10Blob_Function_Table t;
    xsl_fill_default(&t);
    t.UnknownFunction.QueryInterface = xs::QueryInterface;
    t.UnknownFunction.AddRef = xs::AddRef;
    t.UnknownFunction.Release = xs::Release;
    t.GetBufferPointer = Blob_Ptr;
    t.GetBufferSize = Blob_Size;
    return t;
}();
ID3D10Blob* make_blob(PcObj pc, void* data, size_t size, bool owned) {
    auto* b = xs::make<ID3D10Blob, BlobExtra>(xs::Kind::Pso /*unused*/, &g_blob_tbl, pc, nullptr, kBlobIids, Blob_Destroy);
    auto* e = xs::ex<ID3D10Blob, BlobExtra>(b);
    e->data = pc ? pc::blob_ptr(pc) : data;
    e->size = pc ? pc::blob_size(pc) : size;
    e->owned = owned;
    return b;
}
}  // namespace

namespace xs {
// A console blob fronting a PC ID3DBlob (takes the reference).
ID3D10Blob* make_pc_blob(PcObj pc) { return make_blob(pc, nullptr, 0, false); }
}  // namespace xs

extern "C" HRESULT D3DAPI xsl_exp_D3D10CreateBlob(SIZE_T size, ID3D10Blob** out) {
    if (!out) return E_POINTER;
    *out = make_blob(nullptr, calloc(1, size ? size : 1), size, true);
    return S_OK;
}
extern "C" HRESULT D3DAPI xsl_exp_D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* d, D3D_ROOT_SIGNATURE_VERSION v,
                                                              ID3D10Blob** blob, ID3D10Blob** err) {
    PcObj b = nullptr, e = nullptr;
    HRESULT hr = pc::serialize_root_signature(d, v, &b, &e);
    if (blob) *blob = b ? make_blob(b, nullptr, 0, false) : nullptr; else if (b) pc::release(b);
    if (err) *err = e ? make_blob(e, nullptr, 0, false) : nullptr; else if (e) pc::release(e);
    return hr;
}
extern "C" HRESULT D3DAPI xsl_exp_D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* d, ID3D10Blob** blob,
                                                                       ID3D10Blob** err) {
    PcObj b = nullptr, e = nullptr;
    HRESULT hr = pc::serialize_versioned_root_signature(d, &b, &e);
    if (blob) *blob = b ? make_blob(b, nullptr, 0, false) : nullptr; else if (b) pc::release(b);
    if (err) *err = e ? make_blob(e, nullptr, 0, false) : nullptr; else if (e) pc::release(e);
    return hr;
}

// ---- Root signature deserializers: console objects fronting the PC ones -----------------------
namespace {
struct DeserExtra { int unused; };
void Deser_Destroy(IGraphicsUnknown*) {}
const D3D12_ROOT_SIGNATURE_DESC* D3DAPI Deser_Get(ID3D12RootSignatureDeserializer* d) {
    return (const D3D12_ROOT_SIGNATURE_DESC*)pc::rs_deserializer_desc(xs::pc_of(d));
}
HRESULT D3DAPI VDeser_GetAt(ID3D12VersionedRootSignatureDeserializer* d, D3D_ROOT_SIGNATURE_VERSION v, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC** out) {
    const void* p = nullptr;
    HRESULT hr = pc::rs_vdeserializer_desc(xs::pc_of(d), (int)v, &p);
    if (out) *out = (const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*)p;
    return hr;
}
const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* D3DAPI VDeser_Unconverted(ID3D12VersionedRootSignatureDeserializer* d) {
    return (const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*)pc::rs_vdeserializer_unconverted(xs::pc_of(d));
}
const IID* const kDeserIids[] = {&__uuidof(IGraphicsUnknown), &__uuidof(ID3D12RootSignatureDeserializer), nullptr};
const IID* const kVDeserIids[] = {&__uuidof(IGraphicsUnknown), &__uuidof(ID3D12VersionedRootSignatureDeserializer), nullptr};
D3D12RootSignatureDeserializer_Function_Table g_deser_tbl = [] {
    D3D12RootSignatureDeserializer_Function_Table t;
    xsl_fill_default(&t);
    t.ObjectFunction.QueryInterface = xs::QueryInterface;
    t.ObjectFunction.AddRef = xs::AddRef;
    t.ObjectFunction.Release = xs::Release;
    t.GetRootSignature = Deser_Get;
    return t;
}();
D3D12VersionedRootSignatureDeserializer_Function_Table g_vdeser_tbl = [] {
    D3D12VersionedRootSignatureDeserializer_Function_Table t;
    xsl_fill_default(&t);
    t.ObjectFunction.QueryInterface = xs::QueryInterface;
    t.ObjectFunction.AddRef = xs::AddRef;
    t.ObjectFunction.Release = xs::Release;
    t.GetRootSignatureDescAtVersion = VDeser_GetAt;
    t.GetUnconvertedRootSignatureDesc = VDeser_Unconverted;
    return t;
}();
template <class T, class Tbl>
HRESULT make_deser(const void* data, SIZE_T size, bool versioned, Tbl* tbl, const IID* const* iids, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    PcObj pc = nullptr;
    HRESULT hr = pc::create_rs_deserializer(data, size, versioned, &pc);
    if (FAILED(hr)) return hr;
    T* o = xs::make<T, DeserExtra>(xs::Kind::Pso /*unused*/, tbl, pc, nullptr, iids, Deser_Destroy);
    hr = xs::QueryInterface((IGraphicsUnknown*)o, riid, out);
    xs::Release((IGraphicsUnknown*)o);
    return hr;
}
}  // namespace
extern "C" HRESULT D3DAPI xsl_exp_D3D12CreateRootSignatureDeserializer(LPCVOID data, SIZE_T size, REFIID riid, void** out) {
    return make_deser<ID3D12RootSignatureDeserializer>(data, size, false, &g_deser_tbl, kDeserIids, riid, out);
}
extern "C" HRESULT D3DAPI xsl_exp_D3D12CreateVersionedRootSignatureDeserializer(LPCVOID data, SIZE_T size, REFIID riid, void** out) {
    return make_deser<ID3D12VersionedRootSignatureDeserializer>(data, size, true, &g_vdeser_tbl, kVDeserIids, riid, out);
}
// Console memory is CPU memory: zeroing is a memset of each range.
// Ranges that are not committed, writable CPU memory here are skipped (reported).
static bool writable(void* p, SIZE_T n) {
    for (uint8_t* q = (uint8_t*)p; q < (uint8_t*)p + n;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(q, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            !(mi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) || (mi.Protect & PAGE_GUARD))
            return false;
        q = (uint8_t*)mi.BaseAddress + mi.RegionSize;
    }
    return true;
}
extern "C" HRESULT xsl_exp_D3D12ZeroMemoryX(UINT n, const D3D12XBOX_ZERO_MEMORY_RANGE* r, const D3D12XBOX_ZERO_MEMORY_BUFFERS*, D3D12XBOX_ZERO_MEMORY_FLAGS f) {
    static volatile long calls = 0;
    bool log = InterlockedIncrement(&calls) <= 4;
    for (UINT i = 0; r && i < n; i++) {
        if (!r[i].pBaseAddress || !r[i].SizeBytes) continue;
        bool ok = writable(r[i].pBaseAddress, r[i].SizeBytes);
        if (log) XSL_INFO(CH, "D3D12ZeroMemoryX range %u: %p + %zu (flags %d)%s", i, r[i].pBaseAddress, (size_t)r[i].SizeBytes, (int)f, ok ? "" : " not writable: skipped");
        if (ok) memset(r[i].pBaseAddress, 0, r[i].SizeBytes);
    }
    return S_OK;
}

extern "C" UINT xsl_exp_D3D12XboxGetProcessDebugFlags() { return 0; }
extern "C" HRESULT xsl_exp_D3D12XboxSetProcessDebugFlags(UINT) { return S_OK; }
extern "C" void xsl_exp_D3DFlushEntireCpuCache() {}
extern "C" UINT xsl_exp_GetFrameBufferPreserveMemorySizeBytes() { return 0; }

// d3d12_xs.dll is a static import of every Series X|S title, so its DllMain runs before the
// title's entry point: the natural place to interpose the platform surface.
namespace xsl::platform { void install_into_title(); }
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) xsl::platform::install_into_title();
    return TRUE;
}
