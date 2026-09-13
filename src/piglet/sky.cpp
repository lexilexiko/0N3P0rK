// Sky layer — gradient, moon, stars (clouds/rain remain in weather.cpp).
#include "sky.h"
#include "weather.h"
#include "avatar.h"
#include "../core/config.h"
#include <M5Cardputer.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

namespace Sky {

static constexpr int16_t PX = 3;
static inline int16_t snapPx(int16_t v) {
    return (v >= 0) ? (v / PX) * PX : ((v - (PX - 1)) / PX) * PX;
}

static constexpr uint16_t C_SKY_TOP = 0x3C3A;
static uint16_t s_skyTop = C_SKY_TOP;
static constexpr uint16_t C_STAR = 0xFFF0;

static uint16_t s_nightBlend = 0;
static uint32_t s_lastNightBlendMs = 0;
static uint16_t s_cachedNightTarget = 0;
static uint32_t s_lastNightTargetMs = 0;
static constexpr uint32_t NIGHT_BLEND_FULL_MS = 12000;

static uint16_t s_weatherDark = 0;
static uint32_t s_lastWeatherDarkMs = 0;
static constexpr uint32_t WEATHER_DARK_FULL_MS = 4500;

static uint16_t skyFlash(uint16_t c) {
    if (!Avatar::isThunderFlashing()) return c;
    uint16_t r = ((c >> 11) + 31) >> 1;
    uint16_t g = (((c >> 5) & 0x3F) + 63) >> 1;
    uint16_t b = ((c & 0x1F) + 31) >> 1;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

struct Star {
    int16_t x, y;
    uint8_t size;
    uint8_t brightness;
    uint32_t fadeInStart;
    bool isBlinking;
};
static constexpr uint8_t MAX_STARS = 15;
static Star s_stars[MAX_STARS];
static uint8_t s_starCount = 0;
static uint32_t s_lastStarSpawn = 0;
static uint32_t s_nextSpawnDelay = 2000;
static bool s_starsActive = false;

static inline uint16_t lerp565(uint16_t a, uint16_t b, uint8_t num /*0..16*/) {
    int r0 = (a >> 11) & 0x1F, g0 = (a >> 5) & 0x3F, b0 = a & 0x1F;
    int r1 = (b >> 11) & 0x1F, g1 = (b >> 5) & 0x3F, b1 = b & 0x1F;
    int r = r0 + ((r1 - r0) * (int)num) / 16;
    int g = g0 + ((g1 - g0) * (int)num) / 16;
    int bb = b0 + ((b1 - b0) * (int)num) / 16;
    return (uint16_t)((r << 11) | (g << 5) | bb);
}

// Blend day → dusk (0..128) then dusk → night (128..256)
static uint16_t blendDayDuskNight(uint16_t dayC, uint16_t duskC, uint16_t nightC, uint16_t amt) {
    if (amt >= 256) return nightC;
    if (amt == 0) return dayC;
    if (amt <= 128) {
        return lerp565(dayC, duskC, (uint8_t)((amt * 16) / 128));
    }
    return lerp565(duskC, nightC, (uint8_t)(((amt - 128) * 16) / 128));
}

// Minutes-of-day → night amount.
// Spring/Autumn: day and night the same length (12/12).
// Summer: long day. Winter: long night.
static uint16_t nightAmountFromMinutes(int mins, Season season) {
    if (mins < 0) mins = 0;
    if (mins >= 24 * 60) mins %= (24 * 60);
    int dawn0, dawn1, dusk0, dusk1;
    if (season == Season::SUMMER) {
        dawn0 = 4 * 60;  dawn1 = 6 * 60;   // 04–06
        dusk0 = 20 * 60; dusk1 = 22 * 60;  // 20–22
    } else if (season == Season::WINTER) {
        dawn0 = 8 * 60;  dawn1 = 10 * 60;  // 08–10
        dusk0 = 15 * 60; dusk1 = 17 * 60;  // 15–17
    } else {
        // Spring / Autumn / Retro: equal
        dawn0 = 5 * 60;  dawn1 = 7 * 60;   // 05–07
        dusk0 = 17 * 60; dusk1 = 19 * 60;  // 17–19
    }
    if (mins >= dawn0 && mins < dawn1) {
        int span = dawn1 - dawn0;
        return (uint16_t)(256 - ((mins - dawn0) * 256) / span);
    }
    if (mins >= dawn1 && mins < dusk0) return 0;
    if (mins >= dusk0 && mins < dusk1) {
        int span = dusk1 - dusk0;
        return (uint16_t)(((mins - dusk0) * 256) / span);
    }
    return 256;
}

// Synthetic 6-min cycle, same season rules (no clock)
static uint16_t nightAmountFromSynthetic(uint32_t secInCycle, Season season) {
    uint32_t sec = secInCycle % 360u;
    uint32_t dayS, duskS, nightS, dawnS;
    if (season == Season::SUMMER) {
        dayS = 200; duskS = 40; nightS = 80; dawnS = 40;
    } else if (season == Season::WINTER) {
        dayS = 80; duskS = 40; nightS = 200; dawnS = 40;
    } else {
        dayS = 140; duskS = 40; nightS = 140; dawnS = 40;
    }
    if (sec < dayS) return 0;
    sec -= dayS;
    if (sec < duskS) return (uint16_t)((sec * 256u) / duskS);
    sec -= duskS;
    if (sec < nightS) return 256;
    sec -= nightS;
    if (dawnS == 0) return 0;
    if (sec < dawnS) return (uint16_t)(256u - (sec * 256u) / dawnS);
    return 0;
}

static uint16_t computeNightTarget(uint32_t now) {
    if (Weather::getActiveSeason() == Season::NOIR) return 256;
    // DESERT: night clock may tick, but sky stays bright morning
    if (Weather::getActiveSeason() == Season::DESERT) return 0;
    uint8_t sky = Config::personality().skyMode;
    if (sky == (uint8_t)SkyMode::DAY) return 0;
    if (sky == (uint8_t)SkyMode::NIGHT) return 256;

    // Cache target ~1s (RTC / time reads)
    if (s_lastNightTargetMs != 0 && (now - s_lastNightTargetMs) < 1000u) {
        return s_cachedNightTarget;
    }
    s_lastNightTargetMs = now;

    auto dt = M5.Rtc.getDateTime();
    if (dt.date.year >= 2024) {
        int mins = (int)dt.time.hours * 60 + (int)dt.time.minutes;
        s_cachedNightTarget = nightAmountFromMinutes(mins, Weather::getActiveSeason());
        return s_cachedNightTarget;
    }

    time_t unixNow = time(nullptr);
    if (unixNow >= 1700000000) {
        struct tm timeinfo;
        localtime_r(&unixNow, &timeinfo);
        int mins = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        s_cachedNightTarget = nightAmountFromMinutes(mins, Weather::getActiveSeason());
        return s_cachedNightTarget;
    }

    // No clock: living day/night cycle with dusk/dawn ramps
    s_cachedNightTarget = nightAmountFromSynthetic(now / 1000u, Weather::getActiveSeason());
    return s_cachedNightTarget;
}

static void approachU16(uint16_t& value, uint16_t target, uint32_t dt, uint32_t fullMs) {
    if (dt == 0) return;
    if (dt > 80u) dt = 80u;
    uint16_t step = (uint16_t)((dt * 256u) / fullMs);
    if (step < 1) step = 1;
    if (value < target) {
        uint16_t next = (uint16_t)(value + step);
        value = (next > target) ? target : next;
    } else if (value > target) {
        value = (value > step) ? (uint16_t)(value - step) : 0;
        if (value < target) value = target;
    }
}

static void updateNightBlend(uint32_t now) {
    uint16_t target = computeNightTarget(now);
    if (s_lastNightBlendMs == 0) {
        s_nightBlend = target;
        s_lastNightBlendMs = now;
        return;
    }
    uint32_t dt = now - s_lastNightBlendMs;
    s_lastNightBlendMs = now;
    approachU16(s_nightBlend, target, dt, NIGHT_BLEND_FULL_MS);
}

// Smooth sky darkening when rain/storm starts (not hard snap)
static void updateWeatherDark(uint32_t now) {
    uint16_t target = 0;
    if (Weather::isStorming()) target = 256;
    else if (Weather::isRaining()) target = 160;
    if (s_lastWeatherDarkMs == 0) {
        s_weatherDark = target;
        s_lastWeatherDarkMs = now;
        return;
    }
    uint32_t dt = now - s_lastWeatherDarkMs;
    s_lastWeatherDarkMs = now;
    approachU16(s_weatherDark, target, dt, WEATHER_DARK_FULL_MS);
}

// Living sky: day/dusk/night multi-stop gradient + Bayer dither
// Smooth night blend + smooth rain/storm darkening (no hard snap)

void drawBackdrop(M5Canvas& canvas) {
    uint32_t now = millis();
    updateNightBlend(now);
    updateWeatherDark(now);
    const uint16_t nb = s_nightBlend;  // 0 day .. 256 night
    const uint16_t wd = s_weatherDark; // 0 clear .. 160 rain .. 256 storm
    const bool raining = Weather::isRaining();
    const bool storm = Weather::isStorming();
    (void)storm;

    // Clear / rain / storm palettes × day-dusk-night, then cross-fade by wd
    // Clear
    const uint16_t CD0 = 0x3C3A, CD1 = 0x54BB, CD2 = 0x6D3C, CD3 = 0x8DDC;
    const uint16_t CK0 = 0x30A8, CK1 = 0x7150, CK2 = 0xD2C0, CK3 = 0xFD60;
    const uint16_t CN0 = 0x18A8, CN1 = 0x20CA, CN2 = 0x314C, CN3 = 0x49AE;
    // Rain (cooler / duller)
    const uint16_t RD0 = 0x31A8, RD1 = 0x4A6A, RD2 = 0x62EC, RD3 = 0x7B8E;
    const uint16_t RK0 = 0x3128, RK1 = 0x61A8, RK2 = 0xA2C8, RK3 = 0xC3A8;
    const uint16_t RN0 = 0x18A8, RN1 = 0x20CA, RN2 = 0x314C, RN3 = 0x49AE;
    // Storm (darker)
    const uint16_t SD0 = 0x2945, SD1 = 0x39A8, SD2 = 0x4A6A, SD3 = 0x5B0C;
    const uint16_t SK0 = 0x28A4, SK1 = 0x5126, SK2 = 0x8A28, SK3 = 0xB2C8;
    const uint16_t SN0 = 0x10A6, SN1 = 0x18C8, SN2 = 0x28EA, SN3 = 0x38EC;

    auto band = [&](uint16_t d, uint16_t k, uint16_t n) {
        return blendDayDuskNight(d, k, n, nb);
    };
    // clear → rain (0..160), rain → storm (160..256)
    auto wetMix = [&](uint16_t clearC, uint16_t rainC, uint16_t stormC) -> uint16_t {
        if (wd == 0) return clearC;
        if (wd <= 160) {
            return lerp565(clearC, rainC, (uint8_t)((wd * 16) / 160));
        }
        return lerp565(rainC, stormC, (uint8_t)(((wd - 160) * 16) / 96));
    };

    uint16_t S0 = wetMix(band(CD0, CK0, CN0), band(RD0, RK0, RN0), band(SD0, SK0, SN0));
    uint16_t S1 = wetMix(band(CD1, CK1, CN1), band(RD1, RK1, RN1), band(SD1, SK1, SN1));
    uint16_t S2 = wetMix(band(CD2, CK2, CN2), band(RD2, RK2, RN2), band(SD2, SK2, SN2));
    uint16_t S3 = wetMix(band(CD3, CK3, CN3), band(RD3, RK3, RN3), band(SD3, SK3, SN3));

    // RETRO season: force old-film grayscale sky (no blue/pink day tones)
    if (Weather::getActiveSeason() == Season::RETRO) {
        // dark charcoal → mid gray gradient
        S0 = 0x18C3; S1 = 0x3186; S2 = 0x4A69; S3 = 0x632C;
        if (wd > 0) {
            // wet/storm slightly darker
            S0 = 0x10A2; S1 = 0x2104; S2 = 0x39C7; S3 = 0x52AA;
        }
    }
    // NOIR: alley night — ink sky, sodium glow at the horizon
    if (Weather::getActiveSeason() == Season::NOIR) {
        S0 = 0x0842; S1 = 0x1063; S2 = 0x1884; S3 = 0x3120;
        if (wd > 0) {
            S0 = 0x0021; S1 = 0x0842; S2 = 0x1063; S3 = 0x20C0;
        }
    }
    // CITY: smoggy dusk over rooftops (gray-blue, not farm green)
    if (Weather::getActiveSeason() == Season::CITY) {
        S0 = 0x3147; S1 = 0x4A69; S2 = 0x6B8E; S3 = 0x8410;
        if (wd > 0) {
            S0 = 0x20C4; S1 = 0x3186; S2 = 0x52AA; S3 = 0x6B6D;
        }
    }
    // DESERT: bleached noon → warm sand haze at horizon (not cyan beach)
    if (Weather::getActiveSeason() == Season::DESERT) {
        S0 = 0x8D7F; // pale washed blue
        S1 = 0xA5BF; // soft sky
        S2 = 0xD6BB; // dusty peach
        S3 = 0xF6D4; // hot sand horizon
    }
    s_skyTop = S0;

    static const uint8_t bayer4[16] = {
        0,  8,  2, 10,
        12, 4, 14,  6,
        3, 11,  1,  9,
        15, 7, 13,  5
    };
    uint16_t samples[32];
    for (int i = 0; i < 32; i++) {
        int u = i;
        int u2 = (u * u) / 31;
        int u3 = 31 - ((31 - u) * (31 - u)) / 31;
        int e = (u2 + u3) / 2;
        if (e < 10)
            samples[i] = lerp565(S0, S1, (uint8_t)((e * 16) / 10));
        else if (e < 21)
            samples[i] = lerp565(S1, S2, (uint8_t)(((e - 10) * 16) / 11));
        else
            samples[i] = lerp565(S2, S3, (uint8_t)(((e - 21) * 16) / 10));
    }

    // Full sky to grass line — gradient meets turf cleanly
    for (int y = 0; y < 103; y++) {
        int si = (y * 31) / 102;
        if (si > 30) si = 30;
        uint16_t cA = samples[si];
        uint16_t cB = samples[si + 1];
        int sub = ((y * 31) % 102) * 16 / 102;
        if (cA == cB) {
            canvas.drawFastHLine(0, y, 240, skyFlash(cA));
            continue;
        }
        for (int x = 0; x < 240; x += 2) {
            uint8_t thr = bayer4[((y & 3) << 2) | (x & 3)];
            uint16_t c = (sub > (int)thr) ? cB : cA;
            canvas.drawFastHLine(x, y, 2, skyFlash(c));
        }
    }

    // CITY skyline: simple chunky pixel buildings behind the foreground.
    // Keep the windows dark during the day; they fade in with the night blend.
    if (Weather::getActiveSeason() == Season::CITY) {
        struct Building {
            int16_t x;
            uint8_t width;
            uint8_t height;
            uint8_t roof;
            uint16_t body;
            uint16_t shade;
        };
        static const Building skyline[] = {
            {  0, 24, 31, 0, 0x294A, 0x2108 },
            { 22, 18, 47, 1, 0x318C, 0x294A },
            { 39, 27, 24, 0, 0x3A0E, 0x294A },
            { 64, 19, 57, 2, 0x294A, 0x2108 },
            { 82, 30, 36, 0, 0x318C, 0x2529 },
            {110, 21, 66, 1, 0x294A, 0x1CE7 },
            {130, 27, 43, 0, 0x3A0E, 0x2529 },
            {155, 18, 56, 2, 0x294A, 0x2108 },
            {173, 31, 29, 0, 0x318C, 0x2529 },
            {202, 20, 51, 1, 0x294A, 0x1CE7 },
            {220, 20, 38, 0, 0x3A0E, 0x2529 }
        };
        static const uint8_t windowRows[] = {
            0b101, 0b010, 0b111, 0b100, 0b011, 0b110
        };
        const uint16_t nightGate = (nb > 132) ? (uint16_t)(nb - 132) : 0;
        const uint16_t windowColor = skyFlash(
            lerp565(0x5A90, 0xFEA0, (uint8_t)((nightGate * 16) / 124)));

        for (uint8_t i = 0; i < (uint8_t)(sizeof(skyline) / sizeof(skyline[0])); i++) {
            const Building& b = skyline[i];
            const int16_t top = (int16_t)(103 - b.height);
            canvas.fillRect(b.x, top, b.width, b.height, skyFlash(b.body));
            canvas.drawFastHLine(b.x, top, b.width, skyFlash(b.shade));

            if (b.roof == 1) {
                canvas.drawFastHLine(b.x + 3, top - 2, b.width - 6, skyFlash(b.shade));
                canvas.drawFastVLine(b.x + b.width / 2, top - 5, 3, skyFlash(b.shade));
            } else if (b.roof == 2) {
                canvas.drawFastHLine(b.x + 2, top - 1, b.width - 4, skyFlash(b.shade));
            }

            if (nightGate == 0) continue;
            const int16_t firstY = top + 8;
            const int16_t lastY = 97;
            uint8_t row = 0;
            for (int16_t wy = firstY; wy < lastY; wy += 9, row++) {
                uint8_t pattern = windowRows[(i + row) % (sizeof(windowRows) / sizeof(windowRows[0]))];
                for (uint8_t col = 0; col < 3; col++) {
                    if ((pattern & (1u << (2 - col))) == 0) continue;
                    int16_t wx = b.x + 4 + (int16_t)col * 6;
                    if (wx + 3 >= b.x + b.width) continue;
                    uint16_t lit = windowColor;
                    if (((i * 7u + row * 3u + col) & 3u) == 0) {
                        lit = lerp565(windowColor, 0xFFD0, 8);
                    }
                    canvas.fillRect(wx, wy, 3, 4, lit);
                }
            }
        }
    }

    // Moon fades in with night (after mid-dusk); hides as sky wets
    if (nb > 100 && wd < 100) {
        int mx = 198, my = 22;
        // Brightness / size grow with night amount
        uint8_t t = (uint8_t)(((nb - 100) * 16) / 156);  // 0..16
        if (t > 16) t = 16;
        int rOuter = 6 + (4 * (int)t) / 16;   // 6..10
        int rMid   = 4 + (3 * (int)t) / 16;
        int rCore  = 3 + (3 * (int)t) / 16;
        uint16_t halo = lerp565(S0, 0x39E7, t);
        uint16_t body = lerp565(S1, 0xDEFB, t);
        uint16_t core = lerp565(0xC618, 0xFFFF, t);
        canvas.fillCircle(mx, my, rOuter, skyFlash(halo));
        canvas.fillCircle(mx, my, rMid, skyFlash(body));
        canvas.fillCircle(mx - 2, my - 1, rCore, skyFlash(core));
        if (t > 10) {
            canvas.drawPixel(mx + 1, my + 1, skyFlash(0xC618));
            canvas.drawPixel(mx - 1, my + 2, skyFlash(0xC618));
        }
    }

    // Ambient FX: pollen (day) → fireflies (night); skip when sky is wet
    if (wd < 40 && !raining && !storm) {
        uint32_t phase = now % 16000;
        // Day pollen while still mostly day (nb < 80)
        if (nb < 80 && phase < 2800) {
            for (int i = 0; i < 7; i++) {
                uint32_t seed = now / 50 + (uint32_t)i * 97;
                if ((seed & 3) != 0) continue;
                int16_t px = (int16_t)((seed * 17 + i * 41) % 228) + 6;
                int16_t py = (int16_t)((seed * 13 + i * 53) % 48) + 6;
                py += (int16_t)(sinf((float)(now + i * 180) * 0.004f) * 3.0f);
                canvas.fillRect(px, py, 2, 2, skyFlash(0xFFFE));
            }
        }
        // Fireflies once dusk is deep enough
        if (nb > 140 && phase >= 4000 && phase < 9000) {
            for (int i = 0; i < 5; i++) {
                uint32_t seed = now / 80 + (uint32_t)i * 131;
                if ((seed & 2) != 0) continue;
                int16_t px = (int16_t)((seed * 19 + i * 47) % 220) + 10;
                int16_t py = (int16_t)((seed * 11 + i * 29) % 40) + 30;
                bool on = ((now / 200 + i) & 1) != 0;
                if (on) canvas.fillRect(px, py, 2, 2, skyFlash(0xFFE0));
            }
        }
        // Day streak only in clear day
        if (nb < 40 && phase >= 12000 && phase < 12500) {
            int t = (int)(phase - 12000);
            canvas.drawLine(40 + t, 12 + t / 4, 50 + t, 15 + t / 4, skyFlash(0xFFFF));
        }
    }

    // Night / noir stars (twinkle, fat 2px — inspired by a city-night farm)
    const bool noirSky = (Weather::getActiveSeason() == Season::NOIR);
    if ((nb > 160 || noirSky) && wd < 80 && !raining) {
        static const uint8_t SX[18] = {
            12, 28, 47, 63, 81, 99, 118, 134, 152, 171, 189, 206, 221, 38, 74, 143, 198, 16
        };
        static const uint8_t SY[18] = {
            8, 18, 6, 24, 11, 20, 7, 16, 10, 22, 9, 19, 14, 28, 5, 13, 27, 21
        };
        for (int i = 0; i < 18; i++) {
            uint32_t tw = (now / (180u + (uint32_t)i * 17u) + (uint32_t)i);
            if ((tw & 3u) == 3u) continue;
            uint16_t sc = noirSky ? ((tw & 1u) ? (uint16_t)0xFE60 : (uint16_t)0xDEFB)
                                  : ((tw & 1u) ? (uint16_t)0xFFFF : (uint16_t)0xC618);
            canvas.fillRect((int)SX[i], (int)SY[i], 2, 2, skyFlash(sc));
        }
    }
}

// ---------------------------------------------------------------------------


uint16_t topColor() { return s_skyTop; }

bool isNight() {
    uint32_t now = millis();
    uint16_t amt = (s_lastNightBlendMs != 0) ? s_nightBlend : computeNightTarget(now);
    return (amt >= 128);
}

void getHud(char* out, size_t len) {
    if (!out || len < 8) return;
    out[0] = '\0';
    const char* sn = Weather::getSeasonShort();
    uint8_t sky = Config::personality().skyMode;
    if (sky == (uint8_t)SkyMode::DAY) {
        snprintf(out, len, "DAY %s", sn);
        return;
    }
    if (sky == (uint8_t)SkyMode::NIGHT) {
        snprintf(out, len, "NITE %s", sn);
        return;
    }

    Season season = Weather::getActiveSeason();
    int hour = -1, minute = 0, mins = 0;
    auto dt = M5.Rtc.getDateTime();
    if (dt.date.year >= 2024) {
        hour = (int)dt.time.hours;
        minute = (int)dt.time.minutes;
        mins = hour * 60 + minute;
    } else {
        time_t unixNow = time(nullptr);
        if (unixNow >= 1700000000) {
            struct tm timeinfo;
            localtime_r(&unixNow, &timeinfo);
            hour = timeinfo.tm_hour;
            minute = timeinfo.tm_min;
            mins = hour * 60 + minute;
        }
    }

    char phase = isNight() ? 'N' : 'D';
    if (hour >= 0) {
        int dawn0, dusk0;
        if (season == Season::SUMMER) {
            dawn0 = 4 * 60; dusk0 = 20 * 60;
        } else if (season == Season::WINTER) {
            dawn0 = 8 * 60; dusk0 = 15 * 60;
        } else {
            dawn0 = 5 * 60; dusk0 = 17 * 60;
        }
        int leftMin;
        if (!isNight()) {
            leftMin = dusk0 - mins;
            if (leftMin < 0) leftMin += 24 * 60;
        } else {
            leftMin = dawn0 - mins;
            if (leftMin < 0) leftMin += 24 * 60;
        }
        if (leftMin >= 60)
            snprintf(out, len, "%02d:%02d %c %s", hour, minute, phase, sn);
        else
            snprintf(out, len, "%02d:%02d %c%dm %s", hour, minute, phase, leftMin, sn);
        return;
    }

    uint32_t sec = (millis() / 1000u) % 360u;
    uint32_t dayS, duskS, nightS, dawnS;
    if (season == Season::SUMMER) {
        dayS = 200; duskS = 40; nightS = 80; dawnS = 40;
    } else if (season == Season::WINTER) {
        dayS = 80; duskS = 40; nightS = 200; dawnS = 40;
    } else {
        dayS = 140; duskS = 40; nightS = 140; dawnS = 40;
    }
    uint32_t left = 0;
    if (sec < dayS) {
        left = dayS - sec;
        phase = 'D';
    } else if (sec < dayS + duskS) {
        left = dayS + duskS - sec;
        phase = 'D';
    } else if (sec < dayS + duskS + nightS) {
        left = dayS + duskS + nightS - sec;
        phase = 'N';
    } else {
        left = 360u - sec;
        if (left == 0) left = dawnS ? dawnS : 1;
        phase = 'N';
    }
    snprintf(out, len, "%c %u:%02u %s", phase, (unsigned)(left / 60),
             (unsigned)(left % 60), sn);
}


static void initStarPositions() {
    // Pre-gen star positions, hide until spawn
    for (uint8_t i = 0; i < MAX_STARS; i++) {
        // y 20-100 sky/backdrop, bubble still wins
        // x 5-235 near full width
        s_stars[i].x = random(5, 235);
        // Match rain clip: keep stars above grass (rain clips at y < 88)
        s_stars[i].y = random(35, 103);
        s_stars[i].size = 1;
        s_stars[i].brightness = 0;
        s_stars[i].fadeInStart = 0;
        // About 20 percent twinkle
        s_stars[i].isBlinking = (random(0, 100) < 20);
    }
}

void updateStars() {
    uint32_t now = millis();
    // Keep blend advancing even if sky draw order varies
    updateNightBlend(now);
    const uint16_t nb = s_nightBlend;

    // Never show stars while raining
    if (Weather::isRaining()) {
        if (s_starsActive) {
            s_starsActive = false;
            s_starCount = 0;
        }
        return;
    }

    // Stars appear mid-dusk; fade out toward dawn (no hard kill)
    if (nb >= 140 && !s_starsActive) {
        s_starsActive = true;
        s_starCount = 0;
        s_lastStarSpawn = now;
        s_nextSpawnDelay = random(600, 2500);
        initStarPositions();
    } else if (nb < 80 && s_starsActive) {
        s_starsActive = false;
        s_starCount = 0;
        return;
    }

    if (!s_starsActive) return;

    // Spawn new star when timer expires
    if (s_starCount < MAX_STARS && (now - s_lastStarSpawn >= s_nextSpawnDelay)) {
        s_stars[s_starCount].fadeInStart = now;
        s_stars[s_starCount].brightness = 0;
        s_starCount++;
        s_lastStarSpawn = now;
        s_nextSpawnDelay = random(800, 4001);
    }

    // Fade-in per star + global gate from night blend (smooth dusk/dawn)
    // nb 80..200 maps to gate 0..255
    uint16_t gate = 255;
    if (nb < 200) {
        if (nb <= 80) gate = 0;
        else gate = (uint16_t)(((nb - 80) * 255) / 120);
    }

    for (uint8_t i = 0; i < s_starCount; i++) {
        uint32_t age = now - s_stars[i].fadeInStart;
        uint16_t local = (age < 500) ? (uint16_t)((age * 255) / 500) : 255;
        s_stars[i].brightness = (uint8_t)((local * gate) / 255);
    }
}


void drawStars(M5Canvas& canvas) {
    if (!s_starsActive || s_starCount == 0) return;

    uint32_t now = millis();

    for (uint8_t i = 0; i < s_starCount; i++) {
        if (s_stars[i].brightness < 40) continue;
        if (s_stars[i].y >= 103) continue;  // Match rain clip above grass

        int16_t sx = snapPx(s_stars[i].x);
        int16_t sy = snapPx(s_stars[i].y);
        // Dim stars: softer color until fully bright
        uint16_t col = skyFlash(C_STAR);
        if (s_stars[i].brightness < 200) {
            col = lerp565(s_skyTop, C_STAR, (uint8_t)((s_stars[i].brightness * 16) / 255));
            col = skyFlash(col);
        }
        canvas.fillRect(sx, sy, PX, PX, col);
        if (s_stars[i].isBlinking && s_stars[i].brightness > 180) {
            uint32_t phase = (now + i * 700) % 4000;
            if (phase >= 1700 && phase < 2300) {
                canvas.fillRect(sx - PX, sy, PX, PX, col);
                canvas.fillRect(sx + PX, sy, PX, PX, col);
                canvas.fillRect(sx, sy - PX, PX, PX, col);
                canvas.fillRect(sx, sy + PX, PX, PX, col);
            }
        }
    }
}

// --- Phase 8: Direction control helpers ---


void begin() {
    s_skyTop = C_SKY_TOP;
    s_nightBlend = 0;
    s_lastNightBlendMs = 0;
    s_weatherDark = 0;
    s_lastWeatherDarkMs = 0;
    s_starsActive = false;
    s_starCount = 0;
}

}  // namespace Sky
