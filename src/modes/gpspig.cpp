#include "gpspig.h"
#include "mesh_pig.h"

#include "gps_world.h"
#include "../board/board.h"
#include "../cap/sniffer.h"
#include "../core/app.h"
#include "../core/config.h"
#include "../piglet/avatar.h"
#include "../storage/littlefs_ops.h"
#include "../audio/sfx.h"
#include "../ui/display.h"
#include "../ui/keys.h"

#include <SD.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace GpsMode {

static const int kLoraNss = 5;
static const size_t kRxBuf = 4096;
static const uint16_t kTrailCap = 160;
static const float kStepM = 8.0f;
static const char* kDir = "/0N3P0rK/gps";
static const char* kTrack = "/0N3P0rK/gps/track.csv";
static const char* kMapDir = "/0N3P0rK/maps/tiles";
static const char* kMapAttributionPath = "/0N3P0rK/maps/ATTRIBUTION.txt";
static const uint16_t kSpanM[] = {100, 300, 1000, 4000, 20000};
static const uint8_t kMapZoomMin = 2;
static const uint8_t kMapZoomMax = 18;

struct Pt {
    int32_t lat;
    int32_t lon;
};

static HardwareSerial s_uartGrove(2);
static HardwareSerial s_uartAdv(1);
static M5Canvas s_mapCanvas(&M5.Display);
static bool s_run = false;
static bool s_uartOn = false;
static bool s_keyWas = false;
static bool s_sceneSuspended = false;
static uint8_t s_page = 0;
static uint8_t s_zoom = 1;
static uint8_t s_mapZoom = 16;
static uint8_t s_worldZoom = 1;
static float s_worldCenterLat = 0;
static float s_worldCenterLon = 0;
static bool s_mapAllocFailed = false;
static bool s_mapCacheValid = false;
static bool s_mapCacheHasTiles = false;
static bool s_mapCacheHasPosition = false;
static uint8_t s_mapCacheZoom = 0;
static uint8_t s_mapCacheStyle = UINT8_MAX;
static float s_mapCenterLat = 0;
static float s_mapCenterLon = 0;
static bool s_mapManualCenter = false;
static double s_mapManualCenterLat = 0;
static double s_mapManualCenterLon = 0;
static bool s_mapFallbackTried = false;
static bool s_mapFallbackFound = false;
static float s_mapFallbackLat = 0;
static float s_mapFallbackLon = 0;
static uint8_t s_mapFallbackZoom = 0;
static bool s_mapAttributionLoaded = false;
static char s_mapAttribution[40] = "Map data contributors";
static char s_nmea[96];
static uint8_t s_nmeaLen = 0;
static uint32_t s_bytes = 0;
static uint32_t s_lastPointMs = 0;
static bool s_meshTyping = false;
static char s_meshDraft[48] = {};
static bool s_valid = false;
static bool s_havePos = false;
static uint32_t s_fixMs = 0;
static float s_lat = 0;
static float s_lon = 0;
static float s_alt = 0;
static float s_kmh = 0;
static float s_speedSamples[5] = {};
static uint8_t s_speedCount = 0;
static uint8_t s_speedNext = 0;
static uint32_t s_speedMs = 0;
static float s_course = 0;
static int s_sats = 0;
static int s_localHour = 0;
static int s_localMinute = 0;
static bool s_haveTime = false;
static bool s_haveAlt = false;
static bool s_haveSpd = false;
static bool s_haveHdg = false;
static Pt* s_pts = nullptr;
static uint16_t s_nPts = 0;
static HardwareSerial* s_openUart = nullptr;

static int32_t toE7(float d) { return (int32_t)(d * 10000000.0f); }
static float fromE7(int32_t e) { return (float)e / 10000000.0f; }

static bool mapBit(int x, int y) {
    if (x < 0 || y < 0 || x >= GPS_MAP_W || y >= GPS_MAP_H) return false;
    size_t i = (size_t)y * (size_t)GPS_MAP_W + (size_t)x;
    return (GPS_MAP_BITS[i >> 3] >> (7 - (i & 7))) & 1;
}

static float metersBetween(float la1, float lo1, float la2, float lo2) {
    const float k = 0.0174533f;
    float p1 = la1 * k;
    float p2 = la2 * k;
    float dp = (la2 - la1) * k;
    float dl = (lo2 - lo1) * k;
    float a = sinf(dp * 0.5f) * sinf(dp * 0.5f) +
              cosf(p1) * cosf(p2) * sinf(dl * 0.5f) * sinf(dl * 0.5f);
    return 6371000.0f * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

static void holdLoraOff() {
    if (!Board::isAdv() || Config::gps().source != GpsSource::ADV_CAP) return;
    pinMode(kLoraNss, OUTPUT);
    digitalWrite(kLoraNss, HIGH);
}

static bool sourceIsCustom() {
    return Config::gps().source == GpsSource::CUSTOM;
}

static int defaultRxPin() {
    return Config::gps().source == GpsSource::ADV_CAP ? 15 : 1;
}

static int defaultTxPin() {
    return Config::gps().source == GpsSource::ADV_CAP ? 13 : 2;
}

static uint32_t defaultBaud() {
    return 115200u;
}

static uint32_t activeBaud() {
    const GpsConfig& g = Config::gps();
    if (!sourceIsCustom()) return defaultBaud();
    uint8_t i = Board::isAdv() ? g.advIdx : g.groveIdx;
    if (i >= GPS_BAUD_N) i = Board::isAdv() ? 4u : 0u;
    return GPS_BAUD_TAB[i];
}

static int activeRxPin() {
    const GpsConfig& g = Config::gps();
    if (!sourceIsCustom()) return defaultRxPin();
    return Board::isAdv() ? g.advRx : g.groveRx;
}

static int activeTxPin() {
    const GpsConfig& g = Config::gps();
    if (!sourceIsCustom()) return defaultTxPin();
    return Board::isAdv() ? g.advTx : g.groveTx;
}

static HardwareSerial& activeUart() {
    GpsSource source = Config::gps().source;
    if (source == GpsSource::GROVE) return s_uartGrove;
    if (source == GpsSource::ADV_CAP) return s_uartAdv;
    return Board::isAdv() ? s_uartAdv : s_uartGrove;
}

static void closeUart();

static void openUart() {
    if (s_uartOn) closeUart();
    holdLoraOff();
    uint32_t baud = activeBaud();
    int rxPin = activeRxPin();
    int txPin = activeTxPin();
    HardwareSerial& uart = activeUart();
    uart.setRxBufferSize(kRxBuf);
    uart.begin(baud, SERIAL_8N1, rxPin, txPin);
    s_openUart = &uart;
    s_uartOn = true;
    const char* sourceName = Config::gps().source == GpsSource::GROVE ? "GROVE" :
                             Config::gps().source == GpsSource::ADV_CAP ? "ADV CAP" : "CUSTOM";
    Serial.printf("[GPS] %s rx=%d tx=%d baud=%u\n",
                  sourceName,
                  rxPin, txPin,
                  (unsigned)baud);
}

static void closeUart() {
    if (!s_uartOn) return;
    if (s_openUart) s_openUart->end();
    s_openUart = nullptr;
    s_uartOn = false;
    holdLoraOff();
}

static bool shouldKeepUartAwake() {
    const GpsConfig& g = Config::gps();
    return !g.powerSave ||
           (App::mode() == AppMode::GPS && !App::windowHidden()) ||
           Cap::isRunning();
}

static void syncUartPower() {
    if (!s_run) return;
    if (shouldKeepUartAwake()) {
        if (!s_uartOn) openUart();
    } else if (s_uartOn) {
        closeUart();
    }
}

static bool nmeaOk(char* s) {
    if (s[0] != '$') return false;
    char* star = strchr(s, '*');
    if (!star || !star[1] || !star[2]) return true;
    uint8_t x = 0;
    for (char* p = s + 1; p < star; p++) x ^= (uint8_t)*p;
    char hex[3] = {star[1], star[2], 0};
    return (uint8_t)strtol(hex, nullptr, 16) == x;
}

static bool endsWith(const char* s, const char* suf) {
    size_t n = strlen(s);
    size_t m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static bool parseDm(const char* dm, const char* hemi, bool lat, float& out) {
    if (!dm || !dm[0] || !hemi || !hemi[0]) return false;
    double v = atof(dm);
    int deg = (int)(v / 100.0);
    double minutes = v - (double)deg * 100.0;
    double d = (double)deg + minutes / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') d = -d;
    if (lat && (d < -90.0 || d > 90.0)) return false;
    if (!lat && (d < -180.0 || d > 180.0)) return false;
    out = (float)d;
    return true;
}

static void pushPt(int32_t lat, int32_t lon) {
    if (!s_pts) return;
    if (s_nPts < kTrailCap) {
        s_pts[s_nPts].lat = lat;
        s_pts[s_nPts].lon = lon;
        s_nPts++;
        return;
    }
    memmove(s_pts, s_pts + 1, (size_t)(kTrailCap - 1) * sizeof(Pt));
    s_pts[kTrailCap - 1].lat = lat;
    s_pts[kTrailCap - 1].lon = lon;
}

static void appendSd(float lat, float lon) {
    if (!Storage::available()) return;
    if (!SD.exists(kDir)) SD.mkdir(kDir);
    File f = SD.open(kTrack, FILE_APPEND);
    if (!f) return;
    char line[48];
    snprintf(line, sizeof(line), "%.6f,%.6f\n", lat, lon);
    f.print(line);
    f.close();
}

static void notePoint(float lat, float lon) {
    uint32_t now = millis();
    uint32_t interval = (uint32_t)Config::gps().trackIntervalSec * 1000u;
    if (s_nPts > 0 && now - s_lastPointMs < interval) return;
    if (s_pts && s_nPts > 0) {
        float la = fromE7(s_pts[s_nPts - 1].lat);
        float lo = fromE7(s_pts[s_nPts - 1].lon);
        if (metersBetween(la, lo, lat, lon) < kStepM) return;
    }
    pushPt(toE7(lat), toE7(lon));
    appendSd(lat, lon);
    s_lastPointMs = now;
}

static bool updateLocalTime(const char* utc) {
    if (!utc || strlen(utc) < 6) return false;
    for (uint8_t i = 0; i < 6; i++)
        if (utc[i] < '0' || utc[i] > '9') return false;
    int hour = (utc[0] - '0') * 10 + utc[1] - '0';
    int minute = (utc[2] - '0') * 10 + utc[3] - '0';
    int second = (utc[4] - '0') * 10 + utc[5] - '0';
    if (hour > 23 || minute > 59 || second > 60) return false;
    int localMinutes = hour * 60 + minute + Config::gps().timezoneOffset * 60;
    localMinutes = (localMinutes % 1440 + 1440) % 1440;
    s_localHour = localMinutes / 60;
    s_localMinute = localMinutes % 60;
    s_haveTime = true;
    return true;
}

static void takeFix(bool ok, float lat, float lon) {
    if (!ok) return;
    s_valid = true;
    s_havePos = true;
    s_fixMs = millis();
    s_lat = lat;
    s_lon = lon;
    notePoint(lat, lon);
}

static void resetSpeed() {
    s_kmh = 0;
    s_speedCount = 0;
    s_speedNext = 0;
    s_speedMs = 0;
    s_haveSpd = false;
}

static bool updateSpeed(const char* knotsText) {
    if (!knotsText || !knotsText[0]) {
        resetSpeed();
        return false;
    }

    char* end = nullptr;
    float knots = strtof(knotsText, &end);
    if (end == knotsText || *end != '\0' || !isfinite(knots) ||
        knots < 0.0f || knots > 550.0f) {
        resetSpeed();
        return false;
    }

    float kmh = knots * 1.852f;
    if (kmh < 2.5f) kmh = 0.0f;
    s_speedSamples[s_speedNext] = kmh;
    s_speedNext = (uint8_t)((s_speedNext + 1) % 5);
    if (s_speedCount < 5) s_speedCount++;

    float sorted[5];
    for (uint8_t i = 0; i < s_speedCount; i++) sorted[i] = s_speedSamples[i];
    for (uint8_t i = 1; i < s_speedCount; i++) {
        float value = sorted[i];
        uint8_t j = i;
        while (j > 0 && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = value;
    }
    s_kmh = sorted[s_speedCount / 2];
    s_speedMs = millis();
    s_haveSpd = s_speedCount >= 3;
    return s_haveSpd;
}

static void onSentence(char* s) {
    if (!nmeaOk(s)) return;
    char* f[16];
    int nf = 0;
    f[nf++] = s;
    for (char* p = s; *p && nf < 16; p++) {
        if (*p == ',' || *p == '*') {
            *p = 0;
            f[nf++] = p + 1;
        }
    }
    if (nf < 1) return;
    if (endsWith(f[0], "GGA") && nf >= 8) {
        s_sats = atoi(f[7]);
        int q = atoi(f[6]);
        float lat = 0, lon = 0;
        bool pos = q > 0 && parseDm(f[2], f[3], true, lat) && parseDm(f[4], f[5], false, lon);
        if (nf >= 10 && f[9][0]) {
            s_alt = (float)atof(f[9]);
            s_haveAlt = true;
        }
        takeFix(pos, lat, lon);
        return;
    }
    if (endsWith(f[0], "RMC") && nf >= 8) {
        float lat = 0, lon = 0;
        bool pos = f[2][0] == 'A' &&
                   parseDm(f[3], f[4], true, lat) && parseDm(f[5], f[6], false, lon);
        if (pos) updateSpeed(f[7]);
        else resetSpeed();
        if (nf >= 9 && f[8][0]) {
            s_course = (float)atof(f[8]);
            s_haveHdg = true;
        }
        updateLocalTime(f[1]);
        takeFix(pos, lat, lon);
    }
}

static void feed(char c) {
    s_bytes++;
    if (c == '$') {
        s_nmea[0] = '$';
        s_nmeaLen = 1;
        return;
    }
    if (s_nmeaLen == 0) return;
    if (c == '\r') return;
    if (c != '\n') {
        if (s_nmeaLen + 1 < sizeof(s_nmea)) s_nmea[s_nmeaLen++] = c;
        else s_nmeaLen = 0;
        return;
    }
    s_nmea[s_nmeaLen] = 0;
    s_nmeaLen = 0;
    if (s_nmea[0] == '$') onSentence(s_nmea);
}

static void pollUart() {
    if (!s_uartOn) return;
    if (!s_openUart) return;
    HardwareSerial& uart = *s_openUart;
    while (uart.available() > 0) {
        int c = uart.read();
        if (c >= 0) feed((char)c);
    }
}

static void loadTrail() {
    if (!s_pts || !Storage::available() || !SD.exists(kTrack)) return;
    File f = SD.open(kTrack, FILE_READ);
    if (!f) return;
    size_t sz = f.size();
    bool skip = false;
    if (sz > 24576) {
        f.seek(sz - 24576);
        skip = true;
    }
    char line[72];
    uint8_t n = 0;
    while (f.available()) {
        char c = (char)f.read();
        if (c == '\n' || n + 1 >= sizeof(line)) {
            line[n] = 0;
            if (!skip && n > 0) {
                char* comma = strchr(line, ',');
                if (comma && comma != line && comma[1]) {
                    *comma = 0;
                    pushPt(toE7((float)atof(line)), toE7((float)atof(comma + 1)));
                }
            }
            skip = false;
            n = 0;
        } else if (c != '\r') {
            line[n++] = c;
        }
    }
    if (!skip && n > 0) {
        line[n] = 0;
        char* comma = strchr(line, ',');
        if (comma && comma != line && comma[1]) {
            *comma = 0;
            pushPt(toE7((float)atof(line)), toE7((float)atof(comma + 1)));
        }
    }
    f.close();
    Serial.printf("[GPS] trail %u\n", (unsigned)s_nPts);
}

static void syncScene() {
    if (App::mode() == AppMode::GPS && !App::windowHidden()) {
        if (!Avatar::isSceneSuspended()) {
            Avatar::suspendScene();
            s_sceneSuspended = true;
        }
    } else if (s_sceneSuspended) {
        Avatar::resumeScene();
        s_sceneSuspended = false;
    }
}

size_t heapHeld() {
    size_t n = 0;
    if (s_uartOn) n += kRxBuf;
    if (s_pts) n += (size_t)kTrailCap * sizeof(Pt);
    if (s_mapCanvas.getBuffer()) n += (size_t)(DISPLAY_W - 16) * (MAIN_H - 24 - 5);
    return n;
}

static void releaseMapCanvas() {
    if (s_mapCanvas.getBuffer()) s_mapCanvas.deleteSprite();
    s_mapCacheValid = false;
    s_mapAllocFailed = false;
}

void start() {
    if (s_run) return;
    s_pts = (Pt*)malloc((size_t)kTrailCap * sizeof(Pt));
    if (!s_pts) {
        Config::gps().enabled = false;
        Config::save();
        Display::showToast("LOW MEM", 1200);
        Serial.printf("[GPS] trail alloc failed heap=%u\n", (unsigned)ESP.getFreeHeap());
        return;
    }
    s_nPts = 0;
    s_run = true;
    s_page = 0;
    s_zoom = 1;
    s_mapZoom = 16;
    s_worldZoom = 1;
    s_worldCenterLat = 0;
    s_worldCenterLon = 0;
    s_mapManualCenter = false;
    s_mapAllocFailed = false;
    s_mapCacheValid = false;
    s_mapCacheHasTiles = false;
    s_mapAttributionLoaded = false;
    s_nmeaLen = 0;
    s_meshTyping = false;
    s_meshDraft[0] = '\0';
    s_bytes = 0;
    s_lastPointMs = 0;
    s_valid = false;
    s_havePos = false;
    s_fixMs = 0;
    s_haveAlt = false;
    resetSpeed();
    s_haveHdg = false;
    s_sats = 0;
    s_haveTime = false;
    s_keyWas = true;
    loadTrail();
    syncUartPower();
    Config::gps().enabled = true;
    Config::save();
    Serial.printf("[GPS] took %u heap=%u\n", (unsigned)heapHeld(), (unsigned)ESP.getFreeHeap());
    // Start Meshtastic if configured
    if (Config::mesh().enabled) MeshPig::start();
    Display::showToast("GPS", 700);
}

void openMeshPage() {
    if (!s_run || !MeshPig::isRunning()) return;
    s_page = 4;
    s_meshTyping = false;
    s_meshDraft[0] = '\0';
    MeshPig::clearNew();
    releaseMapCanvas();
}

void stop() {
    if (!s_run && !s_pts && !s_uartOn) {
        if (Config::gps().enabled) {
            Config::gps().enabled = false;
            Config::save();
        }
        return;
    }
    size_t held = heapHeld();
    closeUart();
    releaseMapCanvas();
    free(s_pts);
    s_pts = nullptr;
    s_nPts = 0;
    s_run = false;
    s_meshTyping = false;
    s_meshDraft[0] = '\0';
    s_valid = false;
    Config::gps().enabled = false;
    Config::save();
    if (s_sceneSuspended) {
        Avatar::resumeScene();
        s_sceneSuspended = false;
    }
    if (held) {
        Serial.printf("[GPS] returned %u heap=%u\n",
                      (unsigned)held, (unsigned)ESP.getFreeHeap());
    }
}

bool isRunning() { return s_run; }

bool hasFix() {
    return s_run && s_valid && s_fixMs && millis() - s_fixMs <= 8000;
}

void reconfigure() {
    if (!s_run) return;
    closeUart();
    s_nmeaLen = 0;
    s_bytes = 0;
    s_valid = false;
    s_havePos = false;
    s_fixMs = 0;
    s_haveAlt = false;
    resetSpeed();
    s_haveHdg = false;
    s_sats = 0;
    s_haveTime = false;
    syncUartPower();
}

void updatePowerSave() {
    syncUartPower();
}

static bool panMap(int dx, int dy);

void update() {
    if (!s_run) return;
    syncScene();
    syncUartPower();
    pollUart();
    if (App::mode() != AppMode::GPS || App::windowHidden()) {
        releaseMapCanvas();
        return;
    }
    if (!keyNewPress(s_keyWas)) return;
    if (s_meshTyping) {
        auto keys = M5Cardputer.Keyboard.keysState();
        if (keyEsc()) {
            s_meshTyping = false;
            s_meshDraft[0] = '\0';
            SFX::play(SFX::BACK_NAV);
        } else if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || keys.del) {
            size_t len = strlen(s_meshDraft);
            if (len) s_meshDraft[len - 1] = '\0';
        } else if (keys.enter) {
            if (!s_meshDraft[0]) {
                Display::showToast("EMPTY MESSAGE", 900);
            } else if (MeshPig::sendText(s_meshDraft)) {
                s_meshTyping = false;
                s_meshDraft[0] = '\0';
                SFX::play(SFX::CONFIRM);
                Display::showToast("MESH SENT", 900);
            } else {
                Display::showToast("MESH SEND FAILED", 1100);
            }
        } else {
            size_t len = strlen(s_meshDraft);
            for (char ch : keys.word) {
                if (ch < 32 || ch >= 127 || len + 1 >= sizeof(s_meshDraft)) continue;
                s_meshDraft[len++] = ch;
                s_meshDraft[len] = '\0';
            }
        }
        return;
    }
    if (keyMin()) return;
    if (keyEsc()) {
        releaseMapCanvas();
        App::setMode(AppMode::MENU);
        return;
    }
    // Pages 1-4 always; page 5 (MESH) only when Meshtastic enabled
    uint8_t maxPage = MeshPig::isRunning() ? 5 : 4;
    for (uint8_t page = 0; page < maxPage; ++page) {
        if (M5Cardputer.Keyboard.isKeyPressed((char)('1' + page))) {
            s_page = page;
            if (s_page != 3) releaseMapCanvas();
            if (s_page == 4) MeshPig::clearNew();
            SFX::play(SFX::MENU_CLICK);
            return;
        }
    }
    if (s_page == 4 && M5Cardputer.Keyboard.keysState().enter) {
        s_meshTyping = true;
        s_meshDraft[0] = '\0';
        SFX::play(SFX::MENU_CLICK);
        return;
    }

    bool zoomOut = M5Cardputer.Keyboard.isKeyPressed('-') || M5Cardputer.Keyboard.isKeyPressed('_');
    bool zoomIn = M5Cardputer.Keyboard.isKeyPressed('=') || M5Cardputer.Keyboard.isKeyPressed('+');
    if (s_page == 1 || s_page == 3) {
        int dx = 0;
        int dy = 0;
        if (M5Cardputer.Keyboard.isKeyPressed('w') ||
            M5Cardputer.Keyboard.isKeyPressed('W') ||
            M5Cardputer.Keyboard.isKeyPressed(';') ||
            M5Cardputer.Keyboard.isKeyPressed(':')) dy = -1;
        else if (M5Cardputer.Keyboard.isKeyPressed('s') ||
                 M5Cardputer.Keyboard.isKeyPressed('S') ||
                 M5Cardputer.Keyboard.isKeyPressed('.') ||
                 M5Cardputer.Keyboard.isKeyPressed('>')) dy = 1;
        else if (M5Cardputer.Keyboard.isKeyPressed('a') ||
                 M5Cardputer.Keyboard.isKeyPressed('A') ||
                 M5Cardputer.Keyboard.isKeyPressed(',') ||
                 M5Cardputer.Keyboard.isKeyPressed('<')) dx = -1;
        else if (M5Cardputer.Keyboard.isKeyPressed('d') ||
                 M5Cardputer.Keyboard.isKeyPressed('D') ||
                 M5Cardputer.Keyboard.isKeyPressed('/')) dx = 1;

        if (s_page == 1 && (dx || dy)) {
            const float step = 24.0f / s_worldZoom;
            s_worldCenterLon -= dx * step;
            s_worldCenterLat += dy * step;
            if (s_worldCenterLon < -180.0f) s_worldCenterLon += 360.0f;
            if (s_worldCenterLon >= 180.0f) s_worldCenterLon -= 360.0f;
            if (s_worldCenterLat < -85.0f) s_worldCenterLat = -85.0f;
            if (s_worldCenterLat > 85.0f) s_worldCenterLat = 85.0f;
            SFX::play(SFX::MENU_CLICK);
            return;
        }
        if (s_page == 3 && (dx || dy) && panMap(dx, dy)) {
            SFX::play(SFX::MENU_CLICK);
            return;
        }
        if (s_page == 1) {
            if (zoomOut && s_worldZoom > 1) {
                s_worldZoom--;
                SFX::play(SFX::MENU_CLICK);
            } else if (zoomIn && s_worldZoom < 4) {
                s_worldZoom++;
                SFX::play(SFX::MENU_CLICK);
            }
        } else if (s_page == 3) {
            if (zoomOut && s_mapZoom > kMapZoomMin) {
                s_mapZoom--;
                SFX::play(SFX::MENU_CLICK);
            } else if (zoomIn && s_mapZoom < kMapZoomMax) {
                s_mapZoom++;
                SFX::play(SFX::MENU_CLICK);
            }
        }
    }
}

static void drawFix(M5Canvas& canvas) {
    canvas.setTextSize(1);
    canvas.setTextColor(UiStyle::DIM);
    char port[24];
    const char* sourceName = Config::gps().source == GpsSource::GROVE ? "GROVE" :
                             Config::gps().source == GpsSource::ADV_CAP ? "ADV CAP" : "CUSTOM";
    snprintf(port, sizeof(port), "%s   %lu", sourceName, (unsigned long)activeBaud());
    canvas.drawString(port, 8, 22);
    const bool freshFix = hasFix();
    canvas.setTextColor(freshFix ? UiStyle::GREEN : UiStyle::GOLD);
    const char* state = "SEARCH";
    if (s_bytes == 0) state = "NO DATA";
    else if (freshFix) state = "FIX";
    else if (s_havePos && s_fixMs && millis() - s_fixMs > 8000) state = "STALE";
    canvas.drawString(state, 8, 34);
    char b[28];
    canvas.setTextColor(UiStyle::TEXT);
    snprintf(b, sizeof(b), "SATS  %d", s_sats);
    canvas.drawString(b, 90, 34);
    if (s_havePos) {
        snprintf(b, sizeof(b), "LAT  %+10.5f", s_lat);
        canvas.drawString(b, 8, 48);
        snprintf(b, sizeof(b), "LON  %+11.5f", s_lon);
        canvas.drawString(b, 8, 60);
    } else {
        canvas.drawString("LAT  --", 8, 48);
        canvas.drawString("LON  --", 8, 60);
    }
    if (s_haveAlt) snprintf(b, sizeof(b), "ALT  %.0f M", s_alt);
    else snprintf(b, sizeof(b), "ALT  --");
    canvas.drawString(b, 8, 74);
    if (s_haveSpd && millis() - s_speedMs <= 5000)
        snprintf(b, sizeof(b), "%.1f KM/H", s_kmh);
    else snprintf(b, sizeof(b), "SPD  --");
    canvas.drawString(b, 120, 74);
    canvas.setTextColor(UiStyle::DIM);
    if (s_haveHdg) {
        snprintf(b, sizeof(b), "HDG  %.0f", s_course);
        canvas.setTextColor(UiStyle::TEXT);
        canvas.drawString(b, 8, 90);
    } else {
        canvas.drawString(s_bytes ? "COLD START NEEDS SKY" : "PLUG THE GPS AND WAIT", 8, 90);
    }
    if (s_haveTime) {
        char time[20];
        snprintf(time, sizeof(time), "UTC%+d %02d:%02d",
                 Config::gps().timezoneOffset, s_localHour, s_localMinute);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString(time, 125, 90);
    }
}

static void drawWorld(M5Canvas& canvas) {
    constexpr int mapX = 8;
    constexpr int mapY = 22;
    constexpr int mapW = DISPLAY_W - 16;
    constexpr int mapH = MAIN_H - mapY - 5;
    constexpr double baseW = mapW;
    constexpr double baseH = mapH;
    const double worldW = baseW * s_worldZoom;
    const double worldH = baseH * s_worldZoom;
    const double centerX = mapX + mapW * 0.5;
    const double centerY = mapY + mapH * 0.5;
    const double mapLeft = centerX -
        (s_worldCenterLon + 180.0) / 360.0 * worldW;
    const double rowHeight = worldH / GPS_MAP_H;

    canvas.fillRect(mapX, mapY, mapW, mapH, 0x000D);
    canvas.setClipRect(mapX, mapY, mapW, mapH);

    for (int y = 0; y < GPS_MAP_H; ++y) {
        const double latitude = 90.0 - (y + 0.5) * 180.0 / GPS_MAP_H;
        const int screenY = (int)round(
            centerY - (latitude - s_worldCenterLat) * worldH / 180.0 - rowHeight * 0.5);
        const uint16_t landColor = fabs(latitude) > 60.0 ? 0xB6F2 :
                                   fabs(latitude) > 30.0 ? 0x75A8 : 0x4E65;
        int runStart = -1;
        for (int x = 0; x <= GPS_MAP_W; ++x) {
            const bool land = x < GPS_MAP_W && mapBit(x, y);
            if (land && runStart < 0) runStart = x;
            if (!land && runStart >= 0) {
                const double runLon = runStart * 360.0 / GPS_MAP_W - 180.0;
                const double runEndLon = x * 360.0 / GPS_MAP_W - 180.0;
                const double startX = mapLeft + (runLon + 180.0) / 360.0 * worldW;
                const double endX = mapLeft + (runEndLon + 180.0) / 360.0 * worldW;
                for (int wrap = -1; wrap <= 1; ++wrap) {
                    const int drawX = (int)floor(startX + wrap * worldW);
                    const int drawW = (int)ceil(endX + wrap * worldW) - drawX;
                    canvas.fillRect(drawX, screenY, drawW, (int)ceil(rowHeight), landColor);
                }
                runStart = -1;
            }
        }
    }

    for (int longitude = -150; longitude <= 180; longitude += 30) {
        double delta = longitude - s_worldCenterLon;
        while (delta > 180.0) delta -= 360.0;
        while (delta < -180.0) delta += 360.0;
        const int x = (int)round(centerX + delta * worldW / 360.0);
        canvas.drawFastVLine(x, mapY, mapH, 0x2340);
    }
    for (int latitude = -60; latitude <= 60; latitude += 30) {
        const int y = (int)round(
            centerY - (latitude - s_worldCenterLat) * worldH / 180.0);
        canvas.drawFastHLine(mapX, y, mapW, latitude == 0 ? 0x3B86 : 0x2340);
    }

    if (s_havePos) {
        double delta = s_lon - s_worldCenterLon;
        while (delta > 180.0) delta -= 360.0;
        while (delta < -180.0) delta += 360.0;
        const int markerX = (int)round(centerX + delta * worldW / 360.0);
        const int markerY = (int)round(
            centerY - (s_lat - s_worldCenterLat) * worldH / 180.0);
        canvas.drawLine(markerX - 4, markerY, markerX + 4, markerY, UiStyle::BG);
        canvas.drawLine(markerX, markerY - 4, markerX, markerY + 4, UiStyle::BG);
        canvas.fillCircle(markerX, markerY, 2,
                          hasFix() ? UiStyle::PINK : UiStyle::GOLD);
    }
    canvas.clearClipRect();

    canvas.setTextColor(UiStyle::GOLD);
    canvas.setTextDatum(top_right);
    char zoomLabel[8];
    snprintf(zoomLabel, sizeof(zoomLabel), "Z%u", s_worldZoom);
    canvas.drawString(zoomLabel, DISPLAY_W - 8, 20);
    canvas.setTextDatum(top_left);
}

static void projectLocal(float lat, float lon, float clat, float clon, float span,
                          int& sx, int& sy) {
    float dx = (lon - clon) * 111320.0f * cosf(clat * 0.0174533f);
    float dy = (lat - clat) * 110540.0f;
    sx = 120 + (int)(dx / span * 108.0f);
    sy = 62 - (int)(dy / span * 34.0f);
}

static void drawTrail(M5Canvas& canvas) {
    uint16_t span = kSpanM[s_zoom];
    char b[24];
    if (span >= 500) snprintf(b, sizeof(b), "WIDTH %u KM", (unsigned)((span * 2) / 1000));
    else snprintf(b, sizeof(b), "WIDTH %u M", (unsigned)(span * 2));
    canvas.setTextColor(UiStyle::GOLD);
    canvas.setTextDatum(top_right);
    canvas.drawString(b, DISPLAY_W - 8, 20);
    canvas.setTextDatum(top_left);

    canvas.drawRect(8, 32, 224, 68, UiStyle::PANEL);
    for (int i = 1; i < 4; i++) {
        int x = 8 + i * 56;
        canvas.drawFastVLine(x, 33, 66, UiStyle::PANEL);
    }
    canvas.drawFastHLine(9, 32 + 34, 222, UiStyle::PANEL);

    float clat = s_havePos ? s_lat : (s_nPts ? fromE7(s_pts[s_nPts - 1].lat) : 0);
    float clon = s_havePos ? s_lon : (s_nPts ? fromE7(s_pts[s_nPts - 1].lon) : 0);
    if (!s_havePos && s_nPts == 0) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.setTextDatum(top_center);
        canvas.drawString("NO FIX", DISPLAY_W / 2, 58);
        canvas.setTextDatum(top_left);
        return;
    }
    for (uint16_t i = 0; i < s_nPts; i++) {
        int sx, sy;
        projectLocal(fromE7(s_pts[i].lat), fromE7(s_pts[i].lon), clat, clon, (float)span, sx, sy);
        if (sx < 10 || sx > 229 || sy < 34 || sy > 97) continue;
        bool head = i + 1 == s_nPts;
        canvas.fillRect(sx, sy, head ? 3 : 2, head ? 3 : 2, head ? UiStyle::PINK : UiStyle::TEXT);
    }
}

static double mapPixelX(double lon, uint8_t zoom) {
    const double world = 256.0 * (1UL << zoom);
    return (lon + 180.0) / 360.0 * world;
}

static double mapPixelY(double lat, uint8_t zoom) {
    if (lat > 85.05112878) lat = 85.05112878;
    if (lat < -85.05112878) lat = -85.05112878;
    const double radians = lat * 0.017453292519943295;
    const double world = 256.0 * (1UL << zoom);
    return (1.0 - asinh(tan(radians)) / 3.141592653589793) * 0.5 * world;
}

static double mapTileLatitude(double y, uint8_t zoom) {
    const double n = (double)(1UL << zoom);
    return atan(sinh(3.141592653589793 * (1.0 - 2.0 * y / n))) *
           57.29577951308232;
}

static bool panMap(int dx, int dy) {
    if (!s_mapManualCenter) {
        if (s_mapCacheValid) {
            s_mapManualCenterLat = s_mapCenterLat;
            s_mapManualCenterLon = s_mapCenterLon;
        } else if (s_havePos) {
            s_mapManualCenterLat = s_lat;
            s_mapManualCenterLon = s_lon;
        } else if (s_nPts > 0) {
            s_mapManualCenterLat = fromE7(s_pts[s_nPts - 1].lat);
            s_mapManualCenterLon = fromE7(s_pts[s_nPts - 1].lon);
        } else if (s_mapFallbackFound) {
            s_mapManualCenterLat = s_mapFallbackLat;
            s_mapManualCenterLon = s_mapFallbackLon;
        } else {
            return false;
        }
        s_mapManualCenter = true;
    }

    const double worldPixels = 256.0 * (1UL << s_mapZoom);
    const double step = 72.0;
    double x = mapPixelX(s_mapManualCenterLon, s_mapZoom) + dx * step;
    double y = mapPixelY(s_mapManualCenterLat, s_mapZoom) + dy * step;
    x = fmod(x, worldPixels);
    if (x < 0) x += worldPixels;
    if (y < 0) y = 0;
    if (y > worldPixels) y = worldPixels;

    s_mapManualCenterLon = x / worldPixels * 360.0 - 180.0;
    s_mapManualCenterLat = mapTileLatitude(y / 256.0, s_mapZoom);
    s_mapCacheValid = false;
    return true;
}

static bool mapTileFileExists(uint8_t zoom, int x, int y, uint8_t style) {
    char path[112];
    snprintf(path, sizeof(path), "%s/%s/%u/%d/%d.jpg", kMapDir,
             GPS_MAP_STYLE_DIRS[style], (unsigned)zoom, x, y);
    if (SD.exists(path)) return true;
    snprintf(path, sizeof(path), "%s/%s/%u/%d/%d.png", kMapDir,
             GPS_MAP_STYLE_DIRS[style], (unsigned)zoom, x, y);
    if (SD.exists(path)) return true;
    snprintf(path, sizeof(path), "%s/%u/%d/%d.jpg", kMapDir,
             (unsigned)zoom, x, y);
    if (SD.exists(path)) return true;
    snprintf(path, sizeof(path), "%s/%u/%d/%d.png", kMapDir,
             (unsigned)zoom, x, y);
    return SD.exists(path);
}

static bool mapTileCoordinates(float lat, float lon, uint8_t zoom,
                               int& tileX, int& tileY) {
    const int count = 1 << zoom;
    tileX = (int)floor(mapPixelX(lon, zoom) / 256.0);
    tileY = (int)floor(mapPixelY(lat, zoom) / 256.0);
    if (tileY < 0 || tileY >= count) return false;
    tileX = (tileX % count + count) % count;
    return true;
}

static bool findZoomForPosition(float lat, float lon, uint8_t style,
                                uint8_t& foundZoom) {
    for (int distance = 0; distance <= kMapZoomMax - kMapZoomMin; distance++) {
        int candidates[2] = {(int)s_mapZoom - distance, (int)s_mapZoom + distance};
        for (uint8_t side = 0; side < (distance == 0 ? 1 : 2); side++) {
            int zoom = candidates[side];
            if (zoom < kMapZoomMin || zoom > kMapZoomMax) continue;
            int x, y;
            if (!mapTileCoordinates(lat, lon, (uint8_t)zoom, x, y)) continue;
            if (mapTileFileExists((uint8_t)zoom, x, y, style)) {
                foundZoom = (uint8_t)zoom;
                return true;
            }
        }
    }
    return false;
}

static bool parseTileIndex(const char* name, bool image, int& value) {
    if (!name || !name[0]) return false;
    char number[16];
    size_t length = strlen(name);
    if (image) {
        const char* dot = strrchr(name, '.');
        if (!dot || (strcasecmp(dot, ".jpg") != 0 &&
                     strcasecmp(dot, ".jpeg") != 0 &&
                     strcasecmp(dot, ".png") != 0)) return false;
        length = (size_t)(dot - name);
    }
    if (length == 0 || length >= sizeof(number)) return false;
    memcpy(number, name, length);
    number[length] = 0;
    for (size_t i = 0; i < length; i++)
        if (number[i] < '0' || number[i] > '9') return false;
    char* end = nullptr;
    long parsed = strtol(number, &end, 10);
    if (!end || *end || parsed < 0 || parsed > INT_MAX) return false;
    value = (int)parsed;
    return true;
}

static bool findFirstTileInZoom(const char* root, uint8_t zoom,
                                int& tileX, int& tileY) {
    char zoomPath[96];
    snprintf(zoomPath, sizeof(zoomPath), "%s/%u", root, (unsigned)zoom);
    File zoomDir = SD.open(zoomPath);
    if (!zoomDir || !zoomDir.isDirectory()) return false;

    bool found = false;
    File xEntry = zoomDir.openNextFile();
    while (xEntry && !found) {
        int x;
        if (xEntry.isDirectory() && parseTileIndex(Storage::baseName(xEntry.name()), false, x)) {
            char xPath[112];
            snprintf(xPath, sizeof(xPath), "%s/%d", zoomPath, x);
            File xDir = SD.open(xPath);
            if (xDir && xDir.isDirectory()) {
                File yEntry = xDir.openNextFile();
                while (yEntry && !found) {
                    int y;
                    if (!yEntry.isDirectory() &&
                        parseTileIndex(Storage::baseName(yEntry.name()), true, y)) {
                        tileX = x;
                        tileY = y;
                        found = true;
                    }
                    yEntry.close();
                    if (!found) yEntry = xDir.openNextFile();
                }
                xDir.close();
            }
        }
        xEntry.close();
        if (!found) xEntry = zoomDir.openNextFile();
    }
    zoomDir.close();
    return found;
}

static bool findFirstMapTile(uint8_t style, uint8_t& zoom, int& tileX, int& tileY) {
    char stylePath[80];
    snprintf(stylePath, sizeof(stylePath), "%s/%s", kMapDir,
             GPS_MAP_STYLE_DIRS[style]);
    for (int z = s_mapZoom; z >= kMapZoomMin; z--)
        if (findFirstTileInZoom(stylePath, (uint8_t)z, tileX, tileY)) {
            zoom = (uint8_t)z;
            return true;
        }
    for (int z = s_mapZoom + 1; z <= kMapZoomMax; z++)
        if (findFirstTileInZoom(stylePath, (uint8_t)z, tileX, tileY)) {
            zoom = (uint8_t)z;
            return true;
        }
    for (int z = s_mapZoom; z >= kMapZoomMin; z--)
        if (findFirstTileInZoom(kMapDir, (uint8_t)z, tileX, tileY)) {
            zoom = (uint8_t)z;
            return true;
        }
    for (int z = s_mapZoom + 1; z <= kMapZoomMax; z++)
        if (findFirstTileInZoom(kMapDir, (uint8_t)z, tileX, tileY)) {
            zoom = (uint8_t)z;
            return true;
        }
    return false;
}

static bool loadMapFallback(uint8_t style) {
    if (s_mapFallbackTried) return s_mapFallbackFound;
    s_mapFallbackTried = true;
    int tileX, tileY;
    if (!findFirstMapTile(style, s_mapFallbackZoom, tileX, tileY)) return false;
    const double count = (double)(1UL << s_mapFallbackZoom);
    s_mapFallbackLon = (float)(((tileX + 0.5) / count) * 360.0 - 180.0);
    s_mapFallbackLat = (float)mapTileLatitude(tileY + 0.5, s_mapFallbackZoom);
    s_mapFallbackFound = true;
    return true;
}

static void loadMapAttribution() {
    if (s_mapAttributionLoaded) return;
    s_mapAttributionLoaded = true;
    File file = SD.open(kMapAttributionPath, FILE_READ);
    if (!file) return;
    size_t length = 0;
    while (file.available() && length + 1 < sizeof(s_mapAttribution)) {
        int c = file.read();
        if (c < 0 || c == '\n' || c == '\r') break;
        if (c >= 32 && c <= 126) s_mapAttribution[length++] = (char)c;
    }
    s_mapAttribution[length] = 0;
    file.close();
}

class GpsMapImageData : public lgfx::DataWrapper {
public:
    GpsMapImageData() { need_transaction = true; }
    bool open(const char* path) override {
        file = SD.open(path, FILE_READ);
        return (bool)file;
    }
    int read(uint8_t* data, uint32_t length) override {
        return file.read(data, length);
    }
    void skip(int32_t offset) override { file.seek(offset, fs::SeekCur); }
    bool seek(uint32_t offset) override { return file.seek(offset, fs::SeekSet); }
    void close() override { file.close(); }
    int32_t tell() override { return (int32_t)file.position(); }

private:
    File file;
};

static bool drawMapTile(M5Canvas& canvas, uint8_t zoom, int tileX, int tileY,
                        int screenX, int screenY) {
    const int tileCount = 1 << zoom;
    if (tileY < 0 || tileY >= tileCount) return false;
    tileX = (tileX % tileCount + tileCount) % tileCount;

    char path[96];
    snprintf(path, sizeof(path), "%s/%u/%d/%d.jpg", kMapDir,
             (unsigned)zoom, tileX, tileY);
    uint8_t style = Config::gps().mapStyle < GPS_MAP_STYLE_N
                        ? Config::gps().mapStyle : 0;
    char styledPath[96];
    snprintf(styledPath, sizeof(styledPath), "%s/%s/%u/%d/%d.jpg", kMapDir,
             GPS_MAP_STYLE_DIRS[style], (unsigned)zoom, tileX, tileY);
    if (SD.exists(styledPath)) {
        GpsMapImageData image;
        return canvas.drawJpgFile(&image, styledPath, screenX, screenY, 256, 256);
    }
    if (SD.exists(path)) {
        GpsMapImageData image;
        return canvas.drawJpgFile(&image, path, screenX, screenY, 256, 256);
    }

    snprintf(path, sizeof(path), "%s/%u/%d/%d.png", kMapDir,
             (unsigned)zoom, tileX, tileY);
    snprintf(styledPath, sizeof(styledPath), "%s/%s/%u/%d/%d.png", kMapDir,
             GPS_MAP_STYLE_DIRS[style], (unsigned)zoom, tileX, tileY);
    if (SD.exists(styledPath)) {
        GpsMapImageData image;
        return canvas.drawPngFile(&image, styledPath, screenX, screenY, 256, 256);
    }
    if (SD.exists(path)) {
        GpsMapImageData image;
        return canvas.drawPngFile(&image, path, screenX, screenY, 256, 256);
    }
    return false;
}

static void drawStreetMap(M5Canvas& canvas) {
    constexpr int mapX = 0;
    constexpr int mapY = 0;
    constexpr int mapW = DISPLAY_W;
    constexpr int mapH = MAIN_H;
    constexpr int mapCenterX = mapW / 2;
    constexpr int mapCenterY = mapH / 2;
    constexpr int centerX = mapX + mapCenterX;
    constexpr int centerY = mapY + mapCenterY;

    canvas.fillRect(mapX, mapY, mapW, mapH, UiStyle::BG);
    if (s_mapAllocFailed) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.setTextDatum(middle_center);
        canvas.drawString("MAP MEMORY LOW", DISPLAY_W / 2, centerY);
        canvas.setTextDatum(top_left);
        return;
    }
    if (!s_mapCanvas.getBuffer()) {
        s_mapCanvas.setColorDepth(8);
        if (s_mapCanvas.createSprite(mapW, mapH)) {
            s_mapCacheValid = false;
        } else {
            s_mapAllocFailed = true;
        }
    }
    if (s_mapAllocFailed) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.setTextDatum(middle_center);
        canvas.drawString("MAP MEMORY LOW", DISPLAY_W / 2, centerY);
        canvas.setTextDatum(top_left);
        return;
    }

    const uint8_t selectedStyle = Config::gps().mapStyle < GPS_MAP_STYLE_N
                                      ? Config::gps().mapStyle : 0;
    if (s_mapCacheStyle != selectedStyle) {
        s_mapFallbackTried = false;
        s_mapFallbackFound = false;
        s_mapCacheValid = false;
    }

    float viewLat = s_lat;
    float viewLon = s_lon;
    if (s_havePos) {
        viewLat = s_lat;
        viewLon = s_lon;
    } else if (s_nPts > 0) {
        viewLat = fromE7(s_pts[s_nPts - 1].lat);
        viewLon = fromE7(s_pts[s_nPts - 1].lon);
    } else {
        const bool firstFallback = !s_mapFallbackTried;
        if (loadMapFallback(selectedStyle)) {
            viewLat = s_mapFallbackLat;
            viewLon = s_mapFallbackLon;
            if (firstFallback) s_mapZoom = s_mapFallbackZoom;
        }
    }
    if (s_mapManualCenter) {
        viewLat = (float)s_mapManualCenterLat;
        viewLon = (float)s_mapManualCenterLon;
    }

    const bool hasMapPosition = s_havePos || s_nPts > 0;
    if (s_mapCacheValid && s_mapCacheHasPosition != hasMapPosition)
        s_mapCacheValid = false;
    if (hasMapPosition && !s_mapCacheValid && !s_mapManualCenter) {
        uint8_t availableZoom = s_mapZoom;
        const bool positionTilesFound =
            findZoomForPosition(viewLat, viewLon, selectedStyle, availableZoom);
        if (positionTilesFound) {
            s_mapZoom = availableZoom;
        } else if (!s_havePos && s_nPts > 0) {
            const bool firstFallback = !s_mapFallbackTried;
            if (loadMapFallback(selectedStyle)) {
            viewLat = s_mapFallbackLat;
            viewLon = s_mapFallbackLon;
                if (firstFallback) s_mapZoom = s_mapFallbackZoom;
            }
        }
    }

    if (!s_havePos && s_nPts == 0 && !s_mapFallbackFound) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.setTextDatum(middle_center);
        canvas.drawString("NO OFFLINE MAP TILES", DISPLAY_W / 2, centerY - 5);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("Check style folder on SD", DISPLAY_W / 2, centerY + 8);
        canvas.setTextDatum(top_left);
        return;
    }

    if (s_mapCacheValid && (s_mapCacheZoom != s_mapZoom ||
                            s_mapCacheStyle != selectedStyle)) {
        s_mapCacheValid = false;
    }
    if (s_mapCacheValid) {
        const double worldPixels = 256.0 * (1UL << s_mapZoom);
        double dx = (double)(viewLon - s_mapCenterLon) * worldPixels / 360.0;
        if (dx > worldPixels / 2.0) dx -= worldPixels;
        if (dx < -worldPixels / 2.0) dx += worldPixels;
        const double dy = mapPixelY(viewLat, s_mapZoom) -
                          mapPixelY(s_mapCenterLat, s_mapZoom);
        if (fabs(dx) >= 6.0 || fabs(dy) >= 6.0) s_mapCacheValid = false;
    }

    if (!s_mapCacheValid) {
        s_mapCenterLat = viewLat;
        s_mapCenterLon = viewLon;
        const double px = mapPixelX(s_mapCenterLon, s_mapZoom);
        const double py = mapPixelY(s_mapCenterLat, s_mapZoom);
        const int firstTileX = (int)floor((px - mapCenterX) / 256.0);
        const int lastTileX = (int)floor((px + (mapW - mapCenterX)) / 256.0);
        const int firstTileY = (int)floor((py - mapCenterY) / 256.0);
        const int lastTileY = (int)floor((py + (mapH - mapCenterY)) / 256.0);
        bool tileFound = false;

        s_mapCanvas.fillSprite(UiStyle::BG);
        s_mapCanvas.setClipRect(0, 0, mapW, mapH);
        for (int tileY = firstTileY; tileY <= lastTileY; tileY++) {
            for (int tileX = firstTileX; tileX <= lastTileX; tileX++) {
                const int screenX = mapCenterX + tileX * 256 - (int)px;
                const int screenY = mapCenterY + tileY * 256 - (int)py;
                tileFound |= drawMapTile(s_mapCanvas, s_mapZoom, tileX, tileY,
                                         screenX, screenY);
            }
        }
        s_mapCanvas.clearClipRect();
        s_mapCacheHasTiles = tileFound;
        s_mapCacheHasPosition = hasMapPosition;
        s_mapCacheZoom = s_mapZoom;
        s_mapCacheStyle = selectedStyle;
        s_mapCacheValid = true;
    }
    s_mapCanvas.pushSprite(&canvas, mapX, mapY);

    if (!s_mapCacheHasTiles) {
        canvas.fillRect(mapX + 8, centerY - 15, mapW - 16, 30, UiStyle::BG);
        canvas.setTextColor(UiStyle::GOLD);
        canvas.setTextDatum(middle_center);
        if (s_havePos) {
            canvas.drawString("NO TILES FOR THIS LOCATION", DISPLAY_W / 2, centerY - 5);
            canvas.setTextColor(UiStyle::DIM);
            canvas.drawString("Check area and style on SD", DISPLAY_W / 2, centerY + 8);
        } else {
            canvas.drawString("NO OFFLINE MAP TILES", DISPLAY_W / 2, centerY - 5);
            canvas.setTextColor(UiStyle::DIM);
            canvas.drawString("Check style folder on SD", DISPLAY_W / 2, centerY + 8);
        }
        canvas.setTextDatum(top_left);
        return;
    }

    const double worldPixels = 256.0 * (1UL << s_mapZoom);
    double markerDx = (double)(s_lon - s_mapCenterLon) * worldPixels / 360.0;
    if (markerDx > worldPixels / 2.0) markerDx -= worldPixels;
    if (markerDx < -worldPixels / 2.0) markerDx += worldPixels;
    const int markerX = centerX + (int)markerDx;
    const int markerY = centerY +
        (int)(mapPixelY(s_lat, s_mapZoom) - mapPixelY(s_mapCenterLat, s_mapZoom));
    canvas.setClipRect(mapX, mapY, mapW, mapH);
    if (s_havePos) canvas.fillCircle(markerX, markerY, 4, hasFix() ? UiStyle::PINK : UiStyle::GOLD);
    if (s_havePos && s_haveHdg) {
        const float radians = s_course * 0.0174533f;
        canvas.drawLine(markerX, markerY,
                        markerX + (int)(sinf(radians) * 11.0f),
                        markerY - (int)(cosf(radians) * 11.0f),
                        UiStyle::PINK);
    }
    canvas.clearClipRect();
    loadMapAttribution();
}

// -------------------------------------------------------------------------
// Page 5 (index 4): MESH — Meshtastic node list + message feed
// -------------------------------------------------------------------------
static void drawMesh(M5Canvas& canvas) {
    // Header already drawn by draw().
    // Layout: left half = node list, right half = last messages
    const uint8_t nodeCount = MeshPig::nodeCount();
    const uint8_t msgCount  = MeshPig::msgCount();
    const uint16_t y0 = 22;
    const uint16_t midX = DISPLAY_W / 2;

    // --- Node list (left) ---
    canvas.setTextSize(1);
    canvas.setTextColor(UiStyle::DIM);
    canvas.setTextDatum(top_left);
    canvas.drawString("NODES", 8, y0);
    canvas.drawFastVLine(midX - 2, y0, DISPLAY_H - y0 - 14, UiStyle::PANEL);

    if (nodeCount == 0) {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("searching...", 8, y0 + 12);
    } else {
        uint8_t shown = 0;
        for (uint8_t i = 0; i < nodeCount && shown < 6; i++) {
            const MeshNode* n = MeshPig::nodeAt(i);
            if (!n) continue;
            uint16_t ny = y0 + 10 + shown * 16;
            // Name
            const char* name = n->shortName[0] ? n->shortName : "????";
            uint16_t nameCol = n->isSelf ? UiStyle::GOLD : UiStyle::TEXT;
            canvas.setTextColor(nameCol);
            canvas.drawString(name, 8, ny);
            // RSSI
            char rssiStr[8];
            snprintf(rssiStr, sizeof(rssiStr), "%d", (int)n->rssi);
            uint16_t rssiCol = n->rssi > -80 ? UiStyle::GREEN
                             : n->rssi > -100 ? UiStyle::GOLD : UiStyle::RED;
            canvas.setTextColor(rssiCol);
            canvas.setTextDatum(top_right);
            canvas.drawString(rssiStr, midX - 6, ny);
            canvas.setTextDatum(top_left);
            // GPS dot if position known
            if (n->hasPos) {
                canvas.fillCircle(midX - 16, ny + 4, 2, UiStyle::CYAN);
            }
            shown++;
        }
        if (nodeCount > 6) {
            char more[8];
            snprintf(more, sizeof(more), "+%u", nodeCount - 6);
            canvas.setTextColor(UiStyle::DIM);
            canvas.drawString(more, 8, y0 + 10 + 6 * 16);
        }
    }

    // --- Message feed (right) ---
    canvas.setTextColor(UiStyle::DIM);
    canvas.setTextDatum(top_left);
    canvas.drawString("MSGS", midX + 2, y0);

    uint8_t showMsgs = 5;
    uint8_t startMsg = msgCount > showMsgs ? msgCount - showMsgs : 0;
    for (uint8_t mi = startMsg; mi < msgCount && (mi - startMsg) < showMsgs; mi++) {
        const MeshMsg* m = MeshPig::msgAt(mi);
        if (!m) continue;
        uint16_t my = y0 + 10 + (mi - startMsg) * 20;
        bool isSelf = (m->from == MeshPig::selfNodeNum());
        // sender badge
        canvas.setTextColor(isSelf ? UiStyle::GOLD : UiStyle::PINK);
        canvas.drawString(m->fromName, midX + 2, my);
        // message text (truncated)
        canvas.setTextColor(UiStyle::TEXT);
        char truncText[22];
        strncpy(truncText, m->text, sizeof(truncText) - 1);
        truncText[sizeof(truncText) - 1] = '\0';
        canvas.drawString(truncText, midX + 2, my + 9);
    }

    if (msgCount == 0) {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("no messages", midX + 2, y0 + 12);
    }

    if (s_meshTyping) {
        const int inputY = MAIN_H - 14;
        canvas.setClipRect(4, inputY, DISPLAY_W - 8, 12);
        canvas.fillRect(4, inputY, DISPLAY_W - 8, 12, UiStyle::PANEL);
        char input[40];
        const size_t len = strlen(s_meshDraft);
        const char* visible = len > sizeof(input) - 2
            ? s_meshDraft + len - (sizeof(input) - 2) : s_meshDraft;
        snprintf(input, sizeof(input), ">%s", visible);
        canvas.setTextSize(1);
        canvas.setTextDatum(top_left);
        canvas.setTextColor(UiStyle::TEXT);
        canvas.drawString(input, 7, inputY + 2);
        canvas.clearClipRect();
    }
}

void draw(M5Canvas& canvas) {
    uiListBackground(canvas);
    if (s_page == 3) {
        drawStreetMap(canvas);
        return;
    }

    canvas.setTextDatum(top_left);
    canvas.setTextSize(2);
    canvas.setTextColor(UiStyle::TITLE);
    uint8_t totalPages = MeshPig::isRunning() ? 5 : 4;
    const char* title = s_page == 1 ? "WORLD" :
                        (s_page == 2 ? "TRAIL" :
                        (s_page == 3 ? "MAP"   :
                        (s_page == 4 ? "MESH"  : "GPS")));
    canvas.drawString(title, 8, 0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_right);
    char pg[8];
    snprintf(pg, sizeof(pg), "%u/%u", (unsigned)(s_page + 1), (unsigned)totalPages);
    canvas.drawString(pg, DISPLAY_W - 8, 4);
    canvas.setTextDatum(top_left);
    canvas.drawLine(8, 18, DISPLAY_W - 8, 18, UiStyle::TITLE);
    if (s_page == 0) drawFix(canvas);
    else if (s_page == 1) drawWorld(canvas);
    else if (s_page == 4) drawMesh(canvas);
    else drawTrail(canvas);
}

void getStatusLine(char* out, size_t n) {
    if (!out || !n) return;
    if (s_run && s_page == 4 && s_meshTyping) {
        snprintf(out, n, "TYPE MESSAGE  ENT SEND  ESC CANCEL");
        return;
    }
    if (s_run && s_page == 4 && MeshPig::isRunning()) {
        strncpy(out, MeshPig::getStatusLine(), n - 1);
        out[n - 1] = '\0';
        return;
    }
    if (s_run && s_page == 1) {
        snprintf(out, n, "WORLD Z%u  WASD MOVE  -/+ ZOOM", s_worldZoom);
        return;
    }
    if (s_run && s_page == 3) {
        if (s_mapAttribution[0] && (millis() / 5000u) % 2u) {
            snprintf(out, n, "MAP DATA: %s", s_mapAttribution);
        } else {
            snprintf(out, n, "MAP 1-4 PAGE  WASD PAN  -/+ ZOOM");
        }
        return;
    }
    if (!s_run) {
        snprintf(out, n, "OFF");
    } else if (Config::gps().powerSave && !s_uartOn) {
        snprintf(out, n, "SLEEP");
    } else if (hasFix()) {
        snprintf(out, n, "FIX %02d SAT", s_sats);
    } else if (s_bytes == 0) {
        snprintf(out, n, "NO DATA");
    } else if (s_havePos) {
        snprintf(out, n, "STALE %02d SAT", s_sats);
    } else {
        snprintf(out, n, "SEARCH");
    }
}

}  // namespace GpsMode
