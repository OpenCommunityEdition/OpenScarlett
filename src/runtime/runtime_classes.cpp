// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Gaming Runtime class dispatch. The title's static xgameruntime.lib turns every API call into
// QueryApiImpl(&class, &interface, &obj) followed by a call through obj's vtable at a fixed
// slot (tools/gen/gen_runtime_dispatch.py recovers {name, class, interface, slot} for all 419
// APIs of the pinned SDK). Here one static object per interface carries a vtable whose slots
// point at the registered implementations; unimplemented slots log their API name once and
// return E_NOTIMPL.
#include "runtime_classes.h"

#include <algorithm>
#include <array>
#include <map>
#include <utility>
#include <mutex>
#include <string>
#include <vector>

#include "rt_dispatch.h"  // generated
#include "xsl/log.h"

#define CH "rt"

namespace {
std::map<std::string, void*>& impls() { static std::map<std::string, void*> m; return m; }

template <unsigned N> HRESULT stub(void*) {
    static volatile long n = 0;
    if (N < kXslRtThunkCount && InterlockedIncrement(&n) <= 2)
        XSL_WARN("stub", "unimplemented runtime API %s", kXslRtThunks[N].name);
    return E_NOTIMPL;
}
template <unsigned... I> constexpr auto make_stubs(std::integer_sequence<unsigned, I...>) {
    return std::array<void*, sizeof...(I)>{(void*)&stub<I>...};
}
const auto kStubs = make_stubs(std::make_integer_sequence<unsigned, 2048>{});
static_assert(kXslRtThunkCount <= 2048, "raise the stub count");

HRESULT STDMETHODCALLTYPE Obj_QueryInterface(void* self, REFIID, void** out) { *out = self; return S_OK; }
ULONG STDMETHODCALLTYPE Obj_AddRef(void*) { return 2; }
ULONG STDMETHODCALLTYPE Obj_Release(void*) { return 1; }

struct Iface { GUID iid; GUID cls; std::vector<void*> vtbl; void* obj[1]; };
std::vector<Iface*>& ifaces() { static std::vector<Iface*> v; return v; }
std::once_flag g_once;

void build() {
    for (unsigned i = 0; i < kXslRtThunkCount; i++) {
        const auto& t = kXslRtThunks[i];
        Iface* f = nullptr;
        for (auto* x : ifaces()) if (IsEqualGUID(x->iid, t.iid)) f = x;
        if (!f) {
            f = new Iface{t.iid, t.cls, {}, {}};
            ifaces().push_back(f);
        }
        if (f->vtbl.size() <= t.slot) f->vtbl.resize(t.slot + 1, nullptr);
        auto it = impls().find(t.name);
        f->vtbl[t.slot] = it != impls().end() ? it->second : kStubs[i];
    }
    // A class's interfaces form a versioned chain: an extension interface starts where the one
    // before it ends (user: 01acd177 = slots 3-36, eb9bf948 = +37-38, 1bf2f8c5 = +39-41, ...)
    // and inherits every earlier slot, but the thunks of one SDK only show each extension's new
    // slots. Older titles reach base APIs through what was then the newest interface (a March
    // 2022 GDK title calls XUserAddAsync, slot 7, on eb9bf948), so fill an extension's lower
    // slots from the other interfaces of its class. Standalone interfaces start at slot 3.
    {
        std::map<std::pair<unsigned, unsigned>, unsigned> first;  // (class idx, iface idx) -> lowest own slot
        std::vector<GUID> classes;
        std::vector<unsigned> cls_of(ifaces().size());
        for (unsigned i = 0; i < kXslRtThunkCount; i++) {
            const auto& t = kXslRtThunks[i];
            unsigned fi = 0;
            while (!IsEqualGUID(ifaces()[fi]->iid, t.iid)) fi++;
            unsigned ci = 0;
            while (ci < classes.size() && !IsEqualGUID(classes[ci], t.cls)) ci++;
            if (ci == classes.size()) classes.push_back(t.cls);
            cls_of[fi] = ci;
            auto key = std::make_pair(ci, fi);
            if (!first.count(key) || t.slot < first[key]) first[key] = t.slot;
        }
        for (auto& [key, lo] : first) {
            if (lo <= 3) continue;  // a base or standalone interface
            Iface* ext = ifaces()[key.second];
            std::vector<std::pair<unsigned, unsigned>> srcs;  // (iface idx, lowest slot), biggest first
            for (auto& [other, olo] : first)
                if (other.first == key.first && other.second != key.second && olo < lo) srcs.push_back({other.second, olo});
            std::sort(srcs.begin(), srcs.end(), [](auto a, auto b) { return ifaces()[a.first]->vtbl.size() > ifaces()[b.first]->vtbl.size(); });
            for (auto [si, olo] : srcs) {
                Iface* src = ifaces()[si];
                for (unsigned s = olo; s < lo && s < src->vtbl.size(); s++) {
                    if (!src->vtbl[s]) continue;
                    if (ext->vtbl.size() <= s) ext->vtbl.resize(s + 1, nullptr);
                    if (!ext->vtbl[s]) ext->vtbl[s] = src->vtbl[s];
                }
            }
        }
    }
    unsigned implemented = 0;
    for (auto* f : ifaces()) {
        if (f->vtbl.size() < 3) f->vtbl.resize(3, nullptr);
        f->vtbl[0] = (void*)&Obj_QueryInterface;
        f->vtbl[1] = (void*)&Obj_AddRef;
        f->vtbl[2] = (void*)&Obj_Release;
        for (auto& p : f->vtbl) if (!p) p = (void*)&stub<2048>;
        f->obj[0] = f->vtbl.data();
    }
    for (unsigned i = 0; i < kXslRtThunkCount; i++) implemented += impls().count(kXslRtThunks[i].name);
    XSL_INFO(CH, "runtime: %u of %u APIs implemented over %zu interfaces", implemented, kXslRtThunkCount, ifaces().size());
}
}  // namespace

namespace xsl_rt {
void register_impl(const char* name, void* fn) { impls()[name] = fn; }

void* find_class(REFGUID cls, REFGUID iid) {
    std::call_once(g_once, build);
    for (auto* f : ifaces())
        if (IsEqualGUID(f->iid, iid)) return &f->obj[0];
    // An interface GUID no known SDK uses, on a known class: an edition between the known ones
    // (a March 2022 GDK title asks for XNetworking as 37e56907/37e56907; 2022.10 and later use
    // interface bf2346b2 with the same slots). Answer with the class's base interface: the one
    // whose methods start at slot 3, with the most slots.
    Iface* best = nullptr;
    for (auto* f : ifaces()) {
        if (!IsEqualGUID(f->cls, cls)) continue;
        bool base = false;
        for (unsigned i = 0; i < kXslRtThunkCount; i++)
            if (IsEqualGUID(kXslRtThunks[i].iid, f->iid) && kXslRtThunks[i].slot == 3) base = true;
        if (base && (!best || f->vtbl.size() > best->vtbl.size())) best = f;
    }
    if (best) {
        static std::mutex m;
        static std::vector<GUID> logged;
        std::lock_guard<std::mutex> l(m);
        bool seen = false;
        for (auto& g : logged) seen |= !!IsEqualGUID(g, iid);
        if (!seen) {
            logged.push_back(iid);
            XSL_INFO(CH, "interface %08lx of class %08lx unknown to the known SDKs: using the class's base interface %08lx",
                     iid.Data1, cls.Data1, best->iid.Data1);
        }
        return &best->obj[0];
    }
    return nullptr;
}
}  // namespace xsl_rt
