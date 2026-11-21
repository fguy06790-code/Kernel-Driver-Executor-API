# Cross-compile Makefile for building the usermode loader from Linux.
# Targets: krnlexec.exe (via mingw-w64).
#
# The driver (KrnlExec.sys) cannot be cross-compiled from Linux — build it
# on Windows with the WDK using build.bat (or load driver.cpp into a WDK
# "WDM Empty Driver" project in VS 2022). See README.md.

MINGW_CXX := x86_64-w64-mingw32-g++
CXXFLAGS  := -std=c++17 -O2 -municode -static

all: krnlexec.exe

krnlexec.exe: loader.cpp
	$(MINGW_CXX) $(CXXFLAGS) -o $@ $< -lkernel32 -luser32

clean:
	rm -f krnlexec.exe driver.obj KrnlExec.sys

.PHONY: all clean
