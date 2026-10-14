// Host platform layer: window, keyboard/XInput controller and XFB presentation
// (audio output is in audio.cpp).
#include "../common.h"
#include <windows.h>
#include <xinput.h>
#include <timeapi.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "xinput.lib")

static HWND s_wnd;
static bool s_quit;
static bool s_keys[256];
static bool s_fullscreen;
static WINDOWPLACEMENT s_windowed_placement = {sizeof(WINDOWPLACEMENT)};

HWND host_window() { return s_wnd; }

// Borderless fullscreen: a popup window covering the monitor the window is on.  With the flip-model
// swap chain DWM hands it the display directly (independent flip), so this costs nothing over
// exclusive fullscreen and switches instantly.
static void set_fullscreen(bool on) {
    if (on == s_fullscreen) return;
    s_fullscreen = on;
    if (on) {
        GetWindowPlacement(s_wnd, &s_windowed_placement);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoA(MonitorFromWindow(s_wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(s_wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(s_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        LOG("host", "fullscreen %ldx%ld at %ld,%ld", mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top, mi.rcMonitor.left, mi.rcMonitor.top);
    } else {
        SetWindowLongA(s_wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        if (s_windowed_placement.showCmd == SW_SHOWNORMAL) s_windowed_placement.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(s_wnd, &s_windowed_placement);
        SetWindowPos(s_wnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        LOG("host", "windowed");
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: s_quit = true; return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) {   // Alt+Enter (not passed on as Start)
            if (!(l & (1 << 30))) { set_fullscreen(!s_fullscreen); cfg_set("video.fullscreen", s_fullscreen ? "1" : "0"); }
            return 0;
        }
        if (w == VK_F4) break;   // Alt+F4: close
        s_keys[w & 255] = true;
        return 0;
    case WM_KEYDOWN:
        if (w == VK_F11) {
            if (!(l & (1 << 30))) { set_fullscreen(!s_fullscreen); cfg_set("video.fullscreen", s_fullscreen ? "1" : "0"); }
            return 0;
        }
        s_keys[w & 255] = true;
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP: s_keys[w & 255] = false; return 0;
    case WM_KILLFOCUS: memset(s_keys, 0, sizeof s_keys); return 0;
    case WM_SETCURSOR:
        if (s_fullscreen && LOWORD(l) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

void host_init() {
    // 1 ms scheduler granularity: the idle loop sleeps until the next VI field / timer event, and with the
    // default 15.6 ms tick a Sleep(3) can overshoot by a whole field and make the game miss a frame
    timeBeginPeriod(1);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "DoubleDash";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, std::clamp(cfg_int("video.window_width", 1280), 160, 16384),
              std::clamp(cfg_int("video.window_height", 960), 120, 16384)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    // DD_MONITOR=N: open centred on display N (1-based, system enumeration order) without taking the focus
    // (test runs on a second screen)
    const char* mon = getenv("DD_MONITOR");
    struct Find { int want, n; RECT work; bool found; } f = {mon ? atoi(mon) : 0, 0, {}, false};
    if (f.want > 0)
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            Find& f = *(Find*)p;
            if (++f.n != f.want) return TRUE;
            MONITORINFO mi = {sizeof mi};
            GetMonitorInfoA(m, &mi);
            f.work = mi.rcWork;
            f.found = true;
            return FALSE;
        }, (LPARAM)&f);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (f.found) {
        x = f.work.left + std::max(0, (int)(f.work.right - f.work.left - w) / 2);
        y = f.work.top + std::max(0, (int)(f.work.bottom - f.work.top - h) / 2);
    }
    s_wnd = CreateWindowA("DoubleDash", "Mario Kart: Double Dash!!", WS_OVERLAPPEDWINDOW | (f.found ? 0 : WS_VISIBLE), x, y,
                          w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (f.found) {
        ShowWindow(s_wnd, SW_SHOWNOACTIVATE);
        // the console window (console-subsystem exe) goes to the same display's top-left corner
        if (HWND con = GetConsoleWindow())
            SetWindowPos(con, nullptr, f.work.left, f.work.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (cfg_bool("video.fullscreen", false, "DD_FULLSCREEN")) set_fullscreen(true);   // on the window's display
}

void host_shutdown() {
    for (int port = 0; port < 4; port++) host_set_rumble(port, false);
}

void host_pump_messages() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

bool host_should_quit() { return s_quit; }

// ---------------------------------------------------------------- input
struct PadState { u16 buttons; u8 sx, sy, cx, cy, l, r; bool connected; };

enum : u16 {
    PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008, PAD_Z = 0x0010, PAD_R = 0x0020,
    PAD_L = 0x0040, PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400, PAD_Y = 0x0800, PAD_START = 0x1000,
};

static u8 axis(float v) {   // -1..1 -> 0..255 around 0x80
    int i = (int)(128 + v * 100);
    return (u8)(i < 0 ? 0 : i > 255 ? 255 : i);
}

// XInputGetState is slow for an empty slot (it probes for a device every time) and the SI asks for
// every port several times per field: a connected pad is read on every call, an empty slot is probed
// once per second.
static bool xinput_state(int port, XINPUT_STATE* out) {
    struct Slot { XINPUT_STATE st; bool connected; u64 last_ms; bool polled; };
    static Slot slots[4];
    if (port < 0 || port >= 4) return false;
    Slot& sl = slots[port];
    u64 now = GetTickCount64();
    if (!sl.polled || sl.connected || now - sl.last_ms >= 1000) {
        sl.connected = XInputGetState(port, &sl.st) == ERROR_SUCCESS;
        sl.last_ms = now;
        sl.polled = true;
    }
    *out = sl.st;
    return sl.connected;
}

// GameCube pads have one on/off rumble motor; on an XInput pad it drives both motors
void host_set_rumble(int port, bool on) {
    static const bool enabled = cfg_bool("input.rumble", true);
    static const int strength = std::clamp(cfg_int("input.rumble_strength", 100), 0, 100);
    static bool state[4];
    if (port < 0 || port >= 4 || state[port] == on || (on && !enabled)) return;
    state[port] = on;
    static int logged;
    if (logged < 6) LOG("host", "pad %d rumble %s%s", port + 1, on ? "on" : "off", ++logged == 6 ? " (not logged further)" : "");
    XINPUT_VIBRATION v = {};
    if (on) v.wLeftMotorSpeed = v.wRightMotorSpeed = (WORD)(65535 * strength / 100);
    XInputSetState(port, &v);
}

PadState host_get_pad(int port) {
    PadState p = {0, 0x80, 0x80, 0x80, 0x80, 0, 0, port == 0};
    XINPUT_STATE xs;
    bool have_x = xinput_state(port, &xs);
    if (have_x) {
        p.connected = true;
        auto& g = xs.Gamepad;
        auto stick = [](SHORT v, SHORT dz) { if (abs(v) < dz) return 0.0f; return v / 32767.0f; };
        p.sx = axis(stick(g.sThumbLX, 7849));
        p.sy = axis(stick(g.sThumbLY, 7849));
        p.cx = axis(stick(g.sThumbRX, 8689));
        p.cy = axis(stick(g.sThumbRY, 8689));
        p.l = g.bLeftTrigger;
        p.r = g.bRightTrigger;
        WORD b = g.wButtons;
        if (b & XINPUT_GAMEPAD_A) p.buttons |= PAD_A;
        if (b & XINPUT_GAMEPAD_B) p.buttons |= PAD_B;
        if (b & XINPUT_GAMEPAD_X) p.buttons |= PAD_X;
        if (b & XINPUT_GAMEPAD_Y) p.buttons |= PAD_Y;
        if (b & XINPUT_GAMEPAD_START) p.buttons |= PAD_START;
        if (b & (XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_LEFT_SHOULDER)) p.buttons |= PAD_Z;
        if (b & XINPUT_GAMEPAD_BACK) p.buttons |= PAD_START;
        if (b & XINPUT_GAMEPAD_DPAD_UP) p.buttons |= PAD_UP;
        if (b & XINPUT_GAMEPAD_DPAD_DOWN) p.buttons |= PAD_DOWN;
        if (b & XINPUT_GAMEPAD_DPAD_LEFT) p.buttons |= PAD_LEFT;
        if (b & XINPUT_GAMEPAD_DPAD_RIGHT) p.buttons |= PAD_RIGHT;
        if (p.l > 200) p.buttons |= PAD_L;
        if (p.r > 200) p.buttons |= PAD_R;
    }
    if (port == 0 && GetForegroundWindow() == s_wnd) {
        // keyboard (see README): arrows/WASD steer, Space/X accelerate, Z/Backspace brake,
        // Shift/E drift (R), Q = L, C/Ctrl item (X), V = Y, F/Tab swap (Z), Enter/Esc start
        auto k = [](int vk) { return s_keys[vk]; };
        float x = 0, y = 0;
        if (k(VK_LEFT) || k('A')) x -= 1;
        if (k(VK_RIGHT) || k('D')) x += 1;
        if (k(VK_UP) || k('W')) y += 1;
        if (k(VK_DOWN) || k('S')) y -= 1;
        if (x || y) { p.sx = axis(x); p.sy = axis(y); }
        if (k(VK_SPACE) || k('X')) p.buttons |= PAD_A;
        if (k('Z') || k(VK_BACK)) p.buttons |= PAD_B;
        if (k('C') || k(VK_CONTROL)) p.buttons |= PAD_X;
        if (k('V')) p.buttons |= PAD_Y;
        if (k('Q')) { p.buttons |= PAD_L; p.l = 255; }
        if (k(VK_SHIFT) || k('E')) { p.buttons |= PAD_R; p.r = 255; }
        if (k('F') || k(VK_TAB)) p.buttons |= PAD_Z;
        if (k(VK_RETURN) || k(VK_ESCAPE)) p.buttons |= PAD_START;
        if (k('I')) p.buttons |= PAD_UP;
        if (k('K')) p.buttons |= PAD_DOWN;
        if (k('J')) p.buttons |= PAD_LEFT;
        if (k('L')) p.buttons |= PAD_RIGHT;
    }
    // DD_AUTOPRESS=N: for N seconds cycle Start, stick-left, A (one per second) to get through
    // the menus, then hold A (accelerate).  Headless testing aid.
    static const int autopress = getenv("DD_AUTOPRESS") ? atoi(getenv("DD_AUTOPRESS")) : 0;
    static const u64 t_start = GetTickCount64();
    if (autopress > 0 && port == 0) {
        u64 t = GetTickCount64() / 100;
        if (GetTickCount64() - t_start > (u64)autopress * 1000) p.buttons |= PAD_A;
        else if (t % 10 < 2) {
            switch ((t / 10) % 3) {
            case 0: p.buttons |= PAD_START; break;
            case 1: p.sx = axis(-1); break;
            case 2: p.buttons |= PAD_A; break;
            }
        }
    }
    bool dbg_script_pad(int port, u16& buttons, u8& sx, u8& sy);   // host/debug.cpp: DD_SCRIPT
    if (dbg_script_pad(port, p.buttons, p.sx, p.sy)) {
        p.connected = true;
        if (p.buttons & PAD_L) p.l = 255;   // scripted L/R: fully pressed analog trigger, as on the keyboard
        if (p.buttons & PAD_R) p.r = 255;
    }
    return p;
}

// ---------------------------------------------------------------- XFB presentation (software YUV decode)
static std::vector<u32> s_rgb;

static inline u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

bool render_present_efb();   // gx/render.cpp: true if the renderer presented this field itself

void host_present_field(u32 xfb, u32 stride, u32 height) {
    if (render_present_efb()) return;
    if (!xfb || !s_wnd || height == 0 || height > 1024) return;
    u32 w = stride / 2;
    if (w == 0 || w > 1024) w = 640;
    s_rgb.resize((size_t)w * height);
    const u8* p = host_ptr(0x80000000u | xfb);
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < w; x += 2) {
            const u8* q = p + (size_t)y * stride + x * 2;
            int y0 = q[0] - 16, u = q[1] - 128, y1 = q[2] - 16, v = q[3] - 128;
            auto px = [&](int yy) {
                int r = (298 * yy + 409 * v + 128) >> 8, g = (298 * yy - 100 * u - 208 * v + 128) >> 8,
                    b = (298 * yy + 516 * u + 128) >> 8;
                return (u32)clamp8(r) << 16 | (u32)clamp8(g) << 8 | clamp8(b);
            };
            s_rgb[(size_t)y * w + x] = px(y0);
            s_rgb[(size_t)y * w + x + 1] = px(y1);
        }
    }
    HDC dc = GetDC(s_wnd);
    RECT rc;
    GetClientRect(s_wnd, &rc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, w, height, s_rgb.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(s_wnd, dc);
}
