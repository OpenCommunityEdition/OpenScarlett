// SPDX-License-Identifier: MIT
// Writes __XB_MemTime() (the console GPU clock) to TimeBuffer[slot].
#define RS "UAV(u0), RootConstants(num32BitConstants=1, b0)"
RWStructuredBuffer<uint2> TimeBuffer : register(u0);
cbuffer Slot : register(b0) { uint slot; }
[RootSignature(RS)]
[numthreads(1, 1, 1)]
void main() { TimeBuffer[slot] = __XB_MemTime(); }
