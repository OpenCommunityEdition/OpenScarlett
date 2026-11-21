// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <windows.h>
namespace xsl_rt {
// Returns the runtime object for (class, interface), or null if the interface is unknown.
void* find_class(REFGUID cls, REFGUID iid);
// Register the implementation of a runtime API by its public name. The implementation takes
// the runtime object as an extra first parameter, followed by the API's own parameters.
void register_impl(const char* name, void* fn);
}
// Implementations are called as C++ member functions (MSVC x64): `this` in rcx, then the API's
// arguments. An API that returns a struct by value takes the hidden return buffer *after* `this`
// (R* M(void* self, R* ret, args...) returning ret), unlike a free function, which takes it first.
// Bridge a public API function `F` (same signature) into a runtime method with a leading `this`.
template <auto F> struct XslRtFwd;
template <class R, class... A, R (*F)(A...)> struct XslRtFwd<F> {
    static_assert(!__is_class(R), "struct-returning APIs need the member-call form, see above");
    static R call(void*, A... a) { return F(a...); }
};
#define XSL_RT_REGISTER(name, fn) static const bool xsl_reg_##name = (xsl_rt::register_impl(#name, (void*)(fn)), true)
#define XSL_RT_FORWARD(name) XSL_RT_REGISTER(name, &XslRtFwd<&::name>::call)
