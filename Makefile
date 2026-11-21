# Cross-compile Makefile for building the usermode loader from Linux.
# Targets: krnlexec.exe (via mingw-w64).
#
# The driver (KrnlExec.sys) cannot be cross-compiled from Linux — build it
# on Windows with the WDK using build.bat (or load driver.cpp into a WDK
# "WDM Empty Driver" project in VS 2022). See README.md.

MINGW_CXX := x86_64-w64-mingw32-g++
CXXFLAGS  := -std=c++17 -O2 -municode -static

IMGUI_DIR := vendor/imgui
IMGUI_SRC := $(IMGUI_DIR)/imgui.cpp \
             $(IMGUI_DIR)/imgui_draw.cpp \
             $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp \
             $(IMGUI_DIR)/imgui_demo.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_win32.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

TE_DIR    := vendor/texteditor
TE_SRC    := $(TE_DIR)/TextEditor.cpp

GUI_SRC   := gui/main.cpp gui/app.cpp gui/ipc.cpp gui/theme.cpp
GUI_INC   := -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(TE_DIR) -Igui
GUI_LIBS  := -lopengl32 -lgdi32 -luser32 -lkernel32 -lshell32 -lshlwapi -lole32 -ldwmapi -limm32

all: krnlexec.exe krnlexec-gui.exe

krnlexec.exe: loader.cpp
	$(MINGW_CXX) $(CXXFLAGS) -o $@ $< -lkernel32 -luser32

krnlexec-gui.exe: $(GUI_SRC) $(IMGUI_SRC) $(TE_SRC)
	$(MINGW_CXX) $(CXXFLAGS) $(GUI_INC) -mwindows \
	    -DUNICODE -D_UNICODE \
	    -o $@ $(GUI_SRC) $(IMGUI_SRC) $(TE_SRC) $(GUI_LIBS)

clean:
	rm -f krnlexec.exe krnlexec-gui.exe driver.obj KrnlExec.sys

.PHONY: all clean
