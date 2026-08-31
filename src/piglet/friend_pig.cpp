// Companion pig — independent of player stun / play-dead.
#include "friend_pig.h"
#include "avatar.h"
#include "mood.h"
#include "trees.h"
#include "../core/xp.h"
#include "../core/config.h"
#include "../audio/sfx.h"
#include <esp_random.h>
#include <math.h>
#include <string.h>

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
static uint32_t s_nextAi = 0;
static uint32_t s_bittenUntil = 0;
static uint32_t s_fallenUntil = 0;   // visual play-dead for FRIEND only
static uint32_t s_chatCool = 0;
static uint32_t s_actionUntil = 0;
static uint32_t s_jumpUntil = 0;     // hop while fleeing zombie player
static int16_t  s_jumpLift = 0;
static bool     s_ready = false;
static bool     s_fleeZombie = false;

static int16_t worldToScreen(int16_t worldX) {
    int16_t bx = (int16_t)(worldX + s_scroll);
    while (bx > Trees::WORLD_WRAP_HI) bx = (int16_t)(bx - Trees::WORLD_SPAN);
    while (bx < Trees::WORLD_WRAP_LO) bx = (int16_t)(bx + Trees::WORLD_SPAN);
    return bx;
}

static int16_t screenX() { return worldToScreen(s_baseX); }

bool unlocked() { return XP::getLevel() >= 40; }

bool enabled() {
    return unlocked() && Config::personality().friendEnabled;
}

bool isActive() { return enabled() && s_hearts > 0; }

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
    s_nextAi = millis() + 1200;
    s_bittenUntil = 0;
    s_fallenUntil = 0;
    s_chatCool = 0;
    s_actionUntil = 0;
    s_jumpUntil = 0;
    s_jumpLift = 0;
    s_fleeZombie = false;
    s_ready = true;
}

void scroll(int8_t dx) {
    if (!enabled()) return;
    s_scroll = (int16_t)(s_scroll + dx);
}

void onWolfBitten() {
    if (!isActive()) return;
    // Friend only — never call Avatar::onWolfBitten from here
    if (s_hearts > 0) s_hearts--;
    uint32_t now = millis();
    s_bittenUntil = now + 2800;
    s_fallenUntil = now + 1600;   // she falls; player does NOT
    s_sitting = false;
    s_sniffing = false;
    s_vx = (esp_random() & 1) ? 1.6f : -1.6f;
    s_faceRight = s_vx > 0;
    SFX::play(SFX::OINK_SQUEAL);
    Mood::say("FRIEND OUCH");
    if (s_hearts == 0) {
        // Run off, respawn later — not a zombie conversion
        s_baseX = (int16_t)(esp_random() % (uint32_t)Trees::WORLD_SPAN);
        s_nextAi = now + 20000;
        s_hearts = 5;
        s_vx = 0;
        s_fallenUntil = 0;
    }
}

static bool playerIsZombie() {
    return Config::personality().pigSkin == (uint8_t)PigSkin::ZOMBIE;
}

static void aiTick(uint32_t now) {
    if (now < s_nextAi) return;
    s_nextAi = now + 900 + (esp_random() % 2800);
    if (now < s_bittenUntil || isFallen()) return;

    // If player is zombie — flee (and hop), never hang out
    if (playerIsZombie()) {
        s_fleeZombie = true;
        s_sitting = false;
        s_sniffing = false;
        int pig = Avatar::getCurrentX() + 20;
        int sx = screenX();
        // Run away from player
        if (sx < pig) {
            s_faceRight = false;
            s_vx = -0.85f - (float)(esp_random() % 40) / 100.f;
        } else {
            s_faceRight = true;
            s_vx = 0.85f + (float)(esp_random() % 40) / 100.f;
        }
        // Occasional panic jump
        if ((esp_random() % 100) < 35) {
            s_jumpUntil = now + 420;
            s_jumpLift = 10;
        }
        s_nextAi = now + 400 + (esp_random() % 600);
        return;
    }
    s_fleeZombie = false;

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

    // Jump arc while fleeing zombie
    if (now < s_jumpUntil) {
        uint32_t left = s_jumpUntil - now;
        // triangle hop
        if (left > 210) s_jumpLift = (int16_t)((420 - left) / 21);
        else s_jumpLift = (int16_t)(left / 21);
        if (s_jumpLift > 12) s_jumpLift = 12;
    } else {
        s_jumpLift = 0;
    }

    // No walk while fallen
    if (isFallen()) {
        s_vx *= 0.5f;  // slight slide
    }

    s_walk += s_vx;
    while (s_walk >= 1.f) {
        s_baseX = (int16_t)(s_baseX + 1);
        s_walk -= 1.f;
    }
    while (s_walk <= -1.f) {
        s_baseX = (int16_t)(s_baseX - 1);
        s_walk += 1.f;
    }

    while (s_baseX >= Trees::WORLD_SPAN)
        s_baseX = (int16_t)(s_baseX - Trees::WORLD_SPAN);
    while (s_baseX < 0)
        s_baseX = (int16_t)(s_baseX + Trees::WORLD_SPAN);

    // Chat only if not fleeing zombie and paths cross
    if (!s_fleeZombie && !isFallen()) {
        int sx = screenX();
        if (sx >= -20 && sx <= 260) {
            int pig = Avatar::getCurrentX() + 20;
            int dist = abs(pig - sx);
            if (dist < 30 && now > s_chatCool && !s_sitting) {
                s_chatCool = now + 12000;
                static const char* lines[] = {
                    "HI FRIEND", "OINK OINK", "NICE FARM", "BYE BYE",
                    "WOLF BAD", "SNACK?", "U R COOL", "I LIVE HERE"
                };
                Mood::say(lines[esp_random() % 8]);
            }
        }
    } else if (s_fleeZombie) {
        int sx = screenX();
        int pig = Avatar::getCurrentX() + 20;
        if (abs(pig - sx) < 28 && now > s_chatCool) {
            s_chatCool = now + 5000;
            Mood::say("ZOMBIE!!");
            // Panic bump — does not hurt player HP, just reaction
            SFX::play(SFX::OINK_SQUEAL);
        }
    }
}

void draw(M5Canvas& canvas, int16_t yOffset) {
    if (!isActive()) return;
    int16_t sx = screenX();
    if (sx < -30 || sx > 270) return;

    int16_t feetY = (int16_t)(GROUND_Y + yOffset - s_jumpLift);
    bool fallen = isFallen();
    bool walking = !fallen && (fabsf(s_vx) > 0.08f) && !s_sitting;
    bool sniff = !fallen && s_sniffing && !s_sitting && !walking;
    Avatar::drawCompanion(canvas, sx, feetY, s_faceRight, walking, s_sitting && !fallen,
                          sniff, fallen);
    if (s_hearts <= 2 && !fallen) {
        canvas.setTextColor(0xF800);
        canvas.setTextSize(1);
        canvas.drawString("<3", sx - 4, feetY - 40);
    }
}

}  // namespace FriendPig
