// script/pigvm.h — Lua VM for user scripts.
//
// Goal: let a card carry its own programs — games, demos, small radio helpers —
// without reflashing the firmware. Stage 1 is deliberately just the engine:
// boot it, run a script from SD, catch errors and runaway loops, hand every
// byte back. Screen / keyboard / SD bindings and the SCRIPTS menu come next.
//
// Safety is the whole design here:
//   * every allocation goes through a budget, so a script cannot starve the
//     heap the sniffer needs — it fails inside Lua instead;
//   * only base/table/string/math/coroutine are opened. No io, no os, no
//     package, no debug: a script has no business touching files or modules
//     behind the device's back;
//   * an instruction hook aborts a script that overruns its wall-clock budget,
//     so `while true do end` returns an error instead of hanging the device.

#pragma once

#if defined(PORK_LUA) && PORK_LUA

#include <Arduino.h>

namespace PigVm {

// Boot the VM. Returns false when the Lua state cannot be created (low heap);
// callers should simply carry on without scripting.
bool begin();

// Drop the state and give every byte back. Safe to call when not running.
void end();

// True while a state is alive. The SCRIPTS mode uses this to keep one state
// across REPL lines and release it when the tab is left.
bool isOpen();

// Run a script from SD, or a string. `budgetMs` is the wall-clock limit for the
// whole run; 0 means "no limit" and is only meant for tests.
// runString() starts a fresh console; runLine() keeps it, which is what a REPL
// session wants — and it leaves the state open, so variables survive between
// lines until end() is called.
bool runFile(const char* path, uint32_t budgetMs = 3000);
bool runString(const char* code, uint32_t budgetMs = 3000);
bool runLine(const char* code, uint32_t budgetMs = 1000);

// Reason for the last failure ("" when the last run was clean).
const char* lastError();

// Console: everything print() and every error produce, oldest first.
void        consoleClear();
uint8_t     consoleCount();
const char* consoleLine(uint8_t i);
void        consolePush(const char* text);

// What the VM currently holds and the cap it will not grow past.
size_t heapUsed();
size_t heapBudget();

} // namespace PigVm

#endif // PORK_LUA
