// mesh_ui.cpp — MESH screen. Tabs, lists, settings rows, and keys.
// The modem is mesh_pig.cpp. Messages and the keymap are mesh_text.cpp.

#include "mesh_pig.h"
#include "mesh_priv.h"
#include "mesh_glyph.h"
#include "../core/app.h"
#include "../core/config.h"
#include "../ui/display.h"
#include "../ui/keys.h"

#include <M5Cardputer.h>
#include <string.h>

// ════════════════════════════════════════════════════════════════════════════
// DRAW HELPERS
// ════════════════════════════════════════════════════════════════════════════
#define COL_BG      TFT_BLACK
#define COL_HDR     0x0010
#define COL_HDR2    0x2104
#define COL_SEL     0x2965
#define COL_TEXT    TFT_WHITE
#define COL_DIM     0x8410
#define COL_CYAN    TFT_CYAN
#define COL_GREEN   TFT_GREEN
#define COL_YELLOW  TFT_YELLOW
#define COL_PINK    0xF81F
#define COL_RED     TFT_RED
#define COL_ORANGE  0xFD20
#define LH          10
#define TAB_H       11
#define MESH_H      105

// Rows under the tab strip. The key line lives in the real bottom bar.
static int meshRows() {
    int rows = (MESH_H - TAB_H - 2) / LH;
    return rows < 1 ? 1 : rows;
}

static void blitGlyph(M5Canvas& cv, int x, int y, const uint8_t* col, uint16_t fg) {
    for (int c = 0; c < 5; c++) {
        uint8_t bits = col[c];
        for (int r = 0; r < 7; r++) {
            if (bits & (uint8_t)(1u << (6 - r))) cv.drawPixel(x + c, y + r, fg);
        }
    }
}

static int drawUtf8(M5Canvas& cv, int x, int y, int xMax, const char* s,
                    uint16_t fg, uint16_t bg) {
    if (!s) return x;
    while (*s && x + 6 <= xMax) {
        uint32_t cp;
        int n = utf8Next(s, cp);
        if (n <= 0) break;
        s += n;
        if (cp < 0x80) {
            cv.setTextColor(fg, bg);
            cv.setCursor(x, y);
            char ch[2] = { (char)cp, 0 };
            cv.print(ch);
        } else {
            const uint8_t* g = meshGlyph((uint16_t)cp);
            if (g) blitGlyph(cv, x, y, g, fg);
            else {
                cv.setTextColor(fg, bg);
                cv.setCursor(x, y);
                cv.print("?");
            }
        }
        x += 6;
    }
    return x;
}

enum Layer : uint8_t { LY_STRIP = 0, LY_PAGE, LY_DM, LY_WRITE, LY_FIELD };
static uint8_t  s_tab = 0;
static Layer    s_layer = LY_STRIP;
static Layer    s_writeFrom = LY_PAGE;
static uint8_t  s_setRow = 0;
static uint8_t  s_setScroll = 0;
static uint8_t  s_field = 0;
static bool     s_fieldAscii = false;
static uint8_t  s_fieldMax = 47;
static bool     s_holdUi = false;
static bool     s_swallowEnter = false;
static uint8_t  s_lang = 0;
static uint8_t  s_nodeScroll = 0;
static uint8_t  s_chatScroll = 0;
static uint8_t  s_dmScroll = 0;
static bool     s_chatStick = true;
static bool     s_dmStick = true;
static char     s_cmpBuf[48] = {};
static uint8_t  s_cmpLen = 0;
static uint32_t s_lastDraw = 0;

static const char* langName() {
    return s_lang == 1 ? "RU" : s_lang == 2 ? "UA" : "EN";
}

static void pushCp(uint32_t cp) {
    char tmp[4];
    uint8_t n = 0;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else return;
    if ((uint16_t)s_cmpLen + n > s_fieldMax) return;
    memcpy(s_cmpBuf + s_cmpLen, tmp, n);
    s_cmpLen = (uint8_t)(s_cmpLen + n);
    s_cmpBuf[s_cmpLen] = 0;
}

static void popCp() {
    if (!s_cmpLen) return;
    do { s_cmpLen--; } while (s_cmpLen && (((uint8_t)s_cmpBuf[s_cmpLen] & 0xC0) == 0x80));
    s_cmpBuf[s_cmpLen] = 0;
}

static uint16_t sigCol(int16_t rssi) {
    return rssi > -80 ? COL_GREEN : (rssi > -100 ? COL_YELLOW : COL_RED);
}

static void ageStr(uint32_t lastMs, char* out, uint8_t cap) {
    if (!lastMs) { snprintf(out, cap, "--"); return; }
    uint32_t s = (millis() - lastMs) / 1000;
    if (s < 60) snprintf(out, cap, "%lus", (unsigned long)s);
    else if (s < 3600) snprintf(out, cap, "%lum", (unsigned long)(s / 60));
    else snprintf(out, cap, "%luh", (unsigned long)(s / 3600));
}

static int putLine(M5Canvas& cv, int y, int yMax, int w, const char* s, uint16_t col) {
    if (y > yMax - 8) return y;
    drawUtf8(cv, 2, y, w - 2, s, col, COL_BG);
    return y + LH;
}

namespace MeshPig {

void uiArmOpen() { s_swallowEnter = true; }
void uiStick() { s_chatStick = true; s_dmStick = true; }

void uiReset() {
    if (s_holdUi) return;
    s_tab = 0;
    s_layer = LY_STRIP;
    s_setRow = 0;
    s_setScroll = 0;
}

void uiClearNav() {
    s_nodeScroll = 0;
    s_chatScroll = 0;
    s_dmScroll = 0;
    s_chatStick = true;
    s_dmStick = true;
    s_cmpLen = 0;
    memset(s_cmpBuf, 0, sizeof(s_cmpBuf));
}

static uint8_t countOnline() {
    if (!s_nodes) return 0;
    uint32_t now = (uint32_t)millis();
    uint8_t n = 0;
    for (uint8_t i = 0; i < s_nodeN; i++) {
        if (s_nodes[i].isSelf) continue;
        if (now - s_nodes[i].lastSeen < 8UL * 60UL * 1000UL) n++;
    }
    return n;
}

static const char* meshRegionName(uint8_t i) {
    static const char* names[] = {
        "EU868","US915","ANZ923","EU433","JP923","KR921","TW923","RU868",
        "IN865","NZ865","TH923","UA433","UA868","MY433","MY919","SG923","2.4GHz"
    };
    return i < 17 ? names[i] : "?";
}
static const char* meshModemName(uint8_t i) {
    static const char* names[] = {
        "LongFast","LongSlow","VLongSlow","MedSlow","MedFast",
        "ShortSlow","ShortFast","LongMod","ShortTurbo","LongTurbo"
    };
    return i < 10 ? names[i] : "?";
}

static bool heardOther() {
    if (!s_nodes) return false;
    for (uint8_t i = 0; i < s_nodeN; i++) if (!s_nodes[i].isSelf) return true;
    return false;
}

static const char* SET_LBL[] = {
    "MESH", "REGION", "MODEM", "TX dBm", "HOPS", "SLOT",
    "FREQ", "CHANNEL", "NAME", "PSK", "RX BOOST"
};
static const uint8_t SET_N = 11;

static bool keyHas(char a) {
    if (M5Cardputer.Keyboard.isKeyPressed(a)) return true;
    for (char c : M5Cardputer.Keyboard.keysState().word) {
        if (c == a) return true;
    }
    return false;
}

// Comma and slash move the tab from the strip and from inside a page.
// A DM closes onto the new tab. Text boxes keep those keys for letters.
static void moveTab(int dir) {
    s_tab = (uint8_t)((s_tab + (dir < 0 ? 3 : 1)) % 4);
    if (s_layer == LY_DM) s_layer = LY_PAGE;
}

static void radioRestart() {
    if (!Config::mesh().enabled) return;
    s_holdUi = true;
    if (isRunning()) stop();
    start();
    s_holdUi = false;
}

static void drawTabs(M5Canvas& cv, int w) {
    static const char* tabs[] = { "CHAT", "PEOPLE", "RADIO", "SET" };
    const int tw = w / 4;
    for (int i = 0; i < 4; i++) {
        int x = i * tw;
        bool on = (i == (int)s_tab);
        uint16_t bg = on ? (uint16_t)COL_HDR : (uint16_t)COL_HDR2;
        cv.fillRect(x, 0, tw, TAB_H, bg);
        uint16_t fg = on ? (uint16_t)COL_CYAN : (uint16_t)COL_DIM;
        cv.setTextColor(fg, bg);
        int lw = (int)strlen(tabs[i]) * 6;
        cv.setCursor(x + (tw - lw) / 2, 1);
        cv.print(tabs[i]);
        uint16_t mark = (s_layer == LY_STRIP) ? (uint16_t)COL_YELLOW : (uint16_t)COL_CYAN;
        if (on) cv.fillRect(x, TAB_H - 2, tw, 2, mark);
        if (i == 0 && !s_chatStick) {
            cv.setTextColor(COL_YELLOW, bg);
            cv.setCursor(x + 2, 1);
            cv.print("+");
        }
    }
    uint16_t pip = COL_RED;
    if (s_radioOk && s_rs == RS_TX) pip = COL_PINK;
    else if (s_radioOk) pip = COL_GREEN;
    cv.fillRect(tw - 7, 3, 4, 4, pip);
}

static void drawNodeRow(M5Canvas& cv, int y, int w, const MeshNode& n, bool sel) {
    uint16_t bg = COL_BG;
    if (sel) {
        cv.fillRect(0, y - 1, w, LH, COL_SEL);
        cv.fillRect(0, y - 1, 2, LH, COL_CYAN);
        bg = COL_SEL;
    }
    bool fresh = n.isSelf || ((uint32_t)millis() - n.lastSeen < 8UL * 60UL * 1000UL);
    uint16_t nameCol = n.isSelf ? (uint16_t)COL_PINK : (fresh ? (uint16_t)COL_TEXT : (uint16_t)COL_DIM);
    if (n.isSelf) drawUtf8(cv, 4, y, 40, "you", nameCol, bg);
    else if (n.shortName[0]) drawUtf8(cv, 4, y, 40, n.shortName, nameCol, bg);
    else {
        char id[8];
        snprintf(id, sizeof(id), "!%04lx", (unsigned long)(n.nodeNum & 0xFFFF));
        drawUtf8(cv, 4, y, 40, id, nameCol, bg);
    }
    cv.setTextColor(n.isSelf ? (uint16_t)COL_DIM : sigCol(n.rssi), bg);
    cv.setCursor(42, y);
    if (n.isSelf) cv.print("  --");
    else cv.printf("%4d", n.rssi);
    char age[8];
    ageStr(n.lastSeen, age, sizeof(age));
    cv.setTextColor(COL_DIM, bg);
    cv.setCursor(72, y);
    cv.printf("%4s", age);
    if (n.longName[0]) drawUtf8(cv, 104, y, w - 2, n.longName, nameCol, bg);
}

static void drawChat(M5Canvas& cv, int w, int h, bool dm) {
    int y = TAB_H + 2;
    int yMax = h - 2;
    int rows = meshRows();
    uint8_t& scroll = dm ? s_dmScroll : s_chatScroll;
    bool& stick = dm ? s_dmStick : s_chatStick;
    uint8_t nmsg = dm ? s_dmN : s_chatN;
    uint8_t maxTop = (nmsg > (uint8_t)rows) ? (uint8_t)(nmsg - (uint8_t)rows) : 0;
    if (scroll > maxTop) scroll = maxTop;
    if (stick) scroll = maxTop;

    if (!nmsg) {
        if (!Config::mesh().enabled) {
            y = putLine(cv, y, yMax, w, "Mesh is off.", COL_RED);
            putLine(cv, y, yMax, w, "Turn it on in the SET tab.", COL_DIM);
        } else if (!dm && s_radioError[0]) {
            y = putLine(cv, y, yMax, w, s_radioError, COL_RED);
            const MeshConfig& mc = Config::mesh();
            char match[40];
            snprintf(match, sizeof(match), "%s  %s  same key",
                     meshRegionName(mc.region), meshModemName(mc.modem));
            putLine(cv, y, yMax, w, match, COL_DIM);
        } else if (dm) {
            y = putLine(cv, y, yMax, w, "No direct messages yet.", COL_DIM);
            putLine(cv, y, yMax, w, "T writes to this person.", COL_DIM);
        } else {
            y = putLine(cv, y, yMax, w, "Listening for this channel.", COL_TEXT);
            y = putLine(cv, y, yMax, w, "Other people's messages", COL_DIM);
            y = putLine(cv, y, yMax, w, "show up in this chat.", COL_DIM);
            const MeshConfig& mc = Config::mesh();
            char match[40];
            snprintf(match, sizeof(match), "%s  %s  same key",
                     meshRegionName(mc.region), meshModemName(mc.modem));
            y = putLine(cv, y, yMax, w, match, COL_CYAN);
            if (s_radioOk && !s_rfExpanderReady)
                putLine(cv, y, yMax, w, "Antenna switch is off.", COL_YELLOW);
        }
        return;
    }
    for (int i = 0; i < rows; i++) {
        Msg* m = dm ? dmGet((uint8_t)(scroll + i)) : chatGet((uint8_t)(scroll + i));
        if (!m) break;
        int yy = y + i * LH;
        uint16_t col = m->sent ? (uint16_t)COL_GREEN : (uint16_t)COL_TEXT;
        const char* who = m->fromName[0] ? m->fromName : "?";
        int x = drawUtf8(cv, 2, yy, 56, who, col, COL_BG);
        cv.setTextColor(col, (uint16_t)COL_BG);
        cv.setCursor(x, yy);
        cv.print(m->to == BROADCAST_ID ? ":" : ">");
        drawUtf8(cv, x + 6, yy, w - 2, m->text, col, COL_BG);
    }
}

static void drawPeople(M5Canvas& cv, int w, int h) {
    int y = TAB_H + 2;
    int rows = meshRows();
    if (rows < 2) rows = 2;
    cv.setTextColor(COL_DIM, (uint16_t)COL_BG);
    cv.setCursor(4, y);
    if (!s_nodes || !heardOther()) cv.print("nobody yet");
    else cv.printf("%u on", countOnline());
    cv.setCursor(72, y);
    cv.print("dBm  age");
    int listRows = rows - 1;
    if (!s_nodes || !s_nodeN) {
        putLine(cv, y + LH, h - 2, w, "Names land here after a packet.", COL_DIM);
        return;
    }
    if (s_nodeScroll >= s_nodeN) s_nodeScroll = (uint8_t)(s_nodeN - 1);
    int cur = s_nodeScroll;
    int top = cur - listRows + 1;
    if (top < 0) top = 0;
    for (int i = 0; i < listRows && top + i < s_nodeN; i++) {
        int idx = top + i;
        drawNodeRow(cv, y + LH + i * LH, w, s_nodes[idx], idx == cur && s_layer == LY_PAGE);
    }
}

static void drawRadio(M5Canvas& cv, int w, int h) {
    const MeshConfig& mc = Config::mesh();
    float freq = 0, bw = 0;
    uint8_t sf = 0, cr = 0;
    int8_t power = 0;
    bool planned = planRadio(mc, freq, bw, sf, cr, power);
    (void)cr;
    char line[44];
    int y = TAB_H + 2;
    int yMax = h - 2;
    if (!mc.enabled) y = putLine(cv, y, yMax, w, "MESH OFF", COL_RED);
    else if (!s_radioOk && s_radioError[0]) y = putLine(cv, y, yMax, w, s_radioError, COL_RED);
    else if (s_radioOk && s_rs == RS_TX) y = putLine(cv, y, yMax, w, "LORA TX", COL_PINK);
    else if (s_radioOk) y = putLine(cv, y, yMax, w, "LORA OK", COL_GREEN);
    if (planned) {
        snprintf(line, sizeof(line), "%.3f MHz%s", freq, mc.freqOverride ? "  set" : "");
        y = putLine(cv, y, yMax, w, line, COL_TEXT);
    } else {
        y = putLine(cv, y, yMax, w, "Frequency not set", COL_RED);
    }
    snprintf(line, sizeof(line), "%s   %s", meshRegionName(mc.region), meshModemName(mc.modem));
    y = putLine(cv, y, yMax, w, line, COL_CYAN);
    if (planned)
        snprintf(line, sizeof(line), "SF%u  BW%.0f  %ddBm  hop %u", sf, bw, power, mc.hopLimit);
    else
        snprintf(line, sizeof(line), "hop %u", mc.hopLimit);
    y = putLine(cv, y, yMax, w, line, COL_TEXT);
    snprintf(line, sizeof(line), "channel  %s", mc.channel[0] ? mc.channel : "-");
    y = putLine(cv, y, yMax, w, line, COL_TEXT);
    snprintf(line, sizeof(line), "you  !%04lx  %s  %s",
             (unsigned long)(s_nodeNum & 0xFFFF), mc.shortName,
             s_pkiReady ? "KEY" : "NO KEY");
    y = putLine(cv, y, yMax, w, line, s_pkiReady ? COL_PINK : COL_RED);
    snprintf(line, sizeof(line), "rx %u   tx %u   queue %u", s_rxPkts, s_txPkts, s_txN);
    y = putLine(cv, y, yMax, w, line, COL_TEXT);
    if (s_haveSig) {
        snprintf(line, sizeof(line), "last  %d dBm  snr %+d", s_lastRssi, s_lastSnr);
        y = putLine(cv, y, yMax, w, line, sigCol(s_lastRssi));
    } else {
        char age[8] = "--";
        if (s_listenSince) ageStr(s_listenSince, age, sizeof(age));
        snprintf(line, sizeof(line), "last  none yet   %s", age);
        y = putLine(cv, y, yMax, w, line, COL_DIM);
    }
    if (s_rfExpanderReady) putLine(cv, y, yMax, w, "switch on", COL_GREEN);
    else if (s_radioOk) putLine(cv, y, yMax, w, "switch off  short range", COL_YELLOW);
    else putLine(cv, y, yMax, w, "switch off", COL_DIM);
}

static void setValue(uint8_t row, char* out, int cap) {
    const MeshConfig& mc = Config::mesh();
    switch (row) {
        case 0: snprintf(out, cap, "%s", mc.enabled ? "ON" : "OFF"); break;
        case 1: snprintf(out, cap, "%s", meshRegionName(mc.region)); break;
        case 2: snprintf(out, cap, "%s", meshModemName(mc.modem)); break;
        case 3: snprintf(out, cap, "%d", (int)mc.txPower); break;
        case 4: snprintf(out, cap, "%u", mc.hopLimit); break;
        case 5:
            if (!mc.freqSlot) snprintf(out, cap, "AUTO");
            else snprintf(out, cap, "%u", mc.freqSlot);
            break;
        case 6:
            if (!mc.freqOverride) snprintf(out, cap, "AUTO");
            else snprintf(out, cap, "%.3f", (float)mc.freqKHz / 1000.0f);
            break;
        case 7: snprintf(out, cap, "%s", mc.channel[0] ? mc.channel : "-"); break;
        case 8: snprintf(out, cap, "%s", mc.shortName[0] ? mc.shortName : "-"); break;
        case 9:
            snprintf(out, cap, "%s",
                     strcmp(mc.psk, "AQ==") == 0 ? "DEFAULT" :
                     strcmp(mc.psk, "AA==") == 0 ? "OPEN" : "CUSTOM");
            break;
        default: snprintf(out, cap, "%s", mc.rxBoost ? "ON" : "OFF"); break;
    }
}

static void drawSet(M5Canvas& cv, int w, int h) {
    int rows = meshRows();
    if (s_setRow >= SET_N) s_setRow = SET_N - 1;
    if (s_setRow < s_setScroll) s_setScroll = s_setRow;
    if (s_setRow >= s_setScroll + rows)
        s_setScroll = (uint8_t)(s_setRow - rows + 1);
    int y0 = TAB_H + 2;
    for (int i = 0; i < rows && s_setScroll + i < SET_N; i++) {
        uint8_t row = (uint8_t)(s_setScroll + i);
        int y = y0 + i * LH;
        bool sel = (row == s_setRow && s_layer == LY_PAGE);
        uint16_t bg = sel ? (uint16_t)COL_SEL : (uint16_t)COL_BG;
        if (sel) {
            cv.fillRect(0, y - 1, w, LH, COL_SEL);
            cv.fillRect(0, y - 1, 2, LH, COL_CYAN);
        }
        char val[24];
        setValue(row, val, sizeof(val));
        uint16_t vc = COL_TEXT;
        if ((row == 0 || row == 10) && val[1] == 'N') vc = COL_GREEN;
        if ((row == 0 || row == 10) && val[1] == 'F') vc = COL_RED;
        drawUtf8(cv, 4, y, 80, SET_LBL[row], sel ? (uint16_t)COL_CYAN : (uint16_t)COL_TEXT, bg);
        drawUtf8(cv, 84, y, w - 2, val, vc, bg);
    }
    (void)h;
}

static const char* KEY_ROWS[] = { "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./" };

static void drawWriter(M5Canvas& cv, int w, int h, const char* title) {
    cv.fillRect(0, 0, w, TAB_H, COL_HDR);
    cv.setTextColor(COL_CYAN, (uint16_t)COL_HDR);
    cv.setCursor(2, 1);
    cv.print(langName());
    if (s_fieldAscii) cv.print(" ASCII");
    drawUtf8(cv, s_fieldAscii ? 56 : 26, 1, w - 36, title ? title : "", COL_TEXT, COL_HDR);
    char cnt[8];
    snprintf(cnt, sizeof(cnt), "%d", utf8Cols(s_cmpBuf, s_cmpLen));
    cv.setTextColor(COL_DIM, (uint16_t)COL_HDR);
    cv.setCursor(w - 6 * (int)strlen(cnt) - 2, 1);
    cv.print(cnt);

    cv.drawRect(1, TAB_H + 1, w - 2, h - TAB_H - 3, COL_CYAN);
    int x0 = 4;
    int y0 = TAB_H + 4;
    int xMax = w - 4;
    int yMax = h - 4;
    int maxC = (xMax - x0) / 6;
    if (maxC < 1) maxC = 1;
    int col = 0;
    int x = x0;
    int y = y0;
    for (int i = 0; i < s_cmpLen && s_cmpBuf[i];) {
        uint32_t cp;
        int n = utf8Next(s_cmpBuf + i, cp);
        if (n <= 0) break;
        i += n;
        if (col >= maxC) { col = 0; x = x0; y += LH; }
        if (y + 8 > yMax) break;
        if (cp < 0x80) {
            cv.setTextColor(COL_TEXT, (uint16_t)COL_BG);
            cv.setCursor(x, y);
            char ch[2] = { (char)cp, 0 };
            cv.print(ch);
        } else {
            const uint8_t* g = meshGlyph((uint16_t)cp);
            if (g) blitGlyph(cv, x, y, g, COL_TEXT);
            else {
                cv.setTextColor(COL_TEXT, (uint16_t)COL_BG);
                cv.setCursor(x, y);
                cv.print("?");
            }
        }
        x += 6;
        col++;
    }
    if (((uint32_t)millis() / 400u) & 1u) {
        int cx, cy, ccol;
        int nch = utf8Cols(s_cmpBuf, s_cmpLen);
        ccol = maxC ? (nch % maxC) : 0;
        cx = x0 + ccol * 6;
        cy = y0 + (maxC ? (nch / maxC) : 0) * LH;
        if (cy + 8 <= yMax) cv.drawFastVLine(cx, cy, 8, COL_CYAN);
    }
    if (s_cmpLen || s_fieldAscii) return;
    int ky = y0 + LH * 2;
    uint8_t lang = s_lang;
    for (int r = 0; r < 3 && ky + 8 < yMax; r++) {
        int kx = x0;
        for (const char* p = KEY_ROWS[r]; *p && kx + 6 <= xMax; p++) {
            uint32_t cp = mapLay(*p, lang);
            if (cp < 0x80) {
                cv.setTextColor(COL_DIM, (uint16_t)COL_BG);
                cv.setCursor(kx, ky);
                char ch[2] = { (char)cp, 0 };
                cv.print(ch);
            } else {
                const uint8_t* g = meshGlyph((uint16_t)cp);
                if (g) blitGlyph(cv, kx, ky, g, COL_DIM);
            }
            kx += 6;
        }
        ky += LH;
    }
}

static void drawDmHdr(M5Canvas& cv, int w) {
    cv.fillRect(0, 0, w, TAB_H, COL_HDR);
    const char* nm = "?";
    if (s_nodes) {
        for (uint8_t i = 0; i < s_nodeN; i++) {
            if (s_nodes[i].nodeNum == s_dmNode && s_nodes[i].shortName[0]) {
                nm = s_nodes[i].shortName;
                break;
            }
        }
    }
    cv.setTextColor(COL_CYAN, (uint16_t)COL_HDR);
    cv.setCursor(2, 1);
    cv.print("DM");
    drawUtf8(cv, 20, 1, w - 20, nm, COL_TEXT, COL_HDR);
}

void draw(M5Canvas& canvas) {
    uint32_t now = (uint32_t)millis();
    bool typing = (s_layer == LY_WRITE || s_layer == LY_FIELD);
    if (!typing && now - s_lastDraw < 120) return;
    s_lastDraw = now;

    int w = canvas.width();
    int h = canvas.height();
    canvas.fillScreen(COL_BG);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextWrap(false);

    if (s_layer == LY_WRITE || s_layer == LY_FIELD) {
        char title[32];
        if (s_layer == LY_FIELD) {
            const char* t = "PSK";
            if (s_field == 6) t = "FREQ";
            else if (s_field == 7) t = "CHANNEL";
            else if (s_field == 8) t = "NAME";
            snprintf(title, sizeof(title), "%s", t);
        } else if (s_writeFrom == LY_DM) {
            const char* nm = "?";
            if (s_nodes) {
                for (uint8_t i = 0; i < s_nodeN; i++) {
                    if (s_nodes[i].nodeNum == s_dmNode && s_nodes[i].shortName[0]) {
                        nm = s_nodes[i].shortName;
                        break;
                    }
                }
            }
            snprintf(title, sizeof(title), "DM %s", nm);
        } else {
            snprintf(title, sizeof(title), "%s", Config::mesh().channel);
        }
        drawWriter(canvas, w, h, title);
        return;
    }
    if (s_layer == LY_DM) {
        drawDmHdr(canvas, w);
        drawChat(canvas, w, h, true);
        return;
    }
    drawTabs(canvas, w);
    if (s_tab == 0) drawChat(canvas, w, h, false);
    else if (s_tab == 1) drawPeople(canvas, w, h);
    else if (s_tab == 2) drawRadio(canvas, w, h);
    else drawSet(canvas, w, h);
}

static bool fieldCharOk(uint32_t cp) {
    if (!s_fieldAscii) return cp >= 0x20;
    if (cp < 0x20 || cp >= 0x7F) return false;
    char c = (char)cp;
    if (s_field == 6) return (c >= '0' && c <= '9') || c == '.';
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

static void openWrite() {
    if (!s_run || !s_radioOk) {
        Display::showToast(Config::mesh().enabled ? "NO RADIO" : "MESH OFF", 800);
        return;
    }
    s_writeFrom = s_layer;
    s_layer = LY_WRITE;
    s_fieldAscii = false;
    s_fieldMax = 47;
    s_cmpLen = 0;
    s_cmpBuf[0] = 0;
}

static void openField() {
    const MeshConfig& mc = Config::mesh();
    s_field = s_setRow;
    s_cmpBuf[0] = 0;
    s_cmpLen = 0;
    s_fieldAscii = (s_field == 6 || s_field == 9);
    if (s_field == 6) {
        s_fieldMax = 8;
        if (mc.freqOverride) {
            snprintf(s_cmpBuf, sizeof(s_cmpBuf), "%.3f", (float)mc.freqKHz / 1000.0f);
            s_cmpLen = (uint8_t)strlen(s_cmpBuf);
        }
    } else if (s_field == 7) {
        s_fieldMax = 19;
        memcpy(s_cmpBuf, mc.channel, 19);
        s_cmpBuf[19] = 0;
        s_cmpLen = (uint8_t)strlen(s_cmpBuf);
    } else if (s_field == 8) {
        s_fieldMax = 4;
        memcpy(s_cmpBuf, mc.shortName, 4);
        s_cmpBuf[4] = 0;
        s_cmpLen = (uint8_t)strlen(s_cmpBuf);
    } else {
        s_fieldMax = 44;
        memcpy(s_cmpBuf, mc.psk, 44);
        s_cmpBuf[44] = 0;
        s_cmpLen = (uint8_t)strlen(s_cmpBuf);
    }
    s_layer = LY_FIELD;
}

static void commitWrite() {
    if (s_cmpLen && s_run && s_radioOk) {
        s_cmpBuf[s_cmpLen] = 0;
        uint32_t dest = (s_writeFrom == LY_DM) ? s_dmNode : BROADCAST_ID;
        if (sendText(s_cmpBuf, dest)) {
            if (s_writeFrom == LY_DM) s_dmStick = true;
            else s_chatStick = true;
        }
    }
    s_layer = s_writeFrom;
    s_cmpLen = 0;
    s_cmpBuf[0] = 0;
}

static void rememberName() {
    if (!s_nodes) return;
    for (uint8_t i = 0; i < s_nodeN; i++) {
        if (!s_nodes[i].isSelf) continue;
        memset(s_nodes[i].shortName, 0, sizeof(s_nodes[i].shortName));
        memcpy(s_nodes[i].shortName, Config::mesh().shortName, 4);
    }
}

static void commitField() {
    MeshConfig& mc = Config::mesh();
    s_cmpBuf[s_cmpLen] = 0;
    bool restart = false;
    if (s_field == 6) {
        float fMHz = atof(s_cmpBuf);
        if (fMHz >= 137.0f && fMHz <= 960.0f) {
            mc.freqKHz = (uint32_t)(fMHz * 1000.0f + 0.5f);
            mc.freqOverride = true;
            restart = true;
            Display::showToast("FREQ SET", 800);
        } else if (!s_cmpBuf[0] || s_cmpBuf[0] == '0') {
            mc.freqOverride = false;
            restart = true;
            Display::showToast("FREQ AUTO", 800);
        } else {
            Display::showToast("BAD FREQ", 800);
            return;
        }
    } else if (s_field == 7) {
        memset(mc.channel, 0, sizeof(mc.channel));
        if (s_cmpLen) memcpy(mc.channel, s_cmpBuf, s_cmpLen);
        if (!mc.channel[0]) strncpy(mc.channel, "LongFast", sizeof(mc.channel) - 1);
        restart = true;
        Display::showToast("CHANNEL SET", 800);
    } else if (s_field == 8) {
        memset(mc.shortName, 0, sizeof(mc.shortName));
        if (s_cmpLen) memcpy(mc.shortName, s_cmpBuf, s_cmpLen > 4 ? 4 : s_cmpLen);
        if (!mc.shortName[0]) strncpy(mc.shortName, "PIG", sizeof(mc.shortName) - 1);
        rememberName();
        Display::showToast("NAME SET", 800);
    } else {
        memset(mc.psk, 0, sizeof(mc.psk));
        if (s_cmpLen) memcpy(mc.psk, s_cmpBuf, s_cmpLen);
        if (!mc.psk[0]) strncpy(mc.psk, "AQ==", sizeof(mc.psk) - 1);
        restart = true;
        Display::showToast("PSK SET", 800);
    }
    Config::save();
    s_layer = LY_PAGE;
    s_cmpLen = 0;
    s_cmpBuf[0] = 0;
    if (restart) radioRestart();
}

static void bumpSet(int dir) {
    MeshConfig& mc = Config::mesh();
    switch (s_setRow) {
        case 0:
            if (dir > 0 && !mc.enabled) {
                mc.enabled = true;
                Config::save();
                s_holdUi = true;
                if (!isRunning()) start();
                s_holdUi = false;
            } else if (dir < 0 && mc.enabled) {
                mc.enabled = false;
                Config::save();
                if (isRunning()) stop();
            }
            break;
        case 1: {
            int v = (int)mc.region + dir;
            if (v < 0 || v > 16) break;
            mc.region = (uint8_t)v;
            Config::save();
            radioRestart();
            break;
        }
        case 2: {
            int v = (int)mc.modem + dir;
            if (v < 0 || v > 9) break;
            mc.modem = (uint8_t)v;
            Config::save();
            radioRestart();
            break;
        }
        case 3: {
            int v = (int)mc.txPower + dir;
            if (v < 2 || v > 22) break;
            mc.txPower = (int8_t)v;
            Config::save();
            radioRestart();
            break;
        }
        case 4: {
            int v = (int)mc.hopLimit + dir;
            if (v < 1 || v > 7) break;
            mc.hopLimit = (uint8_t)v;
            Config::save();
            break;
        }
        case 5: {
            int v = (int)mc.freqSlot + dir;
            if (v < 0 || v > 104) break;
            mc.freqSlot = (uint8_t)v;
            Config::save();
            radioRestart();
            break;
        }
        case 10:
            if (dir > 0 && !mc.rxBoost) { mc.rxBoost = true; Config::save(); radioRestart(); }
            else if (dir < 0 && mc.rxBoost) { mc.rxBoost = false; Config::save(); radioRestart(); }
            break;
        default:
            break;
    }
}

static void typingKeys(const Keyboard_Class::KeysState& ks, bool esc) {
    if (ks.enter) {
        if (s_layer == LY_WRITE) commitWrite();
        else commitField();
        return;
    }
    if (esc) {
        Layer back = (s_layer == LY_WRITE) ? s_writeFrom : LY_PAGE;
        s_layer = back;
        s_cmpLen = 0;
        s_cmpBuf[0] = 0;
        return;
    }
    if (ks.del) { popCp(); return; }
    uint8_t lang = s_fieldAscii ? 0 : s_lang;
    for (char c : ks.word) {
        if (c < 0x20 || c >= 0x7F) continue;
        uint32_t cp = mapLay(c, lang);
        if (!fieldCharOk(cp)) continue;
        pushCp(cp);
    }
}

static void chatKeys(const Keyboard_Class::KeysState& ks, bool dm) {
    if (!ks.fn) {
        for (char c : ks.word) {
            if (c == 't' || c == 'T') { openWrite(); return; }
        }
    }
    int dir = 0;
    if (keyHas(';')) dir = -1;
    else if (keyHas('.')) dir = 1;
    if (!dir) return;
    uint8_t& scroll = dm ? s_dmScroll : s_chatScroll;
    bool& stick = dm ? s_dmStick : s_chatStick;
    uint8_t nmsg = dm ? s_dmN : s_chatN;
    int rows = meshRows();
    uint8_t maxTop = (nmsg > (uint8_t)rows) ? (uint8_t)(nmsg - (uint8_t)rows) : 0;
    if (dir < 0 && scroll > 0) { scroll--; stick = false; }
    else if (dir > 0 && scroll < maxTop) {
        scroll++;
        if (scroll >= maxTop) stick = true;
    }
}

static void peopleKeys(const Keyboard_Class::KeysState& ks) {
    if (!s_nodes || !s_nodeN) return;
    if (s_nodeScroll >= s_nodeN) s_nodeScroll = (uint8_t)(s_nodeN - 1);
    if (keyHas(';')) {
        if (s_nodeScroll > 0) s_nodeScroll--;
    } else if (keyHas('.')) {
        if (s_nodeScroll + 1 < s_nodeN) s_nodeScroll++;
    } else if (ks.enter && !s_nodes[s_nodeScroll].isSelf) {
        if (!s_run || !s_radioOk) {
            Display::showToast(Config::mesh().enabled ? "NO RADIO" : "MESH OFF", 800);
            return;
        }
        dmLoad(s_nodes[s_nodeScroll].nodeNum);
        s_dmScroll = 0;
        s_dmStick = true;
        s_layer = LY_DM;
        s_dmNew = 0;
    }
}

static void setKeys(const Keyboard_Class::KeysState& ks) {
    if (s_setRow >= SET_N) s_setRow = 0;
    if (keyHas(';')) {
        if (s_setRow > 0) s_setRow--;
        return;
    }
    if (keyHas('.')) {
        if (s_setRow + 1 < SET_N) s_setRow++;
        return;
    }
    // Shift+, and Shift+.  Plain comma and slash already moved the tab.
    if (keyHas('<')) { bumpSet(-1); return; }
    if (keyHas('>')) { bumpSet(1); return; }
    if (!ks.enter) return;
    if (s_setRow == 0 || s_setRow == 10) {
        bool on = (s_setRow == 0) ? Config::mesh().enabled : Config::mesh().rxBoost;
        bumpSet(on ? -1 : 1);
        return;
    }
    if (s_setRow >= 6 && s_setRow <= 9) openField();
}

void handleKeys() {
    if (App::windowHidden()) return;
    // Menu Enter is still down on the next frame. keyNewPress would treat
    // that held key as a fresh press and fall straight into CHAT.
    if (s_swallowEnter) {
        if (M5Cardputer.Keyboard.keysState().enter) return;
        s_swallowEnter = false;
    }
    static bool keyWas = false;
    if (!keyNewPress(keyWas)) return;
    auto ks = M5Cardputer.Keyboard.keysState();
    const bool esc = keyEsc();
    s_lastDraw = 0;

    // Fn alone cycles EN / RU / UA. A letter in the same press still types.
    if (ks.fn && ks.word.empty() && !ks.enter && !ks.del && !esc) {
        s_lang = (uint8_t)((s_lang + 1) % 3);
        return;
    }

    if (s_layer == LY_WRITE || s_layer == LY_FIELD) {
        typingKeys(ks, esc);
        return;
    }
    if (esc) {
        if (s_layer == LY_DM) { s_layer = LY_PAGE; s_tab = 1; return; }
        if (s_layer == LY_PAGE) { s_layer = LY_STRIP; return; }
        App::setMode(AppMode::MENU);
        return;
    }
    if (keyHas(',')) { moveTab(-1); return; }
    if (keyHas('/')) { moveTab(1); return; }
    if (s_layer == LY_STRIP) {
        if (ks.enter) s_layer = LY_PAGE;
        return;
    }
    if (s_layer == LY_DM || s_tab == 0) { chatKeys(ks, s_layer == LY_DM); return; }
    if (s_tab == 1) { peopleKeys(ks); return; }
    if (s_tab == 2) return;
    setKeys(ks);
}

const char* bottomHint() {
    static char b[48];
    const char* rest = ",/ TAB  ENT OPEN  ESC CLOSE";
    if (s_layer == LY_WRITE) rest = "FN LANG  ENT SEND  BKSP  ESC";
    else if (s_layer == LY_FIELD)
        rest = s_fieldAscii ? "ENT SAVE  BKSP  ESC" : "FN LANG  ENT SAVE  BKSP  ESC";
    else if (s_layer == LY_DM) rest = ",/ TAB  T WRITE  ;/. MOVE  ESC";
    else if (s_layer == LY_PAGE && s_tab == 0) rest = ",/ TAB  T WRITE  ;/. MOVE  ESC";
    else if (s_layer == LY_PAGE && s_tab == 1) {
        bool self = s_nodes && s_nodeN && s_nodeScroll < s_nodeN && s_nodes[s_nodeScroll].isSelf;
        rest = self ? ",/ TAB  ;/. MOVE  ESC BACK" : ",/ TAB  ENT CHAT  ;/. MOVE  ESC";
    } else if (s_layer == LY_PAGE && s_tab == 2) rest = ",/ TAB  ESC BACK";
    else if (s_layer == LY_PAGE) rest = ",/ TAB  <> CHG  ;/. MOVE  ENT";
    snprintf(b, sizeof(b), "%s  %s", langName(), rest);
    return b;
}

bool isComposing() { return s_layer == LY_WRITE || s_layer == LY_FIELD; }

} // namespace MeshPig
