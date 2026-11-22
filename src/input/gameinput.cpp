// SPDX-License-Identifier: AGPL-3.0-or-later
//
// GameInput.dll replacement: GameInput v0 as the GDKX's GameInput.h declares it.
//
// Devices: up to four gamepads (XInput; Wine maps SDL/HID pads onto it, the DualSense included
// once Wine's hidraw backend is off, see tools/xsl-run), one keyboard and one mouse.
//   * Device callbacks enumerate them (blocking or async) and report gamepad hot-plug.
//   * Readings: the polling thread (8 ms) records a snapshot with a new sequence number whenever a
//     device's state changes and keeps the last 64 per device. GetCurrentReading returns the
//     newest, GetNext/PreviousReading walk the history (titles that drain it see every button
//     press and release), reading callbacks fire per new snapshot.
//   * Keyboard: key states from GetAsyncKeyState (scan code, virtual key, code point).
//   * Mouse: buttons, wheel and edge motion from raw input on a hidden window, position from the
//     host pointer. Keyboard and mouse only report while one of our windows has focus.
// Mouse position follows the host pointer exactly (see mouse_state); the pointer is hidden and confined
// while focused (update_mouse_capture). XSL_INPUT_TRACE=1 logs the reported positions.
// Titles that never ask for keyboard readings (the GDK samples) also get the keyboard as a
// gamepad stand-in on the aggregate pad: Esc = View, Enter = Menu, arrows = D-pad, Z/X/C/V = A/B/X/Y.
#include <windows.h>
#include <xinput.h>

#include <GameInput.h>

#include <atomic>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "xsl/display.h"
#include "xsl/log.h"

#define CH "input"

namespace {
typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* PFN_XInputSetState)(DWORD, XINPUT_VIBRATION*);
HMODULE g_xinput_mod = [] {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    return m ? m : LoadLibraryW(L"xinput1_3.dll");
}();
PFN_XInputGetState g_xget = g_xinput_mod ? (PFN_XInputGetState)GetProcAddress(g_xinput_mod, "XInputGetState") : nullptr;
PFN_XInputSetState g_xset = g_xinput_mod ? (PFN_XInputSetState)GetProcAddress(g_xinput_mod, "XInputSetState") : nullptr;

uint64_t now_us() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart * 1000000 / f.QuadPart);
}

bool our_focus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
float stick(SHORT v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }

std::atomic<bool> g_title_reads_keyboard{false};

// XSL_INPUT_SCRIPT="<seconds>:<button>[,<seconds>:<button>...]" (testing without a person at the
// machine): from that many seconds after start the button is held for 300 ms, on the first gamepad
// and as the matching key. Buttons: A B X Y Menu View Up Down Left Right (keys Enter Escape and arrows
// for A/Menu, B/View and the D-pad).
struct ScriptedPress { uint64_t at_ms; uint32_t pad; int vk; };
const std::vector<ScriptedPress>& input_script() {
    static std::vector<ScriptedPress> v = [] {
        std::vector<ScriptedPress> r;
        char b[512];
        if (!GetEnvironmentVariableA("XSL_INPUT_SCRIPT", b, sizeof b)) return r;
        static const struct { const char* n; uint32_t pad; int vk; } names[] = {
            {"A", GameInputGamepadA, VK_RETURN}, {"B", GameInputGamepadB, VK_ESCAPE}, {"X", GameInputGamepadX, 'C'}, {"Y", GameInputGamepadY, 'V'},
            {"Menu", GameInputGamepadMenu, VK_RETURN}, {"View", GameInputGamepadView, VK_ESCAPE}, {"Up", GameInputGamepadDPadUp, VK_UP},
            {"Down", GameInputGamepadDPadDown, VK_DOWN}, {"Left", GameInputGamepadDPadLeft, VK_LEFT}, {"Right", GameInputGamepadDPadRight, VK_RIGHT}};
        for (char* t = strtok(b, ","); t; t = strtok(nullptr, ",")) {
            double sec = 0;
            char btn[16] = {};
            if (sscanf(t, "%lf:%15s", &sec, btn) != 2) continue;
            for (auto& n : names)
                if (!_stricmp(n.n, btn)) r.push_back({(uint64_t)(sec * 1000), n.pad, n.vk});
        }
        XSL_INFO(CH, "input script: %zu scripted presses", r.size());
        return r;
    }();
    return v;
}
uint64_t g_start_ms = GetTickCount64();
void scripted(uint32_t* pad, std::vector<int>* vks) {
    uint64_t now = GetTickCount64() - g_start_ms;
    for (auto& p : input_script())
        if (now >= p.at_ms && now < p.at_ms + 300) {
            if (pad) *pad |= p.pad;
            if (vks) vks->push_back(p.vk);
        }
}

// ---- State sources ---------------------------------------------------------------------
bool pad_state(DWORD slot, GameInputGamepadState* s) {
    *s = {};
    XINPUT_STATE x{};
    if (!g_xget || g_xget(slot, &x) != ERROR_SUCCESS) {
        if (slot == 0 && !input_script().empty()) {   // scripted presses need no physical pad
            uint32_t o = 0;
            scripted(&o, nullptr);
            s->buttons = (GameInputGamepadButtons)o;
            return true;
        }
        return false;
    }
    const WORD b = x.Gamepad.wButtons;
    uint32_t o = 0;
    if (b & XINPUT_GAMEPAD_START) o |= GameInputGamepadMenu;
    if (b & XINPUT_GAMEPAD_BACK) o |= GameInputGamepadView;
    if (b & XINPUT_GAMEPAD_A) o |= GameInputGamepadA;
    if (b & XINPUT_GAMEPAD_B) o |= GameInputGamepadB;
    if (b & XINPUT_GAMEPAD_X) o |= GameInputGamepadX;
    if (b & XINPUT_GAMEPAD_Y) o |= GameInputGamepadY;
    if (b & XINPUT_GAMEPAD_DPAD_UP) o |= GameInputGamepadDPadUp;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) o |= GameInputGamepadDPadDown;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) o |= GameInputGamepadDPadLeft;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) o |= GameInputGamepadDPadRight;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) o |= GameInputGamepadLeftShoulder;
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) o |= GameInputGamepadRightShoulder;
    if (b & XINPUT_GAMEPAD_LEFT_THUMB) o |= GameInputGamepadLeftThumbstick;
    if (b & XINPUT_GAMEPAD_RIGHT_THUMB) o |= GameInputGamepadRightThumbstick;
    if (slot == 0) scripted(&o, nullptr);
    s->buttons = (GameInputGamepadButtons)o;
    s->leftTrigger = x.Gamepad.bLeftTrigger / 255.0f;
    s->rightTrigger = x.Gamepad.bRightTrigger / 255.0f;
    s->leftThumbstickX = stick(x.Gamepad.sThumbLX);
    s->leftThumbstickY = stick(x.Gamepad.sThumbLY);
    s->rightThumbstickX = stick(x.Gamepad.sThumbRX);
    s->rightThumbstickY = stick(x.Gamepad.sThumbRY);
    return true;
}

void keyboard_as_pad(GameInputGamepadState* s) {
    *s = {};
    uint32_t o = 0;
    scripted(&o, nullptr);
    if (!our_focus()) { s->buttons = (GameInputGamepadButtons)o; return; }
    if (down(VK_ESCAPE)) o |= GameInputGamepadView;
    if (down(VK_RETURN)) o |= GameInputGamepadMenu;
    if (down(VK_UP)) o |= GameInputGamepadDPadUp;
    if (down(VK_DOWN)) o |= GameInputGamepadDPadDown;
    if (down(VK_LEFT)) o |= GameInputGamepadDPadLeft;
    if (down(VK_RIGHT)) o |= GameInputGamepadDPadRight;
    if (down('Z')) o |= GameInputGamepadA;
    if (down('X')) o |= GameInputGamepadB;
    if (down('C')) o |= GameInputGamepadX;
    if (down('V')) o |= GameInputGamepadY;
    s->buttons = (GameInputGamepadButtons)o;
}

void keyboard_state(std::vector<GameInputKeyState>* keys) {
    keys->clear();
    {
        std::vector<int> vks;
        scripted(nullptr, &vks);
        for (int vk : vks) {
            GameInputKeyState k{};
            k.scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            k.virtualKey = (uint8_t)vk;
            keys->push_back(k);
        }
        if (!vks.empty()) return;
    }
    if (!our_focus()) return;
    BYTE kb[256] = {};
    for (int vk = 0; vk < 256; vk++) {
        SHORT s = GetAsyncKeyState(vk);
        kb[vk] = (BYTE)(((s & 0x8000) ? 0x80 : 0) | ((vk == VK_CAPITAL || vk == VK_NUMLOCK) && (GetKeyState(vk) & 1) ? 1 : 0));
    }
    HKL layout = GetKeyboardLayout(0);
    for (int vk = 8; vk < 255; vk++) {
        // Mouse buttons, and the generic modifiers (the left/right ones are reported instead).
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2 ||
            vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
            continue;
        if (!(kb[vk] & 0x80)) continue;
        GameInputKeyState k{};
        UINT sc = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, layout);
        k.scanCode = (sc & 0xFF00) ? (0xE000 | (sc & 0xFF)) : sc;
        k.virtualKey = (uint8_t)vk;
        WCHAR ch[4] = {};
        int n = ToUnicodeEx(vk, sc & 0xFF, kb, ch, 4, 0x4 /* keep keyboard state */, layout);
        if (n == 1 && ch[0] >= 0x20) k.codePoint = ch[0];
        k.isDeadKey = n < 0;
        keys->push_back(k);
    }
}

// Mouse: raw input accumulates relative motion and wheel on a hidden window's thread.
std::atomic<int64_t> g_mouse_x{0}, g_mouse_y{0}, g_wheel_x{0}, g_wheel_y{0};
std::atomic<bool> g_captured{false};  // see update_mouse_capture
// Buttons from raw input. A press stays visible for at least kMinPressUs so that a click shorter
// than the title's polling interval (one frame) is still seen.
constexpr uint64_t kMinPressUs = 50000;
std::atomic<uint32_t> g_buttons{0};
std::atomic<uint64_t> g_press_until[5];
void raw_button(USHORT f, USHORT downFlag, USHORT upFlag, int i) {
    if (f & downFlag) {
        g_buttons |= 1u << i;
        g_press_until[i] = now_us() + kMinPressUs;
    }
    if (f & upFlag) g_buttons &= ~(1u << i);
}

LRESULT CALLBACK raw_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT) {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE &&
            our_focus()) {
            const RAWMOUSE& rm = ri.data.mouse;
            if (!(rm.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_mouse_x += rm.lLastX;
                g_mouse_y += rm.lLastY;
            }
            static bool first = true;
            if (first && (rm.lLastX || rm.lLastY)) {
                first = false;
                XSL_INFO(CH, "raw mouse input arriving (first motion %ld,%ld, flags %#x)", rm.lLastX, rm.lLastY, rm.usFlags);
            }
            raw_button(rm.usButtonFlags, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 0);
            raw_button(rm.usButtonFlags, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 1);
            raw_button(rm.usButtonFlags, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2);
            raw_button(rm.usButtonFlags, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 3);
            raw_button(rm.usButtonFlags, RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 4);
            if (rm.usButtonFlags & RI_MOUSE_WHEEL) g_wheel_y += (SHORT)rm.usButtonData;
            if (rm.usButtonFlags & RI_MOUSE_HWHEEL) g_wheel_x += (SHORT)rm.usButtonData;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

DWORD WINAPI raw_thread(void*) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = raw_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"xsl_gameinput_raw";
    RegisterClassW(&wc);
    // A hidden top-level window: Windows delivers RIDEV_INPUTSINK raw input to it in the
    // background (a message-only window did not receive any on Windows 10).
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                             wc.hInstance, nullptr);
    RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_INPUTSINK, h};  // generic desktop / mouse
    BOOL ok = RegisterRawInputDevices(&rid, 1, sizeof rid);
    if (!ok) {
        DWORD e = GetLastError();
        rid.dwFlags = 0;
        ok = RegisterRawInputDevices(&rid, 1, sizeof rid);
        XSL_WARN(CH, "raw mouse input with INPUTSINK refused (%lu); foreground-only registration %s", e, ok ? "ok" : "failed too");
    } else {
        XSL_INFO(CH, "raw mouse input registered");
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}

// Mouse position: the host pointer's position in the focused window's client area, plus the raw
// motion the pointer could not follow because it was pinned at an edge of that area.
// Titles take deltas of positionX/Y and integrate them into their own cursor, clamped to their
// screen and starting at (0, 0) (Minecraft Preview: +0x2e2a8c0). Reporting client coordinates
// puts that cursor exactly under the host pointer and keeps it there (no drift from pointer
// acceleration or edges); the edge excess keeps relative motion (camera turning) unbounded, and a
// title's own clamping absorbs it in menus.
std::mutex g_mouse_mu;
int64_t g_extra_x = 0, g_extra_y = 0, g_last_raw_x = 0, g_last_raw_y = 0;
POINT g_last_client{};
POINT g_report{};

void mouse_state(GameInputMouseState* s) {
    *s = {};
    uint32_t b = 0;
    std::lock_guard<std::mutex> l(g_mouse_mu);
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (fg && pid == GetCurrentProcessId()) {
        // GameInputMouseButtons bits 0..4 are left, right, middle, 4, 5: the raw_button order.
        uint64_t t = now_us();
        b = g_buttons;
        for (int i = 0; i < 5; i++)
            if (g_press_until[i] > t) b |= 1u << i;
        // Also the button state itself, in case raw input does not arrive.
        const int vks[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
        for (int i = 0; i < 5; i++) {
            int vk = swapped && i < 2 ? vks[1 - i] : vks[i];
            if (down(vk)) b |= 1u << i;
        }
        POINT p;
        RECT r;
        if (GetCursorPos(&p) && ScreenToClient(fg, &p) && GetClientRect(fg, &r) && r.right > 0 && r.bottom > 0) {
            p.x = p.x < 0 ? 0 : p.x >= r.right ? r.right - 1 : p.x;
            p.y = p.y < 0 ? 0 : p.y >= r.bottom ? r.bottom - 1 : p.y;
            int64_t rx = g_mouse_x, ry = g_mouse_y;
            int64_t rawdx = rx - g_last_raw_x, rawdy = ry - g_last_raw_y;
            g_last_raw_x = rx;
            g_last_raw_y = ry;
            // Pinned at an edge and pushed further: the raw motion beyond what the pointer moved.
            if ((p.x == 0 && rawdx < 0) || (p.x == r.right - 1 && rawdx > 0)) g_extra_x += rawdx - (p.x - g_last_client.x);
            if ((p.y == 0 && rawdy < 0) || (p.y == r.bottom - 1 && rawdy > 0)) g_extra_y += rawdy - (p.y - g_last_client.y);
            g_last_client = p;
            // In the title's coordinates: its window reports the console display size.
            LONG dw, dh;
            LONG sx = p.x, sy = p.y;
            if (xsl::display_size(&dw, &dh)) {
                sx = (LONG)((int64_t)p.x * dw / r.right);
                sy = (LONG)((int64_t)p.y * dh / r.bottom);
            }
            POINT rep{(LONG)(sx + g_extra_x), (LONG)(sy + g_extra_y)};
            static bool trace = [] { char b[4]; return GetEnvironmentVariableA("XSL_INPUT_TRACE", b, 4) && b[0] == '1'; }();
            if (trace && (rep.x != g_report.x || rep.y != g_report.y))
                XSL_INFO(CH, "mouse at client %ld,%ld (extra %lld,%lld) -> position %ld,%ld", p.x, p.y, (long long)g_extra_x,
                         (long long)g_extra_y, rep.x, rep.y);
            g_report = rep;
        }
    }
    s->buttons = (GameInputMouseButtons)b;
    static bool firstButton = true;
    if (firstButton && b) {
        firstButton = false;
        XSL_INFO(CH, "mouse button seen (%#x)", b);
    }
    s->positionX = g_report.x;
    s->positionY = g_report.y;
    s->wheelX = g_wheel_x;
    s->wheelY = g_wheel_y;
}

// Mouse capture: a title that reads the mouse draws its own cursor (or turns its camera) from
// relative motion, which comes from raw input (Wine: XInput2 raw motion, which keeps arriving when
// the pointer is pinned at an edge). While one of our windows has focus the host pointer is hidden
// over it, so only the title's cursor is visible, and confined to its client area, so it cannot
// wander off and click other windows. No pointer warps: Xwayland may ignore them. Focus loss
// releases the pointer. XSL_MOUSE_CAPTURE=0 disables this.
std::atomic<bool> g_title_reads_mouse{false};
void update_mouse_capture() {
    static bool enabled = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_MOUSE_CAPTURE", b, 4) && b[0] == '0'); }();
    static HWND hidden = nullptr;
    static RECT clipped{};
    if (!enabled || !g_title_reads_mouse) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!fg || pid != GetCurrentProcessId() || IsIconic(fg)) {
        if (g_captured) {
            ClipCursor(nullptr);
            g_captured = false;
            clipped = {};
        }
        return;
    }
    RECT r;
    GetClientRect(fg, &r);
    MapWindowPoints(fg, nullptr, (POINT*)&r, 2);
    if (!g_captured || memcmp(&r, &clipped, sizeof r) != 0) {
        ClipCursor(&r);
        clipped = r;
    }
    if (hidden != fg) {
        SetClassLongPtrW(fg, GCLP_HCURSOR, 0);  // DefWindowProc then sets no cursor over it
        hidden = fg;
    }
    if (!g_captured) {
        g_captured = true;
        XSL_INFO(CH, "mouse captured: host pointer hidden and confined to the game window (XSL_MOUSE_CAPTURE=0 disables)");
    }
}

// ---- COM objects -------------------------------------------------------------------------
template <class I> struct RefCounted : I {
    std::atomic<ULONG> refs{1};
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG n = --refs;
        if (!n) delete this;
        return n;
    }
    virtual ~RefCounted() = default;
};

enum class Dev { Pad, Keyboard, Mouse };

struct Device final : IGameInputDevice {
    Dev type;
    DWORD slot = 0;  // pads: XInput user index
    GameInputDeviceInfo info{};
    GameInputKeyboardInfo kbInfo{};
    GameInputMouseInfo mouseInfo{};
    GameInputGamepadInfo padInfo{};
    std::atomic<bool> connected{false};
    std::atomic<uint64_t> seq{1};

    Device(Dev t, DWORD s) : type(t), slot(s) {
        info.infoSize = sizeof info;
        info.deviceFamily = t == Dev::Pad ? GameInputFamilyXboxOne : GameInputFamilyHid;
        info.vendorId = t == Dev::Pad ? 0x045E : 0;  // Microsoft
        info.productId = t == Dev::Pad ? 0x0B12 : 0;  // Xbox Series controller
        info.usage = t == Dev::Pad ? GameInputUsage{0x01, 0x05} : t == Dev::Keyboard ? GameInputUsage{0x01, 0x06} : GameInputUsage{0x01, 0x02};
        memset(&info.deviceId, 0, sizeof info.deviceId);
        const char tag[] = "openscarlett-gin";
        memcpy(&info.deviceId, tag, sizeof tag - 1);
        reinterpret_cast<uint8_t*>(&info.deviceId)[30] = (uint8_t)t;
        reinterpret_cast<uint8_t*>(&info.deviceId)[31] = (uint8_t)s;
        info.deviceRootId = info.deviceId;
        switch (t) {
            case Dev::Pad:
                info.supportedInput = GameInputKindGamepad;
                info.supportedRumbleMotors = (GameInputRumbleMotors)(GameInputRumbleLowFrequency | GameInputRumbleHighFrequency);
                info.gamepadInfo = &padInfo;
                break;
            case Dev::Keyboard:
                info.supportedInput = GameInputKindKeyboard;
                kbInfo.kind = GameInputAnsiKeyboard;
                kbInfo.keyCount = 104;
                kbInfo.functionKeyCount = 12;
                kbInfo.maxSimultaneousKeys = 16;
                info.keyboardInfo = &kbInfo;
                connected = true;
                break;
            case Dev::Mouse:
                info.supportedInput = GameInputKindMouse;
                mouseInfo.supportedButtons = (GameInputMouseButtons)0x1F;
                mouseInfo.sampleRate = 125;
                mouseInfo.sensorDpi = 800;
                mouseInfo.hasWheelX = mouseInfo.hasWheelY = true;
                info.mouseInfo = &mouseInfo;
                connected = true;
                break;
        }
    }
    GameInputDeviceStatus status() const {
        if (!connected) return GameInputDeviceNoStatus;
        uint32_t s = GameInputDeviceConnected | GameInputDeviceInputEnabled;
        if (type == Dev::Pad) s |= GameInputDeviceOutputEnabled;
        return (GameInputDeviceStatus)s;
    }

    // Devices live for the process (titles keep raw pointers across callbacks).
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IGameInputDevice)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(GameInputDeviceInfo const*) GetDeviceInfo() override { return &info; }
    IFACEMETHODIMP_(GameInputDeviceStatus) GetDeviceStatus() override { return status(); }
    IFACEMETHODIMP_(void) GetBatteryState(GameInputBatteryState* s) override {
        if (s) *s = {0, 0, 0, 0, GameInputBatteryUnknown};
    }
    IFACEMETHODIMP CreateForceFeedbackEffect(uint32_t, GameInputForceFeedbackParams const*, IGameInputForceFeedbackEffect** e) override {
        if (e) *e = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP_(bool) IsForceFeedbackMotorPoweredOn(uint32_t) override { return false; }
    IFACEMETHODIMP_(void) SetForceFeedbackMotorGain(uint32_t, float) override {}
    IFACEMETHODIMP SetHapticMotorState(uint32_t, GameInputHapticFeedbackParams const*) override { return E_NOTIMPL; }
    IFACEMETHODIMP_(void) SetRumbleState(GameInputRumbleParams const* p) override {
        if (type != Dev::Pad || !g_xset || !p) return;
        auto w = [](float f) { return (WORD)(f <= 0 ? 0 : f >= 1 ? 65535 : f * 65535); };
        XINPUT_VIBRATION v{w(p->lowFrequency), w(p->highFrequency)};
        g_xset(slot, &v);
    }
    IFACEMETHODIMP_(void) SetInputSynchronizationState(bool) override {}
    IFACEMETHODIMP_(void) SendInputSynchronizationHint() override {}
    IFACEMETHODIMP_(void) PowerOff() override {}
    IFACEMETHODIMP CreateRawDeviceReport(uint32_t, GameInputRawDeviceReportKind, IGameInputRawDeviceReport** r) override {
        if (r) *r = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetRawDeviceFeature(uint32_t, IGameInputRawDeviceReport** r) override {
        if (r) *r = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP SetRawDeviceFeature(IGameInputRawDeviceReport*) override { return E_NOTIMPL; }
    IFACEMETHODIMP SendRawDeviceOutput(IGameInputRawDeviceReport*) override { return E_NOTIMPL; }
    IFACEMETHODIMP SendRawDeviceOutputWithResponse(IGameInputRawDeviceReport*, IGameInputRawDeviceReport** r) override {
        if (r) *r = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP ExecuteRawDeviceIoControl(uint32_t, size_t, void const*, size_t, void*, size_t* o) override {
        if (o) *o = 0;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP_(bool) AcquireExclusiveRawDeviceAccess(uint64_t) override { return false; }
    IFACEMETHODIMP_(void) ReleaseExclusiveRawDeviceAccess() override {}
};

std::vector<Device*>& devices() {
    static std::vector<Device*> d = [] {
        std::vector<Device*> v;
        for (DWORD i = 0; i < 4; i++) v.push_back(new Device(Dev::Pad, i));
        v.push_back(new Device(Dev::Keyboard, 0));
        v.push_back(new Device(Dev::Mouse, 0));
        return v;
    }();
    return d;
}

void log_once(const char* what) {
    static std::mutex m;
    static std::vector<const char*> seen;
    std::lock_guard<std::mutex> l(m);
    for (auto* s : seen)
        if (s == what) return;
    seen.push_back(what);
    XSL_INFO(CH, "%s", what);
}
struct Reading;
std::mutex g_hist_mu;
std::atomic<uint64_t> g_seq{0};

struct Reading final : RefCounted<IGameInputReading> {
    GameInputKind kind = GameInputKindUnknown;
    Device* device = nullptr;
    GameInputGamepadState pad{};
    std::vector<GameInputKeyState> keys;
    GameInputMouseState mouse{};
    uint64_t ts = 0, seq = 0;

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IGameInputReading)) { AddRef(); *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(GameInputKind) GetInputKind() override { return kind; }
    IFACEMETHODIMP_(uint64_t) GetSequenceNumber(GameInputKind) override { return seq; }
    IFACEMETHODIMP_(uint64_t) GetTimestamp() override { return ts; }
    IFACEMETHODIMP_(void) GetDevice(IGameInputDevice** d) override { *d = device; }
    IFACEMETHODIMP_(bool) GetRawReport(IGameInputRawDeviceReport** r) override { *r = nullptr; return false; }
    IFACEMETHODIMP_(uint32_t) GetControllerAxisCount() override { return 0; }
    IFACEMETHODIMP_(uint32_t) GetControllerAxisState(uint32_t, float*) override { return 0; }
    IFACEMETHODIMP_(uint32_t) GetControllerButtonCount() override { return 0; }
    IFACEMETHODIMP_(uint32_t) GetControllerButtonState(uint32_t, bool*) override { return 0; }
    IFACEMETHODIMP_(uint32_t) GetControllerSwitchCount() override { return 0; }
    IFACEMETHODIMP_(uint32_t) GetControllerSwitchState(uint32_t, GameInputSwitchPosition*) override { return 0; }
    IFACEMETHODIMP_(uint32_t) GetKeyCount() override {
        return (kind & GameInputKindKeyboard) ? (uint32_t)keys.size() : 0; }
    IFACEMETHODIMP_(uint32_t) GetKeyState(uint32_t n, GameInputKeyState* out) override {
        if (!(kind & GameInputKindKeyboard) || !out) return 0;
        uint32_t c = n < keys.size() ? n : (uint32_t)keys.size();
        memcpy(out, keys.data(), c * sizeof(GameInputKeyState));
        return c;
    }
    IFACEMETHODIMP_(bool) GetMouseState(GameInputMouseState* s) override {
        g_title_reads_mouse = true;
        if (!(kind & GameInputKindMouse)) return false;
        *s = mouse;
        return true;
    }
    IFACEMETHODIMP_(uint32_t) GetTouchCount() override { return 0; }
    IFACEMETHODIMP_(uint32_t) GetTouchState(uint32_t, GameInputTouchState*) override { return 0; }
    IFACEMETHODIMP_(bool) GetMotionState(GameInputMotionState*) override { return false; }
    IFACEMETHODIMP_(bool) GetArcadeStickState(GameInputArcadeStickState*) override { return false; }
    IFACEMETHODIMP_(bool) GetFlightStickState(GameInputFlightStickState*) override { return false; }
    IFACEMETHODIMP_(bool) GetGamepadState(GameInputGamepadState* s) override {
        if (!(kind & GameInputKindGamepad)) return false;
        *s = pad;
        return true;
    }
    IFACEMETHODIMP_(bool) GetRacingWheelState(GameInputRacingWheelState*) override { return false; }
    IFACEMETHODIMP_(bool) GetUiNavigationState(GameInputUiNavigationState*) override { return false; }
};

// A reading of `want` from one device, or from all devices when `dev` is null (aggregate).
Reading* read(GameInputKind want, Device* dev) {
    auto* r = new Reading();
    r->ts = now_us();
    uint32_t got = 0;
    uint64_t seq = 0;
    for (Device* d : devices()) {
        if (dev && d != dev) continue;
        if (!d->connected && d->type == Dev::Pad) continue;
        uint32_t k = d->info.supportedInput & want;
        if (!k || (got & k)) continue;  // the aggregate takes the first device of each kind
        switch (d->type) {
            case Dev::Pad: pad_state(d->slot, &r->pad); break;
            case Dev::Keyboard: keyboard_state(&r->keys); break;
            case Dev::Mouse: mouse_state(&r->mouse); break;
        }
        got |= k;
        seq += d->seq;
        if (!r->device) r->device = d;
    }
    if (!dev && (want & GameInputKindGamepad) && !(got & GameInputKindGamepad) && !g_title_reads_keyboard) {
        keyboard_as_pad(&r->pad);  // samples: keyboard stand-in for a missing pad
        got |= GameInputKindGamepad;
    }
    if (!got) {
        r->Release();
        return nullptr;
    }
    r->kind = (GameInputKind)got;
    r->seq = seq;
    if (dev || __builtin_popcount(got) > 1) r->device = dev;  // aggregate readings have no single device
    return r;
}

// Reading history: snapshots per device, oldest first.
constexpr size_t kHistory = 64;
std::vector<std::vector<Reading*>>& history() {
    static std::vector<std::vector<Reading*>> h(devices().size());
    return h;
}
size_t index_of(Device* d) {
    for (size_t i = 0; i < devices().size(); i++)
        if (devices()[i] == d) return i;
    return 0;
}
void record(Device* d) {
    Reading* r = read(d->info.supportedInput, d);
    if (!r) return;
    r->seq = ++g_seq;
    std::lock_guard<std::mutex> l(g_hist_mu);
    auto& h = history()[index_of(d)];
    h.push_back(r);
    if (h.size() > kHistory) {
        h.front()->Release();
        h.erase(h.begin());
    }
}
// The history reading of a matching device whose sequence number is the closest after (next) or
// before (previous) `seq`; for `seq` == UINT64_MAX and !next, the newest.
Reading* find(GameInputKind kind, Device* dev, uint64_t seq, bool next) {
    std::lock_guard<std::mutex> l(g_hist_mu);
    Reading* best = nullptr;
    for (size_t i = 0; i < devices().size(); i++) {
        Device* d = devices()[i];
        if ((dev && d != dev) || !(d->info.supportedInput & kind)) continue;
        for (Reading* r : history()[i]) {
            if (next ? (r->seq > seq && (!best || r->seq < best->seq)) : (r->seq < seq && (!best || r->seq > best->seq))) best = r;
        }
    }
    if (best) best->AddRef();
    return best;
}

// ---- Callbacks and the polling thread ----------------------------------------------------
struct DeviceCb {
    GameInputCallbackToken token;
    Device* device;
    GameInputKind kind;
    GameInputDeviceStatus filter;
    void* ctx;
    GameInputDeviceCallback fn;
    bool active;
};
struct ReadingCb {
    GameInputCallbackToken token;
    Device* device;
    GameInputKind kind;
    void* ctx;
    GameInputReadingCallback fn;
    bool active;
    uint64_t lastSeq;
};
std::mutex g_cb_mu;
std::vector<DeviceCb> g_dev_cbs;
std::vector<ReadingCb> g_read_cbs;
std::vector<std::pair<GameInputCallbackToken, std::vector<Device*>>> g_pending_enum;  // async enumerations
std::atomic<uint64_t> g_token{0};

bool matches(const DeviceCb& c, Device* d) { return (!c.device || c.device == d) && (d->info.supportedInput & c.kind); }

void fire_device(GameInputCallbackToken t, Device* d, GameInputDeviceStatus cur, GameInputDeviceStatus prev) {
    DeviceCb c{};
    {
        std::lock_guard<std::mutex> l(g_cb_mu);
        for (auto& x : g_dev_cbs)
            if (x.token == t && x.active) c = x;
    }
    if (c.fn) c.fn(t, c.ctx, d, now_us(), cur, prev);
}

// Snapshot of each device's state, to bump sequence numbers when it changes.
struct Snap {
    GameInputGamepadState pad{};
    std::vector<GameInputKeyState> keys;
    GameInputMouseState mouse{};
};

DWORD WINAPI poll_thread(void*) {
    std::vector<Snap> last(devices().size());
    for (unsigned tick = 0;; tick++) {
        // Async enumerations queued by RegisterDeviceCallback.
        decltype(g_pending_enum) todo;
        {
            std::lock_guard<std::mutex> l(g_cb_mu);
            todo.swap(g_pending_enum);
        }
        for (auto& [t, devs] : todo)
            for (Device* d : devs) fire_device(t, d, d->status(), GameInputDeviceNoStatus);
        if (tick % 6 == 0) update_mouse_capture();  // ~50 ms: window moves, resizes, focus
        // Hot-plug (pads), every ~250 ms.
        if (tick % 31 == 0) {
            for (Device* d : devices()) {
                if (d->type != Dev::Pad) continue;
                XINPUT_STATE x;
                bool now = (g_xget && g_xget(d->slot, &x) == ERROR_SUCCESS) || (d->slot == 0 && !input_script().empty());
                if (now == d->connected) continue;
                GameInputDeviceStatus prev = d->status();
                d->connected = now;
                XSL_INFO(CH, "gamepad %lu %s", d->slot, now ? "connected" : "disconnected");
                std::vector<GameInputCallbackToken> ts;
                {
                    std::lock_guard<std::mutex> l(g_cb_mu);
                    for (auto& c : g_dev_cbs)
                        if (c.active && matches(c, d) && ((prev ^ d->status()) & c.filter)) ts.push_back(c.token);
                }
                for (auto t : ts) fire_device(t, d, d->status(), prev);
            }
        }
        // State changes -> sequence numbers and reading callbacks.
        for (size_t i = 0; i < devices().size(); i++) {
            Device* d = devices()[i];
            if (!d->connected) continue;
            Snap s;
            bool changed = false;
            switch (d->type) {
                case Dev::Pad:
                    pad_state(d->slot, &s.pad);
                    changed = memcmp(&s.pad, &last[i].pad, sizeof s.pad) != 0;
                    break;
                case Dev::Keyboard:
                    keyboard_state(&s.keys);
                    changed = s.keys.size() != last[i].keys.size() ||
                              memcmp(s.keys.data(), last[i].keys.data(), s.keys.size() * sizeof(GameInputKeyState)) != 0;
                    break;
                case Dev::Mouse:
                    mouse_state(&s.mouse);
                    changed = memcmp(&s.mouse, &last[i].mouse, sizeof s.mouse) != 0;
                    break;
            }
            if (!changed) continue;
            last[i] = std::move(s);
            d->seq++;
            record(d);
            std::vector<ReadingCb> cbs;
            {
                std::lock_guard<std::mutex> l(g_cb_mu);
                for (auto& c : g_read_cbs)
                    if (c.active && (!c.device || c.device == d) && (d->info.supportedInput & c.kind)) cbs.push_back(c);
            }
            for (auto& c : cbs)
                if (Reading* r = find((GameInputKind)c.kind, d, UINT64_MAX, false)) {
                    c.fn(c.token, c.ctx, r, false);
                    r->Release();
                }
        }
        Sleep(8);
    }
}

void start_threads() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (Device* d : devices()) {
            XINPUT_STATE x;
            if (d->type == Dev::Pad) d->connected = (g_xget && g_xget(d->slot, &x) == ERROR_SUCCESS) || (d->slot == 0 && !input_script().empty());   // scripted input: a virtual pad
            if (d->connected) record(d);
        }
        CloseHandle(CreateThread(nullptr, 0, raw_thread, nullptr, 0, nullptr));
        CloseHandle(CreateThread(nullptr, 0, poll_thread, nullptr, 0, nullptr));
    });
}

// One log line per (call, kind, per-device or aggregate), to show how a title reads input.
void log_call(const char* what, GameInputKind kind, IGameInputDevice* dev) {
    static std::mutex m;
    static std::vector<std::string> seen;
    char key[96];
    snprintf(key, sizeof key, "%s kind %#x %s", what, (unsigned)kind, dev ? "per device" : "aggregate");
    std::lock_guard<std::mutex> l(m);
    for (auto& s : seen)
        if (s == key) return;
    seen.push_back(key);
    XSL_INFO(CH, "%s", key);
}

void note_kind(GameInputKind k) {
    if (k & GameInputKindKeyboard) g_title_reads_keyboard = true;
}

struct GameInput final : RefCounted<IGameInput> {
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IGameInput)) { AddRef(); *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(uint64_t) GetCurrentTimestamp() override { return now_us(); }
    IFACEMETHODIMP GetCurrentReading(GameInputKind kind, IGameInputDevice* device, IGameInputReading** out) override {
        if (!out) return E_POINTER;
        note_kind(kind);
        // A fresh object per call (a copy of the newest snapshot): titles compare reading pointers
        // to detect new input, as GameInput hands out new reading objects.
        if (Reading* newest = find(kind, static_cast<Device*>(device), UINT64_MAX, false)) {
            auto* r = new Reading();
            r->kind = newest->kind;
            r->device = newest->device;
            r->pad = newest->pad;
            r->keys = newest->keys;
            r->mouse = newest->mouse;
            r->ts = newest->ts;
            r->seq = newest->seq;
            newest->Release();
            *out = r;
        } else {
            *out = nullptr;
        }
        if (!*out && !device && (kind & GameInputKindGamepad) && !g_title_reads_keyboard) *out = read(kind, nullptr);  // keyboard stand-in
        {
            static std::mutex m;
            static std::vector<std::pair<unsigned, IGameInputDevice*>> seen;
            std::lock_guard<std::mutex> l(m);
            bool known = false;
            for (auto& x : seen) known |= x.first == (unsigned)kind && x.second == device;
            if (!known) {
                seen.push_back({(unsigned)kind, device});
                bool ours = false;
                for (Device* d : devices()) ours |= d == device;
                XSL_INFO(CH, "GetCurrentReading(kind %#x, %s device) -> %s", (unsigned)kind, ours ? "per" : device ? "UNKNOWN" : "no",
                         *out ? "reading" : "not found");
            }
        }
        return *out ? S_OK : GAMEINPUT_E_READING_NOT_FOUND;
    }
    IFACEMETHODIMP GetNextReading(IGameInputReading* ref, GameInputKind kind, IGameInputDevice* device, IGameInputReading** out) override {
        if (!out) return E_POINTER;
        note_kind(kind);
        log_call("GetNextReading", kind, device);
        *out = ref ? find(kind, static_cast<Device*>(device), static_cast<Reading*>(ref)->seq, true) : nullptr;
        return *out ? S_OK : GAMEINPUT_E_READING_NOT_FOUND;
    }
    IFACEMETHODIMP GetPreviousReading(IGameInputReading* ref, GameInputKind kind, IGameInputDevice* device, IGameInputReading** out) override {
        if (!out) return E_POINTER;
        note_kind(kind);
        log_call("GetPreviousReading", kind, device);
        *out = ref ? find(kind, static_cast<Device*>(device), static_cast<Reading*>(ref)->seq, false) : nullptr;
        return *out ? S_OK : GAMEINPUT_E_READING_NOT_FOUND;
    }
    IFACEMETHODIMP GetTemporalReading(uint64_t, IGameInputDevice*, IGameInputReading** out) override {
        *out = nullptr;
        return GAMEINPUT_E_READING_NOT_FOUND;
    }
    IFACEMETHODIMP RegisterReadingCallback(IGameInputDevice* device, GameInputKind kind, float, void* ctx, GameInputReadingCallback fn,
                                           GameInputCallbackToken* t) override {
        note_kind(kind);
        GameInputCallbackToken tok = ++g_token;
        if (t) *t = tok;
        std::lock_guard<std::mutex> l(g_cb_mu);
        g_read_cbs.push_back({tok, static_cast<Device*>(device), kind, ctx, fn, fn != nullptr, 0});
        return S_OK;
    }
    IFACEMETHODIMP RegisterDeviceCallback(IGameInputDevice* device, GameInputKind kind, GameInputDeviceStatus filter,
                                          GameInputEnumerationKind enumeration, void* ctx, GameInputDeviceCallback fn,
                                          GameInputCallbackToken* t) override {
        note_kind(kind);
        GameInputCallbackToken tok = ++g_token;
        if (t) *t = tok;
        DeviceCb c{tok, static_cast<Device*>(device), kind, filter, ctx, fn, fn != nullptr};
        std::vector<Device*> now;
        for (Device* d : devices())
            if (d->connected && matches(c, d)) now.push_back(d);
        {
            std::lock_guard<std::mutex> l(g_cb_mu);
            g_dev_cbs.push_back(c);
            if (enumeration == GameInputAsyncEnumeration && !now.empty()) g_pending_enum.push_back({tok, now});
        }
        XSL_INFO(CH, "RegisterDeviceCallback(kind %#x, %s enumeration): %zu device(s)", (unsigned)kind,
                 enumeration == GameInputBlockingEnumeration ? "blocking" : enumeration == GameInputAsyncEnumeration ? "async" : "no",
                 now.size());
        if (enumeration == GameInputBlockingEnumeration)
            for (Device* d : now) fire_device(tok, d, d->status(), GameInputDeviceNoStatus);
        return S_OK;
    }
    IFACEMETHODIMP RegisterGuideButtonCallback(IGameInputDevice*, void*, GameInputGuideButtonCallback, GameInputCallbackToken* t) override {
        if (t) *t = ++g_token;
        return S_OK;
    }
    IFACEMETHODIMP RegisterKeyboardLayoutCallback(IGameInputDevice*, void*, GameInputKeyboardLayoutCallback, GameInputCallbackToken* t) override {
        if (t) *t = ++g_token;
        return S_OK;
    }
    IFACEMETHODIMP_(void) StopCallback(GameInputCallbackToken t) override {
        std::lock_guard<std::mutex> l(g_cb_mu);
        for (auto& c : g_dev_cbs)
            if (c.token == t) c.active = false;
        for (auto& c : g_read_cbs)
            if (c.token == t) c.active = false;
    }
    IFACEMETHODIMP_(bool) UnregisterCallback(GameInputCallbackToken t, uint64_t) override {
        std::lock_guard<std::mutex> l(g_cb_mu);
        for (auto it = g_dev_cbs.begin(); it != g_dev_cbs.end(); ++it)
            if (it->token == t) { g_dev_cbs.erase(it); return true; }
        for (auto it = g_read_cbs.begin(); it != g_read_cbs.end(); ++it)
            if (it->token == t) { g_read_cbs.erase(it); return true; }
        return false;
    }
    IFACEMETHODIMP CreateDispatcher(IGameInputDispatcher** d) override { *d = nullptr; return E_NOTIMPL; }
    IFACEMETHODIMP CreateAggregateDevice(GameInputKind, IGameInputDevice** d) override { *d = nullptr; return E_NOTIMPL; }
    IFACEMETHODIMP FindDeviceFromId(APP_LOCAL_DEVICE_ID const* id, IGameInputDevice** d) override {
        *d = nullptr;
        for (Device* x : devices())
            if (id && !memcmp(id, &x->info.deviceId, sizeof *id)) { *d = x; return S_OK; }
        return GAMEINPUT_E_DEVICE_NOT_FOUND;
    }
    IFACEMETHODIMP FindDeviceFromObject(IUnknown*, IGameInputDevice** d) override { *d = nullptr; return GAMEINPUT_E_DEVICE_NOT_FOUND; }
    IFACEMETHODIMP FindDeviceFromPlatformHandle(HANDLE, IGameInputDevice** d) override { *d = nullptr; return GAMEINPUT_E_DEVICE_NOT_FOUND; }
    IFACEMETHODIMP FindDeviceFromPlatformString(LPCWSTR, IGameInputDevice** d) override { *d = nullptr; return GAMEINPUT_E_DEVICE_NOT_FOUND; }
    IFACEMETHODIMP EnableOemDeviceSupport(uint16_t, uint16_t, uint8_t, uint8_t) override { return S_OK; }
    IFACEMETHODIMP_(void) SetFocusPolicy(GameInputFocusPolicy) override {}
};
}  // namespace

extern "C" HRESULT WINAPI GameInputCreate(IGameInput** out) {
    if (!out) return E_POINTER;
    start_threads();
    *out = new GameInput();
    unsigned pads = 0;
    for (Device* d : devices()) pads += d->type == Dev::Pad && d->connected;
    XSL_INFO(CH, "GameInputCreate: %u gamepad(s) over %s, keyboard, mouse", pads, g_xget ? "XInput" : "nothing (no XInput)");
    return S_OK;
}
