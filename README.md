# krnlexec

Roblox executor API kernel driver for Windows 11 x64. Attaches to the
live `RobloxPlayerBeta.exe` from ring-0, resolves `ScriptContext::LoadString`
via signature scan + an anchored offset chain, and executes Luau source
in-process via a user-mode APC.

Companion to `sober-exec` (Linux) — same API surface, different injection
model for the Windows sandbox.

## Files

- `driver.cpp`   — the WDM kernel driver (one file, ~500 LoC).
- `loader.cpp`   — user-mode CLI (`krnlexec.exe`) that drives the driver via IOCTLs.
- `gui/`         — WebView2-hosted GUI (`krnlexec-gui.exe`): Monaco editor,
  script hub, settings panel. Native shell in `gui/host.cpp` + `gui/ipc.cpp`,
  the UI itself lives under `gui/web/` as HTML/CSS/JS.
- `vendor/webview2/` — WebView2 SDK headers + `WebView2Loader.dll`.
- `KrnlExec.inf` — install script.
- `build.bat`    — Windows build (WDK + MSVC) — driver + CLI + GUI + dist/.
- `Makefile`     — cross-build of the CLI + GUI from Linux via mingw-w64.
- `dist/`        — deployable payload:
    - `krnlexec.exe`      — CLI loader (~2.4 MB).
    - `krnlexec-gui.exe`  — GUI (~2.9 MB).
    - `WebView2Loader.dll`
    - `web/`              — the HTML/CSS/JS frontend.
    - `KrnlExec.sys`      — produced only by the Windows build (needs WDK).

## Build

### Windows (full build)

Requirements: Visual Studio 2022, Windows 11 SDK, WDK 10.

From an **x64 Native Tools Command Prompt for VS 2022** with the WDK
environment loaded:

```cmd
build.bat
```

Produces `KrnlExec.sys` (the driver) and `krnlexec.exe` (the loader).

### Linux (user-mode: CLI + GUI)

Cross-compile the user-mode half:

```bash
sudo apt install -y mingw-w64
make
```

Produces `krnlexec.exe` (CLI) and `krnlexec-gui.exe` (GUI). The driver
cannot be cross-built from Linux — the WDK is Windows-only, and
ddk-shim mismatches between clang/mingw and the real MSVC toolchain
make it a dead end. Build the `.sys` on Windows.

## Install

Test-sign and load:

```cmd
rem one-time, as admin
bcdedit /set testsigning on
rem reboot

sc.exe create KrnlExec type= kernel binPath= C:\path\to\KrnlExec.sys
sc.exe start KrnlExec
```

Or via INF:

```cmd
pnputil /add-driver KrnlExec.inf /install
```

If you don't want to test-sign, manual-map the driver with KDMapper,
kdu, or an equivalent loader of your choice.

## Usage

### GUI

Launch `dist\krnlexec-gui.exe`. Three top tabs:

- **Editor** — tabbed Monaco (VS Code's editor) with Luau highlighting,
  workspace sidebar (`%APPDATA%\krnlexec\workspace`), output console.
  `F5` executes, `Ctrl+S` saves, `Ctrl+N` new tab. `▶ Execute` button
  dispatches the active tab via the driver.
- **Script Hub** — a catalog of scripts with Execute / Open-in-editor
  buttons. Ships a builtin catalog (`dist\web\catalog.json`); point it
  at your own URL in Settings → *Script Hub JSON catalog URL*.
- **Settings** — executor name, default identity, auto-attach, poll
  interval, Monaco theme + font size, hub URL. Persisted in the
  WebView's localStorage. Driver info panel at the bottom shows
  resolved pointers when attached.

Top-right has live status dots: **driver** (service loaded + device
openable) and **attach** (target PID + resolved offsets).

**WebView2 Runtime** is required. Windows 11 ships it by default; on
older Windows install the Evergreen runtime from Microsoft.

### CLI

```cmd
krnlexec attach               :: auto-finds the Roblox window
krnlexec attach 1234          :: or specify pid
krnlexec status               :: show resolved state
krnlexec exec test.lua
krnlexec exec -s "print(identifyexecutor())"
krnlexec release              :: detach, drop refs
```

## Architecture

```
  user                    kernel                     target process
 ┌───────────┐  IOCTL   ┌───────────────────────┐  ┌─────────────────────┐
 │ krnlexec  │ ────────▶│  KrnlExec.sys         │  │ RobloxPlayerBeta    │
 │  attach   │          │  ├─ resolve r_loadstr │  │                     │
 │  exec     │          │  │   via sig scan     │──┤ .text / PEB         │
 │  status   │          │  ├─ walk FakeDM→DM→SC │  │                     │
 │  release  │          │  ├─ ZwAllocateVM in   │  │  user VAD (script)  │
 └───────────┘          │  │   target for script│──▶                     │
                        │  ├─ KeInitializeApc + │  │ main scheduler      │
                        │  │   KeInsertQueueApc │  │   thread fires APC  │
                        │  │   on all threads   │──▶  r_loadstring(...)  │
                        │  └─ UserMode dispatch │  │                     │
                        └───────────────────────┘  └─────────────────────┘
```

Why APC: kernel code cannot call a user-mode function pointer directly
(SMEP + CPL mismatch → bugcheck). The correct mechanism is a user-mode
normal routine that fires when any thread next enters an alertable wait.
x64 APC dispatch places `NormalContext`, `SysArg1`, `SysArg2` in
`rcx, rdx, r8` — exactly the ABI for
`ScriptContext::LoadString(ScriptContext*, const char* src, const char* chunk)`
— so no shellcode is needed.

## Signature maintenance

`driver.cpp` holds four refresh-prone values:

- `kLoadStringPat` — prologue of `ScriptContext::LoadString`.
- `kFakeDMPat`     — a mov-from-rip access to the FakeDataModel singleton.
- `kFakeDM_to_DM`  — offset from FakeDataModel singleton to real `DataModel*`.
- `kDM_to_ScriptContext` — offset from `DataModel` to `ScriptContext*`.

On each Roblox client update:

1. Attach your debugger to the running `RobloxPlayerBeta.exe`.
2. Find `ScriptContext::LoadString` via xref to `"=LoadString"` chunk
   convention or via the `ScriptContext` vtable. Grab 20–30 bytes of
   prologue with register/offset slots masked as `?`.
3. Find the `FakeDataModel::get()` accessor. Pattern anchors to
   `mov rax, [rip+disp32]; test rax,rax; jz; mov rax,[rax+?]` — update
   the disp32 and jz offset if the compiler shuffled.
4. Step from the FakeDataModel through to the real DataModel; note
   `kFakeDM_to_DM`. From DataModel to ScriptContext; note
   `kDM_to_ScriptContext`.
5. Replace the four constants, recompile.

## Correctness checklist

This driver fixes every fatal from the common "offsetless reflection"
pastebin circulating in the roblox executor space:

| issue                                           | status |
|-------------------------------------------------|--------|
| Kernel directly calls user-mode function pointer| fixed (APC) |
| No IOCTL `InputBufferLength` validation         | fixed |
| `PsLookupProcessByProcessId` ref never released | fixed |
| Non-paged alloc of full module size             | fixed (paged, chunked) |
| No `__try/__except` around PEB / user derefs    | fixed |
| Nested `KeStackAttachProcess` + `MmCopyVirtualMemory` | fixed |
| Script pointer was kernel-space                 | fixed (`ZwAllocateVirtualMemory` in target) |
| Class-registry "reflection" scan (broken by design) | removed |
| Narrow `r_loadstring` prologue signature        | widened + masked |
| Comment claimed KMDF, code was WDM              | fixed |

## Known limits

- APC fires on whichever thread next enters an alertable wait; if that
  happens to be a thread other than the Luau task scheduler and
  `ScriptContext::LoadString` requires the scheduler thread, the call
  can race. In practice LoadString itself marshals internally, but
  under heavy load you can see delayed execution. Fix is to
  signature-anchor the scheduler thread specifically and queue only
  there.
- No hooking of the Hyperion anti-cheat is attempted. If Hyperion is
  active, LoadString may be gated behind integrity checks that refuse
  calls originating outside the normal script pipeline. Resolution is
  separate work (Hyperion bypass, out of scope).
- User-mode allocations made per `exec` are intentionally leaked (one
  page each). A production build should queue a cleanup APC after
  LoadString returns.
- Device ACL is `FILE_DEVICE_SECURE_OPEN` only — any elevated process
  can open the device. Tighten with `IoCreateDeviceSecure` + an SDDL
  string if you need lock-down.

## License

MIT.
