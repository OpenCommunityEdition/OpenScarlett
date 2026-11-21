// SPDX-License-Identifier: AGPL-3.0-or-later
//
// XSL_NET_TRACE=1: logs the title's socket calls (name lookups, binds, connects, datagram
// destinations and sources, failures) on channel "net", to diagnose networking that differs
// between hosts. Each distinct (call, endpoint) pair is logged once. Off by default; nothing is
// changed, only observed.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include "xsl/log.h"
#include "xsl_platform.h"

#define CH "net"

namespace {
std::mutex g_lock;
std::set<std::string> g_seen;

std::string addr_str(const sockaddr* a) {
    if (!a) return "(null)";
    char host[INET6_ADDRSTRLEN] = "?";
    unsigned port = 0;
    if (a->sa_family == AF_INET) {
        auto* in = (const sockaddr_in*)a;
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof host);
        port = ntohs(in->sin_port);
    } else if (a->sa_family == AF_INET6) {
        auto* in = (const sockaddr_in6*)a;
        inet_ntop(AF_INET6, &in->sin6_addr, host, sizeof host);
        port = ntohs(in->sin6_port);
        return "[" + std::string(host) + "]:" + std::to_string(port);
    } else {
        return "family " + std::to_string(a->sa_family);
    }
    return std::string(host) + ":" + std::to_string(port);
}

void once(const std::string& key, const char* fmt, const std::string& a, const std::string& b = "") {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_seen.size() > 4000 || !g_seen.insert(key).second) return;
    XSL_INFO(CH, fmt, a.c_str(), b.c_str());
}

std::string err_suffix(int r) {
    if (r != SOCKET_ERROR) return "ok";
    return "error " + std::to_string(WSAGetLastError());
}

SOCKET WSAAPI H_socket(int af, int type, int proto) {
    SOCKET s = socket(af, type, proto);
    int e = WSAGetLastError();
    once("socket" + std::to_string(af) + "/" + std::to_string(type) + "/" + std::to_string(proto),
         "socket(af %s, type/proto %s)", std::to_string(af), std::to_string(type) + "/" + std::to_string(proto) + (s == INVALID_SOCKET ? " FAILED" : ""));
    WSASetLastError(e);
    return s;
}
int WSAAPI H_bind(SOCKET s, const sockaddr* a, int n) {
    int r = bind(s, a, n);
    int e = WSAGetLastError();
    once("bind" + addr_str(a) + err_suffix(r), "bind %s: %s", addr_str(a), err_suffix(r));
    WSASetLastError(e);
    return r;
}
int WSAAPI H_connect(SOCKET s, const sockaddr* a, int n) {
    int r = connect(s, a, n);
    int e = WSAGetLastError();
    once("connect" + addr_str(a) + std::to_string(r == SOCKET_ERROR ? e : 0), "connect %s: %s", addr_str(a),
         r == SOCKET_ERROR ? "error " + std::to_string(e) : "ok");
    WSASetLastError(e);
    return r;
}
int WSAAPI H_sendto(SOCKET s, const char* b, int len, int fl, const sockaddr* a, int n) {
    int r = sendto(s, b, len, fl, a, n);
    int e = WSAGetLastError();
    once("sendto" + addr_str(a) + std::to_string(r == SOCKET_ERROR ? e : 0), "sendto %s: %s", addr_str(a),
         r == SOCKET_ERROR ? "error " + std::to_string(e) : "ok (" + std::to_string(len) + " bytes, first)");
    WSASetLastError(e);
    return r;
}
int WSAAPI H_recvfrom(SOCKET s, char* b, int len, int fl, sockaddr* a, int* n) {
    int r = recvfrom(s, b, len, fl, a, n);
    int e = WSAGetLastError();
    if (r >= 0)
        once("recvfrom" + addr_str(a), "recvfrom %s: %s", addr_str(a), "data (" + std::to_string(r) + " bytes, first)");
    else if (e != WSAEWOULDBLOCK)
        once("recvfromerr" + std::to_string(e), "recvfrom %s: %s", std::string("-"), "error " + std::to_string(e));
    WSASetLastError(e);
    return r;
}
int WSAAPI H_getaddrinfo(PCSTR node, PCSTR svc, const ADDRINFOA* hints, PADDRINFOA* res) {
    DWORD t0 = GetTickCount();
    int r = getaddrinfo(node, svc, hints, res);
    std::string out = r ? "error " + std::to_string(r) : std::string();
    if (!r)
        for (ADDRINFOA* p = *res; p; p = p->ai_next) out += addr_str(p->ai_addr) + " ";
    out += "(" + std::to_string(GetTickCount() - t0) + " ms)";
    once(std::string("gai") + (node ? node : "(null)") + (svc ? svc : "") + out.substr(0, 8),
         "getaddrinfo %s -> %s", std::string(node ? node : "(null)") + (svc ? std::string(" port ") + svc : ""), out);
    return r;
}
int WSAAPI H_getsockname(SOCKET s, sockaddr* a, int* n) {
    int r = getsockname(s, a, n);
    if (r == 0) once("gsn" + addr_str(a), "getsockname -> %s%s", addr_str(a));
    return r;
}
int WSAAPI H_gethostname(char* name, int n) {
    int r = gethostname(name, n);
    if (r == 0) once(std::string("ghn") + name, "gethostname -> %s%s", std::string(name));
    return r;
}
}  // namespace

namespace xsl::platform {
void install_net_trace(HMODULE m) {
    static const bool on = [] {
        const char* e = getenv("XSL_NET_TRACE");
        return e && *e && *e != '0';
    }();
    if (!on) return;
    struct { const char* fn; void* p; } t[] = {
        {"socket", (void*)H_socket},           {"bind", (void*)H_bind},         {"connect", (void*)H_connect},
        {"sendto", (void*)H_sendto},           {"recvfrom", (void*)H_recvfrom}, {"getaddrinfo", (void*)H_getaddrinfo},
        {"getsockname", (void*)H_getsockname}, {"gethostname", (void*)H_gethostname},
    };
    for (auto& e : t) patch_import(m, "ws2_32", e.fn, e.p);
}
}  // namespace xsl::platform

