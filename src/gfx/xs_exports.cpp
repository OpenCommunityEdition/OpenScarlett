// SPDX-License-Identifier: AGPL-3.0-or-later
// Remaining d3d12_xs.dll exports: present in the import library, not yet implemented.
// Each logs its name; IIDs are defined by INITGUID in xs_object.h and exported as DATA.
#include "xs_object.h"

#define XSL_EXPORT_STUB(name) \
    extern "C" HRESULT xsl_exp_##name() { XSL_WARN(CH, "unimplemented export %s", #name); return E_NOTIMPL; }
XSL_EXPORT_STUB(D3D12ConfigureVirtualMemoryX)
XSL_EXPORT_STUB(D3D12CreateDevice)
XSL_EXPORT_STUB(D3D12CreateRootSignatureDeserializer)
XSL_EXPORT_STUB(D3D12CreateVersionedRootSignatureDeserializer)
XSL_EXPORT_STUB(D3D12GetDebugInterface)
XSL_EXPORT_STUB(D3D12UploadCustomMicrocodeX)
XSL_EXPORT_STUB(D3D12XboxEnableMemoryProfiling)
XSL_EXPORT_STUB(D3D12ZeroMemoryX)
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
