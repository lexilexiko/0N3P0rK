// Falling fruit / berries — world-scrolled, foreground draw + collect.

#include "trees_drops.h"
#include "trees.h"
#include "avatar.h"
#include "weather.h"
#include "../ui/display.h"
#include "../core/config.h"
#include "../core/xp.h"
#include <esp_random.h>

namespace Trees {

static constexpr int16_t PX = 3;
static inline int16_t snapPx(int16_t v) {
    return (v >= 0) ? (v / PX) * PX : ((v - (PX - 1)) / PX) * PX;
}

static bool thunder() { return Avatar::isThunderFlashing(); }
static uint16_t fl(uint16_t c) {
    if (!thunder()) return c;
    uint16_t r = ((c >> 11) + 31) >> 1;
    uint16_t g = (((c >> 5) & 0x3F) + 63) >> 1;
    uint16_t b = ((c & 0x1F) + 31) >> 1;
    return (r << 11) | (g << 5) | b;
}

// Produce palette — bright fruit, soft rim (never near-black)
static constexpr uint16_t C_APPLE_RED    = 0xF800;  // pure red body
static constexpr uint16_t C_APPLE_RED2   = 0xC800;  // warm mid shade
static constexpr uint16_t C_APPLE_RED3   = 0xE000;  // deep red (not black)
static constexpr uint16_t C_APPLE_RED_H  = 0xFED6;  // soft pink highlight
static constexpr uint16_t C_APPLE_YEL    = 0xFFE0;
static constexpr uint16_t C_APPLE_YEL2   = 0xD5C0;
static constexpr uint16_t C_APPLE_YEL3   = 0xC540;
static constexpr uint16_t C_APPLE_YEL_H  = 0xFFFB;
static constexpr uint16_t C_STEM   = 0x5A20;  // brown stem
static constexpr uint16_t C_LEAF   = 0x2C00;  // tiny leaf
// Acorns — warmer gold/orange so they pop on autumn leaves (not mud-brown)
static constexpr uint16_t C_CONE   = 0xFD20;  // bright amber nut
static constexpr uint16_t C_CONE2  = 0xC300;  // deep orange cap
static constexpr uint16_t C_CONE_H = 0xFFE0;  // yellow highlight
// Spring cherries — deep red pair (read well on pink blossom canopy)
static constexpr uint16_t C_CHERRY   = 0xE000;  // cherry red
static constexpr uint16_t C_CHERRY2  = 0x9800;  // deep shade
static constexpr uint16_t C_CHERRY_H = 0xFAD0;  // soft highlight
static constexpr uint16_t C_CHERRY_STEM = 0x4A20;
// Berries stay magenta-pink (already contrasty)
static constexpr uint16_t C_BERRY  = 0xD81F;
static constexpr uint16_t C_BERRY2 = 0xF81F;
static constexpr uint16_t C_BERRY_H= 0xFCDF;
// True fir cones stay brown (winter tree is dark green — brown is fine)
static constexpr uint16_t C_FIR_CONE  = 0x9A40;
static constexpr uint16_t C_FIR_CONE2 = 0x6200;
static constexpr uint16_t C_FIR_CONE_H= 0xDE20;

// Stamp one fat pixel cell at grid (lx, ly) relative to center gx,gy
static void cell(M5Canvas& canvas, int16_t gx, int16_t gy, int lx, int ly, uint16_t col) {
    canvas.fillRect(gx + lx * PX, gy + ly * PX, PX, PX, col);
}

struct Drop {
    int16_t x, y;           // world/screen X; spawn Y (fall from here)
    uint8_t r;
    uint32_t dropStart;
    uint32_t groundSince;   // 0 = still falling
    bool active;
    Produce produce;
};

struct Splash {
    float x, y, vx, vy;
    uint32_t t;
    bool active;
    Produce produce;
};

static constexpr uint8_t MAX_DROP = 8;
static constexpr uint32_t GROUND_MS = 9000;
static Drop drops[MAX_DROP];
static Splash splashes[8];
static uint8_t splashIdx = 0;

static int16_t wrapFieldX(int x) {
    const int16_t width = worldWidth();
    x %= width;
    if (x < 0) x += width;
    return (int16_t)x;
}

static int16_t fallY(const Drop& d, uint32_t now) {
    if (d.groundSince != 0) return DROP_GROUND_Y;
    float tt = (float)(now - d.dropStart) / 1000.0f;
    int16_t cy = d.y + (int16_t)(0.5f * 800.0f * tt * tt);
    if (cy >= DROP_GROUND_Y) return DROP_GROUND_Y;
    return cy;
}

void dropsReset() {
    for (int i = 0; i < MAX_DROP; i++) drops[i].active = false;
    for (int i = 0; i < 8; i++) splashes[i].active = false;
    splashIdx = 0;
}

void dropsScroll(int dir) {
    // Same sign as tree Slot::scroll: +1 moves flora right on screen
    const int16_t d = (dir > 0) ? 1 : -1;
    for (uint8_t i = 0; i < MAX_DROP; i++) {
        if (!drops[i].active) continue;
        drops[i].x = wrapFieldX(drops[i].x + d);
    }
    for (uint8_t i = 0; i < 8; i++) {
        if (!splashes[i].active) continue;
        splashes[i].x = (float)wrapFieldX((int)(splashes[i].x + (float)d));
    }
}

bool dropsSpawn(int16_t x, int16_t y, uint8_t r, Produce p) {
    // Fruit lives in screen space and follows the same finite camera scroll.
    if (y < 0) y = 0;
    if (y > DROP_GROUND_Y - 4) y = DROP_GROUND_Y - 4;
    if (r < 2) r = 2;

    for (uint8_t i = 0; i < MAX_DROP; i++) {
        if (drops[i].active) continue;
        drops[i].x = wrapFieldX(x);
        drops[i].y = y;
        drops[i].r = r;
        drops[i].dropStart = millis();
        drops[i].groundSince = 0;
        drops[i].active = true;
        drops[i].produce = p;
        return true;
    }
    return false;  // pool full — caller still may remove from tree
}

// Size tiers: apples always compact (tier 0); rare "plump" = tier 1 still 3-wide
static int produceSizeTier(int r) {
    if (r <= 3) return 0;
    return 1;
}

static bool isRetroSeason() {
    return Weather::getActiveSeason() == Season::RETRO;
}

// Rounded apple — compact only (red was reading as watermelon).
// color: 0=red, 1=yellow, 2=green (autumn bush)
static void drawApple(M5Canvas& canvas, int16_t gx, int16_t gy, int tier, int color) {
    uint16_t body, mid, deep, hi;
    if (isRetroSeason()) {
        // B&W film fruit
        body = fl(0xAD55); mid = fl(0x8410); deep = fl(0x632C); hi = fl(0xDEFB);
    } else if (color == 1) {
        body = fl(C_APPLE_YEL); mid = fl(C_APPLE_YEL2); deep = fl(C_APPLE_YEL3); hi = fl(C_APPLE_YEL_H);
    } else if (color == 2) {
        // Soft green apple — muted, harmonizes with autumn canopy (not neon)
        body = fl(0x8D40);   // soft olive-green body
        mid  = fl(0x6400);   // deeper olive shade
        deep = fl(0x4200);   // dark green
        hi   = fl(0xC6E0);   // warm pale highlight (not toxic lime)
    } else if (color == 3) {
        body = fl(0xFE60);
        mid  = fl(0xD560);
        deep = fl(0xAB40);
        hi   = fl(0xFFF1);
    } else {
        body = fl(C_APPLE_RED); mid = fl(C_APPLE_RED2); deep = fl(C_APPLE_RED3); hi = fl(C_APPLE_RED_H);
    }
    uint16_t stem = fl(isRetroSeason() ? 0x4208 : C_STEM);
    uint16_t leaf = fl(isRetroSeason() ? 0x7BEF : C_LEAF);

    // Core round apple (always)
    //   .
    //  ###
    //   #
    cell(canvas, gx, gy,  0, -2, stem);
    if (tier >= 1) cell(canvas, gx, gy,  1, -2, leaf);  // plump: tiny leaf
    cell(canvas, gx, gy,  0, -1, hi);
    cell(canvas, gx, gy, -1,  0, body);
    cell(canvas, gx, gy,  0,  0, body);
    cell(canvas, gx, gy,  1,  0, mid);
    cell(canvas, gx, gy,  0,  1, deep);
    if (tier >= 1) {
        // Slightly fuller but still apple-sized (not 5-wide)
        cell(canvas, gx, gy, -1, -1, body);
        cell(canvas, gx, gy,  1, -1, mid);
        cell(canvas, gx, gy, -1,  1, deep);
    }
}

static void drawBerry(M5Canvas& canvas, int16_t gx, int16_t gy, int tier) {
    uint16_t a = fl(isRetroSeason() ? 0x8410 : C_BERRY);
    uint16_t b = fl(isRetroSeason() ? 0xAD55 : C_BERRY2);
    uint16_t h = fl(isRetroSeason() ? 0xDEFB : C_BERRY_H);
    if (tier <= 0) {
        // tiny round berry
        cell(canvas, gx, gy,  0, -1, h);
        cell(canvas, gx, gy, -1,  0, a);
        cell(canvas, gx, gy,  0,  0, b);
        cell(canvas, gx, gy,  1,  0, a);
        cell(canvas, gx, gy,  0,  1, a);
        return;
    }
    // slightly larger plump berry
    cell(canvas, gx, gy,  0, -2, h);
    cell(canvas, gx, gy, -1, -1, a);
    cell(canvas, gx, gy,  0, -1, b);
    cell(canvas, gx, gy,  1, -1, a);
    cell(canvas, gx, gy, -1,  0, a);
    cell(canvas, gx, gy,  0,  0, b);
    cell(canvas, gx, gy,  1,  0, a);
    cell(canvas, gx, gy,  0,  1, a);
}

static void drawAcorn(M5Canvas& canvas, int16_t gx, int16_t gy, int tier) {
    // Bright amber acorn + dark outline so it reads on orange oak leaves
    uint16_t cap = fl(isRetroSeason() ? 0x632C : C_CONE2);
    uint16_t nut = fl(isRetroSeason() ? 0x9CF3 : C_CONE);
    uint16_t hi  = fl(isRetroSeason() ? 0xDEFB : C_CONE_H);
    uint16_t out = fl(isRetroSeason() ? 0x2104 : 0x8000);
    cell(canvas, gx, gy, -1, -1, out);
    cell(canvas, gx, gy,  0, -1, cap);
    cell(canvas, gx, gy,  1, -1, out);
    cell(canvas, gx, gy,  0, -2, hi);  // stem nub
    cell(canvas, gx, gy, -1,  0, nut);
    cell(canvas, gx, gy,  0,  0, hi);
    cell(canvas, gx, gy,  1,  0, nut);
    cell(canvas, gx, gy,  0,  1, nut);
    if (tier >= 1) {
        cell(canvas, gx, gy, -1,  1, cap);
        cell(canvas, gx, gy,  1,  1, cap);
    }
}

static void drawCone(M5Canvas& canvas, int16_t gx, int16_t gy, int tier) {
    // Winter fir cones — brown (tree is dark; snow helps silhouette)
    uint16_t a = fl(isRetroSeason() ? 0x7BEF : C_FIR_CONE);
    uint16_t b = fl(isRetroSeason() ? 0x4A49 : C_FIR_CONE2);
    uint16_t h = fl(isRetroSeason() ? 0xC618 : C_FIR_CONE_H);
    // tapered cone (point up)
    cell(canvas, gx, gy,  0, -2, h);
    cell(canvas, gx, gy, -1, -1, b);
    cell(canvas, gx, gy,  0, -1, a);
    cell(canvas, gx, gy,  1, -1, b);
    cell(canvas, gx, gy, -1,  0, a);
    cell(canvas, gx, gy,  0,  0, a);
    cell(canvas, gx, gy,  1,  0, b);
    if (tier >= 1) {
        cell(canvas, gx, gy, -1,  1, b);
        cell(canvas, gx, gy,  0,  1, a);
        cell(canvas, gx, gy,  1,  1, b);
    }
}

static void drawCherry(M5Canvas& canvas, int16_t gx, int16_t gy, int tier) {
    // Paired hanging cherries — spring signature fruit
    //     |
    //    / \
    //   o   o
    uint16_t body = fl(isRetroSeason() ? 0x9CF3 : C_CHERRY);
    uint16_t deep = fl(isRetroSeason() ? 0x632C : C_CHERRY2);
    uint16_t hi   = fl(isRetroSeason() ? 0xDEFB : C_CHERRY_H);
    uint16_t stem = fl(isRetroSeason() ? 0x4208 : C_CHERRY_STEM);
    // stem fork
    cell(canvas, gx, gy,  0, -2, stem);
    cell(canvas, gx, gy, -1, -1, stem);
    cell(canvas, gx, gy,  1, -1, stem);
    // left cherry
    cell(canvas, gx, gy, -2,  0, body);
    cell(canvas, gx, gy, -1,  0, hi);
    cell(canvas, gx, gy, -2,  1, deep);
    cell(canvas, gx, gy, -1,  1, body);
    // right cherry
    cell(canvas, gx, gy,  1,  0, hi);
    cell(canvas, gx, gy,  2,  0, body);
    cell(canvas, gx, gy,  1,  1, body);
    cell(canvas, gx, gy,  2,  1, deep);
    if (tier >= 1) {
        // slightly fuller cherries
        cell(canvas, gx, gy, -2, -1, deep);
        cell(canvas, gx, gy,  2, -1, deep);
        cell(canvas, gx, gy,  0,  0, stem);
    }
}

void drawProduce(M5Canvas& canvas, int16_t cx, int16_t cy, int r, Produce p) {
    int16_t gx = snapPx(cx), gy = snapPx(cy);
    int tier = produceSizeTier(r);

    switch (p) {
        case Produce::RED_APPLE:
            drawApple(canvas, gx, gy, tier, 0);
            break;
        case Produce::YELLOW_APPLE:
            drawApple(canvas, gx, gy, tier, 1);
            break;
        case Produce::GREEN_APPLE:
            drawApple(canvas, gx, gy, tier, 2);
            break;
        case Produce::GOLD_APPLE:
            drawApple(canvas, gx, gy, tier, 3);
            break;
        case Produce::BERRY:
            drawBerry(canvas, gx, gy, tier);
            break;
        case Produce::ACORN:
            drawAcorn(canvas, gx, gy, tier);
            break;
        case Produce::CONE:
            drawCone(canvas, gx, gy, tier);
            break;
        case Produce::CHERRY:
            drawCherry(canvas, gx, gy, tier);
            break;
        default:
            drawBerry(canvas, gx, gy, tier);
            break;
    }
}

bool tryCollectNearbyFruit(int pigCenterX, int pigFeetY, int radius) {
    // Generous oval: ground fruit sits at DROP_GROUND_Y, pig feet ~106
    const int radY = radius + 18;
    uint32_t now = millis();

    for (uint8_t i = 0; i < MAX_DROP; i++) {
        if (!drops[i].active) continue;
        int16_t fy = fallY(drops[i], now);
        int dx = abs((int)wrapFieldX(drops[i].x) - wrapFieldX(pigCenterX));
        const int width = worldWidth();
        if (dx > width / 2) dx = width - dx;
        int dy = (int)fy - pigFeetY;
        if (dy < 0) dy = -dy;
        // Prefer ground pickups: slightly larger X once landed
        const int radX = (drops[i].groundSince != 0) ? (radius + 14) : (radius + 10);
        if (dx <= radX && dy <= radY) {
            drops[i].active = false;
            if (drops[i].produce == Produce::GOLD_APPLE)
                XP::addXP(XPEvent::GOLD_APPLE);
            else
                XP::addXP(XPEvent::FRUIT_PICKED);
            return true;
        }
    }
    return false;
}

void drawDropsForeground(M5Canvas& canvas) {
    uint32_t now = millis();
    for (uint8_t i = 0; i < MAX_DROP; i++) {
        if (!drops[i].active) continue;
        int16_t cy;
        if (drops[i].groundSince != 0) {
            cy = DROP_GROUND_Y;
            if (now - drops[i].groundSince > GROUND_MS) {
                drops[i].active = false;
                continue;
            }
        } else {
            float tt = (float)(now - drops[i].dropStart) / 1000.0f;
            cy = drops[i].y + (int16_t)(0.5f * 800.0f * tt * tt);
            if (cy >= DROP_GROUND_Y) {
                cy = DROP_GROUND_Y;
                drops[i].groundSince = now;
                Splash& sp = splashes[splashIdx];
                splashIdx = (splashIdx + 1) % 8;
                sp.x = (float)drops[i].x;
                sp.y = (float)DROP_GROUND_Y + 3.f;
                sp.vx = ((float)(esp_random() % 400) - 200.f) / 100.f;
                sp.vy = -1.5f;
                sp.t = now;
                sp.active = true;
                sp.produce = drops[i].produce;
            }
        }
        const int16_t x = wrapFieldX(drops[i].x);
        const int16_t width = worldWidth();
        for (int pass = -1; pass <= 1; pass++) {
            const int16_t drawX = (int16_t)(x + pass * width);
            if (drawX < -20 || drawX > DISPLAY_W + 20) continue;
            drawProduce(canvas, drawX, cy, drops[i].r, drops[i].produce);
        }
    }

    for (uint8_t i = 0; i < 8; i++) {
        if (!splashes[i].active) continue;
        float pr = (float)(now - splashes[i].t) / 500.f;
        if (pr >= 1.f) { splashes[i].active = false; continue; }
        int16_t sx = wrapFieldX((int)(splashes[i].x + splashes[i].vx * pr * 20));
        int16_t sy = (int16_t)(splashes[i].y + splashes[i].vy * pr * 20 + 40 * pr * pr);
        // Splash tint matches produce (not always red apple)
        uint16_t col = fl(isRetroSeason() ? 0xAD55 : C_APPLE_RED);
        if (!isRetroSeason()) {
            switch (splashes[i].produce) {
                case Produce::YELLOW_APPLE: col = fl(C_APPLE_YEL); break;
                case Produce::GREEN_APPLE:  col = fl(0x8D40); break;
                case Produce::GOLD_APPLE:   col = fl(0xFE60); break;
                case Produce::CHERRY:       col = fl(C_CHERRY); break;
                case Produce::ACORN:        col = fl(C_CONE); break;
                case Produce::CONE:         col = fl(C_FIR_CONE); break;
                case Produce::BERRY:        col = fl(C_BERRY); break;
                default: break;
            }
        } else {
            switch (splashes[i].produce) {
                case Produce::BERRY: col = fl(0x8410); break;
                case Produce::CONE:
                case Produce::ACORN: col = fl(0x7BEF); break;
                default: break;
            }
        }
        const int16_t width = worldWidth();
        for (int pass = -1; pass <= 1; pass++) {
            const int16_t drawX = (int16_t)(sx + pass * width);
            if (drawX < -PX || drawX >= DISPLAY_W) continue;
            canvas.fillRect(drawX, sy, PX, PX, col);
        }
    }
}

}  // namespace Trees
