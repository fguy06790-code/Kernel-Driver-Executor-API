#include "app.h"
#include "theme.h"

#include "imgui.h"
#include "TextEditor.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace krnlexec {

namespace fs = std::filesystem;

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::string appdata_workspace() {
    char buf[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf)))
        return std::string(buf) + "\\krnlexec\\workspace";
    const char* home = std::getenv("USERPROFILE");
    return std::string(home ? home : ".") + "\\krnlexec\\workspace";
}

static std::unique_ptr<TextEditor> make_editor() {
    auto ed = std::make_unique<TextEditor>();
    ed->SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    ed->SetShowWhitespaces(false);
    ed->SetTabSize(4);

    TextEditor::Palette pal = TextEditor::GetDarkPalette();
    pal[(int)TextEditor::PaletteIndex::Background] = 0xff1a1a21;
    pal[(int)TextEditor::PaletteIndex::Default]    = 0xffdcdddc;
    pal[(int)TextEditor::PaletteIndex::Keyword]    = 0xfff39b6b;
    pal[(int)TextEditor::PaletteIndex::Number]     = 0xff9bc96b;
    pal[(int)TextEditor::PaletteIndex::String]     = 0xff8fcaf0;
    pal[(int)TextEditor::PaletteIndex::Comment]    = 0xff666876;
    pal[(int)TextEditor::PaletteIndex::Identifier] = 0xffdcdddc;
    pal[(int)TextEditor::PaletteIndex::LineNumber] = 0xff4a4d58;
    ed->SetPalette(pal);
    return ed;
}

static std::string read_text(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream s; s << f.rdbuf();
    return s.str();
}

static void write_text(const std::string& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path);
    f << body;
}

static void log_line(AppState& s, const std::string& line) {
    std::time_t t = std::time(nullptr);
    char ts[16]; std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
    s.output_log.push_back(std::string("[") + ts + "] " + line);
    if (s.output_log.size() > 1000)
        s.output_log.erase(s.output_log.begin(), s.output_log.begin() + 200);
    s.scroll_output_to_bottom = true;
}

static void new_tab(AppState& s, const std::string& name = "untitled.lua",
                    const std::string& path = "", const std::string& body = "") {
    ScriptTab t;
    t.name = name;
    t.path = path;
    t.editor = make_editor();
    t.editor->SetText(body);
    s.tabs.push_back(std::move(t));
    s.active_tab = (int)s.tabs.size() - 1;
}

static void save_current(AppState& s) {
    if (s.tabs.empty()) return;
    auto& t = s.tabs[s.active_tab];
    if (t.path.empty()) t.path = s.workspace_dir + "\\" + t.name;
    write_text(t.path, t.editor->GetText());
    t.dirty = false;
    log_line(s, "saved: " + t.path);
}

static void scan_workspace(AppState& s) {
    s.workspace_files.clear();
    std::error_code ec;
    fs::create_directories(s.workspace_dir, ec);
    for (const auto& de : fs::directory_iterator(s.workspace_dir, ec)) {
        if (!de.is_regular_file()) continue;
        auto ext = de.path().extension().string();
        if (ext == ".lua" || ext == ".luau" || ext == ".txt")
            s.workspace_files.push_back(de.path().filename().string());
    }
    std::sort(s.workspace_files.begin(), s.workspace_files.end());
}

static void poll_driver(AppState& s) {
    s.driver_loaded = s.drv.open();
    if (!s.driver_loaded) {
        s.ready = false; s.pid = 0;
        s.load_string = s.script_ctx = s.data_model = 0;
        return;
    }
    StatusReply r{};
    std::string err;
    if (s.drv.status(r, err)) {
        s.ready       = r.Ready != 0;
        s.pid         = r.Pid;
        s.load_string = r.LoadString;
        s.script_ctx  = r.ScriptContext;
        s.data_model  = r.DataModel;
    }
}

static void do_attach(AppState& s) {
    DWORD pid = Driver::find_roblox_pid();
    if (!pid) { log_line(s, "attach: no Roblox window found"); return; }
    std::string err;
    if (!s.drv.attach(pid, err)) { log_line(s, "attach failed: " + err); return; }
    log_line(s, "attached pid " + std::to_string(pid));
    poll_driver(s);
}

static void do_release(AppState& s) {
    std::string err;
    if (!s.drv.release(err)) { log_line(s, "release failed: " + err); return; }
    log_line(s, "released");
    poll_driver(s);
}

static void do_execute(AppState& s) {
    if (s.tabs.empty()) { log_line(s, "no tab"); return; }
    if (!s.ready)       { log_line(s, "not attached — click Attach first"); return; }
    auto& t = s.tabs[s.active_tab];
    std::string src = t.editor->GetText();
    if (src.empty()) { log_line(s, "empty script"); return; }
    std::string err;
    if (!s.drv.execute(s.pid, src, err)) {
        log_line(s, "exec failed: " + err);
    } else {
        log_line(s, "executed [" + t.name + "] (" + std::to_string(src.size()) + " bytes)");
    }
}

static void menubar(AppState& s) {
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New tab", "Ctrl+N"))  new_tab(s);
        if (ImGui::MenuItem("Save",    "Ctrl+S"))  save_current(s);
        if (ImGui::MenuItem("Execute", "F5"))      do_execute(s);
        ImGui::Separator();
        if (ImGui::MenuItem("Rescan workspace"))   scan_workspace(s);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4"))     PostQuitMessage(0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Driver")) {
        if (ImGui::MenuItem("Attach (auto)"))      do_attach(s);
        if (ImGui::MenuItem("Release"))            do_release(s);
        if (ImGui::MenuItem("Refresh status"))     poll_driver(s);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Executor API",  nullptr, &s.show_api_window);
        ImGui::MenuItem("About",         nullptr, &s.show_about_window);
        ImGui::EndMenu();
    }

    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(avail - 220);

    ImVec4 drv_c = s.driver_loaded ? ImVec4(0.65f, 0.80f, 0.45f, 1.0f)
                                   : ImVec4(0.90f, 0.35f, 0.40f, 1.0f);
    ImGui::TextColored(drv_c, s.driver_loaded ? "● driver" : "○ driver");
    ImGui::SameLine();
    ImVec4 att_c = s.ready ? ImVec4(0.35f, 0.80f, 0.45f, 1.0f)
                           : ImVec4(0.90f, 0.35f, 0.40f, 1.0f);
    ImGui::TextColored(att_c, s.ready ? "● attached" : "○ not attached");

    ImGui::EndMenuBar();
}

static void sidebar(AppState& s) {
    ImGui::BeginChild("sidebar", ImVec2(220, 0), true);

    ImGui::TextDisabled("DRIVER");
    if (ImGui::SmallButton("Attach")) do_attach(s);
    ImGui::SameLine();
    if (ImGui::SmallButton("Release")) do_release(s);

    if (s.ready) {
        ImGui::Text("pid: %lu", s.pid);
        ImGui::TextDisabled("LS:   0x%llx", s.load_string);
        ImGui::TextDisabled("SC:   0x%llx", s.script_ctx);
    } else {
        ImGui::TextDisabled("not attached");
    }

    ImGui::Separator();
    ImGui::TextDisabled("WORKSPACE");
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x - 28, 0));
    ImGui::SameLine();
    if (ImGui::SmallButton("⟲")) scan_workspace(s);

    for (const auto& name : s.workspace_files) {
        if (ImGui::Selectable(name.c_str())) {
            std::string p = s.workspace_dir + "\\" + name;
            new_tab(s, name, p, read_text(p));
        }
    }
    if (s.workspace_files.empty()) {
        ImGui::TextDisabled("(no scripts)");
        ImGui::TextDisabled("%s", s.workspace_dir.c_str());
    }

    ImGui::EndChild();
}

static void editor_area(AppState& s) {
    ImGui::BeginChild("editor_area", ImVec2(0, 0), false);

    if (ImGui::Button("▶ Execute")) do_execute(s);
    ImGui::SameLine();
    if (ImGui::Button("Save")) save_current(s);
    ImGui::SameLine();
    if (ImGui::Button("+ New")) new_tab(s);
    ImGui::SameLine();
    if (ImGui::Button("Clear output")) s.output_log.clear();

    ImGui::Separator();

    float out_h = 170.0f;
    float ed_h  = ImGui::GetContentRegionAvail().y - out_h - ImGui::GetStyle().ItemSpacing.y;

    ImGui::BeginChild("edtabs", ImVec2(0, ed_h), true);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
        for (int i = 0; i < (int)s.tabs.size(); ++i) {
            auto& t = s.tabs[i];
            bool open = true;
            std::string label = (t.dirty ? "● " : "") + t.name + "###tab" + std::to_string(i);
            if (ImGui::BeginTabItem(label.c_str(), &open)) {
                s.active_tab = i;
                ImVec2 sz = ImGui::GetContentRegionAvail();
                t.editor->Render("##ed", sz, false);
                if (t.editor->IsTextChanged()) t.dirty = true;
                ImGui::EndTabItem();
            }
            if (!open) t.pending_close = true;
        }
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
            new_tab(s);
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::BeginChild("output", ImVec2(0, out_h), true);
    ImGui::TextDisabled("OUTPUT");
    ImGui::Separator();
    for (const auto& ln : s.output_log) ImGui::TextUnformatted(ln.c_str());
    if (s.scroll_output_to_bottom) { ImGui::SetScrollHereY(1.0f); s.scroll_output_to_bottom = false; }
    ImGui::EndChild();

    ImGui::EndChild();

    for (int i = (int)s.tabs.size() - 1; i >= 0; --i) {
        if (s.tabs[i].pending_close) {
            s.tabs.erase(s.tabs.begin() + i);
            if (s.active_tab >= (int)s.tabs.size()) s.active_tab = (int)s.tabs.size() - 1;
        }
    }
}

static void api_window(AppState& s) {
    if (!s.show_api_window) return;
    ImGui::SetNextWindowSize(ImVec2(480, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Executor API", &s.show_api_window)) {
        ImGui::TextWrapped("Routed through ScriptContext::LoadString in Roblox. "
                           "Bootstrap your UNC surface from the first script.");
        ImGui::Separator();
        static const char* fns[] = {
            "loadstring(src[, chunk])",
            "identifyexecutor() / getexecutorname()",
            "getgenv() / getrenv()",
            "checkcaller()",
            "setreadonly / isreadonly",
            "getrawmetatable / setrawmetatable",
            "getnamecallmethod / setnamecallmethod",
            "hookfunction / replaceclosure / restorefunction",
            "hookmetamethod",
            "newcclosure / iscclosure / islclosure",
            "gethui()",
            "HttpGet / httpget",
            "queue_on_teleport / queueonteleport",
            "writefile / readfile / isfile / delfile",
            "getscripts / getloadedmodules / getconnections",
            "getinstances / getnilinstances",
            "crypt.base64_encode / base64_decode / generatebytes",
            "base64.encode / base64.decode",
        };
        for (const char* f : fns) ImGui::BulletText("%s", f);
    }
    ImGui::End();
}

static void about_window(AppState& s) {
    if (!s.show_about_window) return;
    ImGui::SetNextWindowSize(ImVec2(360, 160), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About krnlexec", &s.show_about_window)) {
        ImGui::Text("krnlexec 1.0");
        ImGui::TextDisabled("Roblox executor API kernel driver + UI.");
        ImGui::Separator();
        ImGui::Text("github.com/fguy06790-code/Kernel-Driver-Executor-API");
        ImGui::TextDisabled("driver: KrnlExec.sys");
        ImGui::TextDisabled("device: \\\\.\\KrnlExec");
    }
    ImGui::End();
}

static void shortcuts(AppState& s) {
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save_current(s);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) new_tab(s);
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))        do_execute(s);
}

void init_state(AppState& s) {
    s.workspace_dir = appdata_workspace();
    scan_workspace(s);
    new_tab(s, "scratch.lua", "", "-- krnlexec scratch\nprint(identifyexecutor())\n");
    poll_driver(s);
}

void render_frame(AppState& s) {
    if (now_s() - s.last_poll > 0.5) { poll_driver(s); s.last_poll = now_s(); }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoSavedSettings);

    menubar(s);
    shortcuts(s);
    sidebar(s);
    ImGui::SameLine();
    editor_area(s);

    ImGui::End();

    api_window(s);
    about_window(s);
}

} // namespace krnlexec
