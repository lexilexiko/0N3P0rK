// Cards table — large table, stand near center, G to play (jump never starts).
#include "cards_table.h"
#include "avatar.h"
#include "mood.h"
#include "../core/xp.h"
#include "../audio/sfx.h"
#include "../ui/keys.h"
#include <M5Cardputer.h>
#include <stdlib.h>

namespace CardsTable {

static constexpr int16_t GROUND_Y = 106;
// Acceptable distance from table center (px). Was 6 — too tight, G never fired.
static constexpr int CENTER_PX = 20;
static int16_t s_worldX = 200;
static int16_t s_scroll = 0;
static uint32_t s_cool = 0;
static uint32_t s_hintMs = 0;
static bool s_ready = false;
static bool s_onCenter = false;
static bool s_active = false;
static bool s_keyLatch = false;
static bool s_gWas = false;

bool unlocked() {
    return XP::getLevel() >= 45;
}

static int16_t screenX() {
    int16_t x = (int16_t)(s_worldX + s_scroll);
    while (x > 280) x = (int16_t)(x - 300);
    while (x < -40) x = (int16_t)(x + 300);
    return x;
}

bool atCenter() { return s_onCenter; }
bool isActive() { return s_active; }

void close() {
    s_active = false;
    s_keyLatch = false;
    s_gWas = false;
}

void begin() {
    s_worldX = 200;
    s_scroll = 0;
    s_cool = 0;
    s_hintMs = 0;
    s_ready = true;
    s_onCenter = false;
    s_active = false;
    s_keyLatch = false;
    s_gWas = false;
}

void scroll(int8_t dx) {
    if (!unlocked()) return;
    if (s_active) return;
    s_scroll = (int16_t)(s_scroll + dx);
}

void update() {
    if (!unlocked()) {
        s_onCenter = false;
        if (s_active) close();
        return;
    }
    if (!s_ready) begin();

    // --- Overlay open: ` / ENT exit ---
    if (s_active) {
        if (!keyNewPress(s_keyLatch)) return;
        if (keyEsc()) {
            close();
            Mood::say("later");
            return;
        }
        auto st = M5Cardputer.Keyboard.keysState();
        if (st.enter) {
            close();
            return;
        }
        return;
    }

    // Jump does NOT count as "at table" for play — avoids tree-smash conflict
    if (Avatar::isJumping()) {
        s_onCenter = false;
        s_gWas = false;
        return;
    }

    int16_t sx = screenX();
    // Pig feet / body reference (same +20 as original table hitbox)
    int pig = Avatar::getCurrentX() + 20;
    int dist = abs(pig - (int)sx);
    s_onCenter = (dist <= CENTER_PX);

    // G edge detect every frame while on farm (do not require isChange)
    bool gNow = M5Cardputer.Keyboard.isKeyPressed('g') ||
                M5Cardputer.Keyboard.isKeyPressed('G');
    bool gEdge = gNow && !s_gWas;
    s_gWas = gNow;

    if (!s_onCenter) return;

    uint32_t now = millis();
    if (now - s_hintMs > 2800) {
        s_hintMs = now;
        Mood::say("G = PLAY");
    }

    if (!gEdge) return;
    if (now < s_cool) return;

    s_cool = now + 1200;
    s_active = true;
    s_keyLatch = false;
    Mood::say("CARDS NOT READY");
    SFX::play(SFX::MENU_CLICK);
    Avatar::setState(AvatarState::SAD);
}

void draw(M5Canvas& canvas, int16_t yOffset) {
    if (!unlocked()) return;

    int16_t x = screenX();
    int16_t y = GROUND_Y + yOffset;

    // --- Larger table (was ~26px; now ~44px top) ---
    // legs (thicker, farther apart)
    canvas.fillRect(x - 16, y - 14, 4, 14, 0x8200);
    canvas.fillRect(x + 13, y - 14, 4, 14, 0x8200);
    // leg tips
    canvas.fillRect(x - 17, y - 2, 6, 3, 0x6100);
    canvas.fillRect(x + 12, y - 2, 6, 3, 0x6100);

    // table top (thick slab)
    canvas.fillRect(x - 20, y - 18, 42, 7, 0x9A40);
    canvas.drawRect(x - 20, y - 18, 42, 7, 0x7200);
    // rim highlight
    canvas.drawFastHLine(x - 19, y - 17, 40, 0xC4A0);

    // deck of cards on top (bigger)
    canvas.fillRect(x - 6, y - 26, 12, 9, 0xF800);
    canvas.drawRect(x - 6, y - 26, 12, 9, 0xC000);
    canvas.fillRect(x - 5, y - 25, 10, 7, 0xFFFF);
    // second card offset
    canvas.fillRect(x - 3, y - 28, 12, 9, 0x001F);
    canvas.drawRect(x - 3, y - 28, 12, 9, 0x000F);
    canvas.fillRect(x - 2, y - 27, 10, 7, 0xFFFF);

    // center marker / prompt when standing at table
    if (s_onCenter && !s_active) {
        canvas.drawRect(x - 21, y - 19, 44, 9, 0xFFE0);
        canvas.setTextSize(1);
        canvas.setTextColor(0xFFE0, 0x0000);
        canvas.setCursor(x - 20, y - 38);
        canvas.print("G:PLAY");
    }
}

void drawActive(M5Canvas& canvas) {
    if (!s_active) return;
    canvas.fillSprite(0x1082);
    canvas.setTextSize(1);
    canvas.setTextColor(0xFFE0, 0x1082);
    canvas.setCursor(40, 20);
    canvas.print("0N3P0rK CARDS");
    canvas.setTextColor(0xC618, 0x1082);
    canvas.setCursor(28, 48);
    canvas.print("Table ready.");
    canvas.setCursor(16, 64);
    canvas.print("Game not built yet.");
    canvas.setTextColor(0x8410, 0x1082);
    canvas.setCursor(20, 100);
    canvas.print("` or ENT = back");
}

}  // namespace CardsTable
