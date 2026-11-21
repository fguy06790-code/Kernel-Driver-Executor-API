@echo off
rem build.bat - KrnlExec driver + loader on Windows (WDK + MSVC).
rem
rem Open an "x64 Native Tools Command Prompt for VS 2022" with the WDK
rem environment loaded, then run this.
rem
rem Requirements: Windows 11 SDK, WDK 10, MSVC 14.3x.

setlocal

if "%WDK_METADATA_VERSION%"=="" (
    echo WDK environment not loaded. Launch from "x64 Native Tools Command
    echo Prompt for VS 2022" after installing the Windows Driver Kit.
    exit /b 1
)

set CL_COMMON=/nologo /std:c++17 /W3 /O2 /GS- /guard:cf-
set DRIVER_CL=%CL_COMMON% /kernel /D_AMD64_ /D_WIN64 /DNDIS60
set DRIVER_LINK=/subsystem:native /driver /entry:DriverEntry ^
    /nodefaultlib /machine:x64 ^
    ntoskrnl.lib hal.lib

echo [1/2] compiling driver.cpp ...
cl %DRIVER_CL% /c driver.cpp
if errorlevel 1 exit /b 1

link %DRIVER_LINK% driver.obj /out:KrnlExec.sys
if errorlevel 1 exit /b 1

echo [2/3] compiling loader.cpp ...
cl %CL_COMMON% /EHsc loader.cpp /Fe:krnlexec.exe
if errorlevel 1 exit /b 1

echo [3/3] compiling GUI ...
set GUI_SRC=gui\main.cpp gui\app.cpp gui\ipc.cpp gui\theme.cpp ^
            vendor\imgui\imgui.cpp ^
            vendor\imgui\imgui_draw.cpp ^
            vendor\imgui\imgui_tables.cpp ^
            vendor\imgui\imgui_widgets.cpp ^
            vendor\imgui\imgui_demo.cpp ^
            vendor\imgui\backends\imgui_impl_win32.cpp ^
            vendor\imgui\backends\imgui_impl_opengl3.cpp ^
            vendor\texteditor\TextEditor.cpp

cl %CL_COMMON% /EHsc /DUNICODE /D_UNICODE ^
   /Ivendor\imgui /Ivendor\imgui\backends /Ivendor\texteditor /Igui ^
   %GUI_SRC% /link /subsystem:windows ^
   opengl32.lib gdi32.lib user32.lib kernel32.lib shell32.lib ^
   shlwapi.lib ole32.lib dwmapi.lib imm32.lib ^
   /out:krnlexec-gui.exe
if errorlevel 1 exit /b 1

echo.
echo build ok:
dir /b KrnlExec.sys krnlexec.exe krnlexec-gui.exe

endlocal
