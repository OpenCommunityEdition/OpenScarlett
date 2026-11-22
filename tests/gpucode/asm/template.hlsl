// SPDX-License-Identifier: MIT
// Template container for hand-assembled ISA tests: its header and binding records are kept and
// its machine code is replaced (xsl-gpucode translate --code). Bindings: input words at t1
// (ByteAddressBuffer), results at u0 (RWByteAddressBuffer), 64 threads, wave64.
#include "../kernels/common.hlsli"
ByteAddressBuffer In : register(t1);
RWByteAddressBuffer Out : register(u0);
[RootSignature(RS)][numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) {
    uint i = t.x;
    Out.Store(i * 4, In.Load(i * 4) + 1);
}
