#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace krnlexec {

#pragma pack(push, 1)
struct InitRequest { ULONG Pid; };
struct ExecRequest { ULONG Pid; ULONG Size; CHAR Script[1]; };
struct StatusReply {
    ULONGLONG LoadString;
    ULONGLONG ScriptContext;
    ULONGLONG DataModel;
    ULONG     Pid;
    ULONG     Ready;
};
#pragma pack(pop)

class Driver {
public:
    bool is_open() const { return h_ != INVALID_HANDLE_VALUE; }
    bool open();
    void close();

    bool attach(DWORD pid, std::string& err);
    bool release(std::string& err);
    bool status(StatusReply& out, std::string& err);
    bool execute(DWORD pid, const std::string& src, std::string& err);

    // Finds the foreground Roblox window's PID. 0 = not found.
    static DWORD find_roblox_pid();

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

} // namespace krnlexec
