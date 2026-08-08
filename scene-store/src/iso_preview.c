/*
 * iso_preview.c — Windows GDI live preview of the full scene stack.
 *
 * Renders the compositor framebuffer into a GDI window so you can see
 * the shell + wallpaper running without wlroots/DRM. Uses the same
 * loopback transport + compositor + client + shell code as on Linux.
 */
#ifdef _WIN32

#include "scene_shell.h"
#include "scene_compositor.h"
#include "scene_transport.h"
#include "scene_client.h"
#include "scene_server.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static LONG WINAPI veh_handler(EXCEPTION_POINTERS *ep) {
    char msg[512];
    snprintf(msg, sizeof(msg), "CRASH: exception 0x%lX at %p\n",
             ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
    OutputDebugStringA(msg);
    MessageBoxA(NULL, msg, "ISO Preview Crash", MB_OK);
    _exit(1);
    return EXCEPTION_CONTINUE_SEARCH;
}

#define FPS 30
#define TIMER_ID 1
#define WM_INIT_STACK (WM_USER + 1)

static int g_cw = 0;     /* client width, set in WM_CREATE from actual rect */
static int g_ch = 0;     /* client height, set in WM_CREATE from actual rect */

static scene_loopback    *g_lb;
static scene_transport   *g_server_ts;
static scene_client      *g_cl;
static scene_compositor  *g_cp;
static scene_shell       *g_shell;
static BITMAPINFO         g_bmi;
static HBITMAP            g_hbmp;
static void              *g_bmp_bits;
static int                g_needs_paint;
static int                g_mouse_down;
static int                g_fullscreen;
static RECT               g_win_rect;
static DWORD              g_win_style;
static scene_style_ref    g_hover_ref;
static scene_style_ref    g_active_ref;

static void pump_loop(void)
{
    scene_client_flush(g_cl);
    uint8_t buf[8192];
    uint32_t got;
    while (scene_transport_recv(g_server_ts, buf, sizeof(buf), &got) == 0
           && got) {
        scene_server_feed(scene_compositor_server(g_cp), buf, got);
    }
    const uint8_t *f;
    uint32_t flen;
    while (scene_server_out_next_frame(scene_compositor_server(g_cp),
                                       &f, &flen) == 1)
        scene_transport_send(g_server_ts, f, flen);
    scene_client_pump(g_cl);
}

static void pump_and_render(void)
{
    scene_shell_tick(g_shell);

    int round;
    for (round = 0; round < 4; round++) {
        scene_client_flush(g_cl);
        uint8_t buf[8192];
        uint32_t got;
        while (scene_transport_recv(g_server_ts, buf, sizeof(buf), &got) == 0
               && got) {
            scene_server_feed(scene_compositor_server(g_cp), buf, got);
        }
        const uint8_t *f;
        uint32_t flen;
        while (scene_server_out_next_frame(scene_compositor_server(g_cp),
                                           &f, &flen) == 1)
            scene_transport_send(g_server_ts, f, flen);
        scene_client_pump(g_cl);
    }

    scene_compositor_force_repaint(g_cp);
    scene_compositor_frame(g_cp);

    const scene_fb *fb = scene_compositor_fb(g_cp);
    if (!fb || !fb->px) { g_needs_paint = 1; return; }

    uint32_t y;
    for (y = 0; y < fb->h && y < (uint32_t)g_ch; y++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)g_bmp_bits +
                         (size_t)y * g_cw * 4);
        const uint32_t *src = fb->px + y * fb->pitch;
        uint32_t x;
        for (x = 0; x < fb->w && x < (uint32_t)g_cw; x++) {
            uint32_t p = src[x];
            uint32_t a = (p >> 24) & 0xFF;
            if (a == 0)   { dst[x] = 0x00000000; continue; }
            if (a == 255) { dst[x] = p; continue; }
            uint32_t r = ((p >> 16) & 0xFF) * 255 / a;
            uint32_t g = ((p >>  8) & 0xFF) * 255 / a;
            uint32_t b = ((p      ) & 0xFF) * 255 / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            dst[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
    g_needs_paint = 1;
}

static void init_stack(void)
{
    g_lb = scene_loopback_new();
    g_server_ts = scene_loopback_server_end(g_lb);
    g_cl = scene_client_new();
    g_cp = scene_compositor_new(NULL, g_cw, g_ch);
    scene_compositor_set_effects(g_cp, 1);
    scene_compositor_set_clear(g_cp, 0xFF1A1A2E);

    /* Set up hover and active styles for interactive feedback */
    g_hover_ref = scene_compositor_setup_hover_style(g_cp,
        0xFF2A2A4E,   /* fill: blue-gray hover */
        0xFFFFFFFF);  /* text: white */
    g_active_ref = scene_compositor_setup_active_style(g_cp,
        0xFF4A4A6E,   /* fill: brighter blue-gray for focused */
        0xFFFFFFFF);  /* text: white */

    scene_server_attach(scene_compositor_server(g_cp));
    scene_client_connect(g_cl, scene_loopback_client_end(g_lb),
                         "iso-preview", NULL, NULL);

    /* Pump WELCOME — cli_emit requires welcomed=1 before any ops */
    pump_loop();

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.wallpaper_mode = SCENE_WP_STATIC;

    /* Resolve wallpaper path relative to the exe directory */
    char exe_path[512] = "wallpaper.bmp";
    DWORD len = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path) - 16);
    if (len > 0) {
        char *slash = strrchr(exe_path, '\\');
        if (slash) { slash[1] = '\0'; strcat(exe_path, "wallpaper.bmp"); }
    }
    strncpy(cfg.wallpaper_path, exe_path, sizeof(cfg.wallpaper_path) - 1);
    cfg.panel_height = 32;
    cfg.clock_12h = 0;

    g_shell = scene_shell_new(g_cl, scene_compositor_store(g_cp),
                              g_cp, &cfg);
    scene_shell_set_hover_style(g_shell, g_hover_ref);
    scene_shell_set_active_style(g_shell, g_active_ref);
    scene_shell_build(g_shell, g_cw, g_ch);

    int i;
    for (i = 0; i < 8; i++)
        pump_loop();
    scene_shell_tick(g_shell);
    for (i = 0; i < 4; i++)
        pump_loop();

    scene_compositor_force_repaint(g_cp);
    scene_compositor_frame(g_cp);
    pump_loop();
    g_needs_paint = 1;
}

static void handle_input_pointer(int32_t x, int32_t y, uint8_t buttons)
{
    scene_node_id hit = scene_shell_handle_pointer(g_shell, x, y, buttons);
    if (hit && (buttons & 0x01)) {
        scene_shell_handle_activate(g_shell, hit);
    }
    scene_compositor_input_pointer(g_cp, 0, x, y, buttons);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, TIMER_ID, 1000 / FPS, NULL);
        /* Defer init to after the window is fully sized */
        PostMessage(hwnd, WM_INIT_STACK, 0, 0);
        return 0;

    case WM_INIT_STACK: {
        /* Now the window is fully created — GetClientRect is reliable */
        RECT rc;
        GetClientRect(hwnd, &rc);
        g_cw = rc.right - rc.left;
        g_ch = rc.bottom - rc.top;
        if (g_cw < 1) g_cw = 1;
        if (g_ch < 1) g_ch = 1;

        /* Create bitmap matching the actual client area */
        memset(&g_bmi, 0, sizeof(g_bmi));
        g_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        g_bmi.bmiHeader.biWidth       = g_cw;
        g_bmi.bmiHeader.biHeight      = -g_ch;
        g_bmi.bmiHeader.biPlanes      = 1;
        g_bmi.bmiHeader.biBitCount    = 32;
        g_bmi.bmiHeader.biCompression = BI_RGB;

        HDC hdc = GetDC(hwnd);
        g_hbmp = CreateDIBSection(hdc, &g_bmi, DIB_RGB_COLORS,
                                  &g_bmp_bits, NULL, 0);
        ReleaseDC(hwnd, hdc);
        memset(g_bmp_bits, 0, (size_t)g_cw * g_ch * 4);

        init_stack();

        /* Debug: show actual dimensions in title bar */
        {
            char dbg[128];
            snprintf(dbg, sizeof(dbg), "ISO Preview [%dx%d]", g_cw, g_ch);
            SetWindowTextA(hwnd, dbg);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_ID) {
            pump_and_render();
            if (g_needs_paint) {
                InvalidateRect(hwnd, NULL, FALSE);
                g_needs_paint = 0;
            }
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        HDC memdc = CreateCompatibleDC(hdc);
        HBITMAP oldbmp = (HBITMAP)SelectObject(memdc, g_hbmp);
        BitBlt(hdc, 0, 0, g_cw, g_ch, memdc, 0, 0, SRCCOPY);
        SelectObject(memdc, oldbmp);
        DeleteDC(memdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int32_t x = (int32_t)(short)LOWORD(lp);
        int32_t y = (int32_t)(short)HIWORD(lp);
        g_mouse_down = 1;
        SetCapture(hwnd);
        handle_input_pointer(x, y, 0x01);
        pump_and_render();
        g_needs_paint = 1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        int32_t x = (int32_t)(short)LOWORD(lp);
        int32_t y = (int32_t)(short)HIWORD(lp);
        g_mouse_down = 0;
        ReleaseCapture();
        handle_input_pointer(x, y, 0x00);
        pump_and_render();
        g_needs_paint = 1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int32_t x = (int32_t)(short)LOWORD(lp);
        int32_t y = (int32_t)(short)HIWORD(lp);
        uint8_t btn = g_mouse_down ? 0x01 : 0x00;
        scene_shell_handle_pointer(g_shell, x, y, btn);
        if (g_mouse_down) {
            scene_compositor_input_pointer(g_cp, 0, x, y, btn);
            pump_and_render();
            g_needs_paint = 1;
            InvalidateRect(hwnd, NULL, FALSE);
        } else {
            pump_and_render();
            g_needs_paint = 1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        uint32_t key = (uint32_t)wp;
        uint8_t mod = 0;
        if (GetKeyState(VK_SHIFT) & 0x8000) mod |= 0x01;
        if (GetKeyState(VK_CONTROL) & 0x8000) mod |= 0x02;
        if (GetKeyState(VK_MENU) & 0x8000) mod |= 0x04;

        /* F11 = toggle fullscreen */
        if (key == VK_F11) {
            int new_w, new_h;
            if (!g_fullscreen) {
                GetWindowRect(hwnd, &g_win_rect);
                g_win_style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
                SetWindowLongPtr(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                MONITORINFO mi;
                memset(&mi, 0, sizeof(mi));
                mi.cbSize = sizeof(mi);
                GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
                RECT wa = mi.rcWork;
                new_w = wa.right - wa.left;
                new_h = wa.bottom - wa.top;
                SetWindowPos(hwnd, NULL, wa.left, wa.top, new_w, new_h,
                             SWP_FRAMECHANGED);
                g_fullscreen = 1;
            } else {
                SetWindowLongPtr(hwnd, GWL_STYLE, g_win_style);
                new_w = g_win_rect.right - g_win_rect.left;
                new_h = g_win_rect.bottom - g_win_rect.top;
                SetWindowPos(hwnd, NULL,
                             g_win_rect.left, g_win_rect.top,
                             new_w, new_h,
                             SWP_FRAMECHANGED);
                g_fullscreen = 0;
            }

            if (new_w > 0 && new_h > 0 && (new_w != g_cw || new_h != g_ch)) {
                if (g_hbmp) { DeleteObject(g_hbmp); g_hbmp = NULL; }
                g_cw = new_w; g_ch = new_h;
                memset(&g_bmi, 0, sizeof(g_bmi));
                g_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                g_bmi.bmiHeader.biWidth       = g_cw;
                g_bmi.bmiHeader.biHeight      = -g_ch;
                g_bmi.bmiHeader.biPlanes      = 1;
                g_bmi.bmiHeader.biBitCount    = 32;
                g_bmi.bmiHeader.biCompression = BI_RGB;
                HDC hdc = GetDC(hwnd);
                g_hbmp = CreateDIBSection(hdc, &g_bmi, DIB_RGB_COLORS,
                                          &g_bmp_bits, NULL, 0);
                ReleaseDC(hwnd, hdc);
                memset(g_bmp_bits, 0, (size_t)g_cw * g_ch * 4);
                scene_compositor_resize(g_cp, g_cw, g_ch);
                scene_shell_resize(g_shell, g_cw, g_ch);
                scene_compositor_force_repaint(g_cp);
                pump_and_render();
            }
            g_needs_paint = 1;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        uint32_t scene_key = 0;
        if (key == VK_TAB)    scene_key = 0xFF09;
        if (key == VK_RETURN) scene_key = 0xFF0D;
        if (key == VK_ESCAPE) scene_key = 0xFF1B;

        if (scene_key) {
            scene_shell_handle_key(g_shell, scene_key, 1, mod);
            scene_compositor_input_key(g_cp, scene_key, 1, mod);
            pump_and_render();
            g_needs_paint = 1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return 1;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        scene_shell_free(g_shell);
        scene_compositor_free(g_cp);
        scene_client_free(g_cl);
        scene_loopback_free(g_lb);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    (void)hPrev; (void)cmd;
    AddVectoredExceptionHandler(1, veh_handler);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = wndproc;
    wc.hInstance      = hInst;
    wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName  = "ISO_Preview";
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    /* Get work area to center the window */
    RECT workarea;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &workarea, 0);
    int screen_w = workarea.right - workarea.left;
    int screen_h = workarea.bottom - workarea.top;

    DWORD style = WS_OVERLAPPEDWINDOW;

    /* Don't fight DPI — create window, then query actual frame sizes */
    int win_w = 1024 + 16;  /* rough estimate for frame */
    int win_h = 768 + 39;
    int win_x = workarea.left + (screen_w - win_w) / 2;
    int win_y = workarea.top + (screen_h - win_h) / 2;

    HWND hwnd = CreateWindowExA(0, "ISO_Preview",
        "ISO - Scene Store Preview",
        style | WS_VISIBLE,
        win_x, win_y, win_w, win_h,
        NULL, NULL, hInst, NULL);

    /* Force client area to exactly 1024x768 */
    {
        RECT window_rc, client_rc;
        GetWindowRect(hwnd, &window_rc);
        GetClientRect(hwnd, &client_rc);
        int actual_frame_w = (window_rc.right - window_rc.left) - (client_rc.right - client_rc.left);
        int actual_frame_h = (window_rc.bottom - window_rc.top) - (client_rc.bottom - client_rc.top);
        SetWindowPos(hwnd, NULL, 0, 0,
                     1024 + actual_frame_w, 768 + actual_frame_h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

#else
int main(void) { return 0; }
#endif
