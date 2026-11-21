#pragma once

#include "ipc.h"
#include "TextEditor.h"

#include <memory>
#include <string>
#include <vector>

namespace krnlexec {

struct ScriptTab {
    std::string name;
    std::string path;
    std::unique_ptr<TextEditor> editor;
    bool dirty = false;
    bool pending_close = false;
};

struct AppState {
    std::vector<ScriptTab> tabs;
    int active_tab = 0;

    std::vector<std::string> output_log;
    bool scroll_output_to_bottom = true;

    std::string workspace_dir;
    std::vector<std::string> workspace_files;

    bool show_api_window   = false;
    bool show_about_window = false;

    double last_poll = 0.0;
    bool   driver_loaded = false;
    bool   ready         = false;
    DWORD  pid           = 0;
    ULONGLONG load_string = 0;
    ULONGLONG script_ctx  = 0;
    ULONGLONG data_model  = 0;

    Driver drv;
};

void render_frame(AppState& s);
void init_state(AppState& s);

} // namespace krnlexec
