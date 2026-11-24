// SPDX-License-Identifier: MIT
// pso_check <translated.dxil> <original.cso>: creates a compute pipeline from a translated shader
// (shader cache entry) with the root signature of the original console container, on the default
// adapter, and prints the result. With the debug layer installed, its messages are printed too.
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <vector>

// The D3D12 runtime beside the program (D3D12\D3D12Core.dll, as tools/xsl-warm stages it), when it
// is there: translated shaders use shader model 6.6, which the Windows 10 system runtime refuses.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 620;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
static std::vector<char> read(const char* p) {
    std::vector<char> v;
    FILE* f = fopen(p, "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    v.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(v.data(), 1, v.size(), f);
    fclose(f);
    return v;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: pso_check <translated.dxil> <original.cso>\n"); return 2; }
    auto dxil = read(argv[1]), cso = read(argv[2]);
    ID3D12Debug* dbg = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); printf("debug layer on\n"); }
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) { printf("device %08lx\n", hr); return 1; }
    ID3D12RootSignature* rs = nullptr;
    hr = dev->CreateRootSignature(0, cso.data(), cso.size(), IID_PPV_ARGS(&rs));
    printf("root signature from container: %08lx\n", hr);
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rs;
    d.CS = {dxil.data(), dxil.size()};
    ID3D12PipelineState* pso = nullptr;
    const ULONGLONG t0 = GetTickCount64();
    hr = dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
    printf("CreateComputePipelineState: %08lx after %.1f s (%zu bytes of shader)\n", hr, (GetTickCount64() - t0) / 1000.0, dxil.size());
    fflush(stdout);
    ID3D12InfoQueue* q = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&q)))) {
        for (UINT64 i = 0; i < q->GetNumStoredMessages(); i++) {
            SIZE_T n = 0;
            q->GetMessage(i, nullptr, &n);
            std::vector<char> m(n);
            auto* msg = (D3D12_MESSAGE*)m.data();
            q->GetMessage(i, msg, &n);
            printf("  [%d] %s\n", (int)msg->Severity, msg->pDescription);
        }
    }
    return SUCCEEDED(hr) ? 0 : 1;
}
