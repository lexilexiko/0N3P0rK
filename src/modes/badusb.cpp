// BadUSB + BadBLE: SCRIPTS / LIVE / PANEL — UI never blocks on link.
#include "badusb.h"
#include "badusb_hid.h"
#include "../storage/littlefs_ops.h"
#include "../cap/sniffer.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include "../core/app.h"
#include "../piglet/avatar.h"
#include "../audio/sfx.h"
#include "../modes/usbsd.h"
#include "../modes/blepig.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

namespace BadUsbMode {

static constexpr const char* DIR_BADUSB = "/0N3P0rK/badusb";
static constexpr uint8_t MAX_SCRIPTS = 16;
static constexpr uint8_t kNameMax = 40;

enum class Tab : uint8_t { Scripts = 0, Live = 1, Panel = 2 };
enum class Phase : uint8_t { Idle, WaitLink, Ready, Running, Done, Fail };
enum class Transport : uint8_t { Usb = 0, Ble = 1 };
enum class Profile : uint8_t { PC = 0, Phone = 1 };

struct Preset {
    const char* label;
    void (*fn)();
};

static bool s_run = false;
static Tab s_tab = Tab::Scripts;
static Phase s_phase = Phase::Idle;
static Transport s_tr = Transport::Usb;
static Profile s_prof = Profile::PC;
static char s_files[MAX_SCRIPTS][kNameMax];
static uint8_t s_fileCount = 0;
static uint8_t s_sel = 0;
static uint8_t s_scroll = 0;
static uint8_t s_panelIdx = 0;
static char s_status[48] = "ready";
static char s_lastErr[40] = "";
static bool s_keyLatch = false;
static bool s_linkOk = false;
static bool s_liveArmed = false; // Live tab: ENT arms it (all keys -> target), FN+` disarms
static uint32_t s_waitStart = 0;
static bool s_hidStarted = false;
static uint32_t s_defaultDelayMs = 0;
static char s_prevLine[192] = "";
static bool s_hasPrevLine = false;

static void setStatus(const char* s) {
    strncpy(s_status, s, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
}

static bool hidConnected() {
    if (s_tr == Transport::Usb) return BadUsbHid::usbMounted();
    return BadUsbHid::bleConnected();
}

static void hidPress(uint8_t k) {
    if (s_tr == Transport::Usb) BadUsbHid::usbPress(k);
    else BadUsbHid::blePress(k);
}
static void hidRelease(uint8_t k) {
    if (s_tr == Transport::Usb) BadUsbHid::usbRelease(k);
    else BadUsbHid::bleRelease(k);
}
static void hidReleaseAll() {
    if (s_tr == Transport::Usb) BadUsbHid::usbReleaseAll();
    else BadUsbHid::bleReleaseAll();
}
static void hidPrint(const char* t) {
    if (s_tr == Transport::Usb) BadUsbHid::usbPrint(t);
    else BadUsbHid::blePrint(t);
}
static void hidCombo(bool gui, bool alt, bool ctrl, bool shift, uint8_t key) {
    if (s_tr == Transport::Usb) BadUsbHid::usbModifierCombo(gui, alt, ctrl, shift, key);
    else BadUsbHid::bleModifierCombo(gui, alt, ctrl, shift, key);
}
static void tap(uint8_t k) {
    hidPress(k);
    delay(s_tr == Transport::Ble ? 30 : 20);
    hidRelease(k);
}

static void p_guiR() { hidCombo(true, false, false, false, 'r'); }
static void p_altTab() { hidCombo(false, true, false, false, BAD_KEY_TAB); }
static void p_ctrlC() { hidCombo(false, false, true, false, 'c'); }
static void p_ctrlV() { hidCombo(false, false, true, false, 'v'); }
static void p_ctrlA() { hidCombo(false, false, true, false, 'a'); }
static void p_enter() { tap(BAD_KEY_RETURN); }
static void p_esc() { tap(BAD_KEY_ESC); }
static void p_tab() { tap(BAD_KEY_TAB); }
static void p_up() { tap(BAD_KEY_UP_ARROW); }
static void p_down() { tap(BAD_KEY_DOWN_ARROW); }
static void p_left() { tap(BAD_KEY_LEFT_ARROW); }
static void p_right() { tap(BAD_KEY_RIGHT_ARROW); }
static void p_space() { tap(' '); }
static void p_bs() { tap(BAD_KEY_BACKSPACE); }
static void p_hello() {
    hidPrint("Hello from 0N3P0rK");
    tap(BAD_KEY_RETURN);
}
static void p_notepad() {
    p_guiR();
    delay(400);
    hidPrint("notepad");
    delay(100);
    tap(BAD_KEY_RETURN);
}
static void p_cmd() {
    p_guiR();
    delay(400);
    hidPrint("cmd");
    delay(100);
    tap(BAD_KEY_RETURN);
}

static const Preset PRESETS_PC[] = {
    {"Win+R", p_guiR},   {"Notepad", p_notepad}, {"CMD", p_cmd},
    {"AltTab", p_altTab}, {"Ctrl+C", p_ctrlC},   {"Ctrl+V", p_ctrlV},
    {"Ctrl+A", p_ctrlA},  {"Enter", p_enter},    {"Esc", p_esc},
    {"Hello", p_hello},   {"Up", p_up},           {"Down", p_down},
};
static const uint8_t PRESETS_PC_N = sizeof(PRESETS_PC) / sizeof(PRESETS_PC[0]);

static const Preset PRESETS_PHONE[] = {
    {"Enter", p_enter}, {"Esc", p_esc},   {"Tab", p_tab},
    {"Space", p_space}, {"Bksp", p_bs},   {"Up", p_up},
    {"Down", p_down},   {"Left", p_left}, {"Right", p_right},
    {"Hello", p_hello}, {"Ctrl+C", p_ctrlC}, {"Ctrl+V", p_ctrlV},
};
static const uint8_t PRESETS_PHONE_N = sizeof(PRESETS_PHONE) / sizeof(PRESETS_PHONE[0]);

static const Preset* presets() {
    return s_prof == Profile::PC ? PRESETS_PC : PRESETS_PHONE;
}
static uint8_t presetCount() {
    return s_prof == Profile::PC ? PRESETS_PC_N : PRESETS_PHONE_N;
}

struct ListCtx { uint8_t n; };
static void onFile(const char* name, size_t size, void* ctx) {
    ListCtx* c = (ListCtx*)ctx;
    if (!name || c->n >= MAX_SCRIPTS) return;
    if (strlen(name) < 5) return;
    const char* dot = strrchr(name, '.');
    if (!dot || strcasecmp(dot, ".txt") != 0) return;
    if (name[0] == '.') return;
    strncpy(s_files[c->n], name, kNameMax - 1);
    s_files[c->n][kNameMax - 1] = '\0';
    c->n++;
    (void)size;
}

static void rescan() {
    s_fileCount = 0;
    s_sel = 0;
    s_scroll = 0;
    Storage::ensureDir(DIR_BADUSB);
    ListCtx ctx{0};
    Storage::forEachInDir(DIR_BADUSB, onFile, &ctx);
    s_fileCount = ctx.n;
    if (s_fileCount == 0)
        setStatus("no .txt in /0N3P0rK/badusb");
    else
        snprintf(s_status, sizeof(s_status), "%u script(s)", (unsigned)s_fileCount);
}

static void stopHid() {
    BadUsbHid::usbEnd();
    BadUsbHid::bleEnd();
    s_linkOk = false;
    s_hidStarted = false;
}

// Start HID stack only — never block UI. Link polled in update().
static void startHidStack() {
    if (Cap::isRunning()) Cap::stop();

    if (s_tr == Transport::Usb) {
        if (UsbSdMode::isRunning()) UsbSdMode::stop();
        if (!BadUsbHid::usbBegin()) {
            setStatus("USB HID unavailable");
            strncpy(s_lastErr, "USB_MODE?", sizeof(s_lastErr) - 1);
            s_phase = Phase::Fail;
            return;
        }
        s_hidStarted = true;
        if (BadUsbHid::usbMounted()) {
            s_linkOk = true;
            s_phase = Phase::Ready;
            setStatus("USB Host Connected");
        } else {
            s_linkOk = false;
            s_phase = Phase::WaitLink;
            s_waitStart = millis();
            setStatus("Waiting USB Host...");
        }
        return;
    }

    // BLE
    if (BlePigMode::isRunning()) BlePigMode::stop();
    WiFi.mode(WIFI_OFF);
    delay(30);
    if (!BadUsbHid::bleBegin("0N3P0rK", "lexilexiko")) {
        setStatus("BLE lib missing");
        strncpy(s_lastErr, "ESP32 BLE Keyboard", sizeof(s_lastErr) - 1);
        s_phase = Phase::Fail;
        return;
    }
    s_hidStarted = true;
    if (BadUsbHid::bleConnected()) {
        s_linkOk = true;
        s_phase = Phase::Ready;
        setStatus("BLE Connected");
    } else {
        s_linkOk = false;
        s_phase = Phase::WaitLink;
        s_waitStart = millis();
        {
            uint32_t pin = BadUsbHid::blePasskey();
            if (pin)
                snprintf(s_status, sizeof(s_status), "Waiting BLE... PIN %06u", (unsigned)pin);
            else
                snprintf(s_status, sizeof(s_status), "Waiting BLE pair...");
        }
    }
}

static bool needLinkForAction() {
    if (s_linkOk && hidConnected()) return true;
    if (!s_hidStarted) startHidStack();
    return s_linkOk && hidConnected();
}

static uint8_t mapSpecial(const char* tok) {
    if (!tok) return 0;
    if (!strcasecmp(tok, "ENTER") || !strcasecmp(tok, "RETURN")) return BAD_KEY_RETURN;
    if (!strcasecmp(tok, "ESC") || !strcasecmp(tok, "ESCAPE")) return BAD_KEY_ESC;
    if (!strcasecmp(tok, "TAB")) return BAD_KEY_TAB;
    if (!strcasecmp(tok, "SPACE")) return ' ';
    if (!strcasecmp(tok, "BACKSPACE") || !strcasecmp(tok, "DELETE")) return BAD_KEY_BACKSPACE;
    if (!strcasecmp(tok, "UP")) return BAD_KEY_UP_ARROW;
    if (!strcasecmp(tok, "DOWN")) return BAD_KEY_DOWN_ARROW;
    if (!strcasecmp(tok, "LEFT")) return BAD_KEY_LEFT_ARROW;
    if (!strcasecmp(tok, "RIGHT")) return BAD_KEY_RIGHT_ARROW;
    if (!strcasecmp(tok, "F1")) return BAD_KEY_F1;
    if (!strcasecmp(tok, "F2")) return BAD_KEY_F2;
    if (!strcasecmp(tok, "F3")) return BAD_KEY_F3;
    if (!strcasecmp(tok, "F4")) return BAD_KEY_F4;
    if (!strcasecmp(tok, "F5")) return BAD_KEY_F5;
    if (!strcasecmp(tok, "F6")) return BAD_KEY_F6;
    if (!strcasecmp(tok, "F7")) return BAD_KEY_F7;
    if (!strcasecmp(tok, "F8")) return BAD_KEY_F8;
    if (!strcasecmp(tok, "F9")) return BAD_KEY_F9;
    if (!strcasecmp(tok, "F10")) return BAD_KEY_F10;
    if (!strcasecmp(tok, "F11")) return BAD_KEY_F11;
    if (!strcasecmp(tok, "F12")) return BAD_KEY_F12;
    return 0;
}

static void trimInPlace(char* s) {
    if (!s) return;
    char* a = s;
    while (*a && isspace((unsigned char)*a)) a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static bool runLine(char* line) {    trimInPlace(line);
    if (!line[0] || line[0] == '#') return true;
    if (!strncasecmp(line, "REM", 3) && (line[3] == 0 || isspace((unsigned char)line[3])))
        return true;
    if (!strncasecmp(line, "DELAY", 5) && isspace((unsigned char)line[5])) {
        int ms = atoi(line + 5);
        if (ms < 0) ms = 0;
        if (ms > 60000) ms = 60000;
        delay((uint32_t)ms);
        return true;
    }
    if (!strncasecmp(line, "DEFAULT_DELAY", 13) &&
        (line[13] == 0 || isspace((unsigned char)line[13]))) {
        int ms = atoi(line + 13);
        if (ms < 0) ms = 0;
        if (ms > 60000) ms = 60000;
        s_defaultDelayMs = (uint32_t)ms;
        return true;
    }
    if (!strncasecmp(line, "DEFAULTDELAY", 12) &&
        (line[12] == 0 || isspace((unsigned char)line[12]))) {
        int ms = atoi(line + 12);
        if (ms < 0) ms = 0;
        if (ms > 60000) ms = 60000;
        s_defaultDelayMs = (uint32_t)ms;
        return true;
    }
    if (!strncasecmp(line, "STRING", 6) && isspace((unsigned char)line[6])) {
        char* p = line + 6;
        while (*p && isspace((unsigned char)*p)) p++;
        hidPrint(p);
        return true;
    }
    uint8_t sk = mapSpecial(line);
    if (sk) {
        tap(sk);
        return true;
    }
    bool gui = false, alt = false, ctrl = false, shift = false;
    char* p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char tok[24];
        int ti = 0;
        while (*p && !isspace((unsigned char)*p) && ti < 23) tok[ti++] = *p++;
        tok[ti] = 0;
        if (!strcasecmp(tok, "GUI") || !strcasecmp(tok, "WINDOWS") || !strcasecmp(tok, "META") ||
            !strcasecmp(tok, "SUPER"))
            gui = true;
        else if (!strcasecmp(tok, "ALT"))
            alt = true;
        else if (!strcasecmp(tok, "CTRL") || !strcasecmp(tok, "CONTROL"))
            ctrl = true;
        else if (!strcasecmp(tok, "SHIFT"))
            shift = true;
        else {
            uint8_t k = mapSpecial(tok);
            if (!k && tok[0] && !tok[1]) k = (uint8_t)tok[0];
            if (!k && tok[0]) k = (uint8_t)tolower((unsigned char)tok[0]);
            hidCombo(gui, alt, ctrl, shift, k);
            gui = alt = ctrl = shift = false;
        }
    }
    if (gui || alt || ctrl || shift) hidCombo(gui, alt, ctrl, shift, 0);
    return true;
}

// REM / DELAY / DEFAULT_DELAY / DEFAULTDELAY are timing/no-op lines — they
// don't count as a "last action" for REPEAT and don't get the default
// inter-key delay stacked after them (they already control timing directly).
static bool isTimingOnly(const char* line) {
    if (!line[0] || line[0] == '#') return true;
    if (!strncasecmp(line, "REM", 3) && (line[3] == 0 || isspace((unsigned char)line[3]))) return true;
    if (!strncasecmp(line, "DELAY", 5) && (line[5] == 0 || isspace((unsigned char)line[5]))) return true;
    if (!strncasecmp(line, "DEFAULT_DELAY", 13) && (line[13] == 0 || isspace((unsigned char)line[13]))) return true;
    if (!strncasecmp(line, "DEFAULTDELAY", 12) && (line[12] == 0 || isspace((unsigned char)line[12]))) return true;
    return false;
}

// One script line, with REPEAT and DEFAULT_DELAY handling on top of runLine().
static void execDuckyLine(char* line) {
    trimInPlace(line);
    if (!strncasecmp(line, "REPEAT", 6) && (line[6] == 0 || isspace((unsigned char)line[6]))) {
        int n = atoi(line + 6);
        if (n <= 0) n = 1;
        if (n > 50) n = 50; // a typo'd REPEAT 5000 shouldn't hang the device
        if (s_hasPrevLine) {
            for (int i = 0; i < n && s_run; i++) {
                runLine(s_prevLine);
                if (s_defaultDelayMs) delay(s_defaultDelayMs);
            }
        }
        return;
    }
    runLine(line);
    if (!isTimingOnly(line)) {
        strncpy(s_prevLine, line, sizeof(s_prevLine) - 1);
        s_prevLine[sizeof(s_prevLine) - 1] = '\0';
        s_hasPrevLine = true;
        if (s_defaultDelayMs) delay(s_defaultDelayMs);
    }
}

static bool runScriptFile(const char* name) {
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", DIR_BADUSB, name);
    if (!Storage::fileExists(path)) {
        snprintf(s_lastErr, sizeof(s_lastErr), "missing %s", name);
        return false;
    }
    File f = SD.open(path, FILE_READ);
    if (!f) {
        strncpy(s_lastErr, "open fail", sizeof(s_lastErr) - 1);
        return false;
    }
    setStatus("running...");
    s_defaultDelayMs = 0;
    s_hasPrevLine = false;
    char line[192];
    size_t li = 0;
    while (f.available()) {
        char c = (char)f.read();
        if (c == '\r') continue;
        if (c == '\n' || li >= sizeof(line) - 1) {
            line[li] = 0;
            execDuckyLine(line);
            li = 0;
            yield();
            if (!s_run) break;
        } else {
            line[li++] = c;
        }
    }
    if (li > 0 && s_run) {
        line[li] = 0;
        execDuckyLine(line);
    }
    f.close();
    hidReleaseAll();
    return true;
}

static void runSelectedScript() {
    if (s_fileCount == 0 || s_sel >= s_fileCount) {
        setStatus("no script");
        return;
    }
    if (!needLinkForAction()) return; // startHidStack() already set an accurate status (incl. BLE PIN)
    s_phase = Phase::Running;
    bool ok = runScriptFile(s_files[s_sel]);
    if (!s_run) return;
    s_phase = ok ? Phase::Done : Phase::Fail;
    setStatus(ok ? "done — ENT" : "fail — ENT");
    SFX::play(ok ? SFX::Event::CONFIRM : SFX::Event::ERROR);
}

static void liveHandleKeys(const Keyboard_Class::KeysState& st) {
    if (!needLinkForAction()) return; // startHidStack() already set an accurate status (incl. BLE PIN)

    if (st.enter) {
        tap(BAD_KEY_RETURN);
        return;
    }
    if (st.del) {
        tap(BAD_KEY_BACKSPACE);
        return;
    }
    if (st.tab) {
        tap(BAD_KEY_TAB);
        return;
    }
    if (st.space) {
        tap(' ');
        return;
    }
    for (char c : st.word) {
        bool shift = st.shift;
        bool ctrl = st.ctrl;
        bool alt = st.alt;
        bool gui = st.fn;
        if (ctrl || alt || gui) {
            char k = (char)tolower((unsigned char)c);
            hidCombo(gui, alt, ctrl, shift, (uint8_t)k);
        } else {
            char buf[2] = {c, 0};
            hidPrint(buf);
        }
    }
}

void start() {
    s_run = true;
    s_tab = Tab::Scripts;
    s_phase = Phase::Idle;
    s_tr = Transport::Usb;
    s_prof = Profile::PC;
    s_keyLatch = false;
    s_linkOk = false;
    s_liveArmed = false;
    s_hidStarted = false;
    s_panelIdx = 0;
    s_lastErr[0] = 0;
    Avatar::suspendScene();
    rescan();
    setStatus("1/2/3 tabs  U/B  P");
    Display::showToast("BADUSB", 700);
}

void stop() {
    s_run = false;
    s_liveArmed = false;
    stopHid();
    Avatar::resumeScene();
}

void update() {
    if (!s_run) return;

    // Poll link in background — never blocks keys
    if (s_hidStarted && s_phase == Phase::WaitLink) {
        if (hidConnected()) {
            s_linkOk = true;
            s_phase = Phase::Ready;
            setStatus(s_tr == Transport::Usb ? "USB Host Connected" : "BLE Connected");
            SFX::play(SFX::Event::CONFIRM);
        }
    } else if (s_hidStarted && s_linkOk && !hidConnected()) {
        s_linkOk = false;
        s_liveArmed = false;
        s_phase = Phase::WaitLink;
        setStatus(s_tr == Transport::Usb ? "USB lost" : "BLE lost");
    }

    if (!keyNewPress(s_keyLatch)) return;

    Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();

    // Armed Live typing bypasses every other hotkey below — that's the whole
    // point: 1/2/3, U/B/P/R/C and plain `/ESC are real keys to send to the
    // target now, not menu shortcuts. Only FN+` (the same physical key as
    // "exit", held with FN) disarms it.
    if (s_tab == Tab::Live && s_liveArmed) {
        if (st.fn && keyEsc()) {
            s_liveArmed = false;
            setStatus("LIVE not armed — ENT to type");
            SFX::play(SFX::Event::BACK_NAV);
            return;
        }
        liveHandleKeys(st);
        return;
    }

    if (keyEsc()) {
        stop();
        return;
    }

    // Always allow tab / transport / profile switches
    bool uiKey = false;
    for (char c : st.word) {
        if (c == '1') {
            s_tab = Tab::Scripts;
            uiKey = true;
        } else if (c == '2') {
            s_tab = Tab::Live;
            if (!s_hidStarted) startHidStack();
            uiKey = true;
        } else if (c == '3') {
            s_tab = Tab::Panel;
            s_panelIdx = 0;
            if (!s_hidStarted) startHidStack();
            uiKey = true;
        } else if (c == 'u' || c == 'U') {
            stopHid();
            s_tr = Transport::Usb;
            s_phase = Phase::Idle;
            setStatus("press C to link");
            uiKey = true;
        } else if (c == 'b' || c == 'B') {
            stopHid();
            s_tr = Transport::Ble;
            s_phase = Phase::Idle;
            setStatus("press C to link");
            uiKey = true;
        } else if (c == 'p' || c == 'P') {
            s_prof = (s_prof == Profile::PC) ? Profile::Phone : Profile::PC;
            s_panelIdx = 0;
            uiKey = true;
        } else if (c == 'r' || c == 'R') {
            if (s_tab == Tab::Scripts) rescan();
            uiKey = true;
        } else if (c == 'c' || c == 'C') {
            // explicit connect / advertise
            startHidStack();
            uiKey = true;
        }
    }
    if (uiKey) return;

    if (s_phase == Phase::Done || s_phase == Phase::Fail) {
        if (st.enter) {
            s_phase = Phase::Idle;
            setStatus("ready");
        }
        return;
    }

    if (s_tab == Tab::Scripts) {
        for (char c : st.word) {
            if (c == ';' || c == ',') {
                if (s_sel > 0) s_sel--;
            } else if (c == '.' || c == '/') {
                if (s_sel + 1 < s_fileCount) s_sel++;
            }
        }
        if (s_sel < s_scroll) s_scroll = s_sel;
        if (s_sel >= s_scroll + 3) s_scroll = (uint8_t)(s_sel - 2);
        if (st.enter && s_fileCount > 0) runSelectedScript();
        return;
    }

    if (s_tab == Tab::Panel) {
        uint8_t n = presetCount();
        for (char c : st.word) {
            if (c == ';' || c == ',') {
                if (s_panelIdx > 0) s_panelIdx--;
            } else if (c == '.' || c == '/') {
                if (s_panelIdx + 1 < n) s_panelIdx++;
            }
        }
        if (st.enter) {
            if (!needLinkForAction()) return; // startHidStack() already set an accurate status (incl. BLE PIN)
            if (s_panelIdx < n && presets()[s_panelIdx].fn) {
                presets()[s_panelIdx].fn();
                setStatus(presets()[s_panelIdx].label);
                SFX::play(SFX::Event::CLICK);
            }
        }
        return;
    }

    if (s_tab == Tab::Live) {
        // Not armed yet: ENT arms it (needs a real link first); every other
        // key here is still a menu shortcut, not something to send.
        if (st.enter) {
            if (!needLinkForAction()) return; // startHidStack() already set an accurate status (incl. BLE PIN)
            s_liveArmed = true;
            setStatus("LIVE armed - FN+` exit");
            SFX::play(SFX::Event::CONFIRM);
        }
    }
}

void draw(M5Canvas& canvas) {
    canvas.fillSprite(0x0841);
    canvas.setTextSize(1);

    canvas.setTextColor(0xFFE0, 0x0841);
    canvas.setCursor(2, 2);
    canvas.printf("BAD [%s][%s]", s_tr == Transport::Usb ? "USB" : "BLE",
                  s_prof == Profile::PC ? "PC" : "PH");

    const char* tabs[3] = {"1:SCR", "2:LIVE", "3:PAD"};
    for (int i = 0; i < 3; i++) {
        bool on = ((int)s_tab == i);
        int16_t x = 2 + i * 52;
        if (on) canvas.fillRect(x, 14, 50, 11, 0x1B3F);
        canvas.setTextColor(on ? 0xFFE0 : 0x8410, on ? 0x1B3F : 0x0841);
        canvas.setCursor(x + 2, 15);
        canvas.print(tabs[i]);
    }

    uint16_t linkCol = hidConnected() ? 0x07E0 : (s_phase == Phase::WaitLink ? 0xFE60 : 0xF800);
    canvas.fillCircle(230, 8, 4, linkCol);

    canvas.setTextColor(0xC618, 0x0841);
    canvas.setCursor(2, 28);
    canvas.print(s_status);

    if (s_phase == Phase::Fail && s_lastErr[0]) {
        canvas.setTextColor(0xF800, 0x0841);
        canvas.setCursor(2, 40);
        canvas.print(s_lastErr);
    }

    if (s_tab == Tab::Scripts) {
        if (s_fileCount == 0) {
            canvas.setTextColor(0xF800, 0x0841);
            canvas.setCursor(2, 48);
            canvas.print("No .txt in /0N3P0rK/badusb");
        } else {
            for (uint8_t i = 0; i < 3; i++) {
                uint8_t idx = s_scroll + i;
                if (idx >= s_fileCount) break;
                int16_t y = 44 + (int16_t)i * 12;
                bool on = (idx == s_sel);
                if (on) canvas.fillRect(1, y - 1, 238, 12, 0x1B3F);
                canvas.setTextColor(on ? 0xFFE0 : 0xC618, on ? 0x1B3F : 0x0841);
                canvas.setCursor(4, y);
                canvas.print(s_files[idx]);
            }
        }
        canvas.setTextColor(0x8410, 0x0841);
        canvas.setCursor(2, 92);
        canvas.print(";/. ENT  C=link  U/B  `");
    } else if (s_tab == Tab::Live) {
        canvas.setTextColor(0xC618, 0x0841);
        canvas.setCursor(2, 48);
        canvas.print(s_liveArmed ? "ARMED - typing live" : "ENT to start typing");
        canvas.setTextColor(0x8410, 0x0841);
        canvas.setCursor(2, 92);
        canvas.print(s_liveArmed ? "FN+` exit  (all keys -> target)" : "ENT start  U/B  `");
    } else {
        uint8_t n = presetCount();
        uint8_t base = s_panelIdx > 1 ? (uint8_t)(s_panelIdx - 1) : 0;
        for (uint8_t i = 0; i < 4; i++) {
            uint8_t idx = base + i;
            if (idx >= n) break;
            int16_t y = 44 + (int16_t)i * 12;
            bool on = (idx == s_panelIdx);
            if (on) canvas.fillRect(1, y - 1, 238, 12, 0x1B3F);
            canvas.setTextColor(on ? 0xFFE0 : 0xC618, on ? 0x1B3F : 0x0841);
            canvas.setCursor(4, y);
            canvas.print(presets()[idx].label);
        }
        canvas.setTextColor(0x8410, 0x0841);
        canvas.setCursor(2, 92);
        canvas.print(";/. ENT  C=link  P=`");
    }
}

bool isRunning() { return s_run; }

void getStatusLine(char* out, size_t n) {
    if (!out || !n) return;
    if (!s_run) {
        out[0] = 0;
        return;
    }
    const char* tab = s_tab == Tab::Scripts ? "SCR" : (s_tab == Tab::Live ? "LIVE" : "PAD");
    snprintf(out, n, "BAD %s %s %s", s_tr == Transport::Usb ? "USB" : "BLE", tab, s_status);
}

}  // namespace BadUsbMode
