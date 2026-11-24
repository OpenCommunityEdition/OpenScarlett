// SPDX-License-Identifier: AGPL-3.0-or-later
//
// XSL_NET_TRACE=1: logs the title's socket calls (name lookups, binds, connects, datagram
// destinations and sources, failures) on channel "net", to diagnose networking that differs
// between hosts. Each distinct (call, endpoint) pair is logged once. Off by default; nothing is
// changed, only observed.
//
// The title is offline unless XSL_NET=on (also "online" or "1") is set. Its name lookups fail (host not found), its connects and
// datagrams to anything but the loopback interface fail (network unreachable), and its WinHTTP
// requests, and those of the HTTP libraries shipped next to it, fail to connect. Nothing leaves
// the machine; titles handle this like a console without a network.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>

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
bool g_block = true;    // offline unless XSL_NET=on

bool loopback(const sockaddr* a) {
    if (!a) return true;   // connected sockets (no destination given)
    if (a->sa_family == AF_INET) return (ntohl(((const sockaddr_in*)a)->sin_addr.s_addr) >> 24) == 127;
    if (a->sa_family == AF_INET6) return IN6_IS_ADDR_LOOPBACK(&((const sockaddr_in6*)a)->sin6_addr) != 0;
    return false;
}
bool local_name(const char* node) {
    if (!node || !*node || !_stricmp(node, "localhost") || !strncmp(node, "127.", 4) || !strcmp(node, "::1")) return true;
    char self[256] = "";   // the machine's own name is answered locally (titles look up their own address)
    return gethostname(self, sizeof self) == 0 && !_stricmp(node, self);
}

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
    if (g_block && !loopback(a)) {
        once("blk-connect" + addr_str(a), "offline: connect %s refused%s", addr_str(a), "");
        WSASetLastError(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
    int r = connect(s, a, n);
    int e = WSAGetLastError();
    once("connect" + addr_str(a) + std::to_string(r == SOCKET_ERROR ? e : 0), "connect %s: %s", addr_str(a),
         r == SOCKET_ERROR ? "error " + std::to_string(e) : "ok");
    WSASetLastError(e);
    return r;
}
int WSAAPI H_sendto(SOCKET s, const char* b, int len, int fl, const sockaddr* a, int n) {
    if (g_block && !loopback(a)) {
        once("blk-sendto" + addr_str(a), "offline: datagram to %s dropped%s", addr_str(a), "");
        WSASetLastError(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
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
    if (g_block && !local_name(node)) {
        once(std::string("blk-gai") + node, "offline: lookup of %s fails%s", node, "");
        if (res) *res = nullptr;
        WSASetLastError(WSAHOST_NOT_FOUND);
        return WSAHOST_NOT_FOUND;
    }
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
hostent* WSAAPI H_gethostbyname(const char* name) {
    if (g_block && !local_name(name)) {
        once(std::string("blk-ghbn") + name, "offline: lookup of %s fails%s", name, "");
        WSASetLastError(WSAHOST_NOT_FOUND);
        return nullptr;
    }
    return gethostbyname(name);
}
// WinHTTP (offline): the request fails where a connection would be made.
BOOL WINAPI H_WinHttpSendRequest(void*, const wchar_t*, DWORD, void*, DWORD, DWORD, DWORD_PTR) {
    once("blk-winhttp", "offline: WinHTTP requests fail to connect%s%s", "", "");
    SetLastError(12029);   // ERROR_WINHTTP_CANNOT_CONNECT
    return FALSE;
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
    g_block = [] {
        // Offline is the default: a title reaches the network only when the user asks for it.
        const char* e = getenv("XSL_NET");
        return !(e && (!_stricmp(e, "on") || !_stricmp(e, "online") || !strcmp(e, "1")));
    }();
    if (on) {
        struct { const char* fn; void* p; } t[] = {
            {"socket", (void*)H_socket},           {"bind", (void*)H_bind},         {"connect", (void*)H_connect},
            {"sendto", (void*)H_sendto},           {"recvfrom", (void*)H_recvfrom}, {"getaddrinfo", (void*)H_getaddrinfo},
            {"getsockname", (void*)H_getsockname}, {"gethostname", (void*)H_gethostname},
        };
        for (auto& e : t) patch_import(m, "ws2_32", e.fn, e.p);
    }
    if (!g_block) return;
    struct { const char* fn; void* p; } b[] = {
        {"connect", (void*)H_connect}, {"sendto", (void*)H_sendto}, {"getaddrinfo", (void*)H_getaddrinfo}, {"gethostbyname", (void*)H_gethostbyname},
    };
    // The title and the modules shipped in its directory (HTTP libraries); system DLLs are left alone.
    wchar_t dir[MAX_PATH];
    DWORD dn = GetModuleFileNameW(m, dir, MAX_PATH);
    while (dn && dir[dn - 1] != L'\\') dn--;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    unsigned mods = 0;
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{sizeof me};
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
            if (_wcsnicmp(me.szExePath, dir, dn) != 0) continue;
            HMODULE self = nullptr;   // not this module: its own calls are the real ones
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&H_connect, &self);
            if (me.hModule == self) continue;
            for (auto& e : b) patch_import(me.hModule, "ws2_32", e.fn, e.p);
            patch_import(me.hModule, "winhttp", "WinHttpSendRequest", (void*)H_WinHttpSendRequest);
            mods++;
        }
        CloseHandle(snap);
    }
    XSL_INFO(CH, "offline (the default; XSL_NET=on allows the network): lookups, connects, datagrams and WinHTTP requests of the title fail (%u modules)", mods);
}
}  // namespace xsl::platform

