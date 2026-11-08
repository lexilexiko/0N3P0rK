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
// Pixel-art GPS antenna tower. Grows from ground like a tree, collapses
// when GPS is disabled. Three stomps knock it down (like trees).
//
// Visual design — matches the game's pixel style:
//   • Truss mast: alternating cross-beams every 5 px, narrowing towards top
//   • Wide base legs spreading outward from centre
//   • Dish at the top (flat arc, 3 rows)
//   • Animated signal arcs (concentric quarter-circles) in green/gold
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

// Season-aware metal colour (body / highlight)
static void metalColors(uint16_t& col, uint16_t& hi) {
    switch (Weather::getActiveSeason()) {
        case Season::NOIR:
            col = 0x2104; hi = 0x4208; break;
        case Season::RETRO:
            col = 0x4208; hi = 0x8410; break;
        case Season::CITY:
            col = 0x528A; hi = 0x8410; break;
        default:
            col = 0x6B4D; hi = 0xAD75; break;   // warm grey steel
    }
}

// ---------------------------------------------------------------------------
// Draw the tower anchored at (cx, BASE_Y), grown to `height` pixels.
// `lean` shifts the top horizontally (fall animation).
// `signalColor` = green (fix) or gold (searching).
// ---------------------------------------------------------------------------
static void drawAt(M5Canvas& canvas, int16_t cx, int height, int lean,
                   uint16_t signalColor, uint32_t now) {
    if (cx < -40 || cx > DISPLAY_W + 40) return;
    if (height <= 0) return;

    uint16_t mc, mh;
    metalColors(mc, mh);

    const int topY = BASE_Y - height;
    const int tipX = cx + lean;

    // shadow / base plate
    canvas.fillRect(cx - 8, BASE_Y, 17, 2, 0x2104);

    // base legs: spread at ground, converge to mast mid-point
    const int midY = topY + height / 2;
    canvas.drawLine(cx - 7, BASE_Y,     tipX - 1, midY, mc);
    canvas.drawLine(cx - 6, BASE_Y - 1, tipX - 1, midY, mh);
    canvas.drawLine(cx + 7, BASE_Y,     tipX + 1, midY, mc);
    canvas.drawLine(cx + 6, BASE_Y - 1, tipX + 1, midY, mh);

    // central mast (3 px wide)
    canvas.drawFastVLine(tipX - 1, topY, height, mc);
    canvas.drawFastVLine(tipX,     topY, height, mh);
    canvas.drawFastVLine(tipX + 1, topY, height, mc);

    // cross-braces (truss pattern), every 5 px, only in upper half
    for (int y = midY - 4; y > topY + 4; y -= 5) {
        int frac = (BASE_Y - y);
        int hw   = 2 + frac * 3 / height;   // narrowing up
        canvas.drawFastHLine(tipX - hw, y, hw * 2 + 1, mc);
        canvas.drawLine(tipX - hw, y, tipX, y - 4, mc);
        canvas.drawLine(tipX + hw, y, tipX, y - 4, mc);
    }

    // dish + spike (appear at ~65% growth)
    if (height > (int)(MAX_HEIGHT * 0.60f)) {
        const int dishY = topY - 2;
        // spike
        canvas.drawFastVLine(tipX, dishY - 6, 7, mh);
        // dish arc (pixel art, 3 rows wide)
        canvas.fillRect(tipX - 4, dishY,     9, 2, mc);
        canvas.fillRect(tipX - 3, dishY + 2, 7, 1, mc);
        canvas.fillRect(tipX - 1, dishY + 3, 3, 1, mc);
        // highlight rim
        canvas.drawFastHLine(tipX - 4, dishY, 9, mh);

        // animated signal arcs (pixel-art concentric quarter-circles)
        const uint8_t wave = (uint8_t)((now / 480u) % 3u);
        const int ax = tipX;
        const int ay = dishY - 5;   // arc origin = spike tip

        // arc 0 — always visible once dish appears
        canvas.drawPixel(ax - 2, ay - 1, signalColor);
        canvas.drawPixel(ax + 2, ay - 1, signalColor);
        canvas.drawPixel(ax - 1, ay - 2, signalColor);
        canvas.drawPixel(ax + 1, ay - 2, signalColor);

        if (wave >= 1) {
            canvas.drawPixel(ax - 4, ay - 1, signalColor);
            canvas.drawPixel(ax + 4, ay - 1, signalColor);
            canvas.drawPixel(ax - 3, ay - 3, signalColor);
            canvas.drawPixel(ax + 3, ay - 3, signalColor);
            canvas.drawPixel(ax - 1, ay - 4, signalColor);
            canvas.drawPixel(ax + 1, ay - 4, signalColor);
        }
        if (wave >= 2) {
            canvas.drawPixel(ax - 6, ay - 1, signalColor);
            canvas.drawPixel(ax + 6, ay - 1, signalColor);
            canvas.drawPixel(ax - 5, ay - 3, signalColor);
            canvas.drawPixel(ax + 5, ay - 3, signalColor);
            canvas.drawPixel(ax - 3, ay - 5, signalColor);
            canvas.drawPixel(ax + 3, ay - 5, signalColor);
            canvas.drawPixel(ax,     ay - 6, signalColor);
        }
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
