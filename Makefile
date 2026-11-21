# Cross-compile Makefile for building the usermode loader from Linux.
# Targets: krnlexec.exe (via mingw-w64).
#
# The driver (KrnlExec.sys) cannot be cross-compiled from Linux — build it
# on Windows with the WDK using build.bat (or load driver.cpp into a WDK
# "WDM Empty Driver" project in VS 2022). See README.md.

MINGW_CXX := x86_64-w64-mingw32-g++
CXXFLAGS  := -std=c++17 -O2 -municode

WV2_DIR   := vendor/webview2
WV2_INC   := -I$(WV2_DIR)/include
WV2_LIB   := -L$(WV2_DIR)/x64 -lWebView2Loader

GUI_SRC   := gui/host.cpp gui/ipc.cpp
GUI_INC   := $(WV2_INC) -Igui
GUI_LIBS  := -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -lkernel32 -luser32 -ladvapi32

all: krnlexec.exe krnlexec-gui.exe

krnlexec.exe: loader.cpp
	$(MINGW_CXX) $(CXXFLAGS) -static -o $@ $< -lkernel32 -luser32

krnlexec-gui.exe: $(GUI_SRC)
	$(MINGW_CXX) $(CXXFLAGS) $(GUI_INC) -mwindows \
	    -DUNICODE -D_UNICODE \
	    -static-libgcc -static-libstdc++ \
	    -o $@ $(GUI_SRC) $(WV2_LIB) $(GUI_LIBS)

clean:
	rm -f krnlexec.exe krnlexec-gui.exe driver.obj KrnlExec.sys

.PHONY: all clean
