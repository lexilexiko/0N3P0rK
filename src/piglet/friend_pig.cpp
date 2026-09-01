// Companion pig — own life, own zombie, own speech bubbles.
#include "friend_pig.h"
#include "avatar.h"
#include "trees.h"
#include "weather.h"
#include "../core/xp.h"
#include "../core/config.h"
#include "../audio/sfx.h"
#include <esp_random.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace FriendPig {

static constexpr int16_t GROUND_Y = 106;
static constexpr int16_t PX = 3;

static int16_t s_baseX = 200;
static int16_t s_scroll = 0;
static float   s_walk = 0.f;
static float   s_vx = 0.f;
static bool    s_faceRight = false;
static bool    s_sitting = false;
static bool    s_sniffing = false;
static uint8_t s_hearts = 5;
static bool    s_zombie = false;     // HER undead state only
static uint32_t s_nextAi = 0;
static uint32_t s_bittenUntil = 0;
static uint32_t s_fallenUntil = 0;
static uint32_t s_chatCool = 0;
static uint32_t s_actionUntil = 0;
static uint32_t s_jumpUntil = 0;
static int16_t  s_jumpLift = 0;
static bool     s_ready = false;

// Own bubble (never Mood::say — that is the player's mouth)
static char     s_phrase[28] = {};
static uint32_t s_phraseUntil = 0;

static int16_t worldToScreen(int16_t worldX) {
    int16_t bx = (int16_t)(worldX + s_scroll);
    while (bx > Trees::WORLD_WRAP_HI) bx = (int16_t)(bx - Trees::WORLD_SPAN);
    while (bx < Trees::WORLD_WRAP_LO) bx = (int16_t)(bx + Trees::WORLD_SPAN);
    return bx;
}

static int16_t screenX() { return worldToScreen(s_baseX); }

static bool playerIsZombie() {
    return Config::personality().pigSkin == (uint8_t)PigSkin::ZOMBIE;
}

static void sayOwn(const char* line, uint32_t holdMs = 2200) {
    if (!line) return;
    strncpy(s_phrase, line, sizeof(s_phrase) - 1);
    s_phrase[sizeof(s_phrase) - 1] = '\0';
    s_phraseUntil = millis() + holdMs;
}

static void drawBubble(M5Canvas& canvas, int16_t pigX) {
    if (!s_phrase[0] || (int32_t)(millis() - s_phraseUntil) >= 0) {
        s_phrase[0] = '\0';
        return;
    }
    const char* ph = s_phrase;
    int chars = (int)strlen(ph);
    int bubbleW = chars * 6 + 12;
    if (bubbleW < 40) bubbleW = 40;
    if (bubbleW > 150) bubbleW = 150;
    int bubbleH = 14;
    int bubbleX = pigX + 16;
    int bubbleY = 18;  // a bit lower than player bubble so they don't stack
    if (bubbleX + bubbleW > 236) bubbleX = pigX - bubbleW - 4;
    if (bubbleX < 2) bubbleX = 2;

    uint16_t fg = 0xAFEA;  // soft green tint — friend
    uint16_t bg = 0x2145;
    if (s_zombie) {
        fg = 0x5FEA;
        bg = 0x2000;
    }
    if (Weather::getActiveSeason() == Season::RETRO) {
        fg = 0xC618; bg = 0x1082;
    }

    canvas.fillRoundRect(bubbleX, bubbleY, bubbleW, bubbleH, 3, fg);
    canvas.fillTriangle(bubbleX + 10, bubbleY + bubbleH,
                        bubbleX + 16, bubbleY + bubbleH,
                        bubbleX + 12, bubbleY + bubbleH + 4, fg);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(bg);
    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(ph, bubbleX + 5, bubbleY + 3);
}

bool unlocked() { return XP::getLevel() >= 40; }

bool enabled() {
    return unlocked() && Config::personality().friendEnabled;
}

bool isZombie() { return s_zombie; }

bool isActive() {
    // Alive friend OR undead friend still on the farm
    return enabled();
}

bool isFallen() {
    return s_fallenUntil != 0 && (int32_t)(millis() - s_fallenUntil) < 0;
}

int getFeetX() { return (int)screenX(); }

void begin() {
    s_baseX = (int16_t)(180 + (int)(esp_random() % 200));
    s_scroll = 0;
    s_walk = 0.f;
    s_vx = 0.f;
    s_faceRight = (esp_random() & 1) != 0;
    s_sitting = false;
    s_sniffing = false;
    s_hearts = 5;
    s_zombie = false;
    s_nextAi = millis() + 1200;
    s_bittenUntil = 0;
    s_fallenUntil = 0;
    s_chatCool = millis() + 8000;
    s_actionUntil = 0;
    s_jumpUntil = 0;
    s_jumpLift = 0;
    s_phrase[0] = '\0';
    s_ready = true;
}

void scroll(int8_t dx) {
    if (!enabled()) return;
    s_scroll = (int16_t)(s_scroll + dx);
}

void onWolfBitten() {
    if (!enabled()) return;
    if (s_zombie) return;  // undead — wolf loses interest

    if (s_hearts > 0) s_hearts--;
    uint32_t now = millis();
    s_bittenUntil = now + 2800;
    s_fallenUntil = now + 1600;
    s_sitting = false;
    s_sniffing = false;
    s_vx = (esp_random() & 1) ? 1.6f : -1.6f;
    s_faceRight = s_vx > 0;
    SFX::play(SFX::OINK_SQUEAL);
    sayOwn("OUCH", 1500);

    if (s_hearts == 0) {
        // SHE becomes zombie — player skin untouched
        s_zombie = true;
        s_fallenUntil = 0;
        s_vx = 0;
        sayOwn("GRRR..", 2500);
        SFX::play(SFX::OINK_GRUNT);
    }
}

static void aiTick(uint32_t now) {
    if (now < s_nextAi) return;
    if (now < s_bittenUntil || isFallen()) {
        s_nextAi = now + 300;
        return;
    }

    int pig = Avatar::getCurrentX() + 20;
    int sx = screenX();

    // --- Friend is zombie: hunt the living player ---
    if (s_zombie) {
        s_sitting = false;
        s_sniffing = false;
        if (sx < pig - 4) {
            s_faceRight = true;
            s_vx = 0.55f + (float)(esp_random() % 25) / 100.f;
        } else if (sx > pig + 4) {
            s_faceRight = false;
            s_vx = -0.55f - (float)(esp_random() % 25) / 100.f;
        } else {
            s_vx = 0;
        }
        s_nextAi = now + 350 + (esp_random() % 400);
        return;
    }

    // --- Player is zombie: flee (her own life, not "join undead") ---
    if (playerIsZombie()) {
        s_sitting = false;
        s_sniffing = false;
        if (sx < pig) {
            s_faceRight = false;
            s_vx = -0.9f - (float)(esp_random() % 35) / 100.f;
        } else {
            s_faceRight = true;
            s_vx = 0.9f + (float)(esp_random() % 35) / 100.f;
        }
        if ((esp_random() % 100) < 40) {
            s_jumpUntil = now + 420;
            s_jumpLift = 10;
        }
        s_nextAi = now + 400 + (esp_random() % 500);
        return;
    }

    // --- Normal independent roam ---
    s_nextAi = now + 900 + (esp_random() % 2800);
    uint8_t r = (uint8_t)(esp_random() % 100);
    if (r < 20) {
        s_sitting = true;
        s_sniffing = false;
        s_vx = 0;
        s_nextAi = now + 2500 + (esp_random() % 3500);
    } else if (r < 35) {
        s_sitting = false;
        s_vx = 0;
        s_sniffing = (esp_random() % 2) == 0;
        s_actionUntil = now + 700 + (esp_random() % 1400);
    } else if (r < 60) {
        s_sitting = false;
        s_sniffing = false;
        s_faceRight = false;
        s_vx = -0.3f - (float)(esp_random() % 30) / 100.f;
    } else if (r < 85) {
        s_sitting = false;
        s_sniffing = false;
        s_faceRight = true;
        s_vx = 0.3f + (float)(esp_random() % 30) / 100.f;
    } else {
        s_sitting = false;
        s_sniffing = false;
        s_vx = 0;
    }
}

void update() {
    if (!enabled()) return;
    if (!s_ready) begin();
    uint32_t now = millis();
    aiTick(now);
    if (now >= s_actionUntil) s_sniffing = false;

    // Jump arc while fleeing
    if (s_jumpUntil != 0 && (int32_t)(now - s_jumpUntil) < 0) {
        uint32_t left = s_jumpUntil - now;
        // simple arc: peak mid-flight
        float t = 1.f - (float)left / 420.f;
        s_jumpLift = (int16_t)(10.f * 4.f * t * (1.f - t));
    } else {
        s_jumpLift = 0;
        s_jumpUntil = 0;
    }

    s_walk += s_vx;
    while (s_walk >= 1.f) { s_baseX = (int16_t)(s_baseX + 1); s_walk -= 1.f; }
    while (s_walk <= -1.f) { s_baseX = (int16_t)(s_baseX - 1); s_walk += 1.f; }
    while (s_baseX >= Trees::WORLD_SPAN)
        s_baseX = (int16_t)(s_baseX - Trees::WORLD_SPAN);
    while (s_baseX < 0)
        s_baseX = (int16_t)(s_baseX + Trees::WORLD_SPAN);

    // Rare talk ~ every 20s (own bubble only)
    if (now >= s_chatCool) {
        int sx = screenX();
        if (sx >= -10 && sx <= 250) {
            s_chatCool = now + 20000 + (esp_random() % 8000);  // 20–28 s
            if (s_zombie) {
                static const char* growls[] = {
                    "GRRR", "BRAINS?", "HSSSS", "UHHH", "NOM NOM", "Zzz..NO"
                };
                sayOwn(growls[esp_random() % 6], 2000);
                SFX::play(SFX::OINK_GRUNT);
            } else if (playerIsZombie()) {
                static const char* scare[] = {
                    "ZOMBIE!!", "RUN", "NOOO", "EEK", "BYE"
                };
                sayOwn(scare[esp_random() % 5], 1800);
            } else {
                static const char* lines[] = {
                    "HI", "OINK", "NICE", "SNACK?", "WOLF?", "ZZZ", "OK"
                };
                sayOwn(lines[esp_random() % 7], 1800);
            }
        } else {
            s_chatCool = now + 5000;  // retry later if off-screen
        }
    }
}

void draw(M5Canvas& canvas, int16_t yOffset) {
    if (!enabled()) return;
    int16_t sx = screenX();
    if (sx < -30 || sx > 270) return;

    int16_t feetY = (int16_t)(GROUND_Y + yOffset - s_jumpLift);
    bool fallen = isFallen();
    bool walking = !fallen && (fabsf(s_vx) > 0.08f) && !s_sitting;
    bool sniff = !fallen && s_sniffing && !s_sitting && !walking && !s_zombie;
    Avatar::drawCompanion(canvas, sx, feetY, s_faceRight, walking,
                          s_sitting && !fallen, sniff, fallen, s_zombie);
    drawBubble(canvas, sx);
    if (!s_zombie && s_hearts <= 2 && !fallen) {
        canvas.setTextColor(0xF800);
        canvas.setTextSize(1);
        canvas.drawString("<3", sx - 4, feetY - 40);
    }
}

}  // namespace FriendPig
