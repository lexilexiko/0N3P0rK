// Toolchain quirks, force-included into every translation unit through
// build_flags in platformio.ini (-include src/compat.h). Project code does not
// belong here.
#pragma once

// --- ESP32-S2 USB CDC does not export Serial from Arduino.h ----------------
#if CONFIG_IDF_TARGET_ESP32S2
#include <Arduino.h>
#include <USBCDC.h>
#ifndef Serial
extern USBCDC USBSerial;
#define Serial USBSerial
#endif
#endif

// --- newlib limits.h hides the C99 integer limits from C++ ----------------
// GCC's limits.h (lib/gcc/.../include-fixed/limits.h) defines LLONG_MAX and
// ULLONG_MAX only when __STDC_VERSION__ >= 199901L, and a C++ compiler never
// defines __STDC_VERSION__ (it sets __cplusplus instead). Lua's luaconf.h
// probes `defined(LLONG_MAX)` to decide whether the compiler has 'long long'
// and hits its own #error when the macro is missing, so script/pigvm.cpp —
// the one C++ translation unit that includes <lua.h> — would not build even
// though the vendored Lua C files compile fine. LLONG_MAX is an ordinary C99
// macro, so defining it the way <limits.h> would is all it takes.
#if defined(__cplusplus) && !defined(LLONG_MAX)
#include <limits.h>
#ifndef LLONG_MAX
#define LLONG_MAX __LONG_LONG_MAX__
#endif
#endif
