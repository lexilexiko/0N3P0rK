#include "gps_antenna.h"

#include "ground.h"
#include "trees.h"
#include "weather.h"
#include "../audio/sfx.h"
#include "../core/config.h"
#include "../ui/display.h"

#include <math.h>
#include <stdlib.h>

namespace GpsAntenna {

// ---------------------------------------------------------------------------
// Pixel-art GPS tree. It grows and collapses with GPS state and can be
// knocked down with three stomps, like the other farm trees.
// ---------------------------------------------------------------------------

enum class Phase : uint8_t { HIDDEN, GROWING, ALIVE, COLLAPSING };

static Phase    s_phase      = Phase::HIDDEN;
static float    s_growth     = 0.0f;
static uint32_t s_phaseAt    = 0;
static uint32_t s_regrowAt   = 0;
static uint8_t  s_stompHits  = 0;
static bool     s_stompArmed = true;
static int8_t   s_fallSide   = 1;
static bool     s_enabled    = false;

static const uint32_t GROW_MS         = 1800;
static const uint32_t FALL_MS         = 900;
static const uint8_t  REQUIRED_STOMPS = 3;
static const uint8_t  MAX_HEIGHT      = 48;
static const int16_t  BASE_Y          = 106;

void init() {
    s_phase      = Phase::HIDDEN;
    s_growth     = 0.0f;
    s_phaseAt    = 0;
    s_regrowAt   = 0;
    s_stompHits  = 0;
    s_stompArmed = true;
    s_fallSide   = 1;
    s_enabled    = false;
}

static int16_t worldX() {
    const int16_t width = Trees::worldWidth();
    int16_t x = 180;
    if (x > width - 32) x = width - 32;
    if (x < 32) x = 32;
    return x;
}
static int16_t screenX() { return (int16_t)(worldX() - Ground::worldOffset()); }

static int16_t wrappedDistance(int16_t a, int16_t b) {
    const int16_t width = Trees::worldWidth();
    int16_t d = (int16_t)abs((int)(a - b));
    d %= width;
    return d > width / 2 ? (int16_t)(width - d) : d;
}

static void treeColors(uint16_t& trunk, uint16_t& shadow, uint16_t& highlight) {
    switch (Weather::getActiveSeason()) {
        case Season::NOIR:
            trunk = 0x2104; shadow = 0x1082; highlight = 0x4208; break;
        case Season::RETRO:
            trunk = 0x4208; shadow = 0x2104; highlight = 0x8410; break;
        case Season::AUTUMN:
            trunk = 0x6A00; shadow = 0x4100; highlight = 0x9B20; break;
        case Season::WINTER:
            trunk = 0x630C; shadow = 0x39C7; highlight = 0xBDF7; break;
        case Season::CITY:
            trunk = 0x4208; shadow = 0x2104; highlight = 0x8410; break;
        case Season::DESERT:
            trunk = 0xA2A0; shadow = 0x71E0; highlight = 0xFEA0; break;
        default:
            trunk = 0x6A20; shadow = 0x4100; highlight = 0xB4C7; break;
    }
}

static void drawAt(M5Canvas& canvas, int16_t cx, int height, int lean,
                   uint16_t signalColor, uint32_t now) {
    if (cx < -40 || cx > DISPLAY_W + 40) return;
    if (height <= 0) return;

    uint16_t trunk, trunkShadow, trunkHighlight;
    treeColors(trunk, trunkShadow, trunkHighlight);
    const int topY = BASE_Y - height;
    const int leanAtBase = lean / 5;
    const int baseWidth = height >= 24 ? 7 : 5;

    canvas.fillRect(cx - 9, BASE_Y, 19, 2, 0x2104);
    for (int row = 0; row < height; row += 2) {
        const int y = BASE_Y - row - 2;
        const int x = cx + leanAtBase + (lean - leanAtBase) * row / height;
        const int width = row < height / 4 ? baseWidth : baseWidth - 2;
        canvas.fillRect(x - width / 2, y, width, 3, trunk);
        canvas.drawFastVLine(x - width / 2, y, 2, trunkHighlight);
        canvas.drawFastVLine(x + width / 2, y, 2, trunkShadow);
    }

    const float growth = fminf(1.0f, fmaxf(0.0f, (height - 10.0f) / 24.0f));
    const int branchStartX = cx + lean / 2;
    const int branchStartY = topY + height * 2 / 5;
    const int branchSpread = (int)(15.0f * growth);
    const int branchLift = (int)(11.0f * growth);
    if (branchSpread > 0) {
        for (int thickness = -1; thickness <= 1; ++thickness) {
            canvas.drawLine(branchStartX + thickness, branchStartY,
                            branchStartX - branchSpread + thickness,
                            branchStartY - branchLift, trunk);
            canvas.drawLine(branchStartX + thickness, branchStartY,
                            branchStartX + branchSpread + thickness,
                            branchStartY - branchLift, trunk);
        }
        canvas.drawLine(branchStartX - branchSpread, branchStartY - branchLift,
                        branchStartX - branchSpread - 3, branchStartY - branchLift - 5,
                        trunk);
    }

    if (height > 18) {
        const int sway = (int)roundf(sinf((float)now / 210.0f) * 2.0f);
        const int leafX = branchStartX + branchSpread + sway;
        const int leafY = branchStartY - branchLift -
                          (int)roundf(sinf((float)now / 350.0f) * 2.0f);
        canvas.fillRect(leafX - 4, leafY - 2, 8, 5, trunkShadow);
        canvas.fillRect(leafX - 3, leafY - 2, 6, 4, signalColor);
        canvas.drawLine(leafX - 2, leafY + 1, leafX + 2, leafY - 1, 0xFFFF);
    } else if (height > 8) {
        canvas.fillRect(cx + 1, topY, 5, 4, signalColor);
    }
}

static void advance(bool enabled, uint32_t now) {
    if (enabled && !s_enabled) {
        if (s_phase == Phase::HIDDEN) {
            s_phase     = Phase::GROWING;
            s_phaseAt   = now;
            s_growth    = 0.0f;
            s_stompHits = 0;
        }
    } else if (!enabled && s_enabled &&
               (s_phase == Phase::GROWING || s_phase == Phase::ALIVE)) {
        s_phase   = Phase::COLLAPSING;
        s_phaseAt = now;
        s_fallSide = 1;
    }
    s_enabled = enabled;

    if (s_phase == Phase::GROWING) {
        uint32_t elapsed = now - s_phaseAt;
        s_growth = (float)elapsed / (float)GROW_MS;
        if (s_growth >= 1.0f) { s_growth = 1.0f; s_phase = Phase::ALIVE; }
    } else if (s_phase == Phase::COLLAPSING) {
        uint32_t elapsed = now - s_phaseAt;
        s_growth = 1.0f - (float)elapsed / (float)FALL_MS;
        if (s_growth <= 0.0f) {
            s_growth  = 0.0f;
            s_phase   = Phase::HIDDEN;
            if (enabled) s_regrowAt = now + 2400u;
        }
    } else if (s_phase == Phase::HIDDEN && enabled &&
               (int32_t)(now - s_regrowAt) >= 0) {
        s_phase     = Phase::GROWING;
        s_phaseAt   = now;
        s_growth    = 0.0f;
        s_stompHits = 0;
    }
}

void draw(M5Canvas& canvas, bool enabled, bool hasFix) {
    const uint32_t now = millis();
    advance(enabled, now);
    if (s_phase == Phase::HIDDEN) return;

    const float    fall   = (s_phase == Phase::COLLAPSING) ? (1.0f - s_growth) : 0.0f;
    const int      height = (int)(MAX_HEIGHT * s_growth);
    const int      lean   = (int)(fall * 30.0f) * s_fallSide;
    const uint16_t sigCol = hasFix ? 0x07E0 : 0xFFE0;   // green : gold

    int16_t x = screenX();
    drawAt(canvas, x, height, lean, sigCol, now);

    const int16_t width = Trees::worldWidth();
    if (x < 40)             drawAt(canvas, (int16_t)(x + width),  height, lean, sigCol, now);
    if (x > DISPLAY_W - 40) drawAt(canvas, (int16_t)(x - width), height, lean, sigCol, now);
}

bool tryStomp(int pigFeetX, bool airborne) {
    if (!airborne) { s_stompArmed = true; return false; }
    if (!s_enabled || s_phase != Phase::ALIVE || !s_stompArmed) return false;
    if (wrappedDistance((int16_t)pigFeetX, screenX()) > 22) return false;

    s_stompArmed = false;
    s_stompHits++;
    s_fallSide = (pigFeetX < screenX()) ? 1 : -1;
    if (s_stompHits >= REQUIRED_STOMPS) {
        s_phase   = Phase::COLLAPSING;
        s_phaseAt = millis();
        s_growth  = 1.0f;
        SFX::play(SFX::OINK_HAPPY);
    } else {
        SFX::play(SFX::ATTACK_HOP);
    }
    return true;
}

}  // namespace GpsAntenna
