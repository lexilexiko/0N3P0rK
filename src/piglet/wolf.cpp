// Fat-pixel wolf that visits the avatar scene and chases the pig.
// Up to 2 concurrent wolves (Fruit Run hard mode).

#include "wolf.h"
#include "avatar.h"
#include "friend_pig.h"
#include "weather.h"
#include "../core/app.h"
#include "../ui/display.h"
#include "../audio/sfx.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../storage/littlefs_ops.h"
#include <esp_random.h>
#include <math.h>

namespace Wolf {

enum class Phase : uint8_t {
    HIDDEN = 0,
    ENTER,
    CHASE,
    SCARE,
    LEAVE
};

struct Actor {
    Phase phase;
    float x, y, vx;
    bool faceRight;
    uint8_t phaseTick;
    uint32_t phaseStartMs;
    uint32_t lastStepMs;
    uint8_t legPhase;
    bool howled;
};

static constexpr uint8_t MAX_WOLVES = 2;
static Actor wolves[MAX_WOLVES];
static uint32_t nextVisitMs = 0;
static bool autoSpawn = true;
static uint8_t maxActive = 1;
static bool biteEvent = false;

static constexpr int16_t PX = 3;
static constexpr float CHASE_SPEED = 3.2f;
static constexpr float LEAVE_SPEED = 4.2f;
static constexpr int SCARE_DIST = 36;
static constexpr uint32_t MIN_GAP_MS = 45000;
static constexpr uint32_t MAX_GAP_MS = 150000;
static constexpr uint32_t SCARE_MS = 900;

static void scheduleNext(uint32_t now) {
    nextVisitMs = now + MIN_GAP_MS + (esp_random() % (MAX_GAP_MS - MIN_GAP_MS + 1));
}

static void clearActor(Actor& w) {
    w.phase = Phase::HIDDEN;
    w.x = -40.0f;
    w.y = 106.0f;
    w.vx = 0.0f;
    w.faceRight = true;
    w.phaseTick = 0;
    w.phaseStartMs = 0;
    w.lastStepMs = 0;
    w.legPhase = 0;
    w.howled = false;
}

void init() {
    reset();
    autoSpawn = true;
    maxActive = 1;
    scheduleNext(millis() + 8000);
}

void reset() {
    for (uint8_t i = 0; i < MAX_WOLVES; i++) clearActor(wolves[i]);
    biteEvent = false;
}

void setAutoSpawn(bool enabled) { autoSpawn = enabled; }
bool getAutoSpawn() { return autoSpawn; }

void setMaxActive(uint8_t n) {
    if (n < 1) n = 1;
    if (n > MAX_WOLVES) n = MAX_WOLVES;
    maxActive = n;
}

bool isActive() {
    for (uint8_t i = 0; i < MAX_WOLVES; i++)
        if (wolves[i].phase != Phase::HIDDEN) return true;
    return false;
}

uint8_t getActiveCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_WOLVES; i++)
        if (wolves[i].phase != Phase::HIDDEN) n++;
    return n;
}

int16_t getX() {
    for (uint8_t i = 0; i < MAX_WOLVES; i++)
        if (wolves[i].phase != Phase::HIDDEN) return (int16_t)wolves[i].x;
    return -1;
}

int16_t getY() {
    for (uint8_t i = 0; i < MAX_WOLVES; i++)
        if (wolves[i].phase != Phase::HIDDEN) return (int16_t)wolves[i].y;
    return 106;
}

bool consumeBiteEvent() {
    if (!biteEvent) return false;
    biteEvent = false;
    return true;
}

static bool beginEnter(Actor& w, uint32_t now, bool preferOpposite) {
    if (w.phase != Phase::HIDDEN) return false;
    w.howled = false;
    w.legPhase = 0;
    w.phaseTick = 0;
    int pigX = Avatar::getCurrentX() + 14 * PX;
    bool fromLeft = preferOpposite ? (pigX > 120) : ((esp_random() & 1) == 0);
    if (preferOpposite && pigX <= 60) fromLeft = false;
    if (fromLeft) {
        w.x = -28.0f;
        w.faceRight = true;
        w.vx = CHASE_SPEED;
    } else {
        w.x = (float)(DISPLAY_W + 28);
        w.faceRight = false;
        w.vx = -CHASE_SPEED;
    }
    w.y = 106.0f;
    w.phase = Phase::CHASE;
    w.phaseStartMs = now;
    w.lastStepMs = now;
    SFX::play(SFX::WOLF);
    return true;
}

void spawnNow() {
    if (App::mode() != AppMode::FARM) return;
    const bool lab = Config::personality().animTest;
    if (!Config::personality().wolfEnabled && !lab) return;
    if (!lab && !Avatar::isNightTime()) return;
    uint32_t now = millis();
    // Prefer empty slot
    for (uint8_t i = 0; i < maxActive && i < MAX_WOLVES; i++) {
        if (wolves[i].phase == Phase::HIDDEN) {
            beginEnter(wolves[i], now, true);
            return;
        }
    }
}

bool spawnSecond() {
    if (!Config::personality().wolfEnabled) return false;
    if (!Avatar::isNightTime()) return false;
    if (maxActive < 2) return false;
    uint32_t now = millis();
    // Need one free slot and at least one already active (or just free slot)
    for (uint8_t i = 0; i < MAX_WOLVES; i++) {
        if (wolves[i].phase == Phase::HIDDEN) {
            // Enter from opposite side of first active / pig
            bool preferOpp = true;
            return beginEnter(wolves[i], now, preferOpp);
        }
    }
    return false;
}

static void spitEatenLoot() {
    uint8_t n = Storage::restoreWolfLoot();
    if (!n) return;
    char msg[28];
    snprintf(msg, sizeof(msg), "WOLF GAVE %u FILE%s",
             (unsigned)n, n == 1 ? "" : "S");
    Display::showToast(msg, 2200);
}

void scareAway() {
    // Scare nearest active wolf to pig
    int pigFeet = Avatar::getCurrentX() + 14 * PX;
    int best = -1;
    int bestD = 9999;
    for (uint8_t i = 0; i < MAX_WOLVES; i++) {
        if (wolves[i].phase == Phase::HIDDEN || wolves[i].phase == Phase::LEAVE) continue;
        int d = (int)wolves[i].x - pigFeet;
        if (d < 0) d = -d;
        if (d < bestD) { bestD = d; best = (int)i; }
    }
    if (best < 0) return;
    Actor& w = wolves[best];
    uint32_t now = millis();
    w.phase = Phase::LEAVE;
    w.phaseStartMs = now;
    if (w.x < (float)pigFeet) {
        w.faceRight = false;
        w.vx = -LEAVE_SPEED * 1.35f;
    } else {
        w.faceRight = true;
        w.vx = LEAVE_SPEED * 1.35f;
    }
    w.howled = true;
    SFX::play(SFX::WOLF_HIT);
    XP::addXP(XPEvent::WOLF_SCARE);
    spitEatenLoot();
}

void scareNear(int feetX, int radius) {
    uint32_t now = millis();
    bool any = false;
    for (uint8_t i = 0; i < MAX_WOLVES; i++) {
        Actor& w = wolves[i];
        if (w.phase == Phase::HIDDEN || w.phase == Phase::LEAVE) continue;
        int d = (int)w.x - feetX;
        if (d < 0) d = -d;
        if (d > radius) continue;
        w.phase = Phase::LEAVE;
        w.phaseStartMs = now;
        if (w.x < (float)feetX) {
            w.faceRight = false;
            w.vx = -LEAVE_SPEED * 1.35f;
        } else {
            w.faceRight = true;
            w.vx = LEAVE_SPEED * 1.35f;
        }
        w.howled = true;
        any = true;
    }
    if (any) {
        SFX::play(SFX::WOLF_HIT);
        XP::addXP(XPEvent::WOLF_SCARE);
        spitEatenLoot();
    }
}

static void updateActor(Actor& w, uint32_t now) {
    if (w.phase == Phase::HIDDEN) return;
    if (now - w.lastStepMs < 33) return;
    w.lastStepMs = now;
    w.legPhase++;

    int playerFeet = Avatar::getCurrentX() + 14 * PX;
    int pigFeet = playerFeet;
    bool targetingFriend = false;
    // Chase nearest of player or friend pig
    if (FriendPig::isActive() && !FriendPig::isFallen()) {
        int fx = FriendPig::getFeetX();
        int dP = playerFeet - (int)w.x; if (dP < 0) dP = -dP;
        int dF = fx - (int)w.x; if (dF < 0) dF = -dF;
        if (dF < dP) {
            pigFeet = fx;
            targetingFriend = true;
        }
    }
    float target = (float)pigFeet;

    switch (w.phase) {
        case Phase::ENTER:
        case Phase::CHASE: {
            // Sitting/zombie immunity only when the TARGET is the player
            const bool pigSitting = !targetingFriend && Avatar::isSitting();
            const bool zombiePig =
                !targetingFriend &&
                (Config::personality().pigSkin == (uint8_t)PigSkin::ZOMBIE);
            float dx = target - w.x;
            if (pigSitting) {
                if (w.vx == 0.0f) {
                    if (dx > 0) { w.faceRight = true;  w.vx = CHASE_SPEED * 0.9f; }
                    else        { w.faceRight = false; w.vx = -CHASE_SPEED * 0.9f; }
                }
            } else {
                if (dx > 1.0f) {
                    w.faceRight = true;
                    w.vx = CHASE_SPEED;
                } else if (dx < -1.0f) {
                    w.faceRight = false;
                    w.vx = -CHASE_SPEED;
                } else {
                    w.vx = 0.0f;
                }
            }
            w.y = 106.0f - (float)((w.legPhase / 2) & 1);
            w.x += w.vx;

            float adx = dx < 0 ? -dx : dx;
            if (adx < (float)SCARE_DIST) {
                if (pigSitting || zombiePig) {
                    // Player hide / undead — wolf walks past (friend still biteable)
                    w.phase = Phase::LEAVE;
                    w.phaseStartMs = now;
                    if (w.faceRight) w.vx = LEAVE_SPEED * 0.95f;
                    else             w.vx = -LEAVE_SPEED * 0.95f;
                    if (pigSitting) {
                        XP::addXP(XPEvent::WOLF_HIDE);
                    }
                } else {
                    w.phase = Phase::SCARE;
                    w.phaseStartMs = now;
                    w.phaseTick = 0;
                    w.vx = 0.0f;
                    if (!w.howled) {
                        w.howled = true;
                        SFX::play(SFX::WOLF);
                        biteEvent = true;
                        // Bite ONLY the nearest target — never both
                        if (targetingFriend) {
                            FriendPig::onWolfBitten();
                        } else if (FriendPig::isActive()) {
                            int fx = FriendPig::getFeetX();
                            int adF = (int)w.x - fx; if (adF < 0) adF = -adF;
                            int adP = (int)w.x - playerFeet; if (adP < 0) adP = -adP;
                            if (adF <= adP) FriendPig::onWolfBitten();
                            else Avatar::onWolfBitten();
                        } else {
                            Avatar::onWolfBitten();
                        }
                    }
                }
            }

            if (now - w.phaseStartMs > 18000) {
                w.phase = Phase::LEAVE;
                w.phaseStartMs = now;
                if (w.x < 120) {
                    w.faceRight = false;
                    w.vx = -LEAVE_SPEED;
                } else {
                    w.faceRight = true;
                    w.vx = LEAVE_SPEED;
                }
            }
            break;
        }
        case Phase::SCARE: {
            w.phaseTick++;
            w.y = 106.0f - ((w.phaseTick / 3) & 1) * 2.0f;
            w.faceRight = (target >= w.x);
            if (now - w.phaseStartMs > SCARE_MS) {
                w.phase = Phase::LEAVE;
                w.phaseStartMs = now;
                if (w.x < target) {
                    w.faceRight = false;
                    w.vx = -LEAVE_SPEED;
                } else {
                    w.faceRight = true;
                    w.vx = LEAVE_SPEED;
                }
            }
            break;
        }
        case Phase::LEAVE: {
            w.x += w.vx;
            w.y = 106.0f - (float)((w.legPhase / 2) & 1);
            if (w.x < -40.0f || w.x > (float)(DISPLAY_W + 40)) {
                clearActor(w);
                if (autoSpawn) scheduleNext(now);
            }
            break;
        }
        default:
            break;
    }
}

void update() {
    uint32_t now = millis();

    // Wolf only on the idle farm — never during attack / loot / menus.
    if (App::mode() != AppMode::FARM) {
        if (isActive()) reset();
        nextVisitMs = 0;
        return;
    }

    if (!Config::personality().wolfEnabled) {
        if (isActive()) reset();
        return;
    }

    const bool night = Avatar::isNightTime();
    if (!night) {
        for (uint8_t i = 0; i < MAX_WOLVES; i++) {
            if (wolves[i].phase != Phase::HIDDEN && wolves[i].phase != Phase::LEAVE) {
                wolves[i].phase = Phase::LEAVE;
                wolves[i].phaseStartMs = now;
                wolves[i].vx = wolves[i].faceRight ? LEAVE_SPEED : -LEAVE_SPEED;
            }
        }
        nextVisitMs = 0;
    } else if (autoSpawn && getActiveCount() == 0) {
        if (nextVisitMs == 0) scheduleNext(now);
        if (now >= nextVisitMs) {
            spawnNow();
        }
    }

    for (uint8_t i = 0; i < MAX_WOLVES; i++)
        updateActor(wolves[i], now);
}

// Fat-pixel side wolf facing right (mirrored if !faceRight)
static void stamp(M5Canvas& canvas, int16_t ox, int16_t oy, int lx, int ly,
                  bool right, uint16_t col) {
    int16_t px = right ? (ox + lx * PX) : (ox - (lx + 1) * PX);
    int16_t py = oy + ly * PX;
    if (px < -PX || px > DISPLAY_W || py < -PX || py > 120) return;
    canvas.fillRect(px, py, PX, PX, col);
}

static void stampBlock(M5Canvas& canvas, int16_t ox, int16_t oy,
                       int lx, int ly, int w, int h, bool right, uint16_t col) {
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            stamp(canvas, ox, oy, lx + xx, ly + yy, right, col);
}

static void drawOne(M5Canvas& canvas, const Actor& w,
                    uint16_t OUT, uint16_t FUR, uint16_t FUR2, uint16_t FURH,
                    uint16_t BELLY, uint16_t NOSE, uint16_t EYE, uint16_t EAR,
                    uint16_t PUP, uint16_t TONG, bool flash) {
    if (w.phase == Phase::HIDDEN) return;

    int16_t ox = (int16_t)w.x;
    int16_t oy = (int16_t)w.y;
    bool right = w.faceRight;
    int leg = (w.legPhase / 2) & 1;

    for (int dx = -12; dx <= 14; dx++) {
        int adx = dx < 0 ? -dx : dx;
        int16_t sx = right ? (ox + dx * PX) : (ox - (dx + 1) * PX);
        if (sx < 0 || sx >= DISPLAY_W) continue;
        if (adx <= 10) {
            canvas.drawPixel(sx, oy, 0x4208);
            if ((dx & 1) == 0) canvas.drawPixel(sx, oy - 1, 0x5ACB);
        }
    }

    stampBlock(canvas, ox, oy, -8, -6 + leg, 2, 6, right, FUR);
    stampBlock(canvas, ox, oy, -7, -5 + leg, 2, 4, right, OUT);
    stampBlock(canvas, ox, oy, -5, -6 + (1 - leg), 2, 6, right, FUR2);
    stampBlock(canvas, ox, oy, -4, -5 + (1 - leg), 2, 4, right, OUT);
    stampBlock(canvas, ox, oy, 2, -6 + (1 - leg), 2, 6, right, FUR);
    stampBlock(canvas, ox, oy, 3, -5 + (1 - leg), 2, 4, right, OUT);
    stampBlock(canvas, ox, oy, 6, -6 + leg, 2, 6, right, FUR2);
    stampBlock(canvas, ox, oy, 7, -5 + leg, 2, 4, right, OUT);
    stampBlock(canvas, ox, oy, -8, -1 + leg, 2, 1, right, OUT);
    stampBlock(canvas, ox, oy, -5, -1 + (1 - leg), 2, 1, right, OUT);
    stampBlock(canvas, ox, oy, 2, -1 + (1 - leg), 2, 1, right, OUT);
    stampBlock(canvas, ox, oy, 6, -1 + leg, 2, 1, right, OUT);

    stampBlock(canvas, ox, oy, -7, -12, 14, 2, right, OUT);
    stampBlock(canvas, ox, oy, -8, -10, 16, 2, right, OUT);
    stampBlock(canvas, ox, oy, -8, -8, 16, 2, right, OUT);
    stampBlock(canvas, ox, oy, -5, -11, 10, 3, right, FUR);
    stampBlock(canvas, ox, oy, -4, -9, 8, 2, right, FURH);
    stampBlock(canvas, ox, oy, -6, -7, 12, 2, right, FUR);
    stampBlock(canvas, ox, oy, -5, -5, 10, 1, right, FUR);
    stampBlock(canvas, ox, oy, -3, -4, 6, 1, right, BELLY);
    stamp(canvas, ox, oy, -6, -9, right, FUR2);
    stamp(canvas, ox, oy, -2, -10, right, FUR2);
    stamp(canvas, ox, oy, -1, -8, right, FURH);
    stampBlock(canvas, ox, oy, -1, -13, 3, 2, right, FUR2);
    stamp(canvas, ox, oy, -3, -13, right, FUR);

    int tw = ((w.legPhase / 2) & 3) - 1;
    stampBlock(canvas, ox, oy, -13, -11 + tw, 6, 5, right, OUT);
    stampBlock(canvas, ox, oy, -12, -10 + tw, 5, 4, right, FUR);
    stampBlock(canvas, ox, oy, -14, -10 + tw, 4, 3, right, FURH);
    stamp(canvas, ox, oy, -15, -9 + tw, right, OUT);

    stampBlock(canvas, ox, oy, 6, -14, 5, 6, right, OUT);
    stamp(canvas, ox, oy, 7, -15, right, EAR);
    stamp(canvas, ox, oy, 9, -15, right, EAR);
    stampBlock(canvas, ox, oy, 8, -13, 3, 2, right, FUR);
    stampBlock(canvas, ox, oy, 11, -13, 6, 4, right, OUT);
    stampBlock(canvas, ox, oy, 13, -11, 4, 3, right, FURH);
    stampBlock(canvas, ox, oy, 15, -10, 2, 2, right, NOSE);
    stampBlock(canvas, ox, oy, 10, -12, 3, 3, right, FUR2);
    stampBlock(canvas, ox, oy, 12, -12, 2, 2, right, FUR);
    stampBlock(canvas, ox, oy, 9, -11, 3, 2, right, BELLY);

    uint16_t eyeCol = EYE;
    if (w.phase == Phase::SCARE || w.howled) eyeCol = 0xF800;
    stamp(canvas, ox, oy, 10, -12, right, eyeCol);
    stamp(canvas, ox, oy, 11, -12, right, PUP);

    if (w.phase == Phase::SCARE) {
        stamp(canvas, ox, oy, 14, -9, right, 0xFFFF);
        stamp(canvas, ox, oy, 14, -11, right, 0xFFFF);
        stampBlock(canvas, ox, oy, 12, -10, 3, 1, right, TONG);
    } else {
        stampBlock(canvas, ox, oy, 13, -9, 3, 1, right, NOSE);
    }

    stampBlock(canvas, ox, oy, 2, -13, 2, 3, right, FURH);
    stamp(canvas, ox, oy, 0, -14, right, FUR2);
    if (w.howled) stamp(canvas, ox, oy, 10, -13, right, 0xF800);
    (void)flash;
}

void draw(M5Canvas& canvas) {
    if (!isActive()) return;

    const bool flash = Avatar::isThunderFlashing();
    Season season = Weather::getActiveSeason();
    uint16_t OUT, FUR, FUR2, FURH, BELLY, NOSE, EYE, EAR;
    switch (season) {
        case Season::WINTER:
            OUT  = 0x6B6D; FUR = 0xEF5D; FUR2 = 0xC618; FURH = 0xFFFF;
            BELLY = 0xFFFF; NOSE = 0x3186; EYE = 0x5D7F; EAR = 0xF6B0;
            break;
        case Season::SPRING:
            OUT  = 0x6200; FUR = 0xD4A0; FUR2 = 0x9A40; FURH = 0xF6E0;
            BELLY = 0xFFF0; NOSE = 0x28A2; EYE = 0x07E0; EAR = 0xFD2F;
            break;
        case Season::AUTUMN:
            OUT  = 0x4000; FUR = 0xC2C0; FUR2 = 0x8000; FURH = 0xE3E0;
            BELLY = 0xFDE0; NOSE = 0x2100; EYE = 0xFD20; EAR = 0xFBE0;
            break;
        case Season::RETRO:
            // Silver-screen wolf — pure B&W film stock
            OUT  = 0x2104; FUR = 0xAD55; FUR2 = 0x7BEF; FURH = 0xC618;
            BELLY = 0xDEFB; NOSE = 0x0000; EYE = 0xFFFF; EAR = 0x8410;
            break;
        case Season::NOIR:
            OUT  = 0x1082; FUR = 0x3186; FUR2 = 0x2104; FURH = 0x5AEB;
            BELLY = 0x4A49; NOSE = 0x0000; EYE = 0xFE60; EAR = 0x4208;
            break;
        case Season::CITY:
            // Well-groomed city wolf — clean cream coat, blue collar eye
            OUT  = 0x6B4D; FUR = 0xEF5D; FUR2 = 0xD69A; FURH = 0xFFFF;
            BELLY = 0xFFDF; NOSE = 0x4208; EYE = 0x5D7F; EAR = 0xFCB2;
            break;
        case Season::DESERT:
            // Coyote — sandy tan, lean look
            OUT  = 0x8200; FUR = 0xD4A0; FUR2 = 0xAB40; FURH = 0xF6C0;
            BELLY = 0xFDE0; NOSE = 0x4100; EYE = 0xFE60; EAR = 0xC2C0;
            break;
        case Season::SUMMER:
        default:
            OUT  = 0x41E0; FUR = 0x9A60; FUR2 = 0x7200; FURH = 0xD4A0;
            BELLY = 0xF6D0; NOSE = 0x28A2; EYE = 0xFE60; EAR = 0xC2C8;
            break;
    }
    if (flash) {
        OUT = FUR = FUR2 = FURH = BELLY = NOSE = EYE = EAR = 0xFFFF;
    }
    const uint16_t PUP  = flash ? 0x0000 : 0x2100;
    const uint16_t TONG = flash ? 0xFFFF : 0xF98C;

    for (uint8_t i = 0; i < MAX_WOLVES; i++)
        drawOne(canvas, wolves[i], OUT, FUR, FUR2, FURH, BELLY, NOSE, EYE, EAR, PUP, TONG, flash);
}

}  // namespace Wolf
