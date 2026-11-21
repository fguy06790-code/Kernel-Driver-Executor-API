// gui/host.cpp — WebView2-hosted krnlexec GUI.
//
// The entire UI lives in web/*. This file:
//   1. Creates a Win32 window and embeds a WebView2 that fills it.
//   2. Points the WebView at a virtual host mapped to .\web\ so it can
//      load index.html, style.css, app.js, catalog.json by relative URL.
//   3. Implements a message bridge: UI posts JSON {id, cmd, ...}, host
//      replies with {id, result} or {id, error}; host pushes events via
//      ExecuteScript("window.postMessage({event, ...}, '*')").
//
// Commands: attach, release, status, exec, ws_list, ws_read, ws_write,
//           workspace_path.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"
#include "ipc.h"

// mingw-w64 doesn't register IIDs from MIDL_INTERFACE macros automatically;
// declare the ones we need via __CRT_UUID_DECL so __uuidof works.
__CRT_UUID_DECL(ICoreWebView2WebMessageReceivedEventHandler,
    0x57213f19,0x00e6,0x49fa,0x8e,0x07,0x89,0x8e,0xa0,0x1e,0xcb,0xd2)
__CRT_UUID_DECL(ICoreWebView2_3,
    0xA0D6DF20,0x3B92,0x416D,0xAA,0x0C,0x43,0x7A,0x9C,0x72,0x78,0x57)
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
    0x4e8a3389,0xc9d8,0x4bd2,0xb6,0xb5,0x12,0x4f,0xee,0x6c,0xc1,0x4d)
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
    0x6c4819f3,0xc9b7,0x4260,0x81,0x27,0xc9,0xf5,0xbd,0xe7,0xf6,0x8c)

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Hand-rolled COM callbacks. mingw-w64 only ships wrl/client.h (ComPtr), no
// WRL::Callback template. Three one-off sinks is less code than the WRL
// wrappers anyway.
// ---------------------------------------------------------------------------

#define IMPL_IUNKNOWN(iface)                                                   \
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {             \
        if (iid == __uuidof(iface) || iid == IID_IUnknown) {                   \
            *ppv = static_cast<iface*>(this); AddRef(); return S_OK;           \
        }                                                                      \
        *ppv = nullptr; return E_NOINTERFACE;                                  \
    }                                                                          \
    STDMETHODIMP_(ULONG) AddRef() override  { return InterlockedIncrement(&refs_); } \
    STDMETHODIMP_(ULONG) Release() override {                                  \
        ULONG r = InterlockedDecrement(&refs_);                                \
        if (!r) delete this;                                                   \
        return r;                                                              \
    }

class EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    using Fn = std::function<HRESULT(HRESULT, ICoreWebView2Environment*)>;
    explicit EnvHandler(Fn fn) : fn_(std::move(fn)) {}
    IMPL_IUNKNOWN(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
    STDMETHODIMP Invoke(HRESULT r, ICoreWebView2Environment* env) override { return fn_(r, env); }
private:
    Fn fn_; LONG refs_ = 1;
};

class CtrlHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    using Fn = std::function<HRESULT(HRESULT, ICoreWebView2Controller*)>;
    explicit CtrlHandler(Fn fn) : fn_(std::move(fn)) {}
    IMPL_IUNKNOWN(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
    STDMETHODIMP Invoke(HRESULT r, ICoreWebView2Controller* c) override { return fn_(r, c); }
private:
    Fn fn_; LONG refs_ = 1;
};

class MsgHandler : public ICoreWebView2WebMessageReceivedEventHandler {
public:
    using Fn = std::function<HRESULT(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)>;
    explicit MsgHandler(Fn fn) : fn_(std::move(fn)) {}
    IMPL_IUNKNOWN(ICoreWebView2WebMessageReceivedEventHandler)
    STDMETHODIMP Invoke(ICoreWebView2* s, ICoreWebView2WebMessageReceivedEventArgs* a) override {
        return fn_(s, a);
    }
private:
    Fn fn_; LONG refs_ = 1;
};

namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
// Minimal JSON helpers. The payloads are simple and we never need the full
// spec — a key lookup and string escape is plenty.
// -----------------------------------------------------------------------------

static std::string json_escape(const std::string& s) {
    std::string o; o.reserve(s.size() + 2);
    o.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o.push_back(c);
                }
        }
    }
    o.push_back('"');
    return o;
}

// Shallow extract: given a top-level flat JSON object, pull the value of a
// given key as string (unescaped). Only handles strings and ints.
static std::optional<std::string> json_get(const std::string& obj, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return std::nullopt;
    p = obj.find(':', p + pat.size());
    if (p == std::string::npos) return std::nullopt;
    ++p;
    while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
    if (p >= obj.size()) return std::nullopt;

    if (obj[p] == '"') {
        std::string out;
        ++p;
        while (p < obj.size() && obj[p] != '"') {
            if (obj[p] == '\\' && p + 1 < obj.size()) {
                char n = obj[p + 1];
                switch (n) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u':
                        if (p + 5 < obj.size()) {
                            char hx[5] = {obj[p+2],obj[p+3],obj[p+4],obj[p+5],0};
                            int v = (int)std::strtol(hx, nullptr, 16);
                            if (v < 0x80) out.push_back((char)v);
                            else { out.push_back((char)(0xC0 | (v >> 6))); out.push_back((char)(0x80 | (v & 0x3F))); }
                            p += 4;
                        }
                        break;
                    default: out += n; break;
                }
                p += 2;
            } else {
                out.push_back(obj[p]);
                ++p;
            }
        }
        return out;
    }

    std::string num;
    while (p < obj.size() && (std::isdigit((unsigned char)obj[p]) ||
                              obj[p] == '-' || obj[p] == '.')) {
        num.push_back(obj[p++]);
    }
    if (!num.empty()) return num;
    return std::nullopt;
}

// -----------------------------------------------------------------------------
// String conversion helpers.
// -----------------------------------------------------------------------------

static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                 nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        &s[0], n, nullptr, nullptr);
    return s;
}

// -----------------------------------------------------------------------------
// Workspace: %APPDATA%\krnlexec\workspace
// -----------------------------------------------------------------------------

static std::wstring workspace_dir() {
    wchar_t buf[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf)))
        return L"";
    std::wstring p = buf;
    p += L"\\krnlexec\\workspace";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

// -----------------------------------------------------------------------------
// Host app state.
// -----------------------------------------------------------------------------

struct HostApp {
    HWND  hwnd = nullptr;
    HINSTANCE hinst = nullptr;

    ComPtr<ICoreWebView2Controller> ctrl;
    ComPtr<ICoreWebView2>           wv;

    krnlexec::Driver drv;
    std::mutex       drv_mu;

    std::thread poll_th;
    std::atomic<bool> poll_run{true};

    void poll_loop();
    void push_status();
    void push_event(const std::string& evt_json);

    void handle_message(const std::wstring& msg);

    std::string cmd_attach(const std::string& body);
    std::string cmd_release();
    std::string cmd_status();
    std::string cmd_exec(const std::string& body);
    std::string cmd_ws_list();
    std::string cmd_ws_read(const std::string& body);
    std::string cmd_ws_write(const std::string& body);
    std::string cmd_workspace_path();
};

static HostApp g_app;

void HostApp::push_event(const std::string& evt_json) {
    if (!wv) return;
    std::wstring script =
        L"window.chrome && window.chrome.webview && "
        L"window.dispatchEvent && "
        L"(window.__krnlEvt && window.__krnlEvt(" + to_wide(evt_json) + L"));"
        L"window.chrome.webview.dispatchEvent && null;";
    // Use PostWebMessageAsString so the webview's registered message
    // listener receives it as a normal message event (same path as
    // bridge.onMessage).
    wv->PostWebMessageAsString(to_wide(evt_json).c_str());
}

void HostApp::push_status() {
    krnlexec::StatusReply r{};
    std::string err;
    bool loaded, ok;
    {
        std::lock_guard<std::mutex> lk(drv_mu);
        loaded = drv.open();
        ok = loaded && drv.status(r, err);
    }
    std::ostringstream os;
    os << "{\"event\":\"status\",\"data\":{"
       << "\"driver_loaded\":" << (loaded ? "true" : "false")
       << ",\"ready\":" << ((ok && r.Ready) ? "true" : "false")
       << ",\"pid\":"         << (ok ? r.Pid : 0)
       << ",\"load_string\":" << (ok ? r.LoadString : 0)
       << ",\"script_ctx\":"  << (ok ? r.ScriptContext : 0)
       << ",\"data_model\":"  << (ok ? r.DataModel : 0)
       << "}}";
    push_event(os.str());
}

void HostApp::poll_loop() {
    while (poll_run.load()) {
        push_status();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// --- command handlers --------------------------------------------------------

std::string HostApp::cmd_attach(const std::string& /*body*/) {
    DWORD pid = krnlexec::Driver::find_roblox_pid();
    if (!pid) return R"({"error":"no Roblox window"})";
    std::string err;
    std::lock_guard<std::mutex> lk(drv_mu);
    if (!drv.attach(pid, err)) return R"({"error":)" + json_escape(err) + "}";
    return R"({"result":{"pid":)" + std::to_string(pid) + "}}";
}

std::string HostApp::cmd_release() {
    std::string err;
    std::lock_guard<std::mutex> lk(drv_mu);
    if (!drv.release(err)) return R"({"error":)" + json_escape(err) + "}";
    return R"({"result":true})";
}

std::string HostApp::cmd_status() {
    krnlexec::StatusReply r{};
    std::string err;
    std::lock_guard<std::mutex> lk(drv_mu);
    if (!drv.status(r, err)) return R"({"error":)" + json_escape(err) + "}";
    std::ostringstream os;
    os << R"({"result":{"ready":)" << (r.Ready ? "true" : "false")
       << R"(,"pid":)" << r.Pid
       << R"(,"load_string":)" << r.LoadString
       << R"(,"script_ctx":)"  << r.ScriptContext
       << R"(,"data_model":)"  << r.DataModel
       << "}}";
    return os.str();
}

std::string HostApp::cmd_exec(const std::string& body) {
    auto src = json_get(body, "source");
    if (!src) return R"({"error":"no source"})";
    std::lock_guard<std::mutex> lk(drv_mu);
    krnlexec::StatusReply r{}; std::string err;
    if (!drv.status(r, err) || !r.Ready) return R"({"error":"not attached"})";
    if (!drv.execute(r.Pid, *src, err))
        return R"({"error":)" + json_escape(err) + "}";
    return R"({"result":{"bytes":)" + std::to_string(src->size()) + "}}";
}

static bool safe_name(const std::string& n) {
    if (n.empty()) return false;
    for (char c : n) if (c == '/' || c == '\\' || c == ':') return false;
    if (n == "." || n == "..") return false;
    return true;
}

std::string HostApp::cmd_ws_list() {
    std::wstring dir = workspace_dir();
    std::string out = R"({"result":[)";
    bool first = true;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_regular_file()) continue;
        auto ext = de.path().extension().wstring();
        if (ext != L".lua" && ext != L".luau" && ext != L".txt") continue;
        if (!first) out += ",";
        out += json_escape(to_utf8(de.path().filename().wstring()));
        first = false;
    }
    out += "]}";
    return out;
}

std::string HostApp::cmd_ws_read(const std::string& body) {
    auto name = json_get(body, "name");
    if (!name || !safe_name(*name)) return R"({"error":"bad name"})";
    std::wstring p = workspace_dir() + L"\\" + to_wide(*name);
    fs::path fp = p;
    std::ifstream f(fp);
    if (!f) return R"({"result":""})";
    std::stringstream ss; ss << f.rdbuf();
    return R"({"result":)" + json_escape(ss.str()) + "}";
}

std::string HostApp::cmd_ws_write(const std::string& body) {
    auto name = json_get(body, "name");
    auto bodyv = json_get(body, "body");
    if (!name || !safe_name(*name)) return R"({"error":"bad name"})";
    if (!bodyv) return R"({"error":"no body"})";
    std::wstring p = workspace_dir() + L"\\" + to_wide(*name);
    fs::path fp = p;
    std::ofstream f(fp);
    f << *bodyv;
    return R"({"result":true})";
}

std::string HostApp::cmd_workspace_path() {
    return R"({"result":)" + json_escape(to_utf8(workspace_dir())) + "}";
}

// --- dispatch ----------------------------------------------------------------

void HostApp::handle_message(const std::wstring& wmsg) {
    std::string msg = to_utf8(wmsg);
    auto id  = json_get(msg, "id");
    auto cmd = json_get(msg, "cmd");
    if (!id || !cmd) return;

    std::string reply;
    if      (*cmd == "attach")          reply = cmd_attach(msg);
    else if (*cmd == "release")         reply = cmd_release();
    else if (*cmd == "status")          reply = cmd_status();
    else if (*cmd == "exec")            reply = cmd_exec(msg);
    else if (*cmd == "ws_list")         reply = cmd_ws_list();
    else if (*cmd == "ws_read")         reply = cmd_ws_read(msg);
    else if (*cmd == "ws_write")        reply = cmd_ws_write(msg);
    else if (*cmd == "workspace_path")  reply = cmd_workspace_path();
    else reply = R"({"error":"unknown cmd"})";

    // Reply is a bare object missing the id; splice it in.
    std::string out;
    if (reply.size() >= 2 && reply.front() == '{') {
        out = "{\"id\":" + *id + "," + reply.substr(1);
    } else {
        out = R"({"id":)" + *id + R"(,"error":"bad reply"})";
    }
    push_event(out);
}

// -----------------------------------------------------------------------------
// WebView2 bootstrap.
// -----------------------------------------------------------------------------

static void resize_wv() {
    if (!g_app.ctrl) return;
    RECT r; GetClientRect(g_app.hwnd, &r);
    g_app.ctrl->put_Bounds(r);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_SIZE:    resize_wv(); return 0;
        case WM_DESTROY:
            g_app.poll_run = false;
            if (g_app.poll_th.joinable()) g_app.poll_th.join();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static HRESULT on_wv_ready(HRESULT hr, ICoreWebView2Controller* c) {
    if (FAILED(hr) || !c) {
        MessageBoxW(g_app.hwnd,
                    L"WebView2 Runtime not found or failed to start.\n"
                    L"Install the Evergreen WebView2 Runtime from Microsoft.",
                    L"krnlexec", MB_ICONERROR);
        return hr;
    }
    g_app.ctrl = c;
    c->get_CoreWebView2(&g_app.wv);

    ComPtr<ICoreWebView2Settings> st;
    g_app.wv->get_Settings(&st);
    st->put_AreDefaultContextMenusEnabled(TRUE);
    st->put_AreDevToolsEnabled(TRUE);
    st->put_IsStatusBarEnabled(FALSE);
    st->put_IsZoomControlEnabled(FALSE);

    // Map .\web\ to virtual host "krnlexec.local".
    ComPtr<ICoreWebView2_3> wv3;
    if (SUCCEEDED(g_app.wv.As(&wv3))) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        PathRemoveFileSpecW(exe);
        std::wstring web = std::wstring(exe) + L"\\web";
        wv3->SetVirtualHostNameToFolderMapping(
            L"krnlexec.local", web.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }

    EventRegistrationToken tok;
    auto* msg_h = new MsgHandler(
        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR s = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&s)) && s) {
                g_app.handle_message(s);
                CoTaskMemFree(s);
            }
            return S_OK;
        });
    g_app.wv->add_WebMessageReceived(msg_h, &tok);
    msg_h->Release();

    resize_wv();
    g_app.wv->Navigate(L"http://krnlexec.local/index.html");

    g_app.poll_th = std::thread([]{ g_app.poll_loop(); });
    return S_OK;
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int nShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_app.hinst = hinst;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style  = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"KrnlExecGUI";
    RegisterClassExW(&wc);

    RECT r{0, 0, 1180, 760};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"krnlexec",
                                 WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 r.right - r.left, r.bottom - r.top,
                                 nullptr, nullptr, hinst, nullptr);
    if (!g_app.hwnd) return 1;

    auto* env_h = new EnvHandler(
        [](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
            if (!env) return E_FAIL;
            auto* ctrl_h = new CtrlHandler(&on_wv_ready);
            env->CreateCoreWebView2Controller(g_app.hwnd, ctrl_h);
            ctrl_h->Release();
            return S_OK;
        });
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr, env_h);
    env_h->Release();
    if (FAILED(hr)) {
        MessageBoxW(g_app.hwnd,
            L"WebView2 Runtime not installed.\n"
            L"Install the Evergreen WebView2 Runtime (comes with Win11 by default).",
            L"krnlexec", MB_ICONERROR);
        return 2;
    }

    ShowWindow(g_app.hwnd, nShow);
    UpdateWindow(g_app.hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    CoUninitialize();
    return 0;
}
