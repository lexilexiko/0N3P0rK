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

enum class Phase : uint8_t { HIDDEN, GROWING, ALIVE, COLLAPSING };

struct Palette {
    uint16_t trunk;
    uint16_t trunkLight;
    uint16_t leaves;
    uint16_t leavesLight;
};

static Phase s_phase = Phase::HIDDEN;
static float s_growth = 0.0f;
static uint32_t s_phaseAt = 0;
static uint32_t s_regrowAt = 0;
static uint8_t s_stompHits = 0;
static bool s_stompArmed = true;
static int8_t s_fallSide = 1;
static bool s_enabled = false;

static const uint32_t GROW_MS = 1800;
static const uint32_t FALL_MS = 900;
static const uint8_t REQUIRED_STOMPS = 3;
static const uint8_t MAX_HEIGHT = 46;
static const int16_t BASE_Y = 106;

void init() {
    s_phase = Phase::HIDDEN;
    s_growth = 0.0f;
    s_phaseAt = 0;
    s_regrowAt = 0;
    s_stompHits = 0;
    s_stompArmed = true;
    s_fallSide = 1;
    s_enabled = false;
}

static int16_t worldX() {
    const int16_t width = Trees::worldWidth();
    int16_t x = 180;
    if (x > width - 32) x = width - 32;
    if (x < 32) x = 32;
    return x;
}

static int16_t screenX() {
    return (int16_t)(worldX() - Ground::worldOffset());
}

static int16_t wrappedDistance(int16_t a, int16_t b) {
    const int16_t width = Trees::worldWidth();
    int16_t d = (int16_t)abs((int)(a - b));
    d %= width;
    return d > width / 2 ? (int16_t)(width - d) : d;
}

static Palette seasonPalette() {
    switch (Weather::getActiveSeason()) {
        case Season::SPRING: return {0x7A21, 0xB4C7, 0xF9B6, 0xFB76};
        case Season::SUMMER: return {0x6A20, 0xB4C7, 0x35A0, 0x7FC0};
        case Season::AUTUMN: return {0x6A00, 0xB4C7, 0xE4A0, 0xFD20};
        case Season::WINTER: return {0x630C, 0xBDF7, 0x39E7, 0xFFFF};
        case Season::RETRO: return {0x4208, 0xBDF7, 0x8410, 0xFFFF};
        case Season::NOIR: return {0x2104, 0x632C, 0x2945, 0x4A49};
        case Season::CITY: return {0x4208, 0xBDF7, 0x03EF, 0x7DFF};
        case Season::DESERT: return {0xA2A0, 0xFEA0, 0xC4A0, 0xFE60};
        default: return {0x6A20, 0xB4C7, 0x35A0, 0x7FC0};
    }
}

static void advance(bool enabled, uint32_t now) {
    if (enabled && !s_enabled) {
        if (s_phase == Phase::HIDDEN) {
            s_phase = Phase::GROWING;
            s_phaseAt = now;
            s_growth = 0.0f;
            s_stompHits = 0;
        }
    } else if (!enabled && s_enabled &&
               (s_phase == Phase::GROWING || s_phase == Phase::ALIVE)) {
        s_phase = Phase::COLLAPSING;
        s_phaseAt = now;
        s_fallSide = 1;
    }
    s_enabled = enabled;

    if (s_phase == Phase::GROWING) {
        uint32_t elapsed = now - s_phaseAt;
        s_growth = (float)elapsed / (float)GROW_MS;
        if (s_growth >= 1.0f) {
            s_growth = 1.0f;
            s_phase = Phase::ALIVE;
        }
    } else if (s_phase == Phase::COLLAPSING) {
        uint32_t elapsed = now - s_phaseAt;
        s_growth = 1.0f - (float)elapsed / (float)FALL_MS;
        if (s_growth <= 0.0f) {
            s_growth = 0.0f;
            s_phase = Phase::HIDDEN;
            if (enabled) s_regrowAt = now + 2400u;
        }
    } else if (s_phase == Phase::HIDDEN && enabled &&
               (int32_t)(now - s_regrowAt) >= 0) {
        s_phase = Phase::GROWING;
        s_phaseAt = now;
        s_growth = 0.0f;
        s_stompHits = 0;
    }
}

static void drawAt(M5Canvas& canvas, int16_t x, const Palette& p, uint16_t signalColor) {
    if (x < -32 || x > DISPLAY_W + 32) return;
    const uint32_t now = millis();
    float growth = s_growth;
    if (s_phase == Phase::COLLAPSING) {
        growth = s_growth;
    }
    int height = (int)(MAX_HEIGHT * growth);
    if (height <= 0) return;

    const int topY = BASE_Y - height;
    const float fall = s_phase == Phase::COLLAPSING ? (1.0f - growth) : 0.0f;
    const int lean = (int)(fall * 28.0f) * s_fallSide;
    const int trunkTopX = x + lean;

    canvas.fillRect(x - 10, BASE_Y, 21, 2, 0x2104);
    for (int row = 0; row < height; row += 3) {
        const int rowY = BASE_Y - row - 3;
        const int rowX = x + (height > 0 ? lean * row / height : 0);
        canvas.fillRect(rowX - 1, rowY, 3, 4, p.trunk);
        canvas.drawFastVLine(rowX, rowY, 3, p.trunkLight);
    }

    const float branchProgress = fminf(1.0f, fmaxf(0.0f, (growth - 0.25f) * 1.7f));
    const int branchY = topY + (int)(height * 0.55f);
    const int branchLen = (int)(12.0f * branchProgress);
    if (branchLen > 0) {
        canvas.drawLine(trunkTopX, branchY + 9, trunkTopX - branchLen, branchY, p.trunkLight);
        canvas.drawLine(trunkTopX, branchY + 9, trunkTopX + branchLen, branchY, p.trunkLight);
        canvas.drawLine(trunkTopX, branchY + 17, trunkTopX - branchLen + 3, branchY + 10, p.trunkLight);
        canvas.drawLine(trunkTopX, branchY + 17, trunkTopX + branchLen - 3, branchY + 10, p.trunkLight);

        const bool winter = Weather::getActiveSeason() == Season::WINTER;
        if (winter) {
            for (int i = 0; i < branchLen; i += 4) {
                canvas.fillRect(trunkTopX - i - 2, branchY - i / 2, 4, 3, p.leaves);
                canvas.fillRect(trunkTopX + i - 2, branchY - i / 2, 4, 3, p.leaves);
            }
            canvas.fillRect(trunkTopX - 2, topY + 2, 5, 8, p.leaves);
            canvas.fillRect(trunkTopX - 1, topY, 3, 4, p.leavesLight);
        } else {
            canvas.fillRect(trunkTopX - branchLen - 4, branchY - 3, 8, 6, p.leaves);
            canvas.fillRect(trunkTopX + branchLen - 4, branchY - 3, 8, 6, p.leaves);
            canvas.fillRect(trunkTopX - branchLen / 2 - 3, branchY - 9, 7, 6, p.leaves);
            canvas.fillRect(trunkTopX + branchLen / 2 - 3, branchY - 9, 7, 6, p.leaves);
            canvas.fillRect(trunkTopX - 3, topY + 3, 7, 6, p.leavesLight);
            if ((Weather::getActiveSeason() == Season::SPRING ||
                 Weather::getActiveSeason() == Season::AUTUMN) &&
                ((now / 600u) & 1u) == 0) {
                canvas.drawPixel(trunkTopX - branchLen - 2, branchY - 2, p.leavesLight);
                canvas.drawPixel(trunkTopX + branchLen + 2, branchY - 2, p.leavesLight);
            }
        }
    }

    if (growth > 0.7f) {
        const int mastY = topY - 5;
        canvas.drawFastVLine(trunkTopX, mastY, 9, p.trunkLight);
        canvas.drawFastHLine(trunkTopX - 7, mastY + 3, 15, p.trunk);
        canvas.drawPixel(trunkTopX, mastY - 2, signalColor);
        uint8_t wave = (uint8_t)((now / 420u) % 3u);
        if (wave >= 1 || s_phase == Phase::ALIVE) {
            canvas.drawPixel(trunkTopX - 5, mastY - 1, signalColor);
            canvas.drawPixel(trunkTopX + 5, mastY - 1, signalColor);
        }
        if (wave >= 2 || s_phase == Phase::ALIVE) {
            canvas.drawPixel(trunkTopX - 8, mastY - 3, signalColor);
            canvas.drawPixel(trunkTopX + 8, mastY - 3, signalColor);
        }
    }
}

void draw(M5Canvas& canvas, bool enabled, bool hasFix) {
    const uint32_t now = millis();
    advance(enabled, now);
    if (s_phase == Phase::HIDDEN) return;

    Palette palette = seasonPalette();
    const uint16_t signalColor = hasFix ? 0x07E0 : 0xFFE0;
    int16_t x = screenX();
    drawAt(canvas, x, palette, signalColor);
    const int16_t width = Trees::worldWidth();
    if (x < 32) drawAt(canvas, (int16_t)(x + width), palette, signalColor);
    if (x > DISPLAY_W - 32) drawAt(canvas, (int16_t)(x - width), palette, signalColor);
}

bool tryStomp(int pigFeetX, bool airborne) {
    if (!airborne) {
        s_stompArmed = true;
        return false;
    }
    if (!s_enabled || s_phase != Phase::ALIVE || !s_stompArmed) return false;

    if (wrappedDistance((int16_t)pigFeetX, screenX()) > 22) return false;

    s_stompArmed = false;
    s_stompHits++;
    s_fallSide = (pigFeetX < screenX()) ? 1 : -1;
    if (s_stompHits >= REQUIRED_STOMPS) {
        s_phase = Phase::COLLAPSING;
        s_phaseAt = millis();
        s_growth = 1.0f;
        SFX::play(SFX::OINK_HAPPY);
    } else {
        SFX::play(SFX::ATTACK_HOP);
    }
    return true;
}

}  // namespace GpsAntenna
