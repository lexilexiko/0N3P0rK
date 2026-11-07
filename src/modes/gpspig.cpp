#include "gpspig.h"

#include "gps_world.h"
#include "../board/board.h"
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

static const int kRxPinGrove = 1;
static const int kTxPinGrove = 2;
static const int kRxPinAdv = 15;
static const int kTxPinAdv = 13;
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

static HardwareSerial s_uart(1);
static bool s_run = false;
static bool s_uartOn = false;
static bool s_keyWas = false;
static uint8_t s_page = 0;
static uint8_t s_zoom = 1;
static char s_nmea[96];
static uint8_t s_nmeaLen = 0;
static uint32_t s_bytes = 0;
static bool s_valid = false;
static bool s_havePos = false;
static uint32_t s_fixMs = 0;
static float s_lat = 0;
static float s_lon = 0;
static float s_alt = 0;
static float s_kmh = 0;
static float s_course = 0;
static int s_sats = 0;
static bool s_haveAlt = false;
static bool s_haveSpd = false;
static bool s_haveHdg = false;
static Pt* s_pts = nullptr;
static uint16_t s_nPts = 0;

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
    if (!Board::isAdv()) return;
    pinMode(kLoraNss, OUTPUT);
    digitalWrite(kLoraNss, HIGH);
}

static uint32_t activeBaud() {
    const GpsConfig& g = Config::gps();
    uint8_t i = Board::isAdv() ? g.advIdx : g.groveIdx;
    if (i >= GPS_BAUD_N) i = Board::isAdv() ? 4u : 0u;
    return GPS_BAUD_TAB[i];
}

static void openUart() {
    holdLoraOff();
    uint32_t baud = activeBaud();
    s_uart.setRxBufferSize(kRxBuf);
    if (Board::isAdv()) s_uart.begin(baud, SERIAL_8N1, kRxPinAdv, kTxPinAdv);
    else s_uart.begin(baud, SERIAL_8N1, kRxPinGrove, kTxPinGrove);
    s_uartOn = true;
    Serial.printf("[GPS] %s rx=%d tx=%d baud=%u\n",
                  Board::isAdv() ? "ADV" : "GROVE",
                  Board::isAdv() ? kRxPinAdv : kRxPinGrove,
                  Board::isAdv() ? kTxPinAdv : kTxPinGrove,
                  (unsigned)baud);
}

static void closeUart() {
    if (!s_uartOn) return;
    s_uart.end();
    s_uartOn = false;
    holdLoraOff();
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
    if (s_pts && s_nPts > 0) {
        float la = fromE7(s_pts[s_nPts - 1].lat);
        float lo = fromE7(s_pts[s_nPts - 1].lon);
        if (metersBetween(la, lo, lat, lon) < kStepM) return;
    }
    pushPt(toE7(lat), toE7(lon));
    appendSd(lat, lon);
}

static void takeFix(bool ok, float lat, float lon) {
    if (!ok) {
        s_valid = false;
        return;
    }
    s_valid = true;
    s_havePos = true;
    s_fixMs = millis();
    s_lat = lat;
    s_lon = lon;
    notePoint(lat, lon);
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
        if (f[7][0]) {
            s_kmh = (float)atof(f[7]) * 1.852f;
            s_haveSpd = true;
        }
        if (nf >= 9 && f[8][0]) {
            s_course = (float)atof(f[8]);
            s_haveHdg = true;
        }
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
    int n = s_uart.available();
    if (n > 80) n = 80;
    while (n-- > 0) feed((char)s_uart.read());
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
    if (App::windowHidden()) {
        if (Avatar::isSceneSuspended()) Avatar::resumeScene();
    } else if (!Avatar::isSceneSuspended()) {
        Avatar::suspendScene();
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
    s_valid = false;
    s_havePos = false;
    s_fixMs = 0;
    s_haveAlt = false;
    s_haveSpd = false;
    s_haveHdg = false;
    s_sats = 0;
    s_keyWas = true;
    loadTrail();
    openUart();
    Avatar::suspendScene();
    Serial.printf("[GPS] took %u heap=%u\n", (unsigned)heapHeld(), (unsigned)ESP.getFreeHeap());
    Display::showToast("GPS", 700);
}

void stop() {
    if (!s_run && !s_pts && !s_uartOn) return;
    size_t held = heapHeld();
    closeUart();
    free(s_pts);
    s_pts = nullptr;
    s_nPts = 0;
    s_run = false;
    s_valid = false;
    Avatar::resumeScene();
    if (held) {
        Serial.printf("[GPS] returned %u heap=%u\n",
                      (unsigned)held, (unsigned)ESP.getFreeHeap());
    }
}

bool isRunning() { return s_run; }

void update() {
    if (!s_run) return;
    syncScene();
    pollUart();
    if (App::windowHidden()) return;
    if (!keyNewPress(s_keyWas)) return;
    if (keyMin()) return;
    if (keyEsc()) {
        SFX::play(SFX::BACK_NAV);
        stop();
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
    snprintf(port, sizeof(port), Board::isAdv() ? "ADV CAP   %lu" : "GROVE     %lu",
             (unsigned long)activeBaud());
    canvas.drawString(port, 8, 22);
    canvas.setTextColor(s_valid ? UiStyle::GREEN : UiStyle::GOLD);
    const char* state = "SEARCH";
    if (s_bytes == 0) state = "NO DATA";
    else if (s_valid) state = "FIX";
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
    if (s_haveSpd) snprintf(b, sizeof(b), "%.1f KM/H", s_kmh);
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
    canvas.fillRect(sx - 2, sy - 2, 6, 6, s_valid ? UiStyle::PINK : UiStyle::GOLD);
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
    if (s_page == 2) snprintf(out, n, "-/= ZOOM  ^/v PAGE  ESC BKSP");
    else snprintf(out, n, "^/v PAGE  ESC BACK  BKSP HIDE");
}

}  // namespace GpsMode
