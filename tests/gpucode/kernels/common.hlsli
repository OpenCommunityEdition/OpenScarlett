// SPDX-License-Identifier: MIT
// Shared root signature of the tier-1 kernels: tables of SRVs, UAVs, CBVs and samplers.
#define RS "DescriptorTable(SRV(t0, numDescriptors=8)), DescriptorTable(UAV(u0, numDescriptors=8)), " \
           "DescriptorTable(CBV(b0, numDescriptors=4)), DescriptorTable(Sampler(s0, numDescriptors=2))"
