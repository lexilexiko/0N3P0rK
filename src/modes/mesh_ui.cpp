// mesh_ui.cpp — MESH screen. Tabs, lists, settings rows, and keys.
// The modem is mesh_pig.cpp. Messages and the keymap are mesh_text.cpp.

#include "mesh_pig.h"
#include "mesh_priv.h"
#include "mesh_glyph.h"
#include "../audio/sfx.h"
#include "../core/app.h"
#include "../core/config.h"
#include "../storage/littlefs_ops.h"
#include "../ui/display.h"
#include "../ui/keys.h"

#include <M5Cardputer.h>
#include <stdlib.h>
#include <string.h>

// ════════════════════════════════════════════════════════════════════════════
// DRAW HELPERS
// ════════════════════════════════════════════════════════════════════════════
#define COL_BG      UiStyle::BG
#define COL_HDR     UiStyle::PANEL
#define COL_HDR2    UiStyle::BG
#define COL_SEL     UiStyle::PINK
#define COL_TEXT    UiStyle::TEXT
#define COL_DIM     UiStyle::DIM
#define COL_CYAN    UiStyle::CYAN
#define COL_GREEN   UiStyle::GREEN
#define COL_YELLOW  UiStyle::GOLD
#define COL_PINK    UiStyle::PINK
#define COL_RED     UiStyle::RED
#define LH          10
#define TAB_H       11
#define MESH_H      105
#define CHAT_BODY_X 14

constexpr uint8_t EMOJI_CACHE_CAP = 4;
constexpr uint32_t MAX_EMOJI_PNG_BYTES = 16 * 1024;
struct EmojiPng {
    uint32_t code;
    uint32_t stamp;
    uint32_t length;
    uint16_t width;
    uint16_t height;
    uint8_t* data;
    bool checked;
};
static EmojiPng s_emojiCache[EMOJI_CACHE_CAP] = {};
static uint32_t s_emojiCacheStamp = 0;

// Rows under the tab strip. The key line lives in the real bottom bar.
static int meshRows() {
    int rows = (MESH_H - TAB_H - 2) / LH;
    return rows < 1 ? 1 : rows;
}

// Chat letters: 1 small, 2 medium, 3 large. Medium is the usual size.
static int chatScale() {
    uint8_t s = Config::mesh().chatSize;
    if (s < 1 || s > 3) s = 2;
    return (int)s;
}

static const char* chatSizeName(uint8_t s) {
    if (s <= 1) return "S";
    if (s >= 3) return "L";
    return "M";
}

static void blitGlyph(M5Canvas& cv, int x, int y, const uint8_t* col, uint16_t fg) {
    for (int c = 0; c < 5; c++) {
        uint8_t bits = col[c];
        for (int r = 0; r < 7; r++) {
            if (bits & (uint8_t)(1u << (6 - r))) cv.drawPixel(x + c, y + r, fg);
        }
    }
}

static void blitGlyphScale(M5Canvas& cv, int x, int y, const uint8_t* col,
                           uint16_t fg, int s) {
    for (int c = 0; c < 5; c++) {
        uint8_t bits = col[c];
        for (int r = 0; r < 7; r++) {
            if (bits & (uint8_t)(1u << (6 - r)))
                cv.fillRect(x + c * s, y + r * s, s, s, fg);
        }
    }
}

static uint32_t pngReadBe32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void emojiCacheClear() {
    for (uint8_t i = 0; i < EMOJI_CACHE_CAP; i++) {
        free(s_emojiCache[i].data);
        memset(&s_emojiCache[i], 0, sizeof(s_emojiCache[i]));
    }
    s_emojiCacheStamp = 0;
}

static EmojiPng* emojiPng(uint32_t code) {
    if (!Storage::available()) return nullptr;
    for (uint8_t i = 0; i < EMOJI_CACHE_CAP; i++) {
        if (s_emojiCache[i].checked && s_emojiCache[i].code == code) {
            s_emojiCache[i].stamp = ++s_emojiCacheStamp;
            return &s_emojiCache[i];
        }
    }

    EmojiPng* entry = &s_emojiCache[0];
    for (uint8_t i = 0; i < EMOJI_CACHE_CAP; i++) {
        if (!s_emojiCache[i].checked) {
            entry = &s_emojiCache[i];
            break;
        }
        if (s_emojiCache[i].stamp < entry->stamp) entry = &s_emojiCache[i];
    }
    free(entry->data);
    memset(entry, 0, sizeof(*entry));
    entry->code = code;
    entry->stamp = ++s_emojiCacheStamp;
    entry->checked = true;

    char path[32];
    snprintf(path, sizeof(path), "/0N3P0rK/PigMesh/emoji/u%lX.png",
             (unsigned long)code);
    File file = SD.open(path, FILE_READ);
    if (!file) return entry;

    size_t size = file.size();
    if (size < 24 || size > MAX_EMOJI_PNG_BYTES) {
        file.close();
        Serial.printf("[MESH] invalid emoji PNG size for U+%lX\n", (unsigned long)code);
        return entry;
    }

    uint8_t* data = (uint8_t*)malloc(size);
    if (!data) {
        file.close();
        Serial.printf("[MESH] emoji PNG allocation failed for U+%lX\n", (unsigned long)code);
        return entry;
    }
    size_t read = file.read(data, size);
    file.close();

    static const uint8_t pngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint32_t width = read == size && memcmp(data, pngSignature, sizeof(pngSignature)) == 0
                   ? pngReadBe32(data + 16) : 0;
    uint32_t height = width ? pngReadBe32(data + 20) : 0;
    if (!width || !height || width > 256 || height > 256) {
        free(data);
        Serial.printf("[MESH] invalid emoji PNG for U+%lX\n", (unsigned long)code);
        return entry;
    }

    entry->data = data;
    entry->length = (uint32_t)size;
    entry->width = (uint16_t)width;
    entry->height = (uint16_t)height;
    return entry;
}

static int32_t drawEmojiPng(lgfx::LGFXBase* gfx, int32_t x, int32_t y,
                            uint32_t code, int32_t fontHeight) {
    if (!gfx || fontHeight <= 0) return 0;
    EmojiPng* image = emojiPng(code);
    const int32_t cellWidth = (fontHeight * 6 + 4) / 8;
    if (image && image->data) {
        const int32_t maxWidth = fontHeight * 5 / 8;
        const int32_t maxHeight = fontHeight * 7 / 8;
        float sx = (float)maxWidth / image->width;
        float sy = (float)maxHeight / image->height;
        float scale = sx < sy ? sx : sy;
        int32_t drawWidth = (int32_t)(image->width * scale + 0.5f);
        int32_t drawHeight = (int32_t)(image->height * scale + 0.5f);
        if (gfx->drawPng(image->data, image->length,
                         x + (cellWidth - drawWidth) / 2,
                         y + (fontHeight - drawHeight) / 2,
                         0, 0, 0, 0, scale, scale)) {
            return cellWidth;
        }
        free(image->data);
        image->data = nullptr;
        image->length = 0;
        image->width = 0;
        image->height = 0;
        Serial.printf("[MESH] failed to decode emoji PNG U+%lX\n", (unsigned long)code);
    }

    const uint8_t* glyph = meshGlyph(code);
    if (!glyph) return 0;
    int scale = (fontHeight + 4) / 8;
    if (scale < 1) scale = 1;
    for (int c = 0; c < 5; c++) {
        for (int r = 0; r < 7; r++) {
            if (glyph[c] & (uint8_t)(1u << (6 - r)))
                gfx->fillRect(x + c * scale, y + r * scale, scale, scale, COL_TEXT);
        }
    }
    return cellWidth;
}

// Scale 1 is Font0 6x8. Scale 2 is 12x16. A 5x7 glyph doubles to 10x14.
static void drawCpScaled(M5Canvas& cv, int x, int y, uint32_t cp,
                         uint16_t fg, int scale) {
    if (scale < 1) scale = 1;
    if (cp < 0x80) {
        cv.setTextSize((uint8_t)scale);
        cv.setTextColor(fg, (uint16_t)COL_BG);
        cv.setCursor(x, y);
        char ch[2] = { (char)cp, 0 };
        cv.print(ch);
        cv.setTextSize(1);
        return;
    }
    if (cp >= 0x2300 && drawEmojiPng(&cv, x, y, cp, 8 * scale) > 0) return;
    const uint8_t* g = meshGlyph(cp);
    if (g) {
        blitGlyphScale(cv, x, y + (scale > 1 ? 1 : 0), g, fg, scale);
        return;
    }
    cv.setTextSize((uint8_t)scale);
    cv.setTextColor(fg, (uint16_t)COL_BG);
    cv.setCursor(x, y);
    cv.print("?");
    cv.setTextSize(1);
}

static int drawUtf8(M5Canvas& cv, int x, int y, int xMax, const char* s,
                    uint16_t fg, uint16_t bg, int scale = 1) {
    if (!s) return x;
    if (scale < 1) scale = 1;
    int pitch = 6 * scale;
    while (*s && x + pitch <= xMax) {
        uint32_t cp;
        int n = utf8Next(s, cp);
        if (n <= 0) break;
        s += n;
        if (utf8IsEmojiSequencePart(cp)) continue;
        if (scale >= 2) drawCpScaled(cv, x, y, cp, fg, scale);
        else if (cp < 0x80) {
            cv.setTextColor(fg, bg);
            cv.setCursor(x, y);
            char ch[2] = { (char)cp, 0 };
            cv.print(ch);
        } else {
            int32_t emojiWidth = cp >= 0x2300
                               ? drawEmojiPng(&cv, x, y, cp, 8 * scale) : 0;
            const uint8_t* g = meshGlyph(cp);
            if (emojiWidth > 0) {
                x += emojiWidth;
                continue;
            } else if (g) blitGlyph(cv, x, y, g, fg);
            else {
                cv.setTextColor(fg, bg);
                cv.setCursor(x, y);
                cv.print("?");
            }
        }
        x += pitch;
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
static uint16_t s_fieldMax = 47;
static bool     s_holdUi = false;
static bool     s_swallowEnter = false;
static uint8_t  s_lang = 0;
static uint8_t  s_nodeScroll = 0;
static uint16_t s_chatScroll = 0;
static uint16_t s_dmScroll = 0;
static bool     s_chatStick = true;
static bool     s_dmStick = true;
static char     s_cmpBuf[MAX_TEXT_BYTES + 1] = {};
static uint16_t s_cmpLen = 0;
static uint32_t s_lastDraw = 0;
static bool     s_chatAlert = false;
static uint32_t s_dmUnread[MAX_NODES] = {};
static uint8_t  s_dmUnreadN = 0;

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
    s_cmpLen = (uint16_t)(s_cmpLen + n);
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

static int chatLineHeight() {
    int scale = chatScale();
    return scale < 1 ? 8 : scale * 8;
}

static int chatWrapCols(int width, int scale) {
    int cols = (width - CHAT_BODY_X - 2) / (6 * scale);
    return cols < 1 ? 1 : cols;
}

static uint16_t wrappedTextLines(const char* text, int maxCols) {
    if (maxCols < 1) maxCols = 1;
    uint16_t lines = 1;
    int cols = 0;
    for (int i = 0; text && text[i];) {
        uint32_t cp;
        int n = utf8Next(text + i, cp);
        if (n <= 0) break;
        i += n;
        if (cp == '\n') {
            lines++;
            cols = 0;
        } else if (!utf8IsEmojiSequencePart(cp)) {
            if (cols >= maxCols) {
                lines++;
                cols = 0;
            }
            cols++;
        }
    }
    return lines;
}

static bool wrappedTextLine(const char* text, int maxCols, uint16_t target,
                            char* out, size_t outCap) {
    if (!text || !out || outCap == 0) return false;
    if (maxCols < 1) maxCols = 1;
    uint16_t line = 0;
    int cols = 0;
    size_t start = 0;
    size_t i = 0;
    while (text[i]) {
        uint32_t cp;
        int n = utf8Next(text + i, cp);
        if (n <= 0) break;
        if (cp == '\n') {
            if (line == target) {
                size_t len = i - start;
                if (len >= outCap) len = outCap - 1;
                memcpy(out, text + start, len);
                out[len] = 0;
                return true;
            }
            line++;
            i += n;
            start = i;
            cols = 0;
            continue;
        }
        if (!utf8IsEmojiSequencePart(cp) && cols >= maxCols) {
            if (line == target) {
                size_t len = i - start;
                if (len >= outCap) len = outCap - 1;
                memcpy(out, text + start, len);
                out[len] = 0;
                return true;
            }
            line++;
            start = i;
            cols = 0;
        }
        i += n;
        if (!utf8IsEmojiSequencePart(cp)) cols++;
    }
    if (line != target) return false;
    size_t len = i - start;
    if (len >= outCap) len = outCap - 1;
    memcpy(out, text + start, len);
    out[len] = 0;
    return true;
}

static uint16_t chatTotalLines(bool dm, int width, int scale) {
    uint8_t count = dm ? s_dmN : s_chatN;
    int cols = chatWrapCols(width, scale);
    uint32_t total = 0;
    for (uint8_t i = 0; i < count; i++) {
        Msg* m = dm ? dmGet(i) : chatGet(i);
        if (m) total += 1u + wrappedTextLines(m->text, cols);
    }
    return total > 65535u ? 65535u : (uint16_t)total;
}

static bool dmIsUnread(uint32_t node) {
    for (uint8_t i = 0; i < s_dmUnreadN; i++)
        if (s_dmUnread[i] == node) return true;
    return false;
}

static bool anyDmUnread() {
    if (!s_nodes) return false;
    for (uint8_t i = 0; i < s_dmUnreadN; i++) {
        for (uint8_t j = 0; j < s_nodeN; j++) {
            if (s_nodes[j].nodeNum == s_dmUnread[i] && !s_nodes[j].isSelf) return true;
        }
    }
    return false;
}

static void markDmUnread(uint32_t node) {
    if (!node || node == BROADCAST_ID || dmIsUnread(node)) return;
    if (s_dmUnreadN >= MAX_NODES) return;
    s_dmUnread[s_dmUnreadN++] = node;
}

static void clearDmUnread(uint32_t node) {
    for (uint8_t i = 0; i < s_dmUnreadN; i++) {
        if (s_dmUnread[i] != node) continue;
        s_dmUnread[i] = s_dmUnread[s_dmUnreadN - 1];
        s_dmUnreadN--;
        return;
    }
}

static bool meshScreenUp() {
    return App::mode() == AppMode::MESH && !App::windowHidden();
}

static bool viewingChat() {
    if (!meshScreenUp()) return false;
    if (s_layer == LY_PAGE && s_tab == 0) return true;
    return s_layer == LY_WRITE && s_writeFrom != LY_DM && s_tab == 0;
}

static bool viewingDm(uint32_t peer) {
    if (!meshScreenUp() || s_dmNode != peer) return false;
    if (s_layer == LY_DM) return true;
    return s_layer == LY_WRITE && s_writeFrom == LY_DM;
}

static void drawSmsMark(M5Canvas& cv, int x, int y, uint16_t fg) {
    cv.drawRect(x, y, 7, 5, fg);
    cv.drawLine(x, y, x + 3, y + 2, fg);
    cv.drawLine(x + 6, y, x + 3, y + 2, fg);
}

void meshIncoming(bool dm, uint32_t peer) {
    bool seen = dm ? viewingDm(peer) : viewingChat();
    if (seen) return;
    if (dm) markDmUnread(peer);
    else s_chatAlert = true;
    SFX::play(SFX::CALL_RING);
}

namespace MeshPig {

void uiArmOpen() { s_swallowEnter = true; }
void uiStick() { s_chatStick = true; s_dmStick = true; }
void uiEmojiCacheClear() { emojiCacheClear(); }

void uiReset() {
    if (s_holdUi) return;
    s_tab = 0;
    s_layer = LY_STRIP;
    s_setRow = 0;
    s_setScroll = 0;
    s_chatAlert = false;
    s_dmUnreadN = 0;
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
        if (s_nodes[i].lastSeen &&
            now - s_nodes[i].lastSeen < 8UL * 60UL * 1000UL) n++;
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
    "FREQ", "CHANNEL", "NAME", "PSK", "RX BOOST", "FONT"
};
static const uint8_t SET_N = 12;

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
    if (s_layer == LY_PAGE && s_tab == 0) s_chatAlert = false;
}

static void radioRestart() {
    if (!Config::mesh().enabled) return;
    s_holdUi = true;
    if (isRunning()) stop();
    start();
    s_holdUi = false;
}

static bool validRadioPlan(const MeshConfig& mc) {
    float freq = 0, bw = 0;
    uint8_t sf = 0, cr = 0;
    int8_t power = 0;
    return planRadio(mc, freq, bw, sf, cr, power);
}

static void drawTabs(M5Canvas& cv, int w) {
    static const char* tabs[] = { "CHAT", "PEOPLE", "RADIO", "SET" };
    const int tw = w / 4;
    cv.fillRoundRect(2, 0, w - 4, TAB_H, 4, COL_HDR2);
    for (int i = 0; i < 4; i++) {
        int x = i * tw;
        bool on = (i == (int)s_tab);
        uint16_t bg = on ? (uint16_t)COL_PINK : (uint16_t)COL_HDR2;
        if (on) cv.fillRoundRect(x + 2, 1, tw - 4, TAB_H - 2, 3, bg);
        bool alert = (i == 0 && s_chatAlert) || (i == 1 && anyDmUnread());
        uint16_t fg = on ? (uint16_t)COL_BG :
                      (alert ? (uint16_t)COL_YELLOW : (uint16_t)COL_DIM);
        cv.setTextColor(fg, bg);
        int lw = (int)strlen(tabs[i]) * 6;
        cv.setCursor(x + (tw - lw) / 2, 1);
        cv.print(tabs[i]);
        if (on) cv.fillRoundRect(x + tw / 2 - 5, TAB_H - 2, 10, 2, 1, COL_YELLOW);
        if (i == 0 && !s_chatStick) cv.fillCircle(x + tw - 7, 5, 2, COL_YELLOW);
        if (i == 1 && anyDmUnread()) drawSmsMark(cv, x + tw - 9, 3, COL_YELLOW);
    }
    uint16_t pip = s_radioOk ? (s_rs == RS_TX ? (uint16_t)COL_YELLOW : (uint16_t)COL_GREEN)
                             : (uint16_t)COL_RED;
    cv.fillCircle(3 * tw - 7, 5, 2, pip);
}

static void drawNodeRow(M5Canvas& cv, int y, int w, const MeshNode& n, bool sel) {
    uint16_t bg = COL_BG;
    if (sel) {
        cv.fillRoundRect(3, y - 1, w - 6, LH, 3, COL_SEL);
        cv.fillRoundRect(5, y + 1, 2, LH - 3, 1, COL_YELLOW);
        bg = COL_SEL;
    }
    bool fresh = n.isSelf || (n.lastSeen &&
                 (uint32_t)millis() - n.lastSeen < 8UL * 60UL * 1000UL);
    uint16_t nameCol = sel ? (uint16_t)COL_BG :
                       (n.isSelf ? (uint16_t)COL_PINK :
                        (fresh ? (uint16_t)COL_TEXT : (uint16_t)COL_DIM));
    if (n.isSelf) drawUtf8(cv, 4, y, 40, "you", nameCol, bg);
    else if (n.shortName[0]) drawUtf8(cv, 4, y, 40, n.shortName, nameCol, bg);
    else {
        char id[8];
        snprintf(id, sizeof(id), "!%04lx", (unsigned long)(n.nodeNum & 0xFFFF));
        drawUtf8(cv, 4, y, 40, id, nameCol, bg);
    }
    cv.setTextColor(sel ? (uint16_t)COL_BG :
                    (n.isSelf ? (uint16_t)COL_DIM : sigCol(n.rssi)), bg);
    cv.setCursor(42, y);
    if (n.isSelf) cv.print("  --");
    else cv.printf("%4d", n.rssi);
    char age[8];
    ageStr(n.lastSeen, age, sizeof(age));
    cv.setTextColor(sel ? (uint16_t)COL_BG : (uint16_t)COL_DIM, bg);
    cv.setCursor(72, y);
    cv.printf("%4s", age);
    bool sms = !n.isSelf && dmIsUnread(n.nodeNum);
    int nameMax = sms ? w - 22 : w - 2;
    if (n.longName[0]) drawUtf8(cv, 104, y, nameMax, n.longName, nameCol, bg);
    if (sms) {
        cv.fillRoundRect(w - 24, y - 1, 21, LH, 3, COL_YELLOW);
        cv.setTextColor(COL_BG, COL_YELLOW);
        cv.setCursor(w - 22, y);
        cv.print("SMS");
    }
}

static void drawChat(M5Canvas& cv, int w, int h, bool dm) {
    int y = TAB_H + 2;
    int yMax = h - 2;
    uint16_t& scroll = dm ? s_dmScroll : s_chatScroll;
    bool& stick = dm ? s_dmStick : s_chatStick;
    uint8_t nmsg = dm ? s_dmN : s_chatN;

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
            putLine(cv, y, yMax, w, "ENT writes to this person.", COL_DIM);
        } else {
            y = putLine(cv, y, yMax, w, "Listening for this channel.", COL_TEXT);
            y = putLine(cv, y, yMax, w, "Other people's messages", COL_DIM);
            y = putLine(cv, y, yMax, w, "show up in this chat.", COL_DIM);
            y = putLine(cv, y, yMax, w, "DMs light SMS on PEOPLE.", COL_DIM);
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
    int scale = chatScale();
    int lineH = chatLineHeight();
    int visible = (yMax - y) / lineH;
    if (visible < 1) visible = 1;
    uint16_t total = chatTotalLines(dm, w, scale);
    uint16_t maxTop = total > (uint16_t)visible ? (uint16_t)(total - visible) : 0;
    if (scroll > maxTop) scroll = maxTop;
    if (stick) scroll = maxTop;

    uint16_t logical = 0;
    int screenLine = 0;
    int maxCols = chatWrapCols(w, scale);
    for (uint8_t i = 0; i < nmsg && screenLine < visible; i++) {
        Msg* m = dm ? dmGet(i) : chatGet(i);
        if (!m) continue;
        const char* who = m->fromName[0] ? m->fromName : "?";
        const char* mark = "";
        uint16_t headCol = COL_TEXT;
        if (m->sent) {
            switch (m->delivery) {
                case TX_QUEUED: mark = " Q"; headCol = COL_CYAN; break;
                case TX_WAITING: mark = " .."; headCol = COL_YELLOW; break;
                case TX_SENT: mark = " TX"; headCol = COL_DIM; break;
                case TX_ACKED: mark = " OK"; headCol = COL_GREEN; break;
                case TX_FAILED: mark = " X"; headCol = COL_RED; break;
                default: mark = " ?"; headCol = COL_DIM; break;
            }
        }
        char header[24];
        snprintf(header, sizeof(header), "%s%c%s", who,
                 m->to == BROADCAST_ID ? ':' : '>', mark);
        uint16_t bodyLines = wrappedTextLines(m->text, maxCols);
        uint16_t blockLines = bodyLines + 1;
        uint16_t visibleStart = logical > scroll ? logical : scroll;
        uint16_t visibleEnd = logical + blockLines;
        if (visibleEnd > scroll + visible) visibleEnd = scroll + visible;
        if (visibleStart < visibleEnd) {
            int cardY = y + (visibleStart - scroll) * lineH;
            int cardH = (visibleEnd - visibleStart) * lineH - 1;
            uint16_t accent = m->sent ? (uint16_t)COL_CYAN : (uint16_t)COL_PINK;
            cv.fillRoundRect(3, cardY, w - 6, cardH, 4, COL_HDR);
            if (cardH > 4)
                cv.fillRoundRect(5, cardY + 2, 2, cardH - 4, 1, accent);
        }
        if (logical >= scroll && screenLine < visible)
            drawUtf8(cv, 8, y + screenLine++ * lineH, w - 10,
                     header, headCol, COL_HDR);
        logical++;

        for (uint16_t line = 0; line < bodyLines; line++, logical++) {
            if (logical < scroll || screenLine >= visible) continue;
            char body[MAX_TEXT_BYTES + 1];
            if (!wrappedTextLine(m->text, maxCols, line, body, sizeof(body))) continue;
            drawUtf8(cv, CHAT_BODY_X, y + screenLine++ * lineH, w - 2,
                     body, m->sent ? (uint16_t)COL_CYAN : (uint16_t)COL_TEXT,
                     COL_HDR, scale);
        }
    }
}

static void drawPeople(M5Canvas& cv, int w, int h) {
    int y = TAB_H + 2;
    int rows = meshRows();
    if (rows < 2) rows = 2;
    cv.fillRoundRect(3, y - 1, w - 6, LH, 3, COL_HDR);
    cv.setTextColor(COL_YELLOW, COL_HDR);
    cv.setCursor(8, y);
    cv.print("PEOPLE");
    cv.setTextColor(countOnline() ? (uint16_t)COL_GREEN : (uint16_t)COL_DIM, COL_HDR);
    cv.setCursor(68, y);
    if (!s_nodes || !heardOther()) cv.print("nobody");
    else cv.printf("%u online", countOnline());
    cv.setTextColor(COL_DIM, COL_HDR);
    cv.setCursor(w - 55, y);
    cv.print("dBm age");
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
    const char* status = "LORA OFF";
    uint16_t statusCol = COL_DIM;
    if (!mc.enabled) { status = "MESH OFF"; statusCol = COL_RED; }
    else if (!s_radioOk && s_radioError[0]) { status = s_radioError; statusCol = COL_RED; }
    else if (s_radioOk && s_rs == RS_TX) { status = "LORA TX"; statusCol = COL_YELLOW; }
    else if (s_radioOk) { status = "LORA READY"; statusCol = COL_GREEN; }
    cv.fillRoundRect(4, y - 1, w - 8, LH, 3, COL_HDR);
    cv.fillRoundRect(7, y + 2, 3, LH - 5, 1, statusCol);
    drawUtf8(cv, 14, y, w - 12, status, statusCol, COL_HDR);
    y += LH;
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
        case 10: snprintf(out, cap, "%s", mc.rxBoost ? "ON" : "OFF"); break;
        default: snprintf(out, cap, "%s", chatSizeName(mc.chatSize)); break;
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
            cv.fillRoundRect(3, y - 1, w - 6, LH, 3, COL_SEL);
            cv.fillRoundRect(5, y + 1, 2, LH - 3, 1, COL_YELLOW);
        }
        char val[24];
        setValue(row, val, sizeof(val));
        uint16_t vc = COL_TEXT;
        if ((row == 0 || row == 10) && val[1] == 'N') vc = COL_GREEN;
        if ((row == 0 || row == 10) && val[1] == 'F') vc = COL_RED;
        drawUtf8(cv, 8, y, 80, SET_LBL[row],
                 sel ? (uint16_t)COL_BG : (uint16_t)COL_TEXT, bg);
        drawUtf8(cv, 84, y, w - 8, val, sel ? (uint16_t)COL_BG : vc, bg);
    }
    (void)h;
}

static const char* KEY_ROWS[] = { "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./" };

// Second mark on a Cardputer key: the letter Shift prints.
static char shiftAscii(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    switch (c) {
    case ',': return '<';
    case '.': return '>';
    case '/': return '?';
    case ';': return ':';
    case '\'': return '"';
    case '[': return '{';
    case ']': return '}';
    case '\\': return '|';
    default: return c;
    }
}

// Key legend pinned under the text. Letters follow Shift: small, or capitals.
static void drawShiftLegend(M5Canvas& cv, int w, int h) {
    const int pitch = 12;
    const int rowH = 16;
    const int keyTop = h - 3 * rowH;
    bool upper = M5Cardputer.Keyboard.keysState().shift ||
                 M5Cardputer.Keyboard.capslocked();
    cv.setTextSize(1);
    cv.setTextColor(upper ? (uint16_t)COL_YELLOW : (uint16_t)COL_DIM, (uint16_t)COL_BG);
    cv.setCursor(4, keyTop - 9);
    cv.print(upper ? "SHIFT ACTIVE" : "SHIFT");
    for (int r = 0; r < 3; r++) {
        int n = (int)strlen(KEY_ROWS[r]);
        int kx = (w - n * pitch) / 2;
        if (kx < 2) kx = 2;
        int ky = keyTop + r * rowH;
        for (const char* p = KEY_ROWS[r]; *p; p++) {
            char src = upper ? shiftAscii(*p) : *p;
            cv.fillRoundRect(kx, ky, pitch - 1, rowH - 2, 3, COL_HDR);
            cv.drawRoundRect(kx, ky, pitch - 1, rowH - 2, 3, COL_HDR2);
            drawCpScaled(cv, kx + 1, ky, mapLay(src, s_lang),
                         upper ? COL_YELLOW : COL_TEXT, 2);
            kx += pitch;
        }
    }
}

static void drawWriterBody(M5Canvas& cv, int w, int y0, int pitch, int lineH,
                           int yMax) {
    int x0 = 4;
    int xMax = w - 4;
    int maxC = (xMax - x0) / pitch;
    if (maxC < 1) maxC = 1;
    int vis = (yMax - y0) / lineH;
    if (vis < 1) vis = 1;
    int nch = utf8Cols(s_cmpBuf, s_cmpLen);
    int curLine = nch / maxC;
    int startLine = curLine - (vis - 1);
    if (startLine < 0) startLine = 0;
    int col = 0;
    int line = 0;
    for (int i = 0; i < s_cmpLen && s_cmpBuf[i];) {
        uint32_t cp;
        int n = utf8Next(s_cmpBuf + i, cp);
        if (n <= 0) break;
        i += n;
        if (col >= maxC) { col = 0; line++; }
        if (line >= startLine) {
            int dy = y0 + (line - startLine) * lineH;
            if (dy + lineH <= yMax)
                drawCpScaled(cv, x0 + col * pitch, dy, cp, COL_TEXT, pitch >= 12 ? 2 : 1);
        }
        col++;
    }
    if (((uint32_t)millis() / 400u) & 1u) {
        int ccol = nch % maxC;
        int cline = nch / maxC;
        if (cline >= startLine) {
            int cy = y0 + (cline - startLine) * lineH;
            if (cy + lineH <= yMax)
                cv.drawFastVLine(x0 + ccol * pitch, cy, lineH > 8 ? lineH - 2 : 8, COL_CYAN);
        }
    }
}

static void drawWriter(M5Canvas& cv, int w, int h, const char* title) {
    cv.fillRoundRect(3, 0, w - 6, TAB_H, 3, COL_HDR);
    cv.fillRoundRect(5, 1, 21, 9, 2, COL_PINK);
    cv.setTextColor(COL_BG, (uint16_t)COL_PINK);
    cv.setCursor(8, 1);
    cv.print(langName());
    if (s_fieldAscii) {
        cv.setTextColor(COL_YELLOW, (uint16_t)COL_HDR);
        cv.setCursor(30, 1);
        cv.print("ASCII");
    }
    int titleX = s_fieldAscii ? 62 : 26;
    drawUtf8(cv, titleX, 1, w - 34, title ? title : "", COL_TEXT, COL_HDR);
    char cnt[8];
    snprintf(cnt, sizeof(cnt), "%d", utf8Cols(s_cmpBuf, s_cmpLen));
    int countW = 6 * (int)strlen(cnt) + 4;
    cv.fillRoundRect(w - countW - 3, 1, countW, 9, 2, COL_HDR2);
    cv.setTextColor(COL_YELLOW, (uint16_t)COL_HDR2);
    cv.setCursor(w - 6 * (int)strlen(cnt) - 1, 1);
    cv.print(cnt);

    // Frequency and PSK stay one size and ASCII. No Shift map.
    if (s_fieldAscii) {
        cv.drawRoundRect(2, TAB_H + 1, w - 4, h - TAB_H - 3, 4, COL_PINK);
        drawWriterBody(cv, w, TAB_H + 4, 6, LH, h - 4);
        return;
    }

    // Two bigger text lines sit above the Shift map, so the map never covers them.
    const int keyTop = h - 3 * 16;
    const int boxBottom = keyTop - 11;
    cv.drawRoundRect(2, TAB_H + 1, w - 4, boxBottom - (TAB_H + 1) + 1,
                     4, COL_PINK);
    drawWriterBody(cv, w, TAB_H + 3, 12, 16, boxBottom);
    drawShiftLegend(cv, w, h);
}

static void drawDmHdr(M5Canvas& cv, int w) {
    cv.fillRoundRect(3, 0, w - 6, TAB_H, 3, COL_HDR);
    cv.fillRoundRect(5, 1, 18, 9, 2, COL_PINK);
    const char* nm = "?";
    if (s_nodes) {
        for (uint8_t i = 0; i < s_nodeN; i++) {
            if (s_nodes[i].nodeNum == s_dmNode && s_nodes[i].shortName[0]) {
                nm = s_nodes[i].shortName;
                break;
            }
        }
    }
    cv.setTextColor(COL_BG, (uint16_t)COL_PINK);
    cv.setCursor(8, 1);
    cv.print("DM");
    drawUtf8(cv, 27, 1, w - 6, nm, COL_TEXT, COL_HDR);
}

void draw(M5Canvas& canvas) {
    if (viewingChat()) s_chatAlert = false;
    if (meshScreenUp() && s_layer == LY_DM && s_dmNode) clearDmUnread(s_dmNode);
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
    canvas.setEmojiCallback(drawEmojiPng);

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
    s_fieldMax = MAX_TEXT_BYTES;
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
        if (!s_cmpBuf[0] || strcmp(s_cmpBuf, "0") == 0) {
            mc.freqOverride = false;
            restart = true;
            Display::showToast("FREQ AUTO", 800);
        } else {
            float fMHz = atof(s_cmpBuf);
            if (fMHz < 137.0f || fMHz > 960.0f) {
                Display::showToast("BAD FREQ", 800);
                return;
            }
            MeshConfig candidate = mc;
            candidate.freqKHz = (uint32_t)(fMHz * 1000.0f + 0.5f);
            candidate.freqOverride = true;
            if (!validRadioPlan(candidate)) {
                Display::showToast("OUT OF REGION", 1000);
                return;
            }
            mc.freqKHz = candidate.freqKHz;
            mc.freqOverride = true;
            restart = true;
            Display::showToast("FREQ SET", 800);
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
            if (mc.freqOverride && !validRadioPlan(mc)) {
                mc.freqOverride = false;
                Display::showToast("FREQ AUTO", 800);
            }
            Config::save();
            radioRestart();
            break;
        }
        case 2: {
            int v = (int)mc.modem + dir;
            if (v < 0 || v > 9) break;
            mc.modem = (uint8_t)v;
            if (mc.freqOverride && !validRadioPlan(mc)) {
                mc.freqOverride = false;
                Display::showToast("FREQ AUTO", 800);
            }
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
        case 11: {
            int v = (int)mc.chatSize + dir;
            if (v < 1) v = 1;
            if (v > 3) v = 3;
            if (v == (int)mc.chatSize) break;
            mc.chatSize = (uint8_t)v;
            Config::save();
            break;
        }
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
    if (ks.enter) { openWrite(); return; }
    int dir = 0;
    if (keyHas(';')) dir = -1;
    else if (keyHas('.')) dir = 1;
    if (!dir) return;
    uint16_t& scroll = dm ? s_dmScroll : s_chatScroll;
    bool& stick = dm ? s_dmStick : s_chatStick;
    int scale = chatScale();
    int lineH = chatLineHeight();
    int visible = (MESH_H - (TAB_H + 2) - 2) / lineH;
    if (visible < 1) visible = 1;
    uint16_t total = chatTotalLines(dm, 240, scale);
    uint16_t maxTop = total > (uint16_t)visible ? (uint16_t)(total - visible) : 0;
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
    } else if (ks.del && !s_nodes[s_nodeScroll].isSelf) {
        uint32_t num = s_nodes[s_nodeScroll].nodeNum;
        clearDmUnread(num);
        meshDropNode(s_nodeScroll);
        if (s_nodeN && s_nodeScroll >= s_nodeN) s_nodeScroll = (uint8_t)(s_nodeN - 1);
        Display::showToast("DELETED", 700);
        return;
    } else if (ks.enter && !s_nodes[s_nodeScroll].isSelf) {
        if (!s_run || !s_radioOk) {
            Display::showToast(Config::mesh().enabled ? "NO RADIO" : "MESH OFF", 800);
            return;
        }
        clearDmUnread(s_nodes[s_nodeScroll].nodeNum);
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
        if (ks.enter) {
            s_layer = LY_PAGE;
            if (s_tab == 0) s_chatAlert = false;
        }
        return;
    }
    if (s_layer == LY_DM || s_tab == 0) {
        if (s_tab == 0 && s_layer == LY_PAGE) s_chatAlert = false;
        chatKeys(ks, s_layer == LY_DM);
        return;
    }
    if (s_tab == 1) { peopleKeys(ks); return; }
    if (s_tab == 2) return;
    setKeys(ks);
}

const char* bottomHint() {
    static char b[48];
    const char* rest = "</> TAB  ENT OPEN  ESC BACK";
    if (s_layer == LY_WRITE) rest = "FN LANG  ENT SEND  BKSP  ESC";
    else if (s_layer == LY_FIELD)
        rest = s_fieldAscii ? "ENT SAVE  BKSP  ESC" : "FN LANG  ENT SAVE  BKSP  ESC";
    else if (s_layer == LY_DM || (s_layer == LY_PAGE && s_tab == 0))
        rest = "</> TAB  ^/v MOVE  ENT WRITE  ESC";
    else if (s_layer == LY_PAGE && s_tab == 1) {
        bool self = s_nodes && s_nodeN && s_nodeScroll < s_nodeN && s_nodes[s_nodeScroll].isSelf;
        rest = self ? "</> TAB  ^/v MOVE  ESC BACK"
                    : "</> ^/v ENT CHAT BKSP DEL ESC";
    } else if (s_layer == LY_PAGE && s_tab == 2) rest = "</> TAB  ESC BACK";
    else if (s_layer == LY_PAGE) rest = "</> TAB  ^/v MOVE  <> CHG  ENT";
    snprintf(b, sizeof(b), "%s  %s", langName(), rest);
    return b;
}

bool isComposing() { return s_layer == LY_WRITE || s_layer == LY_FIELD; }

bool grabBackspace() {
    if (s_layer != LY_PAGE || s_tab != 1) return false;
    if (!s_nodes || !s_nodeN || s_nodeScroll >= s_nodeN) return false;
    return !s_nodes[s_nodeScroll].isSelf;
}

} // namespace MeshPig
