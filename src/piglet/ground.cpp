// Ground layer — grass / pavement / pebbles + treadmill scroll coupling to Trees/FX.
#include "ground.h"
#include "trees.h"
#include "seasonal_fx.h"
#include "props.h"
#include "cards_table.h"
#include "friend_pig.h"
#include "weather.h"
#include "avatar.h"
#include "../ui/display.h"
#include <esp_random.h>
#include <math.h>
#include <string.h>

namespace Ground {

static Blade s_blades[BLADE_COUNT];
static int16_t s_offset = 0;
static uint16_t s_speed = 80;
static uint32_t s_lastUpdate = 0;


static constexpr int16_t PX = 3;

static inline int16_t snapPx(int16_t v) {
    return (v >= 0) ? (int16_t)((v / PX) * PX) : (int16_t)(((v - 2) / PX) * PX);
}

static void fatLine(M5Canvas& canvas, int16_t x1, int16_t y1,
                    int16_t x2, int16_t y2, uint16_t color) {
    int dx = abs((int)x2 - (int)x1), dy = abs((int)y2 - (int)y1);
    int sx = (x1 < x2) ? 1 : -1, sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;
    int x = x1, y = y1;
    for (;;) {
        if (x >= 0 && x < 240 && y >= 0 && y < 135) {
            canvas.drawPixel(x, y, color);
            if (x + 1 < 240) canvas.drawPixel(x + 1, y, color);
        }
        if (x == x2 && y == y2) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx)  { err += dx; y += sy; }
    }
}

static uint16_t flash(uint16_t c) {
    if (!Avatar::isThunderFlashing()) return c;
    uint16_t r = ((c >> 11) + 31) >> 1;
    uint16_t g = (((c >> 5) & 0x3F) + 63) >> 1;
    uint16_t b = ((c & 0x1F) + 31) >> 1;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void grassStroke(M5Canvas& canvas, int16_t x1, int16_t y1,
                        int16_t x2, int16_t y2, uint16_t color, uint8_t thick) {
    int dx = abs((int)x2 - (int)x1), dy = abs((int)y2 - (int)y1);
    int sx = (x1 < x2) ? 1 : -1, sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;
    int x = x1, y = y1;
    for (;;) {
        if (x >= 0 && x < 240 && y >= 0 && y < 135) {
            canvas.drawPixel(x, y, color);
            if (thick >= 2 && x + 1 < 240) canvas.drawPixel(x + 1, y, color);
        }
        if (x == x2 && y == y2) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx)  { err += dx; y += sy; }
    }
}

// Tapered stem: thick root/mid → thinner bright tip (thick 1..2 from grass style)
static void drawGrassStem(M5Canvas& canvas, int16_t bx, int16_t by,
                          int16_t tipX, int16_t tipY, uint16_t colBase,
                          uint16_t colMid, uint16_t colTip, uint8_t thick) {
    if (thick < 1) thick = 1;
    if (thick > 2) thick = 2;
    int16_t midX = (int16_t)(bx + (tipX - bx) / 2);
    int16_t midY = (int16_t)(by + (tipY - by) / 2);
    int16_t q3x  = (int16_t)(midX + (tipX - midX) / 2);
    int16_t q3y  = (int16_t)(midY + (tipY - midY) / 2);
    grassStroke(canvas, bx, by, midX, midY, colBase, thick);
    grassStroke(canvas, midX, midY, q3x, q3y, colMid, thick);
    grassStroke(canvas, q3x, q3y, tipX, tipY, colTip, (thick >= 2) ? 1 : 1);
    if (tipX >= 0 && tipX < 240 && tipY >= 0 && tipY < 135) {
        canvas.drawPixel(tipX, tipY, colTip);
        if (tipY > 0) canvas.drawPixel(tipX, tipY - 1, colTip);
    }
}



void begin() {
    resetBlades();
    s_speed = 80;
    s_lastUpdate = millis();
}

void setSpeed(uint16_t ms) {
    if (ms < 10) ms = 10;
    s_speed = ms;
}

uint16_t getSpeed() { return s_speed; }

int16_t offset() { return s_offset; }

void resetBlades() {
    s_offset = 0;
    for (int i = 0; i < BLADE_COUNT; i++) {
        uint8_t r = (uint8_t)(esp_random() % 100);
        if (r < 10) {
            s_blades[i].kind = 4;
            s_blades[i].height = random(5, 9);
            s_blades[i].width = 2;
        } else if (r < 55) {
            s_blades[i].kind = 0;
            s_blades[i].height = random(11, 20);
            s_blades[i].width = 2;
        } else if (r < 85) {
            s_blades[i].kind = 1;
            s_blades[i].height = random(10, 18);
            s_blades[i].width = 2;
        } else if (r < 95) {
            s_blades[i].kind = 2;
            s_blades[i].height = random(11, 16);
            s_blades[i].width = 1;
        } else {
            s_blades[i].kind = 3;
            s_blades[i].height = 2;
            s_blades[i].width = 2;
        }
        s_blades[i].lean = (int8_t)random(-3, 4);
        s_blades[i].shade = (uint8_t)(esp_random() % 4);
        s_blades[i].windPhase = (uint16_t)(i * 197);
        s_blades[i].frontLayer =
            s_blades[i].kind != 3 && random(0, 100) < 65;
    }
}



void updateScroll(bool moving, bool directionRight, int steps) {
    if (!moving) return;

    uint32_t now = millis();
    uint32_t pixelInterval = s_speed / STRIDE;
    if (pixelInterval < 1) pixelInterval = 1;
    if (now - s_lastUpdate < pixelInterval) return;
    s_lastUpdate = now;
    if (steps < 1) steps = 1;

    for (int n = 0; n < steps; n++) {
        if (directionRight) {
            s_offset++;
            Trees::scroll(+1);
            SeasonalFx::scroll(+1);
            Props::scroll(+1);
            CardsTable::scroll(+1);
            FriendPig::scroll(+1);
            if (s_offset >= STRIDE) {
                s_offset = 0;
                Blade last = s_blades[BLADE_COUNT - 1];
                for (int i = BLADE_COUNT - 1; i > 0; i--) {
                    s_blades[i] = s_blades[i - 1];
                }
                s_blades[0] = last;
            }
        } else {
            s_offset--;
            Trees::scroll(-1);
            SeasonalFx::scroll(-1);
            Props::scroll(-1);
            CardsTable::scroll(-1);
            FriendPig::scroll(-1);
            if (s_offset < 0) {
                s_offset = STRIDE - 1;
                Blade first = s_blades[0];
                for (int i = 0; i < BLADE_COUNT - 1; i++) {
                    s_blades[i] = s_blades[i + 1];
                }
                s_blades[BLADE_COUNT - 1] = first;
            }
        }
    }

}


void draw(M5Canvas& canvas, bool frontLayer, const DrawCtx& ctx) {
    // Scroll is driven by Avatar::update via updateScroll(); draw is pure paint.

    uint32_t now = millis();
    const int16_t baseY = 106;  // Ground line — pig hooves sit here

    bool shakeActive = Display::isShaking();
    float shakeDecay = shakeActive ? Display::getShakeDecay() : 0.0f;
    uint8_t shakeIntensity = shakeActive ? Display::getShakeIntensity() : 0;

    // Season drives palette (classic fat geometry always)
    Season season = Weather::getActiveSeason();
    const bool isSpring = (season == Season::SPRING);
    const bool isAutumn = (season == Season::AUTUMN);
    const bool isWinter = (season == Season::WINTER);
    const bool isRetro  = (season == Season::RETRO);
    const bool isNoir   = (season == Season::NOIR);
    const bool isCity   = (season == Season::CITY);
    const bool isDesert = (season == Season::DESERT);
    // SUMMER = classic summer greens

    const bool wetGrass = Weather::isRaining();
    const uint16_t dirtMid  = flash(isDesert ? 0xD4A0 : (isCity ? 0x6B6D : (isNoir ? 0x2104 : (isRetro ? 0x4208 : 0x8A40))));
    const uint16_t dirtDark = flash(isDesert ? 0x9A40 : (isCity ? 0x4208 : (isNoir ? 0x1082 : (isRetro ? 0x2104 : (isWinter ? 0x6B6D : 0x5140)))));
    const uint16_t dirtLite = flash(isDesert ? 0xE5E0 : (isCity ? 0x9CF3 : (isNoir ? 0x5A00 : (isRetro ? 0x8410 : (isWinter ? 0xC618 : 0xA3E0)))));

    // Seasonal fat-blade palettes (same geometry as classic)
    // SUMMER = deep classic green; SPRING = fresh yellow-lime + flower carpet
    static const uint16_t PAL_SUMMER_BASE[4] = { 0x2C60, 0x3D00, 0x4DA0, 0x65C0 };
    static const uint16_t PAL_SUMMER_TIP[4]  = { 0x5DE0, 0x7E60, 0x9F00, 0xBFE0 };
    // Spring: chartreuse / young-shoot yellow-green (clearly not summer)
    static const uint16_t PAL_SPRING_BASE[4] = { 0x4C80, 0x6DA0, 0x8F00, 0xAFE0 };
    static const uint16_t PAL_SPRING_TIP[4]  = { 0xBFE0, 0xDFF2, 0xEFF8, 0xF7FE };
    static const uint16_t PAL_AUTUMN_BASE[4] = { 0x5A20, 0x8AC0, 0xB340, 0xC300 };
    static const uint16_t PAL_AUTUMN_TIP[4]  = { 0xD280, 0xE2C0, 0xFBE0, 0xFD20 };
    // Winter: frosty blue-gray grass with ice tips (the look you liked)
    static const uint16_t PAL_WINTER_BASE[4] = { 0x3C8E, 0x4D10, 0x5DB4, 0x7C9A };
    static const uint16_t PAL_WINTER_TIP[4]  = { 0x9CF3, 0xBDF7, 0xDEFB, 0xFFFF };

    const uint16_t* GRASS_BASE = PAL_SUMMER_BASE;
    const uint16_t* GRASS_TIP  = PAL_SUMMER_TIP;
    uint16_t turf0 = flash(wetGrass ? 0x2C40 : 0x3480);
    uint16_t turf1 = flash(wetGrass ? 0x3C60 : 0x4D00);
    uint16_t turf2 = flash(wetGrass ? 0x2C20 : 0x45A0);
    static const uint16_t FLOWER_SUMMER[4] = { 0xF800, 0xFFE0, 0xFD1F, 0xFFFF };
    // Spring blooms: white daisy, yellow dandelion, pink, violet
    static const uint16_t FLOWER_SPRING[4] = { 0xFFFF, 0xFFE0, 0xFD1F, 0xB01F };
    static const uint16_t FLOWER_AUTUMN[4] = { 0xFBE0, 0xFD20, 0xD280, 0xC300 };  // gold/orange
    static const uint16_t FLOWER_WINTER[4] = { 0xFFFF, 0xDEFB, 0xBDF7, 0x9CF3 };  // frost
    const uint16_t* FLOWER_COLS = FLOWER_SUMMER;

    if (isSpring) {
        GRASS_BASE = PAL_SPRING_BASE;
        GRASS_TIP  = PAL_SPRING_TIP;
        // Lush lime turf carpet (yellower than summer)
        turf0 = flash(wetGrass ? 0x4C60 : 0x6D80);
        turf1 = flash(0x8F20);
        turf2 = flash(0xBFE0);
        FLOWER_COLS = FLOWER_SPRING;
    } else if (isAutumn) {
        GRASS_BASE = PAL_AUTUMN_BASE;
        GRASS_TIP  = PAL_AUTUMN_TIP;
        turf0 = flash(wetGrass ? 0x4920 : 0x6A40);
        turf1 = flash(0x8AC0);
        turf2 = flash(0xB340);
        FLOWER_COLS = FLOWER_AUTUMN;
    } else if (isWinter) {
        GRASS_BASE = PAL_WINTER_BASE;
        GRASS_TIP  = PAL_WINTER_TIP;
        // Frosted blue-gray turf (stable pattern — no flicker)
        turf0 = flash(0x420C);
        turf1 = flash(0x5D14);
        turf2 = flash(0x9CF3);
        FLOWER_COLS = FLOWER_WINTER;
    } else if (isRetro) {
        // B&W film grass — blocky gray blades
        static const uint16_t PAL_RETRO_BASE[4] = { 0x2104, 0x4208, 0x632C, 0x8410 };
        static const uint16_t PAL_RETRO_TIP[4]  = { 0x9CF3, 0xBDF7, 0xDEFB, 0xFFFF };
        static const uint16_t FLOWER_RETRO[4]   = { 0xFFFF, 0xC618, 0x8410, 0xAD55 };
        GRASS_BASE = PAL_RETRO_BASE;
        GRASS_TIP  = PAL_RETRO_TIP;
        turf0 = flash(0x18C3);
        turf1 = flash(0x4208);
        turf2 = flash(0x6B6D);
        FLOWER_COLS = FLOWER_RETRO;
    } else if (isNoir) {
        static const uint16_t PAL_NOIR_BASE[4] = { 0x1082, 0x2104, 0x3186, 0x4A00 };
        static const uint16_t PAL_NOIR_TIP[4]  = { 0x5A00, 0x8200, 0xC480, 0xFE60 };
        static const uint16_t FLOWER_NOIR[4]   = { 0xFE60, 0xC480, 0x8200, 0xF800 };
        GRASS_BASE = PAL_NOIR_BASE;
        GRASS_TIP  = PAL_NOIR_TIP;
        turf0 = flash(0x0841);
        turf1 = flash(0x18C3);
        turf2 = flash(0x3120);
        FLOWER_COLS = FLOWER_NOIR;
    } else if (isCity) {
        // Pavement + pebbles (short "blades" read as grit / curbs)
        static const uint16_t PAL_CITY_BASE[4] = { 0x4208, 0x5AEB, 0x738E, 0x8C71 };
        static const uint16_t PAL_CITY_TIP[4]  = { 0x9CF3, 0xBDF7, 0xC618, 0xDEFB };
        static const uint16_t FLOWER_CITY[4]   = { 0x8410, 0xA514, 0xFD20, 0x07FF }; // trash glints
        GRASS_BASE = PAL_CITY_BASE;
        GRASS_TIP  = PAL_CITY_TIP;
        turf0 = flash(0x3186);
        turf1 = flash(0x4A69);
        turf2 = flash(0x6B6D);
        FLOWER_COLS = FLOWER_CITY;
    } else if (isDesert) {
        // Flat sand + sparse tufts (not farm grass in beige)
        static const uint16_t PAL_DESERT_BASE[4] = { 0x9A40, 0xB4C0, 0xC480, 0xD4A0 };
        static const uint16_t PAL_DESERT_TIP[4]  = { 0xE5C0, 0xE5E0, 0xF6E0, 0xFFD0 };
        static const uint16_t FLOWER_DESERT[4]   = { 0xC480, 0xD4A0, 0xE5C0, 0xFFE0 }; // sand glitter
        GRASS_BASE = PAL_DESERT_BASE;
        GRASS_TIP  = PAL_DESERT_TIP;
        turf0 = flash(0xD4A0);
        turf1 = flash(0xE5C0);
        turf2 = flash(0xF6E0);
        FLOWER_COLS = FLOWER_DESERT;
    }

    // === BACK LAYER ONLY: soil + turf carpet ===
    if (!frontLayer) {
        canvas.fillRect(0, 104, 240, 1, dirtMid);
        canvas.fillRect(0, 105, 240, 2, dirtDark);
        for (int x = 0; x < 240; x += 5) {
            // Static dirt speckles (no time term → no flicker)
            uint32_t h = (uint32_t)x * 2654435761u;
            if ((h & 3) == 0) canvas.drawPixel(x, 104, dirtLite);
        }
        // Classic dense band for all seasons (winter just colder colors)
        canvas.fillRect(0, 99, 240, 5, turf0);
        for (int x = 0; x < 240; x += 2) {
            uint32_t h = (uint32_t)x * 1103515245u + 12345u;  // static, not now
            if ((h & 3) != 0) canvas.drawPixel(x, 98, turf1);
            if ((h & 5) == 0) canvas.drawPixel(x + 1, 97, turf2);
        }
        // Darker turf patch under the pig (ground compression / shade)
        {
            int feet = ctx.pigX + 14 * PX;
            int pl = feet - 16 * PX / 2;
            int pr = feet + 18 * PX / 2;
            uint16_t darkTurf = flash(isWinter ? 0x3186 : (wetGrass ? 0x2140 : 0x2A40));
            uint16_t midDark  = flash(isWinter ? 0x4A49 : 0x3A60);
            for (int x = pl; x <= pr; x++) {
                if (x < 0 || x >= 240) continue;
                int dist = x - feet;
                if (dist < 0) dist = -dist;
                int maxd = (pr - pl) / 2;
                if (maxd < 1) maxd = 1;
                if (dist * 2 > maxd * 3) continue;
                canvas.drawPixel(x, 104, darkTurf);
                canvas.drawPixel(x, 105, darkTurf);
                if (dist * 3 < maxd * 2) {
                    canvas.drawPixel(x, 103, midDark);
                    canvas.drawPixel(x, 99, midDark);
                }
            }
        }
        if (isWinter) {
            // Stable frost dust (no time-based flicker)
            for (int x = 0; x < 240; x += 4) {
                uint32_t h = (uint32_t)x * 2654435761u;
                if ((h & 7) < 3) canvas.drawPixel(x, 97, flash(0xDEFB));
                if ((h & 15) == 0) canvas.fillRect(x, 98, 2, 1, flash(0xBDF7));
            }
        } else if (isSpring) {
            // Flower carpet on turf — little blooms (static hash, no flicker)
            for (int x = 4; x < 236; x += 7) {
                uint32_t h = (uint32_t)x * 2654435761u + 0x9E37u;
                if ((h & 7) > 4) continue;  // sparse
                uint16_t fc = flash(FLOWER_COLS[(h >> 3) & 3]);
                int16_t fy = 96 + (int)(h & 3);
                canvas.drawPixel(x, fy, fc);
                canvas.drawPixel(x + 1, fy, fc);
                // yellow daisy center on white blooms
                if (((h >> 3) & 3) == 0)
                    canvas.drawPixel(x, fy, flash(0xFFE0));
            }
        }
    }

    bool pigOnGround = ctx.pigOnGround;
    int feet = ctx.pigX + 14 * PX;
    int pigLeft  = feet - 14 * PX;
    int pigRight = feet + 16 * PX;
    int pigCenter = (pigLeft + pigRight) / 2;
    int pigHalf  = (pigRight - pigLeft) / 2;
    if (pigHalf < 1) pigHalf = 1;

    int16_t treeX = treeX;
    if (treeX < 0) treeX = Trees::getFruitTreeScreenX();

    // Winter: frosty blades a bit shorter; spring: tender young shoots (+flowers)
    const int16_t heightBoost = isWinter ? 2 : (isSpring ? 3 : 4);

    for (int i = 0; i < BLADE_COUNT; i++) {
        int16_t cx = i * STRIDE + s_offset;
        if (cx < -STRIDE) cx += 240 + STRIDE;
        if (cx >= 240) continue;

        const Blade& b = s_blades[i];
        // The back pass is the full grass field; marked blades get a separate
        // foreground pass so the back layer stays dense when that pass is off.
        bool nearPig = (cx >= pigLeft - 6 && cx <= pigRight + 6);
        if (frontLayer && !b.frontLayer) continue;

        // Winter: only lightly thinned (keep density for "иней" look)
        if (isWinter && ((b.shade & 3) == 0)) continue;
        // Desert: sparse low sand tufts, not a full grass field
        if (isDesert && ((b.shade & 3) != 0)) continue;

        int16_t xJit = (int16_t)(((uint8_t)b.lean ^ b.shade) & 1);
        // Spring: force more flower stems visually (without rewriting blade table)
        uint8_t kind = b.kind;
        if (isSpring && kind == 0 && ((b.shade % 3) == 0)) kind = 2;  // extra blooms
        if (isSpring && kind == 4 && ((b.shade % 2) == 0)) kind = 2;  // stubble → sprouts
        int16_t drawHeight = (int16_t)(b.height + heightBoost);
        if (isDesert) drawHeight = (int16_t)(b.height / 2 + 2);  // short dunes of grit
        if (isSpring && kind == 2) drawHeight = (int16_t)(b.height + 5);  // flower stems taller
        // Front near pig: ankle-high only so body/legs stay readable
        if (frontLayer && nearPig) {
            if (drawHeight > 11) drawHeight = 11;
        } else if (frontLayer) {
            drawHeight = (int16_t)(drawHeight + 1);
        }
        if (drawHeight > 24) drawHeight = 24;
        int8_t drawLean = b.lean;

        // Wind sway — stronger so grass visibly waves
        {
            uint32_t phase = now + b.windPhase;
            int period = wetGrass ? 1400 : (isWinter ? 1800 : 2200);
            int wave = (int)(phase % period);
            int half = period / 2;
            int sway = (wave < half) ? (wave - half / 2) : (period * 3 / 4 - wave);
            // Scale to ~±2..4 px lean (was weak / half-zero)
            sway = (sway * 4) / (half > 0 ? half : 1);
            if (wetGrass) sway = (sway * 3) / 2;
            if (isWinter) sway = (sway * 5) / 4;  // cold wind
            if (sway > 4) sway = 4;
            if (sway < -4) sway = -4;
            drawLean += (int8_t)sway;
        }

        // Bend under pig
        if (pigOnGround && cx >= pigLeft && cx <= pigRight) {
            int distFromCenter = cx - pigCenter;
            if (distFromCenter < 0) distFromCenter = -distFromCenter;
            float bend = 1.0f - (float)distFromCenter / (float)pigHalf;
            if (bend < 0) bend = 0;
            // Front blades bend less so they still poke past the pig
            float bendAmt = frontLayer ? 0.45f : 0.70f;
            drawHeight = drawHeight - (int16_t)((float)drawHeight * bendAmt * bend);
            if (drawHeight < 4) drawHeight = 4;
            int8_t leanPush = (int8_t)((frontLayer ? 3.0f : 5.0f) * bend);
            drawLean = (cx < pigCenter) ? (int8_t)(b.lean - leanPush)
                                        : (int8_t)(b.lean + leanPush);
        }

        if (shakeActive && shakeDecay > 0.05f) {
            float edgeDist = 1.0f - (float)(cx > 120 ? cx - 120 : 120 - cx) / 120.0f;
            if (edgeDist < 0.0f) edgeDist = 0.0f;
            float impact = edgeDist * shakeDecay * ((float)shakeIntensity / 5.0f);
            if (impact > 0.15f) {
                int8_t jitter = ((now / 33) % 2 == 0) ? 1 : -1;
                drawLean += jitter;
            }
        }

        if (ctx.treeColliding && treeX >= 0) {
            int16_t dist = cx > treeX ? cx - treeX : treeX - cx;
            int16_t radius = 60;  // fruit crown influence on grass
            if (dist < radius) {
                float falloff = 1.0f - (float)dist / (float)radius;
                uint32_t phase = now + (uint32_t)(dist * 7);
                int8_t jitter = ((phase / 33) % 2 == 0) ? 1 : -1;
                drawLean += (int8_t)((float)jitter * falloff);
            }
        }

        uint8_t sh = b.shade & 3;
        uint16_t colBase = flash(GRASS_BASE[sh]);
        uint16_t colTip  = flash(GRASS_TIP[sh]);
        // Darker blades under/around the pig (ground shade)
        if (pigOnGround && cx >= pigLeft && cx <= pigRight) {
            colBase = flash(GRASS_BASE[0]);
            colTip  = flash(GRASS_TIP[0]);
            if (frontLayer) {
                // slightly less dark on front so ankles still read
                colBase = flash(GRASS_BASE[sh > 0 ? sh - 1 : 0]);
            }
        }
        if (wetGrass && !isWinter) {
            colBase = flash(GRASS_BASE[0]);
            colTip  = flash(GRASS_TIP[1]);
        }

        int16_t bx = cx + xJit;
        int16_t by = baseY;

        if (kind == 3) {
            // Pebbles only on back layer (spring: tiny flower buds instead)
            if (frontLayer) continue;
            if (bx >= 0 && bx < 239) {
                if (isSpring) {
                    uint16_t fc = flash(FLOWER_COLS[b.shade % 4]);
                    canvas.drawPixel(bx, by - 2, fc);
                    canvas.drawPixel(bx + 1, by - 2, fc);
                    canvas.drawPixel(bx, by - 3, flash(0xFFE0));
                } else {
                    canvas.drawPixel(bx, by - 1, flash(isWinter ? 0xBDF7 : 0x9CD3));
                    canvas.drawPixel(bx + 1, by - 1, flash(0x8410));
                    canvas.drawPixel(bx, by - 2, flash(isWinter ? 0xFFFF : 0xBDF7));
                }
            }
            continue;
        }

        // === Classic fat double-stroke blades (all seasons) ===
        int16_t tipX = snapPx(cx + drawLean);
        int16_t tipY = snapPx(by - drawHeight);

        if (kind == 4) {
            // Short stubble — fat single stroke
            fatLine(canvas, bx, by, tipX, tipY, colBase);
            canvas.drawPixel(tipX, tipY, colTip);
            continue;
        }

        if (kind == 2) {
            // Flower / winter frost tip
            int16_t hy = tipY;
            int16_t hx = snapPx(bx + drawLean / 2);
            fatLine(canvas, bx, by, hx, hy, colBase);
            fatLine(canvas, bx + 1, by, hx + 1, hy, colBase);
            uint16_t fc = flash(FLOWER_COLS[b.shade % 4]);
            if (isWinter) {
                canvas.fillRect(hx, hy - 2, 2, 2, flash(0xDEFB));
                canvas.drawPixel(hx + 1, hy - 3, flash(0xFFFF));
            } else if (isSpring) {
                // Daisy / dandelion: petals + yellow center
                canvas.fillRect(hx - 1, hy - PX - 1, PX + 2, PX + 1, fc);
                canvas.drawPixel(hx - 2, hy - PX + 1, fc);
                canvas.drawPixel(hx + PX + 1, hy - PX + 1, fc);
                canvas.drawPixel(hx + 1, hy - PX - 2, fc);
                canvas.drawPixel(hx, hy - PX, flash(0xFFE0));
            } else {
                canvas.fillRect(hx, hy - PX, PX, PX, fc);
                canvas.drawPixel(hx - 1, hy - PX + 1, fc);
                canvas.drawPixel(hx + PX, hy - PX + 1, fc);
            }
            continue;
        }

        if (kind == 1) {
            // Thick tuft — full 5 blades; spring = clover cluster on top
            for (int t = -2; t <= 2; t++) {
                int16_t tx = snapPx(cx + drawLean + t * 2);
                int16_t ty = snapPx(by - drawHeight + abs(t));
                fatLine(canvas, bx, by, tx, ty, colBase);
                fatLine(canvas, bx + 1, by, tx + 1, ty, colBase);
                canvas.drawPixel(tx, ty, colTip);
                if (isWinter) {
                    canvas.fillRect(tx, ty - 2, 2, 2, flash(0xDEFB));
                } else if (isAutumn && (t & 1) && ty > 0) {
                    canvas.drawPixel(tx, ty - 1, flash(0xFD20));
                }
            }
            if (isSpring) {
                int16_t cxC = snapPx(cx + drawLean);
                int16_t cyC = snapPx(by - drawHeight - 1);
                uint16_t leaf = flash(0x07E0);
                canvas.fillRect(cxC - 2, cyC, 2, 2, leaf);
                canvas.fillRect(cxC + 1, cyC, 2, 2, leaf);
                canvas.fillRect(cxC - 1, cyC - 2, 2, 2, leaf);
                canvas.drawPixel(cxC, cyC + 1, flash(0x04A0));
            }
            continue;
        }

        // Normal blade: double fat stroke base → tip
        int16_t midY = (by + tipY) / 2;
        fatLine(canvas, bx, by, tipX, midY, colBase);
        fatLine(canvas, bx + 1, by, tipX + 1, midY, colBase);
        fatLine(canvas, tipX, midY, tipX, tipY, colTip);
        fatLine(canvas, tipX + 1, midY, tipX + 1, tipY, colTip);
        // Winter: icy tip (blue-white frost)
        if (isWinter) {
            canvas.fillRect(tipX, tipY - 2, 2, 2, flash(0xDEFB));
            canvas.drawPixel(tipX + 1, tipY - 3, flash(0xFFFF));
        } else if (isSpring && drawHeight >= 12) {
            canvas.drawPixel(tipX, tipY - 1, flash(0xEFF8));
            if ((b.shade & 1) && !frontLayer)
                canvas.drawPixel(tipX + 1, tipY, flash(0xFFFF));
        } else if (isAutumn && drawHeight >= 14 && (b.shade & 1)) {
            canvas.drawPixel(tipX, tipY - 1, flash(0xFD20));
        }
    }

}

}  // namespace Ground
