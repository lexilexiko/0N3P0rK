#include "gpspig.h"

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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace GpsMode {

static const int kLoraNss = 5;
static const size_t kRxBuf = 1024;
static const uint16_t kTrailCap = 160;
static const float kStepM = 8.0f;
static const char* kDir = "/0N3P0rK/gps";
static const char* kTrack = "/0N3P0rK/gps/track.csv";
static const uint16_t kSpanM[] = {100, 300, 1000, 4000, 20000};

struct Pt {
    int32_t lat;
    int32_t lon;
};

static HardwareSerial s_uartGrove(2);
static HardwareSerial s_uartAdv(1);
static bool s_run = false;
static bool s_uartOn = false;
static bool s_keyWas = false;
static bool s_sceneSuspended = false;
static uint8_t s_page = 0;
static uint8_t s_zoom = 1;
static char s_nmea[96];
static uint8_t s_nmeaLen = 0;
static uint32_t s_bytes = 0;
static uint32_t s_lastPointMs = 0;
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
static float s_hdop = 99.9f;   // horizontal dilution of precision (lower = better)
static uint8_t s_fixMode = 1;  // 1=no fix, 2=2D, 3=3D (from GSA)
static Pt* s_pts = nullptr;
static uint16_t s_nPts = 0;
static HardwareSerial* s_openUart = nullptr;
static File s_trackFile;

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
    return Config::gps().source == GpsSource::ADV_CAP ? 115200u : 9600u;
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
    if (!star || !star[1] || !star[2] || star[3] != '\0') return false;
    for (uint8_t i = 1; i <= 2; i++) {
        char c = star[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'F') ||
              (c >= 'a' && c <= 'f'))) return false;
    }
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
    char* end = nullptr;
    double v = strtod(dm, &end);
    if (end == dm || *end != '\0' || !isfinite(v) || v < 0.0) return false;
    if (hemi[1] != '\0') return false;
    if (lat ? (hemi[0] != 'N' && hemi[0] != 'S')
            : (hemi[0] != 'E' && hemi[0] != 'W')) return false;
    int deg = (int)(v / 100.0);
    double minutes = v - (double)deg * 100.0;
    int maxDeg = lat ? 90 : 180;
    if (minutes < 0.0 || minutes >= 60.0 ||
        deg > maxDeg || (deg == maxDeg && minutes > 0.0)) return false;
    double d = (double)deg + minutes / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') d = -d;
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

static void openTrackFile() {
    if (s_trackFile) return;
    if (!Storage::available()) return;
    if (!SD.exists(kDir)) SD.mkdir(kDir);
    s_trackFile = SD.open(kTrack, FILE_APPEND);
}

static void closeTrackFile() {
    if (s_trackFile) {
        s_trackFile.flush();
        s_trackFile.close();
    }
}

static void appendSd(float lat, float lon) {
    openTrackFile();
    if (!s_trackFile) return;
    char line[48];
    snprintf(line, sizeof(line), "%.6f,%.6f\n", lat, lon);
    s_trackFile.print(line);
    s_trackFile.flush();
}

static void notePoint(float lat, float lon) {
    if (Config::gps().trackIntervalSec == 0) return;  // 0 = SD track recording OFF
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
    // Empty / missing field: skip this sample, keep existing buffer intact.
    if (!knotsText || !knotsText[0]) return s_haveSpd;

    char* end = nullptr;
    float knots = strtof(knotsText, &end);
    // Completely garbled field: also skip rather than wiping the buffer.
    if (end == knotsText || *end != '\0' || !isfinite(knots) ||
        knots < 0.0f || knots > 550.0f) return s_haveSpd;

    float kmh = knots * 1.852f;
    if (kmh < 1.0f) kmh = 0.0f;
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
    char* f[20];
    int nf = 0;
    f[nf++] = s;
    for (char* p = s; *p && nf < (int)(sizeof(f) / sizeof(f[0])); p++) {
        if (*p == ',' || *p == '*') {
            *p = 0;
            f[nf++] = p + 1;
        }
    }
    if (nf < 1) return;
    // $GPGSA / $GNGSA: fix mode (1/2/3) and HDOP (field 16)
    if (endsWith(f[0], "GSA") && nf >= 17) {
        uint8_t mode = (uint8_t)atoi(f[2]);
        if (mode >= 1 && mode <= 3) s_fixMode = mode;
        float hdop = (float)atof(f[16]);
        if (hdop > 0.0f && hdop < 99.0f) s_hdop = hdop;
        return;
    }
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
        // Status field: 'A'=active, 'V'=void, missing=treat as void
        bool active = (f[2][0] == 'A');
        bool pos = active &&
                   parseDm(f[3], f[4], true, lat) && parseDm(f[5], f[6], false, lon);
        // Only update speed when active; don't wipe buffer on void sentences
        if (active) updateSpeed(f[7]);
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
    int n = uart.available();
    while (n-- > 0) feed((char)uart.read());
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
    return n;
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
    s_nmeaLen = 0;
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
    s_hdop = 99.9f;
    s_fixMode = 1;
    s_keyWas = true;
    loadTrail();
    syncUartPower();
    Config::gps().enabled = true;
    Config::save();
    Serial.printf("[GPS] took %u heap=%u\n", (unsigned)heapHeld(), (unsigned)ESP.getFreeHeap());
    Display::showToast("GPS", 700);
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
    closeTrackFile();
    closeUart();
    free(s_pts);
    s_pts = nullptr;
    s_nPts = 0;
    s_run = false;
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
    closeTrackFile();
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
    s_hdop = 99.9f;
    s_fixMode = 1;
    syncUartPower();
}

void updatePowerSave() {
    syncUartPower();
}

void update() {
    if (!s_run) return;
    syncScene();
    syncUartPower();
    pollUart();
    if (App::mode() != AppMode::GPS || App::windowHidden()) return;
    if (!keyNewPress(s_keyWas)) return;
    if (keyMin()) return;
    if (keyEsc()) {
        App::setMode(AppMode::MENU);
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(';') || M5Cardputer.Keyboard.isKeyPressed(':')) {
        s_page = (uint8_t)((s_page + 2) % 3);
        SFX::play(SFX::MENU_CLICK);
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.') || M5Cardputer.Keyboard.isKeyPressed('>')) {
        s_page = (uint8_t)((s_page + 1) % 3);
        SFX::play(SFX::MENU_CLICK);
        return;
    }
    if (s_page != 2) return;
    bool zoomOut = M5Cardputer.Keyboard.isKeyPressed('-') || M5Cardputer.Keyboard.isKeyPressed('_');
    bool zoomIn = M5Cardputer.Keyboard.isKeyPressed('=') || M5Cardputer.Keyboard.isKeyPressed('+');
    if (zoomOut && s_zoom + 1 < (uint8_t)(sizeof(kSpanM) / sizeof(kSpanM[0]))) {
        s_zoom++;
        SFX::play(SFX::MENU_CLICK);
    } else if (zoomIn && s_zoom > 0) {
        s_zoom--;
        SFX::play(SFX::MENU_CLICK);
    }
}

static void drawFix(M5Canvas& canvas) {
    canvas.setTextSize(1);
    canvas.setTextColor(UiStyle::DIM);
    char port[24];
    const char* sourceName = Config::gps().source == GpsSource::GROVE ? "GROVE" :
                             Config::gps().source == GpsSource::ADV_CAP ? "ADV CAP" : "CUSTOM";
    snprintf(port, sizeof(port), "%s %lu", sourceName, (unsigned long)activeBaud());
    canvas.drawString(port, 8, 22);

    // Fix state label + colour
    const bool fresh = hasFix();
    bool stale = s_havePos && s_fixMs && !fresh;
    uint16_t stateColor = fresh ? UiStyle::GREEN : UiStyle::GOLD;
    const char* state = "SEARCH";
    if (s_bytes == 0)  state = "NO DATA";
    else if (fresh)    state = s_fixMode == 2 ? "FIX 2D" :
                               s_fixMode == 3 ? "FIX 3D" : "FIX";
    else if (stale)    { state = "STALE"; stateColor = UiStyle::DIM; }
    canvas.setTextColor(stateColor);
    canvas.drawString(state, 8, 34);

    // Sats + HDOP on same line
    char b[32];
    canvas.setTextColor(UiStyle::TEXT);
    snprintf(b, sizeof(b), "SATS %d", s_sats);
    canvas.drawString(b, 100, 34);
    if (s_hdop < 90.0f) {
        canvas.setTextColor(s_hdop < 2.0f ? UiStyle::GREEN :
                            s_hdop < 5.0f ? UiStyle::GOLD : UiStyle::RED);
        snprintf(b, sizeof(b), "H%.1f", s_hdop);
        canvas.drawString(b, 168, 34);
        canvas.setTextColor(UiStyle::TEXT);
    }

    // Coordinates — dim if stale
    if (s_havePos) {
        canvas.setTextColor(fresh ? UiStyle::TEXT : UiStyle::DIM);
        snprintf(b, sizeof(b), "LAT  %+10.5f", s_lat);
        canvas.drawString(b, 8, 48);
        snprintf(b, sizeof(b), "LON  %+11.5f", s_lon);
        canvas.drawString(b, 8, 60);
        canvas.setTextColor(UiStyle::TEXT);
    } else {
        canvas.drawString("LAT  --", 8, 48);
        canvas.drawString("LON  --", 8, 60);
    }

    // Altitude
    if (s_haveAlt) snprintf(b, sizeof(b), "ALT  %.0f M", s_alt);
    else           snprintf(b, sizeof(b), "ALT  --");
    canvas.drawString(b, 8, 74);

    // Speed
    if (s_haveSpd && millis() - s_speedMs <= 5000)
        snprintf(b, sizeof(b), "%.1f KM/H", s_kmh);
    else
        snprintf(b, sizeof(b), "SPD  --");
    canvas.drawString(b, 120, 74);

    // Heading / hint
    if (s_haveHdg) {
        snprintf(b, sizeof(b), "HDG  %.0f", s_course);
        canvas.drawString(b, 8, 90);
    } else {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString(s_bytes ? "COLD START - NEEDS SKY" : "PLUG GPS AND WAIT", 8, 90);
        canvas.setTextColor(UiStyle::TEXT);
    }

    // Time (top-right, only if we have it)
    if (s_haveTime) {
        char t[20];
        snprintf(t, sizeof(t), "UTC%+d %02d:%02d",
                 Config::gps().timezoneOffset, s_localHour, s_localMinute);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString(t, 128, 90);
    }
}

static void projectWorld(float lat, float lon, int& x, int& y) {
    float fx = (lon + 180.0f) / 360.0f * (float)GPS_MAP_W;
    float fy = (90.0f - lat) / 180.0f * (float)GPS_MAP_H;
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > GPS_MAP_W - 1) fx = GPS_MAP_W - 1;
    if (fy > GPS_MAP_H - 1) fy = GPS_MAP_H - 1;
    x = (int)fx;
    y = (int)fy;
}

static void drawWorld(M5Canvas& canvas) {
    const int y0 = 22;
    for (int y = 0; y < GPS_MAP_H; y++) {
        int run = -1;
        for (int x = 0; x <= GPS_MAP_W; x++) {
            bool land = x < GPS_MAP_W && mapBit(x, y);
            if (land && run < 0) run = x;
            if (!land && run >= 0) {
                canvas.fillRect(run * 2, y0 + y * 2, (x - run) * 2, 2, 0x2589);
                run = -1;
            }
        }
    }
    if (!s_havePos) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.setTextDatum(top_center);
        canvas.drawString("NO FIX", DISPLAY_W / 2, y0 + 32);
        canvas.setTextDatum(top_left);
        return;
    }
    int mx, my;
    projectWorld(s_lat, s_lon, mx, my);
    int sx = mx * 2;
    int sy = y0 + my * 2;
    canvas.fillRect(sx - 2, sy - 2, 6, 6, hasFix() ? UiStyle::PINK : UiStyle::GOLD);
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

void draw(M5Canvas& canvas) {
    uiListBackground(canvas);
    canvas.setTextDatum(top_left);
    canvas.setTextSize(2);
    canvas.setTextColor(UiStyle::TITLE);
    const char* title = s_page == 1 ? "WORLD" : (s_page == 2 ? "TRAIL" : "GPS");
    canvas.drawString(title, 8, 0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_right);
    char pg[8];
    snprintf(pg, sizeof(pg), "%u/3", (unsigned)(s_page + 1));
    canvas.drawString(pg, DISPLAY_W - 8, 4);
    canvas.setTextDatum(top_left);
    canvas.drawLine(8, 18, DISPLAY_W - 8, 18, UiStyle::TITLE);
    if (s_page == 0) drawFix(canvas);
    else if (s_page == 1) drawWorld(canvas);
    else drawTrail(canvas);
}

void getStatusLine(char* out, size_t n) {
    if (!out || !n) return;
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
