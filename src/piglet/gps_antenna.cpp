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
// Chunky 3x3 pixel-art tower, matching the scale of the farm sprites.
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
static const uint8_t  MAX_HEIGHT      = 36;
static const int16_t  BASE_Y          = 106;
static const int16_t  PIXEL           = 3;

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

static int snapPixel(int value) {
    return ((value + PIXEL / 2) / PIXEL) * PIXEL;
}

static void drawPixel(M5Canvas& canvas, int x, int y, int width, int height,
                      uint16_t color) {
    canvas.fillRect(snapPixel(x), snapPixel(y), width, height, color);
}

static void drawPixelStep(M5Canvas& canvas, int x, int y, uint16_t color) {
    drawPixel(canvas, x, y, PIXEL * 2, PIXEL * 2, color);
}

// Draw a chunky tower anchored at its feet; all detail uses visible 3 px blocks.
static void drawAt(M5Canvas& canvas, int16_t cx, int height, int lean,
                   uint8_t signalLevel, bool hasFix) {
    if (cx < -48 || cx > DISPLAY_W + 48 || height <= 0) return;

    uint16_t mc, mh;
    metalColors(mc, mh);

    const int base = snapPixel(BASE_Y);
    const int visibleHeight = snapPixel(height);
    const int top = base - visibleHeight;
    const int towerHeight = base - top;
    const int towerTopX = snapPixel(cx + lean);

    drawPixel(canvas, cx - PIXEL * 4, base, PIXEL * 8, PIXEL * 2, 0x2104);
    drawPixel(canvas, cx - PIXEL * 3, base - PIXEL * 2, PIXEL * 2, PIXEL * 2, mc);
    drawPixel(canvas, cx - PIXEL * 3, base - PIXEL * 2, PIXEL, PIXEL * 2, mh);
    drawPixel(canvas, cx + PIXEL, base - PIXEL * 2, PIXEL * 2, PIXEL * 2, mc);
    drawPixel(canvas, cx + PIXEL * 2, base - PIXEL * 2, PIXEL, PIXEL * 2, mh);

    if (towerHeight <= 0) return;
    const int sectionHeight = PIXEL * 3;
    const int sections = (towerHeight + sectionHeight - 1) / sectionHeight;
    for (int section = 0; section < sections; section++) {
        const int y = base - (section + 1) * sectionHeight;
        if (y < top - PIXEL * 2) continue;
        const int rowY = snapPixel(y < top ? top : y);
        const int progress = base - rowY;
        const int halfWidth = PIXEL + (MAX_HEIGHT - progress) * PIXEL * 2 / MAX_HEIGHT;
        const int leanAtRow = towerHeight ? lean * progress / towerHeight : 0;
        const int rowX = snapPixel(cx + leanAtRow);
        const int left = rowX - halfWidth;
        const int right = rowX + halfWidth - PIXEL * 2;

        drawPixel(canvas, left, rowY, PIXEL * 2, sectionHeight, mc);
        drawPixel(canvas, left, rowY, PIXEL, sectionHeight, mh);
        drawPixel(canvas, right, rowY, PIXEL * 2, sectionHeight, mc);
        drawPixel(canvas, right + PIXEL, rowY, PIXEL, sectionHeight, mh);
        drawPixel(canvas, left + PIXEL, rowY + PIXEL, right - left, PIXEL, mh);
        if ((section & 1) == 0) {
            drawPixelStep(canvas, left + PIXEL * 2, rowY, mc);
            drawPixelStep(canvas, right - PIXEL * 2, rowY, mc);
        }
    }

    drawPixel(canvas, towerTopX - PIXEL, top, PIXEL * 2, towerHeight, mh);
    drawPixel(canvas, towerTopX - PIXEL * 2, top, PIXEL, towerHeight, mc);

    if (height > (int)(MAX_HEIGHT * 0.40f)) {
        const int dishY = top - PIXEL * 2;
        drawPixel(canvas, towerTopX - PIXEL * 3, dishY, PIXEL * 6, PIXEL * 2, mc);
        drawPixel(canvas, towerTopX - PIXEL * 2, dishY + PIXEL * 2, PIXEL * 4, PIXEL, mh);
        drawPixel(canvas, towerTopX - PIXEL, dishY + PIXEL * 3, PIXEL * 2, PIXEL, mc);
        drawPixel(canvas, towerTopX - PIXEL, dishY - PIXEL * 3, PIXEL * 2, PIXEL * 3, mh);
        const uint16_t signalColor = hasFix ? 0x07E0 : 0xFFE0;
        const int barsBottom = dishY - PIXEL * 2;
        for (uint8_t bar = 0; bar < 3; bar++) {
            const int barHeight = (bar + 1) * PIXEL;
            const int barX = towerTopX + ((int)bar - 1) * PIXEL * 3 - PIXEL;
            const uint16_t color = bar < signalLevel ? signalColor : 0x4208;
            drawPixel(canvas, barX, barsBottom - barHeight,
                      PIXEL * 2, barHeight, color);
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

void draw(M5Canvas& canvas, bool enabled, uint8_t signalLevel, bool hasFix) {
    advance(enabled, millis());
    if (s_phase == Phase::HIDDEN) return;

    const float    fall   = (s_phase == Phase::COLLAPSING) ? (1.0f - s_growth) : 0.0f;
    const int      height = (int)(MAX_HEIGHT * s_growth);
    const int      lean   = (int)(fall * 30.0f) * s_fallSide;
    int16_t x = screenX();
    drawAt(canvas, x, height, lean, signalLevel, hasFix);

    const int16_t width = Trees::worldWidth();
    if (x < 40)             drawAt(canvas, (int16_t)(x + width),  height, lean, signalLevel, hasFix);
    if (x > DISPLAY_W - 40) drawAt(canvas, (int16_t)(x - width), height, lean, signalLevel, hasFix);
}

bool tryStomp(int pigFeetX, bool airborne) {
    if (!airborne) { s_stompArmed = true; return false; }
    if (!s_enabled || s_phase != Phase::ALIVE || !s_stompArmed) return false;
    if (wrappedDistance((int16_t)pigFeetX, screenX()) > 16) return false;

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
