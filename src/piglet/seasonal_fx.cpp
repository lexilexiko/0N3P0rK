// Seasonal decorative animations (not core precip / clouds).
// Keep this file the home for "pretty season stuff".

#include "seasonal_fx.h"
#include "weather.h"
#include "trees.h"
#include "ground.h"
#include "avatar.h"
#include "../ui/display.h"
#include "../audio/sfx.h"
#include <esp_random.h>
#include <math.h>

namespace SeasonalFx {

// ---------------------------------------------------------------------------
// Winter: snow banks (сугробы) — fat-pixel steps on ground line
// Can grow tall; pig trampling melts a path
// ---------------------------------------------------------------------------
struct SnowBank {
    int16_t x;
    uint8_t w;  // width px
    uint8_t h;  // fat rows 1..MAX
};
struct SnowScar {
    int16_t x;
    uint8_t w;
    bool active;
};
static constexpr int SNOW_BANK_COUNT = 18;
static constexpr int SNOW_SCAR_COUNT = 16;
static constexpr uint8_t SNOW_BANK_MAX_H = 8;  // tall drifts
static SnowBank snowBanks[SNOW_BANK_COUNT] = {};
static SnowScar snowScars[SNOW_SCAR_COUNT] = {};
static uint32_t lastBankGrowMs = 0;
static bool snowBanksInited = false;
static bool snowingWas = false;
static uint32_t lastRainSfxMs = 0;

// DESERT: sandstorm grit (horizontal streaks)
struct SandGrit {
    float x, y;
    float vx;
    uint8_t shade;
    bool active;
};
static constexpr int SAND_GRIT_COUNT = 48;  // denser sandstorm
static SandGrit sandGrit[SAND_GRIT_COUNT] = {};
static uint32_t lastSandMs = 0;


// ---------------------------------------------------------------------------
// Autumn: falling leaves (denser / more visible)
// ---------------------------------------------------------------------------
struct Leaf {
    float x, y;
    float vx, vy;
    uint8_t col;
    uint8_t size;
    bool active;
};
static constexpr int LEAF_COUNT = 22;
static Leaf leaves[LEAF_COUNT] = {};
static uint32_t lastLeafUpdate = 0;

// ---------------------------------------------------------------------------
// Autumn: piles of fallen leaves, pushed and scattered by the pig.
// ---------------------------------------------------------------------------
struct LeafPile {
    float x;
    uint32_t lastPushAt;
    uint32_t lastDecayAt;
    uint8_t count;
    uint8_t col;
    bool active;
};
static constexpr uint8_t LEAF_PILE_COUNT = 6;
static LeafPile leafPiles[LEAF_PILE_COUNT] = {};
static int16_t lastPigCenterX = -1;

// Meteor shower: fixed-size state only; no heap allocation during the event.
struct Meteor {
    float x, y, vx, vy;
    bool active;
};
struct MeteorFragment {
    float x, y, vx, vy;
    uint32_t born;
    uint8_t shade;
    bool settled, active;
};
static constexpr uint8_t METEOR_FLIGHT_COUNT = 2;
static constexpr uint8_t METEOR_FRAGMENT_COUNT = 64;
static constexpr uint32_t GAME_DAY_MS = 360000u;
static constexpr uint32_t METEOR_INTERVAL_DAYS = 2u;
static Meteor meteors[METEOR_FLIGHT_COUNT] = {};
static MeteorFragment meteorFragments[METEOR_FRAGMENT_COUNT] = {};
static uint8_t meteorLaunched = 0;
static bool meteorShowerActive = false;
static uint32_t nextMeteorAt = 0;
static uint32_t meteorShowerEndsAt = 0;
static uint32_t nextMeteorGameDay = 2;
static uint8_t meteorFragmentCursor = 0;

// ---------------------------------------------------------------------------
// Summer: butterflies (brighter, more of them)
// ---------------------------------------------------------------------------
struct Butterfly {
    float x, y;
    float vx, vy;
    uint8_t phase;
    uint8_t hue;  // color variant
    bool active;
};
static constexpr int BUTTERFLY_COUNT = 5;
static Butterfly butterflies[BUTTERFLY_COUNT] = {};
static uint32_t lastButterflySpawnMs = 0;

// ---------------------------------------------------------------------------
// Summer: pollen / heat motes (golden dust in air)
// ---------------------------------------------------------------------------
struct PollenMote {
    float x, y;
    float vx, vy;
    uint8_t phase;
    bool active;
};
static constexpr int POLLEN_COUNT = 16;
static PollenMote pollen[POLLEN_COUNT] = {};
static uint32_t lastPollenUpdate = 0;

// ---------------------------------------------------------------------------
// Spring: sky lightning bolts (behind tree/pig)
// ---------------------------------------------------------------------------
struct LightningBolt {
    int16_t x0;           // start X at top
    int8_t  segDx[7];     // horizontal jogs
    int8_t  segDy[7];     // vertical steps (always +)
    uint8_t segs;
    uint8_t life;         // frames remaining
    uint8_t bright;       // 0=dim fork, 1=main
    bool active;
};
static constexpr int BOLT_COUNT = 2;
static LightningBolt bolts[BOLT_COUNT] = {};
static uint32_t lastBoltSpawnMs = 0;
static bool wasThunderFlash = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool isSpring() { return Weather::getActiveSeason() == Season::SPRING; }
static bool isSummer() { return Weather::getActiveSeason() == Season::SUMMER; }
static bool isAutumn() { return Weather::getActiveSeason() == Season::AUTUMN; }
static bool isWinter() { return Weather::getActiveSeason() == Season::WINTER; }
static bool isNoir()   { return Weather::getActiveSeason() == Season::NOIR; }
static bool isDesert() { return Weather::getActiveSeason() == Season::DESERT; }

static float wrapFieldX(float x) {
    const float width = (float)Trees::worldWidth();
    x = fmodf(x, width);
    if (x < 0.0f) x += width;
    return x;
}

static float worldToScreenX(float worldX) {
    return wrapFieldX(worldX - (float)Ground::worldOffset());
}

static float screenToWorldX(float screenX) {
    return wrapFieldX(screenX + (float)Ground::worldOffset());
}

// Noir: moths around the street lamp (center lane)
struct Moth {
    float a;
    float r;
    uint8_t phase;
    bool active;
};
static Moth moths[3] = {};

static void updateMoths(uint32_t now) {
    static uint32_t last = 0;
    if (now - last < 40) return;
    last = now;
    for (int i = 0; i < 3; i++) {
        if (!moths[i].active) {
            moths[i].active = true;
            moths[i].a = (float)i * 2.1f;
            moths[i].r = 10.0f + (float)i * 4.0f;
            moths[i].phase = (uint8_t)(i * 40);
        }
        moths[i].phase++;
        moths[i].a += 0.08f + (float)i * 0.02f;
        moths[i].r = 10.0f + (float)i * 4.0f + sinf((float)moths[i].phase * 0.12f) * 3.0f;
    }
}

static void drawMoths(M5Canvas& canvas, bool flash) {
    int16_t cx = Trees::nearestFloraScreenX(120);
    if (cx < 0) cx = 120;
    const int16_t cy = 42;
    uint16_t c = flash ? 0xFFFF : 0xFE60;
    for (int i = 0; i < 3; i++) {
        if (!moths[i].active) continue;
        int16_t x = cx + (int16_t)(cosf(moths[i].a) * moths[i].r);
        int16_t y = cy + (int16_t)(sinf(moths[i].a) * moths[i].r * 0.45f);
        canvas.fillRect(x, y, 2, 2, c);
    }
}

// ---------------------------------------------------------------------------
// Winter banks
// ---------------------------------------------------------------------------
static bool rangesOverlap(int16_t ax, int16_t aw, int16_t bx, int16_t bw) {
    return ax < (int16_t)(bx + bw) && bx < (int16_t)(ax + aw);
}

static void addScar(int16_t x, uint8_t w) {
    if (w < 8) w = 8;
    for (int i = 0; i < SNOW_SCAR_COUNT; i++) {
        if (snowScars[i].active) continue;
        snowScars[i].x = x;
        snowScars[i].w = w;
        snowScars[i].active = true;
        return;
    }
}

static bool hitsScar(int16_t x, uint8_t w) {
    for (int i = 0; i < SNOW_SCAR_COUNT; i++) {
        if (!snowScars[i].active) continue;
        int16_t sx = snowScars[i].x;
        if (rangesOverlap(x, (int16_t)w, sx, (int16_t)snowScars[i].w)) return true;
    }
    return false;
}

static bool hitsLiveBank(int16_t x, uint8_t w, int skip) {
    for (int i = 0; i < SNOW_BANK_COUNT; i++) {
        if (i == skip || snowBanks[i].h < 1) continue;
        int16_t bx = snowBanks[i].x;
        if (rangesOverlap(x, (int16_t)w, bx, (int16_t)snowBanks[i].w)) return true;
    }
    return false;
}

static int freeBankSlot() {
    for (int i = 0; i < SNOW_BANK_COUNT; i++) {
        if (snowBanks[i].h < 1) return i;
    }
    return -1;
}

static bool trySpawnBank(bool preferOffscreen) {
    int slot = freeBankSlot();
    if (slot < 0) return false;
    for (int tries = 0; tries < 22; tries++) {
        uint8_t w = (uint8_t)(20 + (esp_random() % 22));  // 20..41
        int16_t x = (int16_t)(esp_random() % (uint32_t)Trees::worldWidth());
        x = (int16_t)(x - Ground::worldOffset());
        bool onScreen = (x >= -8 && x <= 248);
        if (preferOffscreen && onScreen) continue;
        if (hitsScar(x, w)) continue;
        if (hitsLiveBank(x, w, slot)) continue;
        snowBanks[slot].x = x;
        snowBanks[slot].w = w;
        snowBanks[slot].h = 1 + (uint8_t)(esp_random() % 2);
        return true;
    }
    return false;
}

static void initSnowBanks() {
    for (int i = 0; i < SNOW_BANK_COUNT; i++) {
        snowBanks[i].x = 0;
        snowBanks[i].w = 0;
        snowBanks[i].h = 0;
    }
    for (int i = 0; i < SNOW_SCAR_COUNT; i++) snowScars[i].active = false;
    snowBanksInited = true;
    snowingWas = false;
    lastBankGrowMs = millis();
}


static void updateSandstorm(uint32_t now) {
    if (!isDesert()) {
        for (int i = 0; i < SAND_GRIT_COUNT; i++) sandGrit[i].active = false;
        return;
    }
    if (now - lastSandMs < 28) return;  // faster churn
    lastSandMs = now;
    for (int i = 0; i < SAND_GRIT_COUNT; i++) {
        if (!sandGrit[i].active) {
            // High spawn — real storm, mostly mid/low band
            if ((esp_random() % 100) < 55) {
                sandGrit[i].active = true;
                sandGrit[i].x = (float)((int)(esp_random() % 40) - 30); // enter from left
                sandGrit[i].y = (float)(35 + (esp_random() % 70));
                sandGrit[i].vx = 2.8f + (float)(esp_random() % 55) / 10.f;
                sandGrit[i].shade = (uint8_t)(esp_random() % 4);
            }
            continue;
        }
        sandGrit[i].x += sandGrit[i].vx;
        sandGrit[i].y += ((int)(esp_random() % 5) - 2) * 0.35f;
        if (sandGrit[i].y < 30.f) sandGrit[i].y = 30.f;
        if (sandGrit[i].y > 105.f) sandGrit[i].y = 105.f;
        if (sandGrit[i].x > 255.f) sandGrit[i].active = false;
    }
}

static void drawSandstorm(M5Canvas& canvas, bool flash) {
    if (!isDesert()) return;
    static const uint16_t COL[4] = { 0xB460, 0xC480, 0xD4A0, 0xE5C0 };
    for (int i = 0; i < SAND_GRIT_COUNT; i++) {
        if (!sandGrit[i].active) continue;
        uint16_t c = flash ? 0xFFFF : COL[sandGrit[i].shade & 3];
        int x = (int)sandGrit[i].x;
        int y = (int)sandGrit[i].y;
        if (y < 28 || y > 106 || x < -4 || x > 244) continue;
        int len = 3 + (sandGrit[i].shade & 3) + ((i & 1) ? 2 : 0);
        canvas.drawFastHLine(x, y, len, c);
        if ((i & 2) == 0) canvas.drawFastHLine(x - 1, y + 1, len - 1, COL[(sandGrit[i].shade + 1) & 3]);
        if ((i & 3) == 0) canvas.drawPixel(x + len / 2, y - 1, c);
    }
}

static void updateSnowBanks(uint32_t now) {
    if (!isWinter() && !isDesert()) {
        snowBanksInited = false;
        snowingWas = false;
        for (int i = 0; i < SNOW_BANK_COUNT; i++) snowBanks[i].h = 0;
        for (int i = 0; i < SNOW_SCAR_COUNT; i++) snowScars[i].active = false;
        return;
    }
    if (!snowBanksInited) initSnowBanks();

    const bool snowing = isDesert() ? true : Weather::isSnowing();
    if (!snowing) {
        snowingWas = false;
        return;
    }

    int live = 0;
    for (int i = 0; i < SNOW_BANK_COUNT; i++)
        if (snowBanks[i].h > 0) live++;

    // First flakes this storm: dump a handful off-screen so walking finds snow
    if (!snowingWas) {
        snowingWas = true;
        for (int n = 0; n < 6; n++) trySpawnBank(true);
        for (int n = 0; n < 2; n++) trySpawnBank(false);
        lastBankGrowMs = now;
        return;
    }

    if (now - lastBankGrowMs < 180) return;
    lastBankGrowMs = now;

    // Raise existing drifts (do not widen — that would fill a stamped path)
    live = 0;
    for (int i = 0; i < SNOW_BANK_COUNT; i++) {
        if (snowBanks[i].h < 1) continue;
        live++;
        uint8_t maxH = isDesert() ? 9 : SNOW_BANK_MAX_H;  // higher sand ridges
        if (snowBanks[i].h < maxH && (esp_random() % 100) < (isDesert() ? 40 : 70))
            snowBanks[i].h++;
    }

    if (live >= SNOW_BANK_COUNT - 2) return;  // keep 2 slots free for splits
    int burst = (live < 6) ? 3 : 1;
    for (int n = 0; n < burst; n++) {
        if (!trySpawnBank((esp_random() % 100) < 45)) break;
    }
}

// Same stepped puff as clouds — rounded mound, not a triangle of bars
static void drawSnowPuff(M5Canvas& canvas, int cx, int cy, int r, uint16_t color) {
    if (r <= 1) {
        canvas.fillRect(cx - 1, cy - 1, 3, 3, color);
        return;
    }
    int inset = (r + 1) / 2;
    canvas.fillRect(cx - r, cy - r + inset, r * 2, r * 2 - inset * 2, color);
    canvas.fillRect(cx - r + inset, cy - r, (r - inset) * 2, inset, color);
    canvas.fillRect(cx - r + inset, cy + r - inset, (r - inset) * 2, inset, color);
}

static void drawSnowBanks(M5Canvas& canvas, bool flash) {
    if ((!isWinter() && !isDesert()) || !snowBanksInited) return;
    const int16_t ground = 107;
    const bool sand = isDesert();

    if (sand) {
        // Flat sand ridges — low trapezoids on the ground, NOT snow balls
        const uint16_t S_DARK = flash ? 0xFFFF : (uint16_t)0xB4A0;
        const uint16_t S_MID  = flash ? 0xFFFF : (uint16_t)0xD4A0;
        const uint16_t S_LIT  = flash ? 0xFFFF : (uint16_t)0xE5E0;
        const uint16_t S_TOP  = flash ? 0xFFFF : (uint16_t)0xF6E0;
        for (int i = 0; i < SNOW_BANK_COUNT; i++) {
            int rows = (int)snowBanks[i].h;
            if (rows < 1) continue;
            if (rows > 9) rows = 9;  // higher visible ridges
            int16_t bx = snowBanks[i].x;
            int w = (int)snowBanks[i].w;
            if (w < 26) w = 26;
            if (w > 64) w = 64;
            // stacked horizontal bands, wider at bottom → sand pile profile
            for (int r = 0; r < rows; r++) {
                int inset = r * 2;
                int ww = w - inset * 2;
                if (ww < 6) ww = 6;
                int yy = ground - 1 - r;
                uint16_t c = (r == 0) ? S_DARK : (r == rows - 1) ? S_TOP : ((r & 1) ? S_MID : S_LIT);
                canvas.fillRect(bx + inset, yy, ww, 1, c);
                // soft lit crest
                if (r == rows - 1)
                    canvas.drawFastHLine(bx + inset + 2, yy, ww / 2, S_TOP);
            }
        }
        return;
    }

    // Winter: original snow puffs
    const uint16_t C_TOP   = flash ? 0xFFFF : (uint16_t)0xFFFF;
    const uint16_t C_MID   = flash ? 0xFFFF : (uint16_t)0xEF7D;
    const uint16_t C_SHADE = flash ? 0xFFFF : (uint16_t)0xC618;
    const uint16_t C_RIM   = flash ? 0xFFFF : (uint16_t)0xDEFB;

    for (int i = 0; i < SNOW_BANK_COUNT; i++) {
        int rows = (int)snowBanks[i].h;
        if (rows < 1) continue;
        if (rows > (int)SNOW_BANK_MAX_H) rows = SNOW_BANK_MAX_H;
        int16_t bx = snowBanks[i].x;
        int w = (int)snowBanks[i].w;
        if (w < 12) w = 12;

        int maxR = 4 + rows;
        if (maxR > 12) maxR = 12;

        int nPuff = 2 + w / 16;
        if (nPuff > 5) nPuff = 5;

        for (int p = 0; p < nPuff; p++) {
            int cx = bx + (nPuff == 1 ? w / 2 : (p * w) / (nPuff - 1));
            int r = maxR;
            if (p == 0 || p == nPuff - 1) r = (maxR * 3) / 4;
            if (r < 3) r = 3;
            int cy = ground - r + 2;
            if (cx + r < -4 || cx - r > DISPLAY_W + 4) continue;
            drawSnowPuff(canvas, cx, cy + 2, r + 1, C_SHADE);
            drawSnowPuff(canvas, cx, cy, r, C_MID);
            drawSnowPuff(canvas, cx - 1, cy - 1, (r > 3) ? r - 2 : r - 1, C_TOP);
            drawSnowPuff(canvas, cx + r / 3, cy + 1, 2, C_RIM);
        }
    }
}

void scroll(int dir) {
    const int16_t d = (dir > 0) ? 1 : -1;
    const int16_t width = Trees::worldWidth();
    auto wrapX = [width](int x) {
        x %= width;
        if (x < 0) x += width;
        return (int16_t)x;
    };
    if (snowBanksInited) {
        for (int i = 0; i < SNOW_BANK_COUNT; i++) {
            if (snowBanks[i].h < 1) continue;
            snowBanks[i].x = wrapX(snowBanks[i].x + d);
        }
        for (int i = 0; i < SNOW_SCAR_COUNT; i++) {
            if (!snowScars[i].active) continue;
            snowScars[i].x = wrapX(snowScars[i].x + d);
        }
    }
    for (uint8_t i = 0; i < LEAF_PILE_COUNT; i++)
        if (leafPiles[i].active) leafPiles[i].x = (float)wrapX((int)leafPiles[i].x + d);
}

void trampleSnow(int pigFeetX) {
    if ((!isWinter() && !isDesert()) || !snowBanksInited) return;
    static uint32_t lastTrampleMs = 0;
    uint32_t now = millis();
    if (now - lastTrampleMs < 80) return;
    lastTrampleMs = now;

    const int PATH = 14;  // stamped road under hooves
    const int16_t pathL = (int16_t)(pigFeetX - PATH);
    const int16_t pathR = (int16_t)(pigFeetX + PATH);
    const uint8_t pathW = (uint8_t)(PATH * 2);

    for (int i = 0; i < SNOW_BANK_COUNT; i++) {
        if (snowBanks[i].h < 1) continue;
        int16_t bx = snowBanks[i].x;
        int16_t left = bx;
        int16_t right = (int16_t)(bx + snowBanks[i].w);
        if (pathR <= left || pathL >= right) continue;

        int16_t leftW = (int16_t)(pathL - left);
        int16_t rightW = (int16_t)(right - pathR);
        addScar(pathL, pathW);

        if (leftW >= 10 && rightW >= 10) {
            // Walked the middle: keep both edges, hole in the center
            int extra = freeBankSlot();
            snowBanks[i].x = left;
            snowBanks[i].w = (uint8_t)leftW;
            if (extra >= 0) {
                snowBanks[extra].x = pathR;
                snowBanks[extra].w = (uint8_t)rightW;
                snowBanks[extra].h = snowBanks[i].h;
            }
        } else if (leftW >= 10 && rightW < 10) {
            snowBanks[i].x = left;
            snowBanks[i].w = (uint8_t)leftW;  // ate the right
        } else if (rightW >= 10 && leftW < 10) {
            snowBanks[i].x = pathR;
            snowBanks[i].w = (uint8_t)rightW;
        } else {
            snowBanks[i].h = 0;  // whole drift stomped
            snowBanks[i].w = 0;
        }
        if (snowBanks[i].w < 8) {
            snowBanks[i].h = 0;
            snowBanks[i].w = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Autumn leaves
// ---------------------------------------------------------------------------
static void updateLeaves(uint32_t now) {
    if (!isAutumn()) {
        for (int i = 0; i < LEAF_COUNT; i++) leaves[i].active = false;
        for (int i = 0; i < LEAF_PILE_COUNT; i++) leafPiles[i].active = false;
        lastPigCenterX = -1;
        return;
    }
    if (now - lastLeafUpdate < 35) return;
    lastLeafUpdate = now;

    bool wet = Weather::isRaining();
    // Dense cascade — autumn signature (more on wet wind)
    int want = wet ? LEAF_COUNT : 14;
    int active = 0;
    for (int i = 0; i < LEAF_COUNT; i++) if (leaves[i].active) active++;

    // Spawn up to 2 per tick so sky fills faster
    int spawns = 0;
    while (active < want && spawns < 2 && random(0, 100) < 55) {
        for (int i = 0; i < LEAF_COUNT; i++) {
            if (leaves[i].active) continue;
            leaves[i].active = true;
            leaves[i].x = (float)random(0, DISPLAY_W);
            leaves[i].y = (float)random(-12, 16);
            leaves[i].vx = (float)(random(0, 24) - 10) / 10.0f;
            leaves[i].vy = 0.55f + (float)random(0, 28) / 20.0f;
            leaves[i].col = (uint8_t)(esp_random() % 5);
            leaves[i].size = (uint8_t)random(3, 6);  // chunkier = more visible
            active++;
            spawns++;
            break;
        }
        if (spawns == 0) break;
    }

    for (int i = 0; i < LEAF_COUNT; i++) {
        if (!leaves[i].active) continue;
        leaves[i].x += leaves[i].vx;
        leaves[i].y += leaves[i].vy;
        // Flutter wiggle
        leaves[i].vx += (float)(random(0, 3) - 1) * 0.08f;
        leaves[i].x += sinf(leaves[i].y * 0.12f) * 0.35f;
        if (leaves[i].vx > 1.5f) leaves[i].vx = 1.5f;
        if (leaves[i].vx < -1.5f) leaves[i].vx = -1.5f;
        if (leaves[i].y > 104.0f) {
            int nearest = -1;
            float nearestDistance = 22.0f;
            for (uint8_t p = 0; p < LEAF_PILE_COUNT; p++) {
                if (!leafPiles[p].active) continue;
                const float width = (float)Trees::worldWidth();
                leafPiles[p].x = wrapFieldX(leafPiles[p].x);
                float distance = fabsf(leafPiles[p].x - wrapFieldX(leaves[i].x));
                if (distance > width * 0.5f) distance = width - distance;
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearest = p;
                }
            }
            if (nearest < 0) {
                for (uint8_t p = 0; p < LEAF_PILE_COUNT; p++) {
                    if (leafPiles[p].active) continue;
                    nearest = p;
                    leafPiles[p] = {
                        leaves[i].x, 0, now, 0, leaves[i].col, true
                    };
                    break;
                }
            }
            if (nearest >= 0 && leafPiles[nearest].count < 12) {
                LeafPile& pile = leafPiles[nearest];
                if (pile.count != 0) pile.x = (pile.x + leaves[i].x) * 0.5f;
                pile.count++;
                pile.lastDecayAt = now;
            }
            leaves[i].active = false;
        } else if (leaves[i].x < -12.0f ||
                   leaves[i].x > (float)DISPLAY_W + 12.0f) {
            leaves[i].active = false;
        }
    }

    const int16_t pigCenterX = (int16_t)(Avatar::getCurrentX() + 51);
    const bool jumping = Avatar::isJumping() && Avatar::getJumpLiftPx() > 3;
    int moveDirection = 0;
    if (lastPigCenterX >= 0) {
        const int16_t dx = (int16_t)(pigCenterX - lastPigCenterX);
        if (dx > 0) moveDirection = 1;
        else if (dx < 0) moveDirection = -1;
    }
    if (moveDirection == 0 && Avatar::isGrassMoving())
        moveDirection = Avatar::isGrassDirectionRight() ? -1 : 1;
    lastPigCenterX = pigCenterX;

    for (uint8_t p = 0; p < LEAF_PILE_COUNT; p++) {
        LeafPile& pile = leafPiles[p];
        if (!pile.active) continue;
        if (now - pile.lastDecayAt >= 2500u) {
            pile.lastDecayAt = now;
            if (pile.count > 0) pile.count--;
            if (pile.count == 0) {
                pile.active = false;
                continue;
            }
        }
        pile.x = wrapFieldX(pile.x);
        float distance = fabsf(pile.x - wrapFieldX((float)pigCenterX));
        const float width = (float)Trees::worldWidth();
        if (distance > width * 0.5f) distance = width - distance;
        if (distance > 18.0f) continue;
        if (jumping) {
            for (int n = 0; n < LEAF_COUNT && pile.count > 0; n++) {
                if (leaves[n].active) continue;
                leaves[n] = {
                    pile.x + (float)random(-7, 8),
                    99.0f - (float)random(0, 5),
                    (float)(moveDirection == 0 ? random(-25, 26) : moveDirection * random(12, 34)) / 10.0f,
                    -(float)random(18, 42) / 10.0f,
                    (uint8_t)((pile.col + n) % 5),
                    (uint8_t)random(3, 6),
                    true
                };
                pile.count--;
            }
            if (pile.count == 0) pile.active = false;
            continue;
        }
        if (moveDirection == 0 || now - pile.lastPushAt < 120u) continue;
        pile.lastPushAt = now;
        pile.x += (float)moveDirection * 2.0f;
        const uint8_t scatterCount = pile.count > 2 ? 2 : 1;
        uint8_t scattered = 0;
        for (int n = 0; n < LEAF_COUNT && pile.count > 0; n++) {
            if (leaves[n].active) continue;
            leaves[n] = {
                pile.x,
                99.0f,
                (float)moveDirection * (0.8f + (float)random(0, 10) / 10.0f),
                -(float)random(8, 20) / 10.0f,
                (uint8_t)((pile.col + n) % 5),
                (uint8_t)random(3, 6),
                true
            };
            pile.count--;
            scattered++;
            if (scattered >= scatterCount) break;
        }
        if (pile.count == 0) pile.active = false;
    }
}

static void drawLeaves(M5Canvas& canvas, bool flash) {
    if (!isAutumn()) return;
    // Crimson / gold / orange / rust / brown — bold autumn palette
    static const uint16_t LEAF_COLS[5] = {
        0xE2C0, 0xFD20, 0xFB40, 0xC300, 0x9A40
    };
    for (int i = 0; i < LEAF_COUNT; i++) {
        if (!leaves[i].active) continue;
        int16_t lx = (int16_t)leaves[i].x;
        int16_t ly = (int16_t)leaves[i].y;
        if (lx < -6 || lx >= DISPLAY_W + 6 || ly < -6 || ly >= 106) continue;
        uint16_t c = flash ? 0xFFFF : LEAF_COLS[leaves[i].col % 5];
        int s = leaves[i].size;
        // Fat diamond leaf shape
        canvas.fillRect(lx, ly, s, s - 1, c);
        canvas.fillRect(lx + 1, ly - 1, s - 2, 1, c);
        canvas.drawPixel(lx + s / 2, ly + s - 1, c);  // tip
    }
}

static void drawPixelTwigLine(M5Canvas& canvas, int x1, int y1, int x2,
                              int y2, uint16_t color, uint8_t block = 2) {
    int dx = abs(x2 - x1), dy = abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1;
    int sy = y1 < y2 ? 1 : -1;
    int error = dx - dy;
    for (;;) {
        canvas.fillRect(x1, y1, block, block, color);
        if (x1 == x2 && y1 == y2) break;
        const int twiceError = error * 2;
        if (twiceError > -dy) { error -= dy; x1 += sx; }
        if (twiceError < dx) { error += dx; y1 += sy; }
    }
}

static void drawLeafPiles(M5Canvas& canvas, bool flash) {
    if (!isAutumn()) return;
    static const uint16_t leafColors[5] = {
        0xE2C0, 0xFD20, 0xFB40, 0xC300, 0x9A40
    };
    const int width = Trees::worldWidth();
    for (uint8_t p = 0; p < LEAF_PILE_COUNT; p++) {
        const LeafPile& pile = leafPiles[p];
        if (!pile.active || pile.count == 0) continue;
        int rows = (pile.count + 1) / 2;
        if (rows < 2) rows = 2;
        const uint16_t dark = flash ? 0xFFFF : 0x8A20;
        const int baseX = (int)wrapFieldX(pile.x);
        for (int pass = -1; pass <= 1; pass++) {
            const int x = baseX + pass * width;
            if (x < -16 || x > DISPLAY_W + 16) continue;
            canvas.fillRect(x - 9, 104 - rows * 3, 18, rows * 3, dark);
            for (uint8_t i = 0; i < pile.count; i++) {
                const int spread = (int)(i % 2) * 6 - 3;
                const int row = (int)(i / 2);
                const uint16_t color = flash ? 0xFFFF : leafColors[(pile.col + i) % 5];
                const int leafY = 102 - row * 3;
                canvas.fillRect(x + spread, leafY, 4, 2, color);
                canvas.drawPixel(x + spread + 2, leafY - 1, color);
            }
        }
    }
}

struct MeteorColors {
    uint16_t tail, hot, core, fragments;
};

static MeteorColors meteorColors() {
    switch (Weather::getActiveSeason()) {
        case Season::SPRING: return { 0xF81F, 0xFD2F, 0xFFFF, 0xFBA0 };
        case Season::SUMMER: return { 0xF980, 0xFEA0, 0xFFFF, 0xB3E0 };
        case Season::AUTUMN: return { 0xB280, 0xFCA0, 0xFFE0, 0x8200 };
        case Season::WINTER: return { 0x3D7F, 0x8EFF, 0xFFFF, 0x5DDF };
        case Season::DESERT: return { 0xA280, 0xFC80, 0xFFE0, 0x7240 };
        case Season::NOIR:   return { 0x781F, 0xD21F, 0xFFF0, 0x500F };
        case Season::RETRO:  return { 0x4208, 0x9CF3, 0xFFFF, 0x630C };
        case Season::CITY:   return { 0x3D6F, 0x9E7F, 0xFFFF, 0x5AEB };
        default:             return { 0xFD20, 0xFEA0, 0xFFFF, 0x9A40 };
    }
}

static void startMeteorShower(uint32_t now) {
    meteorShowerActive = true;
    meteorLaunched = 0;
    nextMeteorAt = now;
    meteorShowerEndsAt = now + (uint32_t)random(10000, 15001);
    for (uint8_t i = 0; i < METEOR_FLIGHT_COUNT; i++)
        meteors[i].active = false;
}

static void launchMeteor(uint32_t now) {
    for (uint8_t i = 0; i < METEOR_FLIGHT_COUNT; i++) {
        Meteor& meteor = meteors[i];
        if (meteor.active) continue;
        meteor.x = screenToWorldX((float)random(24, DISPLAY_W - 24));
        meteor.y = -8.0f;
        meteor.vx = (float)random(-8, 9) / 10.0f;
        meteor.vy = 3.0f + (float)random(0, 11) / 10.0f;
        meteor.active = true;
        meteorLaunched++;
        nextMeteorAt = now + (uint32_t)random(500, 1001);
        return;
    }
}

static void spawnMeteorFragments(float worldX, uint32_t now) {
    for (uint8_t i = 0; i < 12; i++) {
        MeteorFragment& bit =
            meteorFragments[meteorFragmentCursor++ % METEOR_FRAGMENT_COUNT];
        bit.x = wrapFieldX(worldX + (float)random(-2, 3));
        bit.y = 98.0f;
        bit.vx = (float)random(-55, 56) / 10.0f;
        bit.vy = (float)random(-58, 8) / 10.0f;
        bit.born = now;
        bit.shade = (uint8_t)(i % 3);
        bit.settled = false;
        bit.active = true;
    }
}

static void updateMeteorShower(uint32_t now) {
    const uint32_t gameDay = now / GAME_DAY_MS;
    if (!meteorShowerActive && gameDay >= nextMeteorGameDay) {
        startMeteorShower(now);
        nextMeteorGameDay = gameDay + METEOR_INTERVAL_DAYS;
    }

    if (meteorShowerActive && (int32_t)(now - meteorShowerEndsAt) < 0 &&
        (int32_t)(now - nextMeteorAt) >= 0)
        launchMeteor(now);

    for (uint8_t i = 0; i < METEOR_FLIGHT_COUNT; i++) {
        Meteor& meteor = meteors[i];
        if (!meteor.active) continue;
        meteor.x = wrapFieldX(meteor.x + meteor.vx);
        meteor.y += meteor.vy;
        if (meteor.y < 98.0f) continue;

        const int16_t impactX = (int16_t)worldToScreenX(meteor.x);
        meteor.active = false;
        if (impactX >= 0 && impactX < DISPLAY_W) {
            spawnMeteorFragments(meteor.x, now);
            Ground::meteorImpact(impactX);
            Trees::meteorImpact(impactX);
            Display::triggerScreenShake(7, 420);
            SFX::play(SFX::THUNDER);
        }
    }

    for (uint8_t i = 0; i < METEOR_FRAGMENT_COUNT; i++) {
        MeteorFragment& bit = meteorFragments[i];
        if (!bit.active) continue;
        if (now - bit.born >= 9000u) {
            bit.active = false;
            continue;
        }
        if (bit.settled) continue;
        bit.x = wrapFieldX(bit.x + bit.vx);
        bit.y += bit.vy;
        bit.vy += 0.16f;
        bit.vx *= 0.97f;
        if (bit.y >= 101.0f) {
            bit.y = 101.0f;
            bit.vx = 0.0f;
            bit.vy = 0.0f;
            bit.settled = true;
        }
    }

    if (meteorShowerActive && (int32_t)(now - meteorShowerEndsAt) >= 0) {
        bool flying = false;
        for (uint8_t i = 0; i < METEOR_FLIGHT_COUNT; i++)
            flying = flying || meteors[i].active;
        if (!flying) meteorShowerActive = false;
    }
}

static void drawMeteorShower(M5Canvas& canvas, bool flash) {
    const MeteorColors colors = meteorColors();
    for (uint8_t i = 0; i < METEOR_FLIGHT_COUNT; i++) {
        const Meteor& meteor = meteors[i];
        if (!meteor.active) continue;
        const int x = (int)worldToScreenX(meteor.x);
        const int y = (int)meteor.y;
        const uint16_t tail = flash ? 0xFFFF : colors.tail;
        const uint16_t hot = flash ? 0xFFFF : colors.hot;
        drawPixelTwigLine(canvas, x - (int)(meteor.vx * 5.0f),
                          y - (int)(meteor.vy * 5.0f), x, y, tail, 3);
        canvas.fillRect(x - 4, y - 4, 8, 8, hot);
        canvas.fillRect(x - 2, y - 2, 4, 4, flash ? 0xFFFF : colors.core);
    }
    for (uint8_t i = 0; i < METEOR_FRAGMENT_COUNT; i++) {
        const MeteorFragment& bit = meteorFragments[i];
        if (!bit.active) continue;
        const int x = (int)worldToScreenX(bit.x);
        const int y = (int)bit.y;
        const uint16_t color = flash ? 0xFFFF
            : (bit.shade == 0 ? colors.hot : colors.fragments);
        const int size = bit.shade == 0 ? 4 : 3;
        canvas.fillRect(x, y, size, size, color);
        if (!bit.settled && bit.shade != 2)
            canvas.fillRect(x - 1, y - 2, size, 2, flash ? 0xFFFF : colors.tail);
    }
}

// ---------------------------------------------------------------------------
// Summer butterflies + pollen (summer signature)
// ---------------------------------------------------------------------------
static void updateButterflies(uint32_t now) {
    if (!isSummer()) {
        for (int i = 0; i < BUTTERFLY_COUNT; i++) butterflies[i].active = false;
        return;
    }

    // Keep 3–4 fluttering — noticeable summer sky life
    int active = 0;
    for (int i = 0; i < BUTTERFLY_COUNT; i++) if (butterflies[i].active) active++;

    if (active < 4 && now - lastButterflySpawnMs > 1800) {
        lastButterflySpawnMs = now;
        for (int i = 0; i < BUTTERFLY_COUNT; i++) {
            if (butterflies[i].active) continue;
            butterflies[i].active = true;
            butterflies[i].x = (float)random(8, DISPLAY_W - 8);
            butterflies[i].y = (float)random(18, 75);
            butterflies[i].vx = ((float)random(0, 24) - 12) / 10.0f;
            butterflies[i].vy = ((float)random(0, 12) - 6) / 10.0f;
            butterflies[i].phase = 0;
            butterflies[i].hue = (uint8_t)(esp_random() % 4);
            break;
        }
    }

    for (int i = 0; i < BUTTERFLY_COUNT; i++) {
        if (!butterflies[i].active) continue;
        butterflies[i].phase++;
        butterflies[i].x += butterflies[i].vx;
        butterflies[i].y += butterflies[i].vy + sinf((float)butterflies[i].phase * 0.22f) * 0.55f;
        if (butterflies[i].x < -10 || butterflies[i].x > DISPLAY_W + 10 ||
            butterflies[i].y < 6 || butterflies[i].y > 98) {
            butterflies[i].active = false;
            lastButterflySpawnMs = now;
        }
        if ((butterflies[i].phase % 36) == 0) {
            butterflies[i].vx = ((float)random(0, 24) - 12) / 10.0f;
            butterflies[i].vy = ((float)random(0, 12) - 6) / 12.0f;
        }
    }
}

static void drawButterflies(M5Canvas& canvas, bool flash) {
    if (!isSummer()) return;
    // Magenta / gold / cyan / orange — bright summer palette
    static const uint16_t HUES[4] = { 0xF81F, 0xFFE0, 0x07FF, 0xFD20 };
    for (int i = 0; i < BUTTERFLY_COUNT; i++) {
        if (!butterflies[i].active) continue;
        int16_t x = (int16_t)butterflies[i].x;
        int16_t y = (int16_t)butterflies[i].y;
        uint16_t c = flash ? 0xFFFF : HUES[butterflies[i].hue % 4];
        // Bigger fat wings so they read on 240×135
        int flap = ((butterflies[i].phase / 3) & 1) ? 3 : 2;
        canvas.fillRect(x - flap - 1, y, flap + 1, 3, c);
        canvas.fillRect(x + 1, y, flap + 1, 3, c);
        canvas.fillRect(x, y, 2, 3, 0x2104);  // body
        // Tiny antenna tips
        canvas.drawPixel(x, y - 1, c);
        canvas.drawPixel(x + 1, y - 1, c);
    }
}

static void updatePollen(uint32_t now) {
    if (!isSummer()) {
        for (int i = 0; i < POLLEN_COUNT; i++) pollen[i].active = false;
        return;
    }
    if (now - lastPollenUpdate < 45) return;
    lastPollenUpdate = now;

    int active = 0;
    for (int i = 0; i < POLLEN_COUNT; i++) if (pollen[i].active) active++;

    // Golden dust always in the air
    if (active < 12 && random(0, 100) < 40) {
        for (int i = 0; i < POLLEN_COUNT; i++) {
            if (pollen[i].active) continue;
            pollen[i].active = true;
            pollen[i].x = (float)random(0, DISPLAY_W);
            pollen[i].y = (float)random(15, 90);
            pollen[i].vx = ((float)random(0, 10) - 3) / 20.0f;
            pollen[i].vy = -0.15f - (float)random(0, 10) / 40.0f;  // slow rise
            pollen[i].phase = (uint8_t)(esp_random() & 0xFF);
            break;
        }
    }

    for (int i = 0; i < POLLEN_COUNT; i++) {
        if (!pollen[i].active) continue;
        pollen[i].phase++;
        pollen[i].x += pollen[i].vx + sinf((float)pollen[i].phase * 0.08f) * 0.2f;
        pollen[i].y += pollen[i].vy;
        if (pollen[i].y < 8 || pollen[i].y > 100 ||
            pollen[i].x < -4 || pollen[i].x > DISPLAY_W + 4) {
            pollen[i].active = false;
        }
    }
}

static void drawPollen(M5Canvas& canvas, bool flash) {
    if (!isSummer()) return;
    // Warm gold / cream motes — summer heat shimmer
    const uint16_t cBright = flash ? 0xFFFF : 0xFFE0;
    const uint16_t cSoft   = flash ? 0xFFFF : 0xF7B0;
    for (int i = 0; i < POLLEN_COUNT; i++) {
        if (!pollen[i].active) continue;
        int16_t x = (int16_t)pollen[i].x;
        int16_t y = (int16_t)pollen[i].y;
        if (x < 0 || x >= DISPLAY_W || y < 0 || y >= 105) continue;
        uint16_t c = (pollen[i].phase & 2) ? cBright : cSoft;
        // 2×2 fat pixel so motes catch the eye
        canvas.fillRect(x, y, 2, 2, c);
    }
}

// Quiet rain drip SFX — sparse so it never nags
static void updateRainSfx(uint32_t now) {
    if (!Weather::isRaining()) {
        lastRainSfxMs = 0;
        return;
    }
    // First drip a bit after rain starts, then every ~2.8–4.5s
    if (lastRainSfxMs == 0) {
        lastRainSfxMs = now;
        return;
    }
    uint32_t gap = 2800 + (esp_random() % 1700);
    if (now - lastRainSfxMs < gap) return;
    lastRainSfxMs = now;
    // Only queue if speaker is idle — never fight thunder/wolf/jump
    if (!SFX::isPlaying())
        SFX::play(SFX::RAIN_TICK);
}

// ---------------------------------------------------------------------------
// Spring lightning bolts (sky layer)
// ---------------------------------------------------------------------------
static void spawnBolt(bool mainBolt) {
    for (int i = 0; i < BOLT_COUNT; i++) {
        if (bolts[i].active) continue;
        bolts[i].active = true;
        bolts[i].x0 = (int16_t)random(20, DISPLAY_W - 20);
        bolts[i].segs = (uint8_t)random(4, 7);
        bolts[i].life = mainBolt ? 5 : 3;
        bolts[i].bright = mainBolt ? 1 : 0;
        int16_t yLeft = 55 + (int)random(0, 25);  // total drop budget
        for (int s = 0; s < bolts[i].segs; s++) {
            bolts[i].segDx[s] = (int8_t)random(-10, 11);
            int step = yLeft / (bolts[i].segs - s);
            if (step < 6) step = 6;
            if (step > 18) step = 18;
            bolts[i].segDy[s] = (int8_t)step;
            yLeft -= step;
            if (yLeft < 0) yLeft = 0;
        }
        return;
    }
}

static void updateLightning(uint32_t now) {
    if (!isSpring()) {
        for (int i = 0; i < BOLT_COUNT; i++) bolts[i].active = false;
        wasThunderFlash = false;
        return;
    }

    // Tick life
    for (int i = 0; i < BOLT_COUNT; i++) {
        if (!bolts[i].active) continue;
        if (bolts[i].life > 0) bolts[i].life--;
        if (bolts[i].life == 0) bolts[i].active = false;
    }

    // Sync with weather thunder flash — bolt appears with the flash
    bool flash = Weather::isThunderFlashing();
    if (flash && !wasThunderFlash) {
        spawnBolt(true);
        if (random(0, 100) < 40) spawnBolt(false);  // secondary fork
        lastBoltSpawnMs = now;
    }
    wasThunderFlash = flash;

    // Distant rumbles during spring storms (no full-screen flash yet)
    if (Weather::isStorming() && !flash && (now - lastBoltSpawnMs > 3500)) {
        if (random(0, 100) < 12) {
            spawnBolt(false);
            lastBoltSpawnMs = now;
        }
    }
}

static void drawLightningBolts(M5Canvas& canvas) {
    if (!isSpring()) return;
    for (int i = 0; i < BOLT_COUNT; i++) {
        if (!bolts[i].active) continue;
        const bool hot = bolts[i].life >= 3 || bolts[i].bright;
        // Core white-yellow, rim pale cyan-blue for comic bolt look
        const uint16_t core = hot ? 0xFFFF : 0xFFE0;   // white / yellow
        const uint16_t glow = hot ? 0xA7FF : 0x5D5F;   // light cyan / dim blue
        int16_t x = bolts[i].x0;
        int16_t y = 0;  // from top of main canvas (under bar)
        for (int s = 0; s < bolts[i].segs; s++) {
            int16_t nx = x + bolts[i].segDx[s];
            int16_t ny = y + bolts[i].segDy[s];
            // Glow stroke (wider)
            canvas.drawLine(x - 1, y, nx - 1, ny, glow);
            canvas.drawLine(x + 1, y, nx + 1, ny, glow);
            // Fat core
            canvas.drawLine(x, y, nx, ny, core);
            canvas.drawLine(x, y + 1, nx, ny + 1, core);
            // Branch tip spark
            if (s == bolts[i].segs - 1 || (s > 0 && (s & 1))) {
                canvas.fillRect(nx - 1, ny - 1, 3, 3, core);
            }
            // Small side fork
            if (s == 2 && bolts[i].bright) {
                int16_t fx = nx + ((i & 1) ? 8 : -8);
                int16_t fy = ny + 10;
                canvas.drawLine(nx, ny, fx, fy, glow);
                canvas.drawLine(nx, ny, fx, fy - 1, core);
            }
            x = nx;
            y = ny;
            if (y > 90) break;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void init() {
    reset();
    const uint32_t gameDay = millis() / GAME_DAY_MS;
    nextMeteorGameDay = (gameDay / METEOR_INTERVAL_DAYS + 1u) *
                        METEOR_INTERVAL_DAYS;
}

void reset() {
    snowBanksInited = false;
    snowingWas = false;
    for (int i = 0; i < SNOW_BANK_COUNT; i++) snowBanks[i].h = 0;
    for (int i = 0; i < SNOW_SCAR_COUNT; i++) snowScars[i].active = false;
    for (int i = 0; i < LEAF_COUNT; i++) leaves[i].active = false;
    for (uint8_t i = 0; i < LEAF_PILE_COUNT; i++) leafPiles[i].active = false;
    lastPigCenterX = -1;
    lastLeafUpdate = 0;
    lastBankGrowMs = 0;
    lastRainSfxMs = 0;
    for (int i = 0; i < BUTTERFLY_COUNT; i++) butterflies[i].active = false;
    lastButterflySpawnMs = 0;
    for (int i = 0; i < POLLEN_COUNT; i++) pollen[i].active = false;
    lastPollenUpdate = 0;
    for (int i = 0; i < BOLT_COUNT; i++) bolts[i].active = false;
    lastBoltSpawnMs = 0;
    wasThunderFlash = false;
    meteorShowerActive = false;
    meteorLaunched = 0;
    meteorShowerEndsAt = 0;
    for (uint8_t i = 0; i < METEOR_FLIGHT_COUNT; i++) meteors[i].active = false;
    for (uint8_t i = 0; i < METEOR_FRAGMENT_COUNT; i++)
        meteorFragments[i].active = false;
    meteorFragmentCursor = 0;
}

void update() {
    uint32_t now = millis();
    updateMeteorShower(now);
    // RETRO film world: only pixel rain (weather module) — no color FX
    if (Weather::getActiveSeason() == Season::RETRO) return;
    if (isNoir()) {
        updateMoths(now);
        return;
    }
    updateSnowBanks(now);
    updateSandstorm(now);
    updateLeaves(now);
    updateButterflies(now);
    updatePollen(now);
    updateLightning(now);
    updateRainSfx(now);
}

void triggerMeteorShower() {
    startMeteorShower(millis());
}

void drawBackdrop(M5Canvas& canvas) {
    if (Weather::getActiveSeason() == Season::RETRO) return;
    if (isNoir()) return;
    // Sky-layer only (behind tree/pig)
    drawLightningBolts(canvas);
}

void draw(M5Canvas& canvas) {
    const bool flash = Weather::isThunderFlashing();
    if (Weather::getActiveSeason() == Season::RETRO) {
        drawMeteorShower(canvas, flash);
        return;
    }
    if (isNoir()) {
        drawMoths(canvas, flash);
        drawMeteorShower(canvas, flash);
        return;
    }
    // Order: ground decor first, then air
    drawSnowBanks(canvas, flash);
    drawSandstorm(canvas, flash);
    drawLeaves(canvas, flash);
    drawLeafPiles(canvas, flash);
    drawMeteorShower(canvas, flash);
    drawPollen(canvas, flash);
    drawButterflies(canvas, flash);
}

}  // namespace SeasonalFx
