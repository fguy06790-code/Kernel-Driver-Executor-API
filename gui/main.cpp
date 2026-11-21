// krnlexec-gui main — Win32 window + ImGui + OpenGL3 (WGL).
//
// Minimal, single-window. Holds an AppState, pumps messages, and dispatches
// to the app module per frame.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tchar.h>

#include "app.h"
#include "theme.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"

#include <GL/gl.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// -----------------------------------------------------------------------------
// WGL plumbing. Straight PFNs, no extension loader — opengl3 backend asks for
// what it needs via wglGetProcAddress at runtime.
// -----------------------------------------------------------------------------

struct Win32GL {
    HWND  hwnd  = nullptr;
    HDC   hdc   = nullptr;
    HGLRC hrc   = nullptr;
};

static bool create_gl(Win32GL& g) {
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize    = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags  = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;

    g.hdc = GetDC(g.hwnd);
    int pf = ChoosePixelFormat(g.hdc, &pfd);
    if (!pf || !SetPixelFormat(g.hdc, pf, &pfd)) return false;

    g.hrc = wglCreateContext(g.hdc);
    if (!g.hrc) return false;
    return wglMakeCurrent(g.hdc, g.hrc) != 0;
}

static void destroy_gl(Win32GL& g) {
    if (g.hrc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(g.hrc); g.hrc = nullptr; }
    if (g.hdc) { ReleaseDC(g.hwnd, g.hdc); g.hdc = nullptr; }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                glViewport(0, 0, LOWORD(lp), HIWORD(lp));
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int nShow) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style  = CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"KrnlExecGUI";
    RegisterClassExW(&wc);

    RECT r{ 0, 0, 1180, 760 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"krnlexec",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, hinst, nullptr);
    if (!hwnd) return 1;

    Win32GL g; g.hwnd = hwnd;
    if (!create_gl(g)) { DestroyWindow(hwnd); return 2; }

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui_ImplWin32_InitForOpenGL(hwnd);
    ImGui_ImplOpenGL3_Init("#version 130");

    krnlexec::apply_theme();

    krnlexec::AppState st;
    krnlexec::init_state(st);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        krnlexec::render_frame(st);

        ImGui::Render();
        glClearColor(0.03f, 0.03f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(g.hdc);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_gl(g);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hinst);
    return 0;
}
