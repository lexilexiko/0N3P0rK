// modes/scriptmode.cpp
#include "scriptmode.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include "../ui/loot_menu.h"
#include "../core/app.h"
#include "../cap/sniffer.h"
#include "../storage/littlefs_ops.h"
#include "../script/pigvm.h"
#include "../audio/sfx.h"
#include "../modes/blepig.h"
#include "../modes/irport.h"
#include "../modes/evilpig.h"
#include "../modes/pigpass.h"
#include "../modes/spectrum.h"
#include "../modes/usbsd.h"
#include "../modes/filemgr.h"
#include "../modes/xfer.h"
#include "../modes/badusb.h"
#include "../modes/mp3player.h"
#include "../modes/inspectorpig.h"
#include <SD.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// The mode is always compiled; only the execution is conditional, so the menu
// entry works either way and simply says LUA OFF when the flag is missing.
#if defined(PORK_LUA) && PORK_LUA
#define LUA_ON 1
#else
#define LUA_ON 0
#endif

bool ScriptMode::running = false;
ScriptMode::Tab ScriptMode::tab = ScriptMode::Tab::FILES;
ScriptMode::Phase ScriptMode::phase = ScriptMode::Phase::BROWSER;
ScriptMode::Script ScriptMode::scripts[ScriptMode::MAX_SCRIPTS];
uint8_t ScriptMode::count = 0;
uint8_t ScriptMode::sel = 0;
uint8_t ScriptMode::scroll = 0;
bool ScriptMode::keyLatch = false;
char ScriptMode::status[40] = "";
char ScriptMode::input[ScriptMode::INPUT_MAX] = "";
uint8_t ScriptMode::inputLen = 0;
uint8_t ScriptMode::outScroll = 0;

static bool isLuaName(const char* n) {
    if (!n) return false;
    size_t l = strlen(n);
    return l > 4 && strcasecmp(n + l - 4, ".lua") == 0;
}

// Every PigVm call is behind the flag: without Lua the header is empty, so the
// calls must not even be parsed.
void ScriptMode::vmRelease() {
#if LUA_ON
    PigVm::end();
#endif
}

// Drop every background hog before a script runs so a heavy .lua actually has
// the heap. Capture, radio toys, player, wifi — all of it. Lua itself is
// released on stop() / after each FILES run, so leaving SCRIPTS looks like we
// never opened it.
static void hushForLua() {
    if (Cap::isRunning()) Cap::stop();
    if (BlePigMode::isRunning()) BlePigMode::stop();
    if (IrPortMode::isRunning()) IrPortMode::stop();
    if (EvilPigMode::isRunning()) EvilPigMode::stop();
    if (PigpassMode::isRunning()) PigpassMode::stop();
    if (SpectrumMode::isRunning()) SpectrumMode::stop();
    if (UsbSdMode::isRunning()) UsbSdMode::stop();
    if (FileMgrMode::isRunning()) FileMgrMode::stop();
    if (XferMode::isRunning()) XferMode::stop();
    if (BadUsbMode::isRunning()) BadUsbMode::stop();
    if (Mp3PlayerMode::isRunning()) Mp3PlayerMode::stop();
    if (InspectorPig::isRunning()) InspectorPig::stop();
    if (LootMenu::isActive()) LootMenu::hide();
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    yield();
}

void ScriptMode::rescan() {
    count = 0;
    sel = 0;
    scroll = 0;
    if (!Storage::available()) {
        snprintf(status, sizeof(status), "NO SD");
        return;
    }
    Storage::ensureDir(Storage::DIR_SCRIPTS);
    File dir = SD.open(Storage::DIR_SCRIPTS);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        snprintf(status, sizeof(status), "NO /scripts");
        return;
    }
    File f = dir.openNextFile();
    while (f && count < MAX_SCRIPTS) {
        if (!f.isDirectory()) {
            const char* nm = Storage::baseName(f.name());
            if (nm && isLuaName(nm)) {
                Script& s = scripts[count];
                memset(&s, 0, sizeof(s));
                strncpy(s.name, nm, sizeof(s.name) - 1);
                s.size = (uint32_t)f.size();
                count++;
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    if (f) f.close();
    dir.close();
    if (count == 0) snprintf(status, sizeof(status), "NO .LUA");
    else snprintf(status, sizeof(status), "%u SCRIPTS", (unsigned)count);
}

// Tab switching owns the REPL's VM lifetime: the state is kept only while the
// REPL is open, so browsing scripts never holds the heap the capture path wants.
void ScriptMode::enterTab(Tab t) {
    if (tab == t) return;
    if (tab == Tab::REPL) vmRelease();
    tab = t;
    phase = (t == Tab::REPL) ? Phase::REPL : Phase::BROWSER;
    outScroll = 0;
    inputLen = 0;
    input[0] = '\0';
#if LUA_ON
    if (t == Tab::REPL) {
        if (PigVm::begin()) {
            char b[40];
            snprintf(b, sizeof(b), "lua repl  cap %uK",
                     (unsigned)(PigVm::heapBudget() / 1024));
            PigVm::consolePush(b);
        } else {
            PigVm::consolePush("VM FAILED - low heap");
        }
    }
#endif
}

// Runs the highlighted script. The VM is created and destroyed around this one
// call, so between runs the module holds nothing: the capture path is never
// competing with a script for the heap.
void ScriptMode::runSelected() {
#if LUA_ON
    if (count == 0) return;
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", Storage::DIR_SCRIPTS, scripts[sel].name);

    // The Enter that launched this is still held; latch it so the CONSOLE
    // screen cannot read the same press as "go back".
    keyLatch = true;

    vmRelease();                       // one VM at a time: drop any REPL state
    hushForLua();
    Display::showToast("RUN", 600);
    bool ok = PigVm::runFile(path, RUN_BUDGET);
    vmRelease();                       // script is done - give every byte back

    snprintf(status, sizeof(status), "%s %.24s", ok ? "OK" : "ERR",
             scripts[sel].name);
    phase = Phase::CONSOLE;
    outScroll = 0;
    SFX::play(ok ? SFX::CONFIRM : SFX::ERROR);
#else
    Display::showToast("LUA OFF - see lib/lua/README", 1800);
#endif
}

// One REPL line. The state is NOT recreated here, so a line can use the
// variables an earlier line defined - that is the whole point of the tab.
void ScriptMode::replSubmit() {
#if LUA_ON
    if (inputLen == 0) return;
    char echo[INPUT_MAX + 4];
    snprintf(echo, sizeof(echo), "> %s", input);
    PigVm::consolePush(echo);
    // Errors are pushed to the console by the VM itself (protectedCall), so a
    // failed line still shows its reason without any handling here.
    PigVm::runLine(input, LINE_BUDGET);
    inputLen = 0;
    input[0] = '\0';
    outScroll = 0;
#else
    inputLen = 0;
    input[0] = '\0';
    snprintf(status, sizeof(status), "LUA OFF");
#endif
}

void ScriptMode::start() {
    running = true;
    tab = Tab::FILES;
    phase = Phase::BROWSER;
    // The Enter that opened the menu is still held: latch it so it cannot be
    // read as "run this script".
    keyLatch = true;
    status[0] = '\0';
    input[0] = '\0';
    inputLen = 0;
    outScroll = 0;
    hushForLua();
#if LUA_ON
    PigVm::end();
    PigVm::consoleClear();
#endif
    rescan();
}

void ScriptMode::stop() {
    vmRelease();
    running = false;
    tab = Tab::FILES;
    phase = Phase::BROWSER;
    inputLen = 0;
    input[0] = '\0';
}

void ScriptMode::update() {
    if (!running) return;
    if (App::windowHidden()) return;
    handleInput();
}

void ScriptMode::handleInput() {
    if (!keyNewPress(keyLatch)) return;

    Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();

    // TAB switches tabs. It is a dedicated key, so it never collides with the
    // REPL's text input the way `,` / `/` (the LOOT convention) would.
    if (st.tab) {
        enterTab(tab == Tab::FILES ? Tab::REPL : Tab::FILES);
        return;
    }

    // ---- REPL: the keyboard belongs to the line editor ----
    if (tab == Tab::REPL) {
        if (keyEsc()) {                 // ` back to FILES (a second ` leaves)
            enterTab(Tab::FILES);
            return;
        }
        if (st.del) {
            if (inputLen > 0) input[--inputLen] = '\0';
            return;
        }
        if (st.enter) {
            replSubmit();
            return;
        }
        for (char c : st.word) {
            if (c < 32 || c >= 127) continue;
            if (inputLen + 1 >= INPUT_MAX) break;
            input[inputLen++] = c;
            input[inputLen] = '\0';
        }
        return;
    }

    // ---- FILES / CONSOLE ----
    if (keyEsc()) {
        if (phase == Phase::CONSOLE) { phase = Phase::BROWSER; return; }
        stop();
        return;
    }

    if (phase == Phase::CONSOLE) {
        uint8_t n = 0;
#if LUA_ON
        n = PigVm::consoleCount();
#endif
        if (M5Cardputer.Keyboard.isKeyPressed(';')) {
            if (outScroll > 0) outScroll--;
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('.')) {
            if ((uint8_t)(outScroll + OUT_ROWS) < n) outScroll++;
            return;
        }
        if (st.enter) phase = Phase::BROWSER;
        return;
    }

    // ---- BROWSER ----
    if (st.enter) { runSelected(); return; }
    if (M5Cardputer.Keyboard.isKeyPressed('r') ||
        M5Cardputer.Keyboard.isKeyPressed('R')) {
        rescan();
        Display::showToast("RESCAN", 600);
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (sel > 0) sel--;
        if (sel < scroll) scroll = sel;
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (sel + 1 < count) sel++;
        if (sel >= scroll + VIS_ROWS) scroll = (uint8_t)(sel - VIS_ROWS + 1);
        return;
    }
}

void ScriptMode::getStatusLine(char* buf, size_t n) {
    if (!buf || !n) return;
    if (tab == Tab::REPL) {
        snprintf(buf, n, "TAB files  ENT run  ` back  BS del");
        return;
    }
    if (phase == Phase::CONSOLE) {
        snprintf(buf, n, "OUT %.14s  ENT back  ` back", status);
        return;
    }
    snprintf(buf, n, "ENT run  TAB repl  ;/. pick  R rescan");
}

// ---- drawing ---------------------------------------------------------------
void ScriptMode::drawTabs(M5Canvas& canvas) {
    canvas.fillRect(0, 0, DISPLAY_W, 12, UiStyle::PANEL);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextDatum(top_left);

    const bool f = (tab == Tab::FILES);
    canvas.fillRect(2, 1, 50, 10, f ? UiStyle::GOLD : UiStyle::PANEL);
    canvas.setTextColor(f ? UiStyle::BG : UiStyle::DIM);
    canvas.drawString("FILES", 8, 3);

    const bool r = (tab == Tab::REPL);
    canvas.fillRect(54, 1, 46, 10, r ? UiStyle::GOLD : UiStyle::PANEL);
    canvas.setTextColor(r ? UiStyle::BG : UiStyle::DIM);
    canvas.drawString("REPL", 60, 3);

    // The right side of the tab strip carries the last activity line.
    char st[20];
    strncpy(st, status, sizeof(st) - 1);
    st[sizeof(st) - 1] = '\0';
    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString(st, 104, 3);
}

void ScriptMode::drawBrowser(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    drawTabs(canvas);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextDatum(top_left);

    if (count == 0) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString(status, 6, 28);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("/0N3P0rK/scripts", 6, 42);
        canvas.drawString("drop .lua files there", 6, 54);
        canvas.drawString("then press R", 6, 66);
        return;
    }

    const int y0 = 16, lh = 15;
    for (uint8_t i = 0; i < VIS_ROWS && (uint8_t)(scroll + i) < count; i++) {
        const uint8_t idx = (uint8_t)(scroll + i);
        const int y = y0 + (int)i * lh;
        const bool on = (idx == sel);
        if (on) {
            canvas.fillRect(3, y - 1, DISPLAY_W - 6, lh, UiStyle::GOLD);
            canvas.setTextColor(UiStyle::BG);
        } else {
            canvas.setTextColor(UiStyle::TEXT);
        }
        canvas.drawString(scripts[idx].name, 6, y);

        char sz[12];
        snprintf(sz, sizeof(sz), "%uK",
                 (unsigned)((scripts[idx].size + 1023u) / 1024u));
        canvas.setTextColor(on ? UiStyle::BG : UiStyle::DIM);
        canvas.setTextDatum(top_right);
        canvas.drawString(sz, DISPLAY_W - 6, y);
        canvas.setTextDatum(top_left);
    }

    canvas.setTextColor(UiStyle::DIM);
    if (scroll > 0) canvas.drawString("^", DISPLAY_W - 10, 20);
    if ((uint8_t)(scroll + VIS_ROWS) < count)
        canvas.drawString("v", DISPLAY_W - 10, y0 + (VIS_ROWS - 1) * lh);
}

void ScriptMode::drawOutput(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    drawTabs(canvas);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextDatum(top_left);

#if LUA_ON
    const uint8_t n = PigVm::consoleCount();
    const int y0 = 16, lh = 13;
    if (n == 0) {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("(no output)", 4, y0);
    }
    for (uint8_t i = 0; i < OUT_ROWS && (uint8_t)(outScroll + i) < n; i++) {
        canvas.setTextColor(UiStyle::TEXT);
        canvas.drawString(PigVm::consoleLine((uint8_t)(outScroll + i)),
                          4, y0 + (int)i * lh);
    }
    canvas.setTextColor(UiStyle::DIM);
    if ((uint8_t)(outScroll + OUT_ROWS) < n)
        canvas.drawString("v more", DISPLAY_W - 40, MAIN_H - 10);
#else
    canvas.setTextColor(UiStyle::GOLD);
    canvas.drawString("LUA IS OFF IN THIS BUILD", 4, 30);
    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString("see lib/lua/README.md", 4, 44);
#endif
}

void ScriptMode::drawRepl(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    drawTabs(canvas);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);
    canvas.setTextDatum(top_left);

#if LUA_ON
    const uint8_t n = PigVm::consoleCount();
    const int y0 = 15, lh = 13;
    // Newest lines sit just above the prompt, like a terminal would show them.
    const uint8_t first = (n > REPL_ROWS) ? (uint8_t)(n - REPL_ROWS) : 0;
    uint8_t shown = 0;
    for (uint8_t k = first; k < n && shown < REPL_ROWS; k++, shown++) {
        canvas.setTextColor(UiStyle::TEXT);
        canvas.drawString(PigVm::consoleLine(k), 4, y0 + (int)shown * lh);
    }

    const int py = MAIN_H - 13;
    canvas.fillRect(0, py - 1, DISPLAY_W, 13, UiStyle::PANEL);
    canvas.setTextColor(UiStyle::GOLD);
    canvas.drawString(">", 4, py + 1);
    canvas.setTextColor(UiStyle::TITLE);
    canvas.drawString(input, 14, py + 1);
    // Caret: red once the line is full, so the limit is visible before a
    // keystroke is silently dropped.
    const int cx = 14 + (int)inputLen * 6;
    canvas.fillRect(cx, py + 1, 5, 9,
                    (inputLen + 1 < INPUT_MAX) ? UiStyle::GREEN : UiStyle::RED);
#else
    canvas.setTextColor(UiStyle::GOLD);
    canvas.drawString("LUA IS OFF IN THIS BUILD", 4, 30);
    canvas.setTextColor(UiStyle::DIM);
    canvas.drawString("see lib/lua/README.md", 4, 44);
#endif
}

void ScriptMode::draw(M5Canvas& canvas) {
    if (tab == Tab::REPL) { drawRepl(canvas); return; }
    if (phase == Phase::CONSOLE) drawOutput(canvas);
    else drawBrowser(canvas);
}
