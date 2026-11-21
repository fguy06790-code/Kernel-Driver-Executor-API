@echo off
rem build.bat - KrnlExec driver + loader + GUI on Windows (WDK + MSVC).
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

echo [1/3] compiling driver.cpp ...
cl %DRIVER_CL% /c driver.cpp
if errorlevel 1 exit /b 1

link %DRIVER_LINK% driver.obj /out:KrnlExec.sys
if errorlevel 1 exit /b 1

echo [2/3] compiling CLI loader.cpp ...
cl %CL_COMMON% /EHsc loader.cpp /Fe:krnlexec.exe
if errorlevel 1 exit /b 1

echo [3/3] compiling GUI (WebView2) ...
set GUI_SRC=gui\host.cpp gui\ipc.cpp
cl %CL_COMMON% /EHsc /DUNICODE /D_UNICODE ^
   /Ivendor\webview2\include /Igui ^
   %GUI_SRC% /link /subsystem:windows ^
   vendor\webview2\x64\WebView2Loader.dll.lib ^
   ole32.lib oleaut32.lib uuid.lib shell32.lib shlwapi.lib ^
   kernel32.lib user32.lib advapi32.lib ^
   /out:krnlexec-gui.exe
if errorlevel 1 exit /b 1

rem assemble dist/
if not exist dist mkdir dist
if not exist dist\web mkdir dist\web
copy /y KrnlExec.sys dist\ > nul
copy /y krnlexec.exe dist\ > nul
copy /y krnlexec-gui.exe dist\ > nul
copy /y vendor\webview2\x64\WebView2Loader.dll dist\ > nul
copy /y gui\web\*.* dist\web\ > nul

echo.
echo build ok. dist/ layout:
dir /b dist dist\web

endlocal
