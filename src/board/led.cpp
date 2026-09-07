#include "led.h"
#include "../core/config.h"
#include "../core/app.h"
#include "../cap/sniffer.h"
#include "../piglet/avatar.h"
#include "../piglet/wolf.h"

#include <FastLED.h>
#include <M5Cardputer.h>

#ifndef PORK_LED_PIN
#define PORK_LED_PIN 21
#endif
#ifndef PORK_LED_COUNT
#define PORK_LED_COUNT 1
#endif

namespace Led {

static CRGB s_leds[PORK_LED_COUNT];
static bool s_inited = false;

// One-shot / multi-blink (HS "pik pik") — not continuous.
static uint8_t  s_blinkLeft = 0;   // remaining half-cycles (on+off pairs)
static bool     s_blinkOn   = false;
static uint32_t s_blinkAt   = 0;
static CRGB     s_blinkColor = CRGB::Black;
static const uint16_t BLINK_ON_MS  = 90;
static const uint16_t BLINK_OFF_MS = 110;

// Season ambient: rare refresh, keep last color to avoid FastLED.show spam.
static CRGB     s_lastShown = CRGB::Black;
static CRGB     s_ambientColor = CRGB::Black;
static uint32_t s_seasonTick = 0;
static const uint32_t SEASON_REFRESH_MS = 4000;  // rarely — battery
static uint32_t s_ambientTick = 0;
static const uint32_t AMBIENT_REFRESH_MS = 140;

static uint32_t s_lastFiles = 0;

void applyBrightness() {
    if (!s_inited) return;
    uint8_t pct = Config::personality().ledBright;
    if (pct > 100) pct = 100;
    // Cap hardware bright a bit so "40%" isn't harsh on battery
    FastLED.setBrightness((uint8_t)((uint16_t)pct * 200 / 100));
}

static void show(CRGB c) {
    if (!s_inited) return;
    if (c == s_lastShown && s_leds[0] == c) return;
    s_leds[0] = c;
    s_lastShown = c;
    FastLED.show();
}

void begin() {
    if (s_inited) return;
    FastLED.addLeds<WS2812B, PORK_LED_PIN, GRB>(s_leds, PORK_LED_COUNT);
    applyBrightness();
    s_leds[0] = CRGB::Black;
    FastLED.show();
    s_lastShown = CRGB::Black;
    s_inited = true;
    s_lastFiles = 0;
    s_blinkLeft = 0;
}

void off() {
    if (!s_inited) return;
    s_blinkLeft = 0;
    s_blinkOn = false;
    show(CRGB::Black);
}

void setRgb(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_inited) return;
    if (s_blinkLeft) return;  // don't fight HS blink
    show(CRGB(r, g, b));
}

void pulse(uint8_t r, uint8_t g, uint8_t b, uint16_t onMs) {
    if (!s_inited) return;
    if (!Config::personality().ledEnabled) return;
    // Convert single pulse into 1 blink cycle
    s_blinkColor = CRGB(r, g, b);
    s_blinkLeft = 1;
    s_blinkOn = true;
    s_blinkAt = millis();
    show(s_blinkColor);
    (void)onMs;
}

// Green pik-pik a few times (handshake caught).
static void startHsBlink(uint8_t times) {
    if (!Config::personality().ledEnabled) return;
    if (times < 1) times = 1;
    if (times > 4) times = 4;
    s_blinkColor = CRGB(0, 255, 48);
    s_blinkLeft = times;
    s_blinkOn = true;
    s_blinkAt = millis();
    show(s_blinkColor);
}

static CRGB seasonColor() {
    // Map SeasonMode → soft ambient (not full white — battery + eyes)
    uint8_t m = Config::personality().seasonMode;
    // AUTO(0) → mild green default; else mode-1 → Season index
    switch (m) {
        case 1:  return CRGB(60, 200, 90);   // SPRING
        case 2:  return CRGB(255, 170, 40);  // SUMMER
        case 3:  return CRGB(255, 90, 20);   // AUTUMN
        case 4:  return CRGB(140, 180, 255); // WINTER
        case 5:  return CRGB(220, 40, 180);  // RETRO
        case 6:  return CRGB(40, 40, 50);    // NOIR (very dim)
        case 7:  return CRGB(40, 180, 220);  // CITY
        case 8:  return CRGB(255, 190, 90);  // DESERT
        default: return CRGB(50, 160, 70);   // AUTO / farm default
    }
}

static uint8_t breatheLevel(uint32_t now, uint8_t low, uint8_t high) {
    const uint16_t phase = (uint16_t)((now / AMBIENT_REFRESH_MS) % 24);
    const uint8_t wave = phase <= 12 ? (uint8_t)phase : (uint8_t)(24 - phase);
    return (uint8_t)(low + ((uint16_t)(high - low) * wave / 12));
}

static bool updateWolfAlert(uint32_t now) {
    if (!Wolf::isActive()) return false;
    const uint8_t level = breatheLevel(now, 45, 190);
    CRGB red(255, 0, 0);
    red.nscale8(level);
    show(red);
    return true;
}

static bool tickBlink() {
    if (!s_blinkLeft) return false;
    uint32_t now = millis();
    uint16_t hold = s_blinkOn ? BLINK_ON_MS : BLINK_OFF_MS;
    if ((int32_t)(now - s_blinkAt) < (int32_t)hold) {
        show(s_blinkOn ? s_blinkColor : CRGB::Black);
        return true;
    }
    s_blinkAt = now;
    if (s_blinkOn) {
        s_blinkOn = false;
        show(CRGB::Black);
    } else {
        s_blinkOn = true;
        s_blinkLeft--;
        if (s_blinkLeft) show(s_blinkColor);
        else show(CRGB::Black);
    }
    return s_blinkLeft > 0 || s_blinkOn;
}

void update() {
    if (!s_inited) return;

    const PersonalityConfig& p = Config::personality();
    if (!p.ledEnabled) {
        off();
        return;
    }

    applyBrightness();

    // Active blink sequence always wins (short, rare).
    if (tickBlink()) return;

    const Cap::RunMode rm = Cap::runMode();
    const bool aggr = (rm == Cap::RunMode::Aggressive || rm == Cap::RunMode::Pinned);

    // --- GO / AGGR / PIN: mostly OFF, only flash on new HS file ---
    if (aggr && Cap::isRunning()) {
        const Cap::Counters& c = Cap::counters();
        if (c.filesOpened > s_lastFiles) {
            s_lastFiles = c.filesOpened;
            startHsBlink(3);  // pik-pik-pik green, then off
            return;
        }
        s_lastFiles = c.filesOpened;
        // Hunting: stay dark (battery). No solid blue.
        show(CRGB::Black);
        return;
    }

    // Light capture: also quiet — optional single soft blink on HS only
    if (Cap::isRunning()) {
        const Cap::Counters& c = Cap::counters();
        if (c.filesOpened > s_lastFiles) {
            s_lastFiles = c.filesOpened;
            startHsBlink(2);
            return;
        }
        s_lastFiles = c.filesOpened;
        show(CRGB::Black);
        return;
    }

    s_lastFiles = 0;

    // --- Farm / menu / normal game: scene ambient with priority alerts ---
    AppMode mode = App::mode();
    if (mode == AppMode::FARM || mode == AppMode::MENU || mode == AppMode::PIG) {
        uint32_t now = millis();
        if (mode == AppMode::FARM && updateWolfAlert(now)) {
            return;
        }
        if (now - s_seasonTick >= SEASON_REFRESH_MS || s_lastShown == CRGB::Black) {
            s_seasonTick = now;
            s_ambientColor = seasonColor();
            CRGB c = s_ambientColor;
            // Night is +10% brighter; daylight stays soft for the morning.
            const uint8_t base = Avatar::isNightTime() ? 106 : 84;
            const uint8_t breath = breatheLevel(now, 88, 100);
            c.nscale8((uint8_t)((uint16_t)base * breath / 100));
            show(c);
        }
        if (now - s_ambientTick >= AMBIENT_REFRESH_MS) {
            s_ambientTick = now;
            CRGB c = s_ambientColor;
            const uint8_t breath = breatheLevel(now, 88, 100);
            c.nscale8(breath);
            show(c);
        }
        return;
    }

    // Other modes (loot, wifi menus, etc.): LED off
    show(CRGB::Black);
}

}  // namespace Led
