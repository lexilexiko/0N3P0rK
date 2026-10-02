// modes/scriptmode.h — SCRIPTS: run the Lua files that sit on the card.
//
// Two tabs, the same shape as LOOT / InspectorPig:
//   FILES — .lua scripts in /0N3P0rK/scripts, ENT runs the selected one
//   REPL  — type a line of Lua, run it on the device; variables survive
//           between lines until the tab is left
//
// The mode is always compiled, but execution sits behind the optional Lua
// build flag. Without it the screen says LUA OFF and points at lib/lua/README,
// so the menu entry is never a dead end.
//
// isTyping() exists for the same reason FileMgrMode has one: Backspace is the
// global "minimize" key for overlay modes (app.cpp), and the REPL needs it to
// delete a character instead.
#pragma once

#include <Arduino.h>
#include <M5Unified.h>

class ScriptMode {
public:
    static void start();
    static void stop();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isRunning() { return running; }
    static void getStatusLine(char* buf, size_t n);
    static bool isTyping() { return running && tab == Tab::REPL && phase == Phase::REPL; }

private:
    enum class Tab   : uint8_t { FILES = 0, REPL = 1 };
    // CONSOLE, not OUTPUT: <Arduino.h> pulls in esp32-hal-gpio.h, which
    // #defines OUTPUT (pin mode). The preprocessor would rewrite the
    // enumerator into "0x03" and take the whole class down with it.
    // INPUT / PULLUP / INPUT_PULLUP are the same trap.
    enum class Phase : uint8_t { BROWSER, CONSOLE, REPL };

    struct Script {
        char     name[40];
        uint32_t size;
    };

    static constexpr uint8_t  MAX_SCRIPTS = 16;
    static constexpr uint8_t  VIS_ROWS    = 4;   // browser rows on screen
    static constexpr uint8_t  OUT_ROWS    = 6;   // console rows in CONSOLE
    static constexpr uint8_t  REPL_ROWS   = 5;   // console rows above the input
    static constexpr uint8_t  INPUT_MAX   = 60;
    static constexpr uint32_t RUN_BUDGET  = 5000;  // ms for a whole script
    static constexpr uint32_t LINE_BUDGET = 1500;  // ms for one REPL line

    static bool    running;
    static Tab     tab;
    static Phase   phase;
    static Script  scripts[MAX_SCRIPTS];
    static uint8_t count;
    static uint8_t sel;
    static uint8_t scroll;
    static bool    keyLatch;
    static char    status[40];
    static char    input[INPUT_MAX];
    static uint8_t inputLen;
    static uint8_t outScroll;

    static void rescan();
    static void runSelected();
    static void replSubmit();
    static void enterTab(Tab t);
    static void vmRelease();
    static void handleInput();
    static void drawTabs(M5Canvas& canvas);
    static void drawBrowser(M5Canvas& canvas);
    static void drawOutput(M5Canvas& canvas);
    static void drawRepl(M5Canvas& canvas);
};
