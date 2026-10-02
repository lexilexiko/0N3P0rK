// script/pigvm.cpp — see pigvm.h for the rules this file lives by.
#include "pigvm.h"

#if defined(PORK_LUA) && PORK_LUA

#include <SD.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace PigVm {

// The cap on everything Lua may hold. A Cardputer has no PSRAM, so a script
// gets a fixed slice and fails inside Lua when it tries to exceed it — the
// sniffer's heap stays untouched.
static const size_t  kBudget       = 48u * 1024u;
static const uint8_t CONSOLE_LINES = 32;   // ring buffer, oldest-first readout
static const uint8_t CONSOLE_LEN   = 41;
static const uint32_t SCRIPT_MAX   = 16384u;   // source size cap, keeps load cheap
static const int     HOOK_STEPS    = 20000;    // instructions between deadline checks

static lua_State* s_L = nullptr;
static size_t     s_used = 0;
static size_t     s_budget = kBudget;
static uint32_t   s_deadline = 0;
static char       s_error[64] = "";
static char       s_console[CONSOLE_LINES][CONSOLE_LEN];
static uint8_t    s_consoleHead = 0;   // next slot to write
static uint8_t    s_consoleN = 0;      // valid lines (<= CONSOLE_LINES)

// Lua's allocator. osize is a type tag (not a size) when ptr is NULL, so
// fresh mallocs must count nsize in full and must still hit the budget —
// otherwise a script can grow until the device OOMs and the 48K cap is a lie.
static void* vmAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    (void)ud;
    const size_t oldBytes = ptr ? osize : 0;
    if (nsize == 0) {
        if (ptr) {
            s_used = (s_used >= oldBytes) ? s_used - oldBytes : 0;
            free(ptr);
        }
        return nullptr;
    }
    if (s_budget && (s_used - oldBytes + nsize) > s_budget) return nullptr;
    void* np = realloc(ptr, nsize);
    if (!np) return nullptr;
    s_used = s_used - oldBytes + nsize;
    return np;
}

// Fires every HOOK_STEPS instructions: the wall-clock budget is what stops a
// runaway script, not the instruction count, because a sleep-heavy loop can
// burn almost no instructions while still hanging.
static void vmHook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    if (s_deadline && (int32_t)(millis() - s_deadline) > 0)
        luaL_error(L, "script budget exceeded");
}

// ---- console ---------------------------------------------------------------
// A ring buffer: pushing never moves 32 lines around, and the readout index
// stays valid while a script keeps printing.
void consoleClear() {
    s_consoleHead = 0;
    s_consoleN = 0;
    for (uint8_t i = 0; i < CONSOLE_LINES; i++) s_console[i][0] = '\0';
}

void consolePush(const char* text) {
    if (!text) return;
    strncpy(s_console[s_consoleHead], text, CONSOLE_LEN - 1);
    s_console[s_consoleHead][CONSOLE_LEN - 1] = '\0';
    s_consoleHead = (uint8_t)((s_consoleHead + 1) % CONSOLE_LINES);
    if (s_consoleN < CONSOLE_LINES) s_consoleN++;
}

uint8_t consoleCount() { return s_consoleN; }

// Index 0 is the oldest line still held, so the UI can scroll naturally.
const char* consoleLine(uint8_t i) {
    if (i >= s_consoleN) return "";
    uint8_t first = (uint8_t)((s_consoleHead + CONSOLE_LINES - s_consoleN) % CONSOLE_LINES);
    return s_console[(uint8_t)((first + i) % CONSOLE_LINES)];
}

size_t heapUsed() { return s_used; }
size_t heapBudget() { return s_budget; }
const char* lastError() { return s_error; }
bool isOpen() { return s_L != nullptr; }

// ---- bindings --------------------------------------------------------------
// Stage 1 exposes only what cannot touch anything: printing, sleeping, and two
// read-only reads. Screen / keys / SD are stage 2, together with the SCRIPTS
// menu that gives them a place to run.
static int l_print(lua_State* L) {
    int n = lua_gettop(L);
    char line[CONSOLE_LEN];
    size_t o = 0;
    for (int i = 1; i <= n; i++) {
        // luaL_tolstring honours __tostring and leaves the string on the stack,
        // so it must be popped for every argument.
        const char* s = luaL_tolstring(L, i, nullptr);
        if (s) {
            size_t l = strlen(s);
            for (size_t k = 0; k < l && o + 1 < sizeof(line); k++) line[o++] = s[k];
        }
        lua_pop(L, 1);
        if (i < n && o + 1 < sizeof(line)) line[o++] = ' ';
    }
    line[o] = '\0';
    consolePush(line);
    Serial.printf("[LUA] %s\n", line);
    return 0;
}

// A script may yield the CPU, but not hang the device. delay() is one Lua
// instruction, so the count-hook never fires during a sleep — the deadline
// has to be checked here or `while true do delay(1000) end` freezes the board.
static int l_delay(lua_State* L) {
    lua_Integer ms = luaL_optinteger(L, 1, 0);
    if (ms <= 0) return 0;
    if (ms > 1000) ms = 1000;
    const uint32_t t0 = millis();
    while ((int32_t)(millis() - t0) < (int32_t)ms) {
        if (s_deadline && (int32_t)(millis() - s_deadline) > 0)
            luaL_error(L, "script budget exceeded");
        delay(20);
        yield();
    }
    return 0;
}

static int l_millis(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)millis());
    return 1;
}

static int l_heap(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)ESP.getFreeHeap());
    return 1;
}

// Shared entry/exit for every protected call, so the error text and the console
// are handled in exactly one place.
static bool protectedCall(int nargs, int nresults) {
    int rc = lua_pcall(s_L, nargs, nresults, 0);
    if (rc == LUA_OK) return true;
    const char* msg = lua_tostring(s_L, -1);
    snprintf(s_error, sizeof(s_error), "%s", msg ? msg : "lua error");
    consolePush(s_error);
    Serial.printf("[LUA] error: %s\n", s_error);
    lua_pop(s_L, 1);
    return false;
}

static void clearHook() {
    s_deadline = 0;
    if (s_L) lua_sethook(s_L, nullptr, 0, 0);
}

// ---- lifecycle -------------------------------------------------------------
bool begin() {
    if (s_L) return true;
    s_used = 0;
    s_budget = kBudget;
    s_deadline = 0;
    s_error[0] = '\0';
    consoleClear();

    s_L = lua_newstate(vmAlloc, nullptr);
    if (!s_L) {
        snprintf(s_error, sizeof(s_error), "no heap for vm");
        return false;
    }

    // Deliberately NOT luaL_openlibs(): io/os/package/debug stay closed. The
    // four that are opened are pure computation, and `print` is replaced with
    // the console version below.
    luaL_requiref(s_L, LUA_GNAME,       luaopen_base,      1); lua_pop(s_L, 1);
    luaL_requiref(s_L, LUA_TABLIBNAME,  luaopen_table,     1); lua_pop(s_L, 1);
    luaL_requiref(s_L, LUA_STRLIBNAME,  luaopen_string,    1); lua_pop(s_L, 1);
    luaL_requiref(s_L, LUA_MATHLIBNAME, luaopen_math,      1); lua_pop(s_L, 1);
    luaL_requiref(s_L, LUA_COLIBNAME,   luaopen_coroutine, 1); lua_pop(s_L, 1);

    // luaopen_base also installs loadfile/dofile (fopen). A script must not
    // reach the card behind FileMgr's back — REPL already has load() for code.
    lua_pushnil(s_L); lua_setglobal(s_L, "loadfile");
    lua_pushnil(s_L); lua_setglobal(s_L, "dofile");

    lua_register(s_L, "print",  l_print);
    lua_register(s_L, "delay",  l_delay);
    lua_register(s_L, "millis", l_millis);
    lua_register(s_L, "heap",   l_heap);

    Serial.printf("[LUA] vm up, budget %u K\n", (unsigned)(kBudget / 1024));
    return true;
}

void end() {
    clearHook();
    if (s_L) {
        lua_gc(s_L, LUA_GCCOLLECT, 0);
        lua_close(s_L);
        s_L = nullptr;
    }
    s_used = 0;
    s_deadline = 0;
    Serial.printf("[LUA] vm down, free=%u\n", (unsigned)ESP.getFreeHeap());
}

// ---- running ---------------------------------------------------------------
// Shared core. `clearFirst` is off for REPL lines, so a session keeps its
// scrollback instead of being wiped on every Enter.
static bool runCore(const char* code, uint32_t budgetMs, bool clearFirst) {
    if (!code || !code[0]) return false;
    if (!begin()) return false;

    if (clearFirst) consoleClear();
    s_error[0] = '\0';
    s_deadline = budgetMs ? (millis() + budgetMs) : 0;
    lua_sethook(s_L, vmHook, LUA_MASKCOUNT, HOOK_STEPS);

    // Compile before running, so a syntax error is reported without executing
    // a single line of the script.
    int rc = luaL_loadstring(s_L, code);
    if (rc != LUA_OK) {
        const char* msg = lua_tostring(s_L, -1);
        snprintf(s_error, sizeof(s_error), "%s", msg ? msg : "load error");
        consolePush(s_error);
        Serial.printf("[LUA] load error: %s\n", s_error);
        lua_pop(s_L, 1);
        clearHook();
        return false;
    }

    bool ok = protectedCall(0, 0);
    clearHook();
    return ok;
}

bool runString(const char* code, uint32_t budgetMs) {
    return runCore(code, budgetMs, true);
}

bool runLine(const char* code, uint32_t budgetMs) {
    return runCore(code, budgetMs, false);
}

bool runFile(const char* path, uint32_t budgetMs) {
    if (!path || !path[0]) return false;
    File f = SD.open(path, "r");
    if (!f) {
        snprintf(s_error, sizeof(s_error), "open failed");
        consolePush(s_error);
        return false;
    }
    size_t n = f.size();
    if (n == 0 || n > SCRIPT_MAX) {
        f.close();
        snprintf(s_error, sizeof(s_error), "size %u not allowed", (unsigned)n);
        consolePush(s_error);
        return false;
    }
    // The source is on the heap only until it is compiled; the VM owns the
    // compiled form from then on, so a long script costs nothing extra later.
    char* src = (char*)malloc(n + 1);
    if (!src) {
        f.close();
        snprintf(s_error, sizeof(s_error), "no heap for source");
        return false;
    }
    size_t got = f.read((uint8_t*)src, n);
    f.close();
    src[got] = '\0';

    bool ok = runString(src, budgetMs);
    free(src);
    return ok;
}

} // namespace PigVm

#endif // PORK_LUA
