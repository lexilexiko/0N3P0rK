// Cards table — center + G. Provides isActive/drawActive for app/avatar hooks.
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
static constexpr int CENTER_PX = 6;
static int16_t s_worldX = 200;
static int16_t s_scroll = 0;
static uint32_t s_cool = 0;
static uint32_t s_hintMs = 0;
static bool s_ready = false;
static bool s_onCenter = false;
static bool s_active = false;  // overlay / "game" session
static bool s_keyLatch = false;

bool unlocked() {
    // lv 45+ — no PersonalityConfig flag required (avoids merge skew)
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

    // --- Overlay session: only exit keys ---
    if (s_active) {
        if (!keyNewPress(s_keyLatch)) return;
        if (keyEsc()) {
            close();
            Mood::say("later");
            return;
        }
        // ENT also leaves stub
        auto st = M5Cardputer.Keyboard.keysState();
        if (st.enter) {
            close();
            return;
        }
        return;
    }

    if (Avatar::isJumping()) {
        s_onCenter = false;
        return;
    }

    int16_t sx = screenX();
    int pig = Avatar::getCurrentX() + 20;
    int dist = abs(pig - (int)sx);
    s_onCenter = dist <= CENTER_PX;
    if (!s_onCenter) return;

    uint32_t now = millis();
    if (now - s_hintMs > 3500) {
        s_hintMs = now;
        Mood::say("G = PLAY");
    }

    if (now < s_cool) return;
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;

    bool g = M5Cardputer.Keyboard.isKeyPressed('g') ||
             M5Cardputer.Keyboard.isKeyPressed('G');
    if (!g) return;

    s_cool = now + 1500;
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

    canvas.fillRect(x - 10, y - 10, 3, 10, 0x8200);
    canvas.fillRect(x + 8, y - 10, 3, 10, 0x8200);
    canvas.fillRect(x - 12, y - 14, 26, 5, 0x9A40);
    canvas.drawRect(x - 12, y - 14, 26, 5, 0x7200);
    canvas.fillRect(x - 3, y - 18, 8, 5, 0xF800);
    canvas.fillRect(x - 2, y - 17, 6, 3, 0xFFFF);

    if (s_onCenter && !s_active) {
        canvas.drawRect(x - 13, y - 15, 28, 7, 0xFFE0);
        canvas.setTextSize(1);
        canvas.setTextColor(0xFFE0, 0x0000);
        canvas.setCursor(x - 18, y - 28);
        canvas.print("G:PLAY");
    }
}

void drawActive(M5Canvas& canvas) {
    if (!s_active) return;
    // Stub full-panel so avatar/app have something to draw
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
