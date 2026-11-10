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
// Pixel-art radio mast / Meshtastic tower.  All geometry in 2×2 pixel blocks
// (PX=2).  Grows from ground like trees, collapses on 3 stomps.
//
// Visual structure (bottom → top):
//   BASE:   wide concrete footing (3 blocks wide)
//   LEGS:   two angled support legs spreading outward from footing
//   MAST:   central 1-block-wide column, PX*2 wide drawn with fillRect(x,y,PX,PX)
//   TRUSS:  X cross-braces every 4 rows, narrowing towards top
//   DISH:   flat 5-block arc + vertical spike at ~65% growth
//   SIGNAL: animated concentric pixel arcs, green (fix) or gold (searching)
// ---------------------------------------------------------------------------

static constexpr int16_t PX         = 2;   // 2×2 pixel block
static constexpr uint8_t MAX_HEIGHT = 48;  // px, full height
static constexpr int16_t BASE_Y     = 106;

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

// Season-aware metal colours
struct MetalPal { uint16_t dark, mid, light; };
static MetalPal metalPalette() {
    switch (Weather::getActiveSeason()) {
        case Season::NOIR:   return {0x2104, 0x4208, 0x6318};
        case Season::RETRO:  return {0x4208, 0x6318, 0xAD75};
        case Season::CITY:   return {0x528A, 0x738E, 0xAD75};
        case Season::DESERT: return {0x8420, 0xACC0, 0xD6C0};
        case Season::WINTER: return {0x528A, 0x8410, 0xC618};
        default:             return {0x4A49, 0x738E, 0xAD75}; // steel grey
    }
}

// Draw one 2×2 pixel block
static inline void px2(M5Canvas& c, int16_t x, int16_t y, uint16_t col) {
    c.fillRect(x, y, PX, PX, col);
}

// ---------------------------------------------------------------------------
// Draw the tower anchored at (cx, BASE_Y), grown to `height` pixels.
// `lean` = horizontal shift of tower top (fall animation).
// ---------------------------------------------------------------------------
static void drawAt(M5Canvas& canvas, int16_t cx, int height, int lean,
                   uint16_t signalColor, uint32_t now) {
    if (cx < -40 || cx > DISPLAY_W + 40) return;
    if (height <= 0) return;

    MetalPal m = metalPalette();

    const int topY = BASE_Y - height;
    const int tipX = cx + lean;

    // ---- BASE footing (3 blocks wide) ----
    canvas.fillRect(cx - PX*3, BASE_Y, PX*7, PX, m.dark);
    canvas.fillRect(cx - PX*2, BASE_Y + PX, PX*5, PX, m.dark);

    // ---- SUPPORT LEGS (angled outward, pixel-staircase) ----
    const int midY   = topY + height * 55 / 100;
    const int steps  = (BASE_Y - midY) / (PX * 2);
    for (int i = 0; i <= steps && steps > 0; i++) {
        int ly        = BASE_Y - i * (BASE_Y - midY) / steps;
        int lx_left   = cx - PX*3 + i * PX*2 / steps;
        int lx_right  = cx + PX*2 - i * PX*2 / steps;
        px2(canvas, lx_left,  ly - PX, m.mid);
        px2(canvas, lx_right, ly - PX, m.mid);
    }

    // ---- CENTRAL MAST (2px wide, with highlight) ----
    for (int y = topY; y < BASE_Y; y += PX) {
        canvas.fillRect(tipX - PX/2, y, PX, PX, m.mid);
        // alternating highlight dot every 3rd block
        if (((y - topY) / PX) % 3 == 1) {
            canvas.drawPixel(tipX, y, m.light);
        }
    }

    // ---- TRUSS CROSSBEAMS (every 4 blocks, narrowing upward) ----
    for (int y = midY; y > topY + PX*3; y -= PX*4) {
        int dist = BASE_Y - y;
        int hw   = PX*2 + dist * PX*2 / height;
        hw       = (hw / PX) * PX;    // snap to 2px grid
        // horizontal bar
        canvas.fillRect(tipX - hw, y, hw*2 + PX, PX, m.dark);
        // diagonal braces (pixel-art staircase)
        int bsteps = hw / PX;
        for (int s = 0; s < bsteps && s < 6; s++) {
            px2(canvas, tipX - hw + s*PX,       y - s*PX, m.mid);
            px2(canvas, tipX + hw - s*PX - PX,  y - s*PX, m.mid);
        }
    }

    // ---- DISH + SPIKE (appear at ~65% growth) ----
    if (height > MAX_HEIGHT * 65 / 100) {
        const int dishY = topY - PX*2;

        // vertical spike: 4×2 blocks
        canvas.fillRect(tipX - 1, dishY - PX*4, PX, PX*4, m.light);

        // dish arc: 5 columns, pixel-art parabola
        // offsets in PX units: -4,-2, 0,+2,+4 → raises: 2,1,0,1,2
        static const int8_t dOff[5] = {-4, -2, 0, 2, 4};
        static const int8_t dRai[5] = { 2,  1, 0, 1, 2};
        for (int i = 0; i < 5; i++) {
            int dx = dOff[i];
            int dy = dRai[i] * PX;
            px2(canvas, tipX + dx - 1,  dishY + dy,       m.mid);
            px2(canvas, tipX + dx - 1,  dishY + dy + PX,  m.dark);
        }
        // highlight rim of dish
        canvas.drawFastHLine(tipX - PX*4, dishY + PX*2, PX*9, m.light);

        // ---- ANIMATED SIGNAL ARCS (3 rings, 2×2 dots) ----
        const uint8_t wave = (uint8_t)((now / 500u) % 3u);
        const int ax = tipX;
        const int ay = dishY - PX*2;   // arc origin = spike tip

        // Ring 0 — small (always visible once dish appears)
        px2(canvas, ax - PX*2, ay - PX,   signalColor);
        px2(canvas, ax + PX,   ay - PX,   signalColor);
        px2(canvas, ax - PX,   ay - PX*2, signalColor);
        px2(canvas, ax,        ay - PX*2, signalColor);

        // Ring 1 — medium
        if (wave >= 1) {
            px2(canvas, ax - PX*4, ay - PX,   signalColor);
            px2(canvas, ax + PX*3, ay - PX,   signalColor);
            px2(canvas, ax - PX*3, ay - PX*3, signalColor);
            px2(canvas, ax + PX*2, ay - PX*3, signalColor);
            px2(canvas, ax - PX,   ay - PX*4, signalColor);
            px2(canvas, ax,        ay - PX*4, signalColor);
        }

        // Ring 2 — large
        if (wave >= 2) {
            px2(canvas, ax - PX*6, ay - PX,   signalColor);
            px2(canvas, ax + PX*5, ay - PX,   signalColor);
            px2(canvas, ax - PX*5, ay - PX*3, signalColor);
            px2(canvas, ax + PX*4, ay - PX*3, signalColor);
            px2(canvas, ax - PX*3, ay - PX*5, signalColor);
            px2(canvas, ax + PX*2, ay - PX*5, signalColor);
            px2(canvas, ax - PX,   ay - PX*6, signalColor);
            px2(canvas, ax,        ay - PX*6, signalColor);
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
