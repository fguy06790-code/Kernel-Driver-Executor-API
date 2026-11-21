#include "ipc.h"

#include <winioctl.h>
#include <vector>
#include <cstring>

namespace krnlexec {

#define DEV_PATH L"\\\\.\\KrnlExec"

#define IOCTL_INIT    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_EXECUTE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_STATUS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RELEASE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

static std::string win_err(DWORD code) {
    char* buf = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPSTR)&buf, 0, nullptr);
    std::string s = buf ? buf : "unknown";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.'))
        s.pop_back();
    return "[" + std::to_string(code) + "] " + s;
}

bool Driver::open() {
    if (h_ != INVALID_HANDLE_VALUE) return true;
    h_ = CreateFileW(DEV_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                     OPEN_EXISTING, 0, nullptr);
    return h_ != INVALID_HANDLE_VALUE;
}

void Driver::close() {
    if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
}

bool Driver::attach(DWORD pid, std::string& err) {
    if (!open()) { err = "driver not loaded: " + win_err(GetLastError()); return false; }
    InitRequest r{ pid };
    DWORD out = 0;
    if (!DeviceIoControl(h_, IOCTL_INIT, &r, sizeof(r), nullptr, 0, &out, nullptr)) {
        err = "IOCTL_INIT: " + win_err(GetLastError());
        return false;
    }
    return true;
}

bool Driver::release(std::string& err) {
    if (!open()) { err = "driver not loaded: " + win_err(GetLastError()); return false; }
    DWORD out = 0;
    if (!DeviceIoControl(h_, IOCTL_RELEASE, nullptr, 0, nullptr, 0, &out, nullptr)) {
        err = "IOCTL_RELEASE: " + win_err(GetLastError());
        return false;
    }
    return true;
}

bool Driver::status(StatusReply& out, std::string& err) {
    if (!open()) { err = "driver not loaded: " + win_err(GetLastError()); return false; }
    DWORD got = 0;
    if (!DeviceIoControl(h_, IOCTL_STATUS, nullptr, 0, &out, sizeof(out), &got, nullptr)) {
        err = "IOCTL_STATUS: " + win_err(GetLastError());
        return false;
    }
    return true;
}

bool Driver::execute(DWORD pid, const std::string& src, std::string& err) {
    if (!open()) { err = "driver not loaded: " + win_err(GetLastError()); return false; }
    if (src.empty()) { err = "empty script"; return false; }
    size_t total = offsetof(ExecRequest, Script) + src.size();
    std::vector<uint8_t> buf(total);
    auto* r = reinterpret_cast<ExecRequest*>(buf.data());
    r->Pid = pid;
    r->Size = (ULONG)src.size();
    std::memcpy(r->Script, src.data(), src.size());
    DWORD out = 0;
    if (!DeviceIoControl(h_, IOCTL_EXECUTE, buf.data(), (DWORD)buf.size(),
                         nullptr, 0, &out, nullptr)) {
        err = "IOCTL_EXECUTE: " + win_err(GetLastError());
        return false;
    }
    return true;
}

DWORD Driver::find_roblox_pid() {
    HWND hwnd = FindWindowW(L"ROBLOX", nullptr);
    if (!hwnd) hwnd = FindWindowW(nullptr, L"Roblox");
    if (!hwnd) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

} // namespace krnlexec
