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

echo [2/2] compiling loader.cpp ...
cl %CL_COMMON% /EHsc loader.cpp /Fe:krnlexec.exe
if errorlevel 1 exit /b 1

echo.
echo build ok:
dir /b KrnlExec.sys krnlexec.exe

endlocal
