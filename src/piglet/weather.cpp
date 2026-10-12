// Weather effects module - clouds, rain, thunder, wind
// Mood-tied weather system ported from Sirloin

#include "weather.h"
#include "avatar.h"
#include "mood.h"
#include "../ui/display.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../audio/sfx.h"
#include <esp_random.h>

namespace Weather {

// === SEASONS (AUTO every 15 min, or manual from SETTINGS) ===
static constexpr uint32_t SEASON_CYCLE_MS = 15UL * 60UL * 1000UL;  // 15 minutes
static Season activeSeason = Season::SUMMER;
static uint32_t seasonStartedMs = 0;
static uint8_t lastSeasonModeCfg = 255;  // force resync on first update

// === CLOUD SHAPE SYSTEM ===
struct CloudPuff {
    int8_t dx, dy;      // offset from cloud center
    uint8_t radius;     // circle radius (2-6px)
};

struct CloudShape {
    float x;            // current X position (float for smooth drift)
    int8_t y;           // Y center (5-11, keeps circles in Y 0-16 zone)
    uint8_t puffCount;  // 3-5 overlapping circles per cloud
    CloudPuff puffs[5];
    uint8_t scale;      // 0-255 growth animation (controls drawn radius)
    bool active;
    bool growing;       // scaling up toward 255
    bool shrinking;     // scaling down toward 0, deactivates at 0
};

static const uint8_t MAX_CLOUDS = 8;
static CloudShape clouds[MAX_CLOUDS];
static uint32_t lastCloudUpdate = 0;
static const uint16_t cloudSpeed = 14400;  // Ultra slow atmospheric drift (matches sirloin)
static uint32_t lastCloudParallax = 0;
static const uint8_t CLOUD_PARALLAX_GRASS_SHIFTS = 6;  // Shift clouds every N grass shifts
static uint32_t lastDensityCheck = 0;
static uint32_t lastScaleUpdate = 0;

// === RAIN STATE ===
struct RainDrop {
    float x;
    float y;
    uint8_t speed; // pixels per update for visible rain
    uint8_t len;   // streak length 2-4
};
static const int RAIN_DROP_COUNT = 42;   // rain streaks
static const int SNOW_FLAKE_COUNT = 72;  // denser winter snow
static RainDrop rainDrops[SNOW_FLAKE_COUNT] = {{0}};  // reuse pool; rain uses first 42
static bool rainActive = false;
static uint32_t lastRainUpdate = 0;

static const uint16_t RAIN_SPEED_MS = 28;

// === LIVING WEATHER CYCLE ===
// Mood only biases odds — rain/storm appear even when happy
static Phase weatherPhase = Phase::CLEAR;
static uint32_t phaseStartMs = 0;
static uint32_t phaseDurationMs = 45000;
static int currentMood = 50;

// === THUNDER STATE ===
static bool thunderFlashing = false;
static uint32_t lastThunderStorm = 0;
static uint32_t thunderFlashStart = 0;
static uint8_t thunderFlashesRemaining = 0;
static uint8_t thunderFlashState = 0;  // 0=off, 1=on
static uint32_t thunderMinInterval = 12000;
static uint32_t thunderMaxInterval = 28000;
static uint32_t nextThunderInterval = 18000;

// === WIND STATE ===
struct WindParticle {
    float x;
    float y;
    float speed;        // px per update tick
    float spawnX;       // X at birth (for distance calc)
    float maxTravel;    // distance before vanishing (180-280px, randomized)
    uint8_t baseSize;   // initial pixel radius 1-3
    bool active;
    bool dirRight;      // true = moves right, false = moves left
};
static WindParticle windParticles[6] = {{0}};
static bool windActive = false;
static uint32_t lastWindGust = 0;
static uint32_t windGustDuration = 0;
static uint32_t windGustInterval = 15000;  // 15-30s between gusts
static uint32_t lastWindUpdate = 0;

// === BIRD SYSTEM ===
struct SkyBird {
    float x;           // horizontal position (-20 to 260)
    int8_t y;          // sky zone (3-14)
    int8_t vx;         // speed: -2 to +2 px/tick (never 0)
    uint8_t sinePhase; // vertical wobble counter
    uint8_t kind;      // 0=sparrow 1=seagull 2=butterfly
    bool active;
    bool falling;
    float fallVy, fallX, fallY;
    float fallStartY;
};

struct BirdSpark {
    float x, y, vx, vy;
    uint8_t life;      // ticks remaining (0=inactive)
};

struct BirdExplosion {
    float x, y;           // impact center
    uint8_t radius;        // current blast radius (grows from 0)
    uint8_t maxRadius;     // target radius (9-12px)
    uint8_t life;          // ticks remaining
    bool active;
};

struct ImpactSplash {
    float x, y, vx, vy;
    uint8_t life;
    bool active;
};

static SkyBird birds[2];
static BirdSpark sparks[6];
static BirdExplosion explosions[2];     // max 2 concurrent (matches bird pool)
static ImpactSplash impactSplashes[6];  // shared splash pool
static int8_t whistlingBird = -1;       // index of bird currently whistling (-1 = none)
static uint32_t lastBirdUpdate = 0;
static uint32_t nextBirdSpawn = 0;

static void spawnBird() {
    int slot = -1;
    for (int i = 0; i < 2; i++) {
        if (!birds[i].active) { slot = i; break; }
    }
    if (slot < 0) return;

    SkyBird& b = birds[slot];
    b.y = (int8_t)random(8, 28);  // higher sky band (visible above hills)
    b.sinePhase = 0;
    b.active = true;
    b.falling = false;
    // Mix of cute sky critters
    int r = random(0, 100);
    b.kind = (r < 55) ? 0 : (r < 80) ? 1 : 2;  // sparrow / gull / butterfly

    bool goRight = random(0, 2) == 0;
    int spd = (b.kind == 2) ? 1 : random(1, 3);
    b.vx = goRight ? (int8_t)spd : (int8_t)(-spd);
    if (b.vx == 0) b.vx = 1;
    b.x = goRight ? -20.0f : 260.0f;
}

static void updateBirds(uint32_t now) {
    // Guard: skip entirely during rain
    if (rainActive) {
        for (int i = 0; i < 2; i++) birds[i].active = false;
        for (int i = 0; i < 6; i++) sparks[i].life = 0;
        for (int i = 0; i < 2; i++) explosions[i].active = false;
        for (int i = 0; i < 6; i++) impactSplashes[i].active = false;
        whistlingBird = -1;
        nextBirdSpawn = now + random(12000, 22001);
        return;
    }

    // 50ms tick rate (~20fps)
    if (now - lastBirdUpdate < 50) return;
    lastBirdUpdate = now;

    // Spawn check — more frequent for a lively sky
    if (now >= nextBirdSpawn) {
        spawnBird();
        nextBirdSpawn = now + random(6000, 14001);
    }

    // Update flying and falling birds
    for (int i = 0; i < 2; i++) {
        if (!birds[i].active) continue;
        SkyBird& b = birds[i];

        if (!b.falling) {
            // Flying: advance position
            b.x += (float)b.vx;
            b.sinePhase++;

            // Deactivate when off-screen
            if (b.x < -25.0f || b.x > 265.0f) {
                b.active = false;
                continue;
            }

            // Check wave collision
            int16_t drawY = b.y + ((b.sinePhase & 0x08) ? 1 : 0);  // 1px bob
            if (Avatar::checkBirdWaveCollision((int16_t)b.x, drawY)) {
                b.falling = true;
                b.fallVy = -1.5f;  // initial upward flick
                b.fallX = b.x;
                b.fallY = (float)drawY;
                b.fallStartY = (float)drawY;  // remember start for whistle pitch

                // Hit zap SFX
                SFX::play(SFX::BIRD_HIT);

                // Claim whistle slot if available
                if (whistlingBird < 0) whistlingBird = (int8_t)i;

                // Spawn 3 sparks
                int spawned = 0;
                for (int s = 0; s < 6 && spawned < 3; s++) {
                    if (sparks[s].life == 0) {
                        sparks[s].x = b.fallX;
                        sparks[s].y = b.fallY;
                        sparks[s].vx = (float)random(-20, 21) / 10.0f;  // -2.0 to +2.0
                        sparks[s].vy = -1.0f - (float)random(0, 15) / 10.0f;  // -1.0 to -2.5
                        sparks[s].life = random(10, 18);
                        spawned++;
                    }
                }

                // XP reward scaled to pig level: level*1 to level*3
                uint8_t lvl = XP::getLevel();
                if (lvl < 1) lvl = 1;
                uint16_t xp = (uint16_t)(lvl * random(1, 4));
                XP::addXP(xp);

                // Pig celebrates the kill
                Mood::onBirdKill();
            }
        } else {
            // Falling: gravity + drift
            b.fallVy += 0.4f;
            b.fallY += b.fallVy;
            b.fallX += (float)b.vx * 0.5f;

            // Bomb whistle: descending pitch tracks Y position
            if (whistlingBird == i && b.fallY < 106.0f) {
                float range = 106.0f - b.fallStartY;
                float progress = (range > 0.0f) ? (b.fallY - b.fallStartY) / range : 1.0f;
                if (progress < 0.0f) progress = 0.0f;
                if (progress > 1.0f) progress = 1.0f;
                uint16_t freq = (uint16_t)(1200.0f - progress * 1000.0f);  // 1200Hz → 200Hz
                SFX::tone(freq, 60);
            }

            // Ground impact
            if (b.fallY > 106.0f) {
                // Impact SFX
                SFX::play(SFX::BIRD_IMPACT);

                // Release whistle
                if (whistlingBird == i) whistlingBird = -1;

                // Spawn explosion
                for (int e = 0; e < 2; e++) {
                    if (!explosions[e].active) {
                        explosions[e].x = b.fallX;
                        explosions[e].y = 106.0f;
                        explosions[e].radius = 0;
                        explosions[e].maxRadius = (uint8_t)random(9, 13);
                        explosions[e].life = 12;
                        explosions[e].active = true;
                        break;
                    }
                }

                // Spawn 4 impact splashes
                int splashed = 0;
                for (int s = 0; s < 6 && splashed < 4; s++) {
                    if (!impactSplashes[s].active) {
                        impactSplashes[s].x = b.fallX + (float)random(-6, 7);
                        impactSplashes[s].y = 106.0f;
                        impactSplashes[s].vx = (float)random(-30, 31) / 10.0f;  // -3.0 to +3.0
                        impactSplashes[s].vy = -1.0f - (float)random(0, 16) / 10.0f;  // -1.0 to -2.5
                        impactSplashes[s].life = (uint8_t)random(12, 19);
                        impactSplashes[s].active = true;
                        splashed++;
                    }
                }

                b.active = false;
            }
        }
    }

    // Update sparks
    for (int s = 0; s < 6; s++) {
        if (sparks[s].life == 0) continue;
        sparks[s].x += sparks[s].vx;
        sparks[s].y += sparks[s].vy;
        sparks[s].vy += 0.25f;  // lighter gravity
        sparks[s].life--;
    }

    // Update explosions
    for (int e = 0; e < 2; e++) {
        if (!explosions[e].active) continue;
        if (explosions[e].radius < explosions[e].maxRadius) {
            explosions[e].radius++;
        } else {
            explosions[e].life--;
            if (explosions[e].life == 0) explosions[e].active = false;
        }
    }

    // Update impact splashes
    for (int s = 0; s < 6; s++) {
        if (!impactSplashes[s].active) continue;
        impactSplashes[s].x += impactSplashes[s].vx;
        impactSplashes[s].y += impactSplashes[s].vy;
        impactSplashes[s].vy += 0.3f;  // gravity
        impactSplashes[s].life--;
        if (impactSplashes[s].life == 0) impactSplashes[s].active = false;
    }
}

// Forward declarations
static void generateCloudPuffs(CloudShape& cloud);
static void activateCloud();
static void deactivateCloud();
static void updateSeasonCycle(uint32_t now);

// === INITIALIZATION ===
void init() {
    // All clouds start inactive (zero-initialized static)
    for (int i = 0; i < MAX_CLOUDS; i++) {
        clouds[i].active = false;
    }

    // Init wind particles (inactive)
    for (int i = 0; i < 6; i++) {
        windParticles[i].active = false;
    }

    // Init bird system
    for (int i = 0; i < 2; i++) birds[i].active = false;
    for (int i = 0; i < 6; i++) sparks[i].life = 0;
    for (int i = 0; i < 2; i++) explosions[i].active = false;
    for (int i = 0; i < 6; i++) impactSplashes[i].active = false;
    whistlingBird = -1;
    lastBirdUpdate = 0;
    nextBirdSpawn = millis() + random(3000, 8001);

    lastCloudUpdate = millis();
    lastCloudParallax = lastCloudUpdate;
    lastDensityCheck = lastCloudUpdate;
    lastScaleUpdate = lastCloudUpdate;
    lastWindGust = millis();
    lastThunderStorm = millis();
    nextThunderInterval = random(thunderMinInterval, thunderMaxInterval);
    // Kick living weather cycle (first rain arrives ~20-35s)
    phaseStartMs = 0;
    weatherPhase = Phase::CLEAR;
    rainActive = false;
    // Season: apply current config immediately
    seasonStartedMs = millis();
    lastSeasonModeCfg = 255;
    activeSeason = Season::SUMMER;
    updateSeasonCycle(millis());
}

static void generateCloudPuffs(CloudShape& cloud) {
    // Bigger fluffy clouds (visible 2.5D sky)
    cloud.puffs[0] = {0, 0, (uint8_t)random(8, 12)};
    cloud.puffs[1] = {(int8_t)random(-16, -9), (int8_t)random(0, 3), (uint8_t)random(6, 10)};
    cloud.puffs[2] = {(int8_t)random(9, 17), (int8_t)random(0, 3), (uint8_t)random(6, 10)};
    cloud.puffCount = 3;

    if (random(0, 100) < 80) {
        cloud.puffs[cloud.puffCount] = {(int8_t)random(-4, 5), (int8_t)random(-5, -2), (uint8_t)random(4, 7)};
        cloud.puffCount++;
    }
    if (cloud.puffCount < 5 && random(0, 100) < 60) {
        int8_t side = random(0, 2) ? (int8_t)14 : (int8_t)-14;
        cloud.puffs[cloud.puffCount] = {(int8_t)(side + (int8_t)random(-3, 4)), (int8_t)random(0, 3), (uint8_t)random(4, 7)};
        cloud.puffCount++;
    }
}

static int getActiveCloudCount() {
    int count = 0;
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (clouds[i].active) count++;
    }
    return count;
}

static int getTargetCloudCount() {
    switch (weatherPhase) {
        case Phase::STORM:  return MAX_CLOUDS;
        case Phase::RAIN:   return 6;
        case Phase::CLOUDY: return 4;
        case Phase::CLEAR:
        default: {
            int target = 2;
            if (currentMood < -20) target = 3;
            if (currentMood > 40) target = 1;
            return target;
        }
    }
}

static void activateCloud() {
    // Find an inactive slot
    int slot = -1;
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (!clouds[i].active) { slot = i; break; }
    }
    if (slot < 0) return;

    // Try to find a well-spaced X position (5 attempts, keep best)
    float bestX = (float)random(-20, 261);
    float bestDist = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        float tryX = (float)random(-20, 261);
        float minDist = 300.0f;
        for (int i = 0; i < MAX_CLOUDS; i++) {
            if (i == slot || !clouds[i].active) continue;
            float d = tryX - clouds[i].x;
            float dist = d < 0 ? -d : d;
            if (dist > 140.0f) dist = 280.0f - dist;  // wrap distance
            if (dist < minDist) minDist = dist;
        }
        if (minDist > bestDist) {
            bestDist = minDist;
            bestX = tryX;
        }
    }

    CloudShape& c = clouds[slot];
    c.x = bestX;
    c.y = (int8_t)random(5, 12);
    c.scale = 0;
    c.active = true;
    c.growing = true;
    c.shrinking = false;
    generateCloudPuffs(c);
}

static void deactivateCloud() {
    // Pick the active cloud furthest from screen center to shrink away
    float maxDist = -1;
    int pick = -1;
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (!clouds[i].active || clouds[i].shrinking) continue;
        float d = clouds[i].x - 120.0f;  // 240px center
        float dist = d < 0 ? -d : d;
        if (dist > maxDist) {
            maxDist = dist;
            pick = i;
        }
    }
    if (pick >= 0) {
        clouds[pick].shrinking = true;
        clouds[pick].growing = false;
    }
}

// === WEATHER STATE CONTROL ===
// Mood only biases the living cycle — never freezes weather on "happy = clear forever"
void setMoodLevel(int momentum) {
    currentMood = momentum;
}

static bool seasonIsWinter() { return activeSeason == Season::WINTER; }
static bool seasonIsSpring() { return activeSeason == Season::SPRING; }
static bool seasonIsSummer() { return activeSeason == Season::SUMMER; }
static bool seasonIsAutumn() { return activeSeason == Season::AUTUMN; }
static bool seasonIsRetro()  { return activeSeason == Season::RETRO; }

Season getActiveSeason() { return activeSeason; }

const char* getSeasonName() {
    switch (activeSeason) {
        case Season::SPRING: return "SPRING";
        case Season::SUMMER: return "SUMMER";
        case Season::AUTUMN: return "AUTUMN";
        case Season::WINTER: return "WINTER";
        case Season::RETRO:  return "RETRO";
        case Season::NOIR:   return "NOIR";
        case Season::CITY:   return "CITY";
        case Season::DESERT: return "DESERT";
    }
    return "SUMMER";
}

const char* getSeasonShort() {
    switch (activeSeason) {
        case Season::SPRING: return "SPR";
        case Season::SUMMER: return "SUM";
        case Season::AUTUMN: return "AUT";
        case Season::WINTER: return "WIN";
        case Season::RETRO:  return "RET";
        case Season::NOIR:   return "NOI";
        case Season::CITY:   return "CIT";
        case Season::DESERT: return "DES";
    }
    return "SUM";
}

uint32_t secondsUntilSeasonChange() {
    uint8_t mode = Config::personality().seasonMode;
    if (mode != (uint8_t)SeasonMode::AUTO) return 0;
    if (seasonStartedMs == 0) return SEASON_CYCLE_MS / 1000u;
    uint32_t now = millis();
    uint32_t elapsed = now - seasonStartedMs;
    if (elapsed >= SEASON_CYCLE_MS) return 0;
    return (SEASON_CYCLE_MS - elapsed) / 1000u;
}

static void setActiveSeason(Season s, uint32_t now, bool toast) {
    if (activeSeason == s && seasonStartedMs != 0) return;
    activeSeason = s;
    seasonStartedMs = now;
    if (toast) {
        char buf[28];
        snprintf(buf, sizeof(buf), "SEASON: %s", getSeasonName());
        Display::showToast(buf, 1600);
    }
}

static void updateSeasonCycle(uint32_t now) {
    uint8_t mode = Config::personality().seasonMode;
    if (mode >= SEASON_MODE_COUNT) mode = 0;

    // Manual mode — lock season (RETRO included, not part of AUTO cycle)
    if (mode != (uint8_t)SeasonMode::AUTO) {
        Season forced = Season::SUMMER;
        if (mode == (uint8_t)SeasonMode::SPRING) forced = Season::SPRING;
        else if (mode == (uint8_t)SeasonMode::SUMMER) forced = Season::SUMMER;
        else if (mode == (uint8_t)SeasonMode::AUTUMN) forced = Season::AUTUMN;
        else if (mode == (uint8_t)SeasonMode::WINTER) forced = Season::WINTER;
        else if (mode == (uint8_t)SeasonMode::RETRO) {
            if (XP::isSeasonLocked((uint8_t)SeasonMode::RETRO))
                forced = Season::SUMMER;
            else
                forced = Season::RETRO;
        } else if (mode == (uint8_t)SeasonMode::NOIR) {
            if (XP::isSeasonLocked((uint8_t)SeasonMode::NOIR))
                forced = Season::SUMMER;
            else
                forced = Season::NOIR;
        } else if (mode == (uint8_t)SeasonMode::CITY) {
            if (XP::isSeasonLocked((uint8_t)SeasonMode::CITY))
                forced = Season::SUMMER;
            else
                forced = Season::CITY;
        } else if (mode == (uint8_t)SeasonMode::DESERT) {
            if (XP::isSeasonLocked((uint8_t)SeasonMode::DESERT))
                forced = Season::SUMMER;
            else
                forced = Season::DESERT;
        }
        bool changed = (lastSeasonModeCfg != mode) || (activeSeason != forced);
        setActiveSeason(forced, now, changed && lastSeasonModeCfg != 255);
        lastSeasonModeCfg = mode;
        return;
    }

    // AUTO — advance every 15 minutes (SPRING..WINTER only, never RETRO)
    if (lastSeasonModeCfg != mode) {
        // Just switched to AUTO: leave RETRO if stuck there
        if (activeSeason == Season::RETRO || activeSeason == Season::NOIR ||
            activeSeason == Season::CITY || activeSeason == Season::DESERT)
            activeSeason = Season::SUMMER;
        lastSeasonModeCfg = mode;
        seasonStartedMs = now;
        return;
    }
    if (seasonStartedMs == 0) {
        seasonStartedMs = now;
        return;
    }
    if (now - seasonStartedMs >= SEASON_CYCLE_MS) {
        uint8_t cur = (uint8_t)activeSeason;
        if (cur >= SEASON_COUNT) cur = 0;
        Season next = (Season)((cur + 1) % SEASON_COUNT);
        setActiveSeason(next, now, true);
    }
}

void setRaining(bool active) {
    if (active && !rainActive) {
        int n = (seasonIsWinter() || seasonIsRetro()) ? SNOW_FLAKE_COUNT : RAIN_DROP_COUNT;
        for (int i = 0; i < n; i++) {
            rainDrops[i].x = (float)random(0, DISPLAY_W);
            if (seasonIsWinter() || seasonIsRetro()) {
                rainDrops[i].speed = random(1, 3);
                rainDrops[i].len = (uint8_t)random(1, 3);  // 1=tiny, 2=small cross / pixel size
                // Spread full height so first frame already snows from the bar
                rainDrops[i].y = (float)random(0, 100);
            } else if (seasonIsSummer()) {
                rainDrops[i].y = (float)random(8, 100);
                rainDrops[i].speed = random(3, 7);
                rainDrops[i].len = (uint8_t)random(2, 4);
            } else {
                rainDrops[i].y = (float)random(8, 100);
                rainDrops[i].speed = random(4, 9);
                rainDrops[i].len = (uint8_t)random(2, 5);
            }
        }
    } else if (!active && rainActive) {
        thunderFlashing = false;
        thunderFlashState = 0;
        thunderFlashesRemaining = 0;
        lastThunderStorm = millis();
    }
    rainActive = active;
}

void triggerThunderStorm() {
    thunderFlashesRemaining = 3;
    lastThunderStorm = millis();
}

static void applyPhase(Phase p, uint32_t now) {
    // Season rules: summer never storms; winter "storm" = heavy snow (no lightning)
    if (seasonIsSummer() && p == Phase::STORM) p = Phase::RAIN;
    if (seasonIsWinter() && p == Phase::STORM) {
        // keep STORM phase for density, but thunder disabled below
    }
    // DESERT: no rain / snow / lightning — only clear or hazy cloudy
    if (activeSeason == Season::DESERT) {
        if (p == Phase::RAIN || p == Phase::STORM) p = Phase::CLEAR;
        setRaining(false);
    }

    weatherPhase = p;
    phaseStartMs = now;

    switch (p) {
        case Phase::CLEAR:
            phaseDurationMs = (uint32_t)random(35000, 70000);
            setRaining(false);
            thunderMinInterval = 999999;
            thunderMaxInterval = 999999;
            break;
        case Phase::CLOUDY:
            phaseDurationMs = (uint32_t)random(25000, 50000);
            setRaining(false);
            thunderMinInterval = 999999;
            thunderMaxInterval = 999999;
            break;
        case Phase::RAIN:
            phaseDurationMs = (uint32_t)random(20000, 45000);
            setRaining(activeSeason != Season::DESERT);
            thunderMinInterval = 999999;
            thunderMaxInterval = 999999;
            break;
        case Phase::STORM:
            phaseDurationMs = (uint32_t)random(15000, 30000);
            setRaining(activeSeason != Season::DESERT);
            if (seasonIsSpring()) {
                // Spring: real thunderstorms
                thunderMinInterval = 8000;
                thunderMaxInterval = 18000;
                nextThunderInterval = random(thunderMinInterval, thunderMaxInterval);
                lastThunderStorm = now;
                triggerThunderStorm();
            } else if (seasonIsAutumn()) {
                // Rare distant rumble — short flashes only sometimes
                thunderMinInterval = 20000;
                thunderMaxInterval = 40000;
                nextThunderInterval = random(thunderMinInterval, thunderMaxInterval);
                lastThunderStorm = now;
            } else {
                // Winter/summer: no bolts
                thunderMinInterval = 999999;
                thunderMaxInterval = 999999;
                thunderFlashesRemaining = 0;
                thunderFlashing = false;
            }
            break;
    }

    if (currentMood < -30 && (p == Phase::RAIN || p == Phase::STORM)) {
        phaseDurationMs += 10000;
    }
    if (currentMood > 30 && p == Phase::CLEAR) {
        phaseDurationMs += 12000;
    }
}

static Phase pickNextPhase(Phase cur) {
    // DESERT: never schedule rain/storm — sandstorm is seasonal_fx only
    if (activeSeason == Season::DESERT) {
        if (cur == Phase::CLEAR) return (random(0, 100) < 35) ? Phase::CLOUDY : Phase::CLEAR;
        return (random(0, 100) < 50) ? Phase::CLEAR : Phase::CLOUDY;
    }

    int roll = random(0, 100);
    int rainBias = 0;
    if (currentMood <= -40) rainBias = 20;
    else if (currentMood <= -15) rainBias = 10;
    else if (currentMood >= 40) rainBias = -12;

    // Spring: stormy; Summer: gentle rain only; Autumn: rain + leaves; Winter: snow
    int stormBias = 0;
    if (seasonIsSpring()) stormBias = 12;
    else if (seasonIsSummer()) stormBias = -40;  // almost no storms
    else if (seasonIsAutumn()) stormBias = -5;
    else if (seasonIsWinter()) stormBias = 5;    // heavy snow flurries ok

    switch (cur) {
        case Phase::CLEAR:
            if (roll < 55 + rainBias) return Phase::CLOUDY;
            if (roll < 75 + rainBias) return Phase::RAIN;
            if (roll < 85 + rainBias / 2 + stormBias) return Phase::STORM;
            return Phase::CLEAR;
        case Phase::CLOUDY:
            if (roll < 30 - rainBias / 2) return Phase::CLEAR;
            if (roll < 55 + rainBias) return Phase::RAIN;
            if (roll < 70 + rainBias + stormBias) return Phase::STORM;
            return Phase::CLOUDY;
        case Phase::RAIN:
            if (roll < 25 + rainBias / 2 + stormBias) return Phase::STORM;
            if (roll < 55) return Phase::CLOUDY;
            if (roll < 75) return Phase::CLEAR;
            return Phase::RAIN;
        case Phase::STORM:
            if (roll < 40) return Phase::RAIN;
            if (roll < 75) return Phase::CLOUDY;
            return Phase::CLEAR;
    }
    return Phase::CLEAR;
}

static void updateWeatherCycle(uint32_t now) {
    if (phaseStartMs == 0) {
        // First boot: start clear, then natural cycle
        applyPhase(Phase::CLEAR, now);
        // First rain not too far away so user sees it soon
        phaseDurationMs = (uint32_t)random(20000, 35000);
        return;
    }
    if (now - phaseStartMs >= phaseDurationMs) {
        applyPhase(pickNextPhase(weatherPhase), now);
    }
}

// Forward declarations for static update functions
static void updateClouds(uint32_t now);
static void updateRain(uint32_t now);
static void updateThunder(uint32_t now);
static void updateWind(uint32_t now);

// === ANIMATION UPDATES ===
void update() {
    uint32_t now = millis();

    updateSeasonCycle(now);

    // Living weather cycle first (sets rainActive / storm)
    updateWeatherCycle(now);

    updateClouds(now);

    if (rainActive) {
        updateRain(now);
    }

    // Thunder: spring storms (and residual flashes)
    if ((weatherPhase == Phase::STORM && seasonIsSpring()) ||
        thunderFlashesRemaining > 0 || thunderFlashing) {
        if (!(seasonIsWinter() || seasonIsSummer())) {
            updateThunder(now);
        } else {
            thunderFlashing = false;
            thunderFlashesRemaining = 0;
        }
    }

    updateWind(now);
    updateBirds(now);
    // Seasonal decor (banks, leaves, tumbleweed, butterflies) → SeasonalFx
}

static void updateClouds(uint32_t now) {
    // Self-drift: slow rightward movement
    if (now - lastCloudUpdate >= cloudSpeed) {
        lastCloudUpdate = now;
        for (int i = 0; i < MAX_CLOUDS; i++) {
            if (clouds[i].active) clouds[i].x += 0.5f;
        }
    }

    // Parallax: when grass is moving, nudge clouds in the same direction (slower)
    if (Avatar::isGrassMoving()) {
        uint32_t parallaxInterval = (uint32_t)Avatar::getGrassSpeed() * CLOUD_PARALLAX_GRASS_SHIFTS;
        if (parallaxInterval < 150) parallaxInterval = 150;

        if (now - lastCloudParallax >= parallaxInterval) {
            lastCloudParallax = now;
            float shift = Avatar::isGrassDirectionRight() ? 1.0f : -1.0f;
            for (int i = 0; i < MAX_CLOUDS; i++) {
                if (clouds[i].active) clouds[i].x += shift;
            }
        }
    } else {
        lastCloudParallax = now;
    }

    // Wrap cloud positions: virtual range -40 to 280
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (!clouds[i].active) continue;
        if (clouds[i].x > 280.0f) clouds[i].x -= 320.0f;
        if (clouds[i].x < -40.0f) clouds[i].x += 320.0f;
    }

    // Density check: every 2 seconds, add or remove clouds to match mood
    if (now - lastDensityCheck >= 2000) {
        lastDensityCheck = now;
        int target = getTargetCloudCount();
        int active = getActiveCloudCount();
        if (active < target) {
            activateCloud();
        } else if (active > target) {
            deactivateCloud();
        }
    }

    // Scale animation: step growth/shrink every 80ms
    if (now - lastScaleUpdate >= 80) {
        lastScaleUpdate = now;
        for (int i = 0; i < MAX_CLOUDS; i++) {
            if (!clouds[i].active) continue;
            if (clouds[i].growing) {
                if (clouds[i].scale <= 240) {
                    clouds[i].scale += 15;
                } else {
                    clouds[i].scale = 255;
                    clouds[i].growing = false;
                }
            } else if (clouds[i].shrinking) {
                if (clouds[i].scale >= 15) {
                    clouds[i].scale -= 15;
                } else {
                    clouds[i].scale = 0;
                    clouds[i].active = false;
                    clouds[i].shrinking = false;
                }
            }
        }
    }
}

static void updateRain(uint32_t now) {
    // Snow / retro pixels tick a bit slower for soft fall
    uint16_t tickMs = (seasonIsWinter() || seasonIsRetro()) ? 36 : RAIN_SPEED_MS;
    if (now - lastRainUpdate < tickMs) return;
    lastRainUpdate = now;

    float horizontalDrift = 0.0f;
    if (Avatar::isGrassMoving()) {
        if (seasonIsWinter() || seasonIsRetro()) {
            // Soft parallax only — old formula used grassSpeed ms and turned
            // fast SCROLL SPD into a full blizzard while walking
            horizontalDrift = Avatar::isGrassDirectionRight() ? -0.7f : 0.7f;
        } else {
            uint16_t grassSpeedMs = Avatar::getGrassSpeed();
            if (grassSpeedMs == 0) grassSpeedMs = 1;
            const float grassShiftPixels = 8.0f;
            float grassPixelsPerMs = grassShiftPixels / (float)grassSpeedMs;
            float grassPixelsPerUpdate = grassPixelsPerMs * (float)tickMs;
            horizontalDrift = grassPixelsPerUpdate * 0.4f;
            // Cap rain drift so fast treadmill never becomes a sideways wall
            if (horizontalDrift > 3.0f) horizontalDrift = 3.0f;
            if (horizontalDrift < -3.0f) horizontalDrift = -3.0f;
            if (Avatar::isGrassDirectionRight()) {
                horizontalDrift *= -1.0f;
            }
        }
    }
    if (seasonIsWinter() || seasonIsRetro()) {
        // Gentle flutter, not storm
        horizontalDrift += (float)(random(0, 3) - 1) * 0.25f;
    }

    int n = (seasonIsWinter() || seasonIsRetro()) ? SNOW_FLAKE_COUNT : RAIN_DROP_COUNT;
    for (int i = 0; i < n; i++) {
        float fall = (float)rainDrops[i].speed;
        if (seasonIsWinter() || seasonIsRetro()) fall = 0.5f + (float)rainDrops[i].speed * 0.55f;
        rainDrops[i].y += fall;
        rainDrops[i].x += horizontalDrift;

        if (rainDrops[i].x < 0.0f) rainDrops[i].x += (float)DISPLAY_W;
        if (rainDrops[i].x >= (float)DISPLAY_W) rainDrops[i].x -= (float)DISPLAY_W;

        if (rainDrops[i].y >= 103.0f) {
            // Winter/retro: re-enter from the very top of the scene (under top bar)
            rainDrops[i].y = (seasonIsWinter() || seasonIsRetro()) ? (float)random(0, 8)
                                              : (float)random(8, 24);
            rainDrops[i].x = (float)random(0, DISPLAY_W);
            if (seasonIsWinter() || seasonIsRetro()) {
                rainDrops[i].speed = random(1, 3);
                rainDrops[i].len = (uint8_t)random(1, 3);
            } else if (seasonIsSummer()) {
                rainDrops[i].speed = random(3, 7);
                rainDrops[i].len = (uint8_t)random(2, 4);
            } else {
                rainDrops[i].speed = random(4, 9);
                rainDrops[i].len = (uint8_t)random(2, 5);
            }
        }
    }
}

// Compact fat-pixel snow (2px blocks — readable but not giant stars)
static void drawFatSnowflake(M5Canvas& canvas, int16_t x, int16_t y, uint8_t kind) {
    const int S = 2;  // smaller than avatar PX=3
    auto snap = [](int16_t v) -> int16_t {
        return (v >= 0) ? (int16_t)((v / 2) * 2) : (int16_t)(((v - 1) / 2) * 2);
    };
    int16_t sx = snap(x);
    int16_t sy = snap(y);
    // Main canvas is under the top bar — y=0 is already "from the bar"
    if (sy < -S || sy >= 104) return;

    const uint16_t W  = 0xFFFF;
    const uint16_t W2 = 0xDEFB;
    auto blk = [&](int16_t bx, int16_t by, uint16_t c) {
        if (bx < -S || bx >= 240 || by < 0 || by >= 104) return;
        canvas.fillRect(bx, by, S, S, c);
    };

    // kind 1: tiny 2x2 + soft halo (small flake)
    //  #
    // # #
    blk(sx, sy, W);
    if (kind >= 1) {
        blk(sx - S, sy, W2);
        blk(sx + S, sy, W2);
        blk(sx, sy - S, W2);
    }
    // kind 2: small cross only (still compact)
    if (kind >= 2) {
        blk(sx, sy + S, W2);
    }
}

static void updateThunder(uint32_t now) {
    // Check if time for new storm
    if (!thunderFlashing && thunderFlashesRemaining == 0) {
        if (now - lastThunderStorm >= nextThunderInterval) {
            thunderFlashesRemaining = random(2, 4);  // 2-3 flashes
            lastThunderStorm = now;
            nextThunderInterval = random(thunderMinInterval, thunderMaxInterval);
        }
    }

    // Execute flash sequence
    if (thunderFlashesRemaining > 0 && !thunderFlashing) {
        thunderFlashing = true;
        thunderFlashStart = now;
        thunderFlashState = 1;  // Flash ON
        thunderFlashesRemaining--;
        SFX::play(SFX::THUNDER);  // crack on each flash ON
    }

    if (thunderFlashing) {
        uint32_t elapsed = now - thunderFlashStart;

        // Faster flicker: shorter ON/OFF windows
        if (thunderFlashState == 1 && elapsed > random(30, 60)) {
            // Turn flash OFF
            thunderFlashState = 0;
            thunderFlashStart = now;
        } else if (thunderFlashState == 0 && elapsed > random(20, 40)) {
            // Flash complete
            thunderFlashing = false;
            thunderFlashState = 0;
        }
    }
}

static void updateWind(uint32_t now) {
    // Suppress wind during rain
    if (rainActive) {
        if (windActive) {
            windActive = false;
            for (int i = 0; i < 6; i++) windParticles[i].active = false;
        }
        lastWindGust = now;  // Reset so gust doesn't fire immediately after rain stops
        return;
    }

    // Check for new wind gust
    if (!windActive && now - lastWindGust > windGustInterval) {
        bool grassOn = Avatar::isGrassMoving();
        int spawnChance = grassOn ? 70 : 20;

        if ((int)random(0, 100) < spawnChance) {
            windActive = true;
            windGustDuration = random(2000, 4000);
            lastWindGust = now;

            bool goRight = grassOn ? Avatar::isGrassDirectionRight() : (random(0, 2) == 0);

            for (int i = 0; i < 6; i++) {
                float spawnX = goRight
                    ? (-5.0f - random(0, 40))
                    : ((float)DISPLAY_W + 5.0f + random(0, 40));
                windParticles[i].x = spawnX;
                windParticles[i].spawnX = spawnX;
                windParticles[i].y = (float)random(20, 88);
                windParticles[i].speed = 2.0f + (float)random(0, 30) / 10.0f;  // 2.0-5.0
                windParticles[i].maxTravel = (float)random(180, 281);
                windParticles[i].baseSize = random(1, 4);  // 1-3 px radius
                windParticles[i].active = true;
                windParticles[i].dirRight = goRight;
            }
        } else {
            windGustInterval = grassOn ? random(3000, 8000) : random(15000, 30000);
            lastWindGust = now;
        }
    }

    // Update active wind particles
    if (windActive) {
        if (now - lastWindGust > windGustDuration) {
            // Gust finished
            windActive = false;
            windGustInterval = Avatar::isGrassMoving() ? random(3000, 8000) : random(15000, 30000);
            for (int i = 0; i < 6; i++) {
                windParticles[i].active = false;
            }
        } else {
            // Animate particles with directional movement
            if (now - lastWindUpdate > 50) {
                lastWindUpdate = now;
                for (int i = 0; i < 6; i++) {
                    if (!windParticles[i].active) continue;
                    float dir = windParticles[i].dirRight ? 1.0f : -1.0f;
                    windParticles[i].x += windParticles[i].speed * dir;
                    windParticles[i].y += (random(0, 3) - 1) * 0.5f;  // vertical wobble

                    float dist = windParticles[i].x - windParticles[i].spawnX;
                    if (dist < 0) dist = -dist;
                    if (dist >= windParticles[i].maxTravel) {
                        windParticles[i].active = false;
                    }
                }
            }
        }
    }
}

// === THUNDER FLASH QUERY ===
bool isThunderFlashing() {
    return thunderFlashing && thunderFlashState == 1;
}

bool isRaining() {
    if (activeSeason == Season::DESERT) return false;
    return rainActive;
}

bool isSnowing() {
    if (activeSeason == Season::DESERT) return false;
    return rainActive && seasonIsWinter();
}

bool isStorming() {
    // Real lightning storms only in spring (and rare autumn flashes)
    return weatherPhase == Phase::STORM && seasonIsSpring();
}

bool isCloudy() {
    return weatherPhase == Phase::CLOUDY || weatherPhase == Phase::RAIN || weatherPhase == Phase::STORM;
}

Phase getPhase() {
    return weatherPhase;
}

// === DRAWING ===

// Pixel-art circle: 3-band stepped rectangle (blocky puff)
static void drawPixelPuff(M5Canvas& canvas, int cx, int cy, int r, uint16_t color) {
    if (r <= 1) {
        canvas.fillRect(cx - 1, cy - 1, 3, 3, color);
        return;
    }
    int inset = (r + 1) / 2;
    // Wide center band
    canvas.fillRect(cx - r, cy - r + inset, r * 2, r * 2 - inset * 2, color);
    // Narrower top row
    canvas.fillRect(cx - r + inset, cy - r, (r - inset) * 2, inset, color);
    // Narrower bottom row
    canvas.fillRect(cx - r + inset, cy + r - inset, (r - inset) * 2, inset, color);
}

void drawClouds(M5Canvas& canvas, uint16_t colorFG, int16_t yOffset) {
    // 2.5D fluffy clouds — stormy ones darker
    // yOffset = TOP_BAR_H when drawing into topBar so tops bleed above main scene
    (void)colorFG;
    const bool flash = isThunderFlashing();
    const bool storm = (weatherPhase == Phase::STORM);
    const bool wet = rainActive;
    const bool retro = seasonIsRetro();
    uint16_t CLOUD_TOP   = flash ? 0xFFFF : (storm ? 0x9CF3 : 0xFFFF);
    uint16_t CLOUD_MID   = flash ? 0xFFFF : (storm ? 0x7BCF : (wet ? 0xC618 : 0xEF5D));
    uint16_t CLOUD_SHADE = flash ? 0xFFFF : (storm ? 0x528A : (wet ? 0x8410 : 0xBDF7));
    uint16_t CLOUD_RIM   = flash ? 0xFFFF : (storm ? 0x6B4D : 0xDEDB);
    if (retro && !flash) {
        // Film-stock clouds — pure grayscale
        CLOUD_TOP   = 0xFFFF;
        CLOUD_MID   = 0xC618;
        CLOUD_SHADE = 0x8410;
        CLOUD_RIM   = 0xAD55;
    }

    float rainBoost = wet ? 1.7f : (weatherPhase == Phase::CLOUDY ? 1.25f : 1.0f);
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (!clouds[i].active || clouds[i].scale == 0) continue;
        float scaleFactor = (float)clouds[i].scale / 255.0f;

        for (int p = 0; p < clouds[i].puffCount; p++) {
            int r = (int)((float)clouds[i].puffs[p].radius * scaleFactor * rainBoost + 0.5f);
            if (r < 1) continue;
            int cx = (int)(clouds[i].x + clouds[i].puffs[p].dx);
            // Slightly higher in main canvas so puffs naturally spill into top bar
            int cy = clouds[i].y + clouds[i].puffs[p].dy + (storm ? 2 : 0) - 2 + (int)yOffset;

            auto puff = [&](int x) {
                drawPixelPuff(canvas, x, cy + 2, r + 1, CLOUD_SHADE);
                drawPixelPuff(canvas, x, cy, r, CLOUD_MID);
                drawPixelPuff(canvas, x - 1, cy - 1, (r > 2) ? r - 1 : 1, CLOUD_TOP);
                drawPixelPuff(canvas, x + r / 2, cy + 1, 1, CLOUD_RIM);
            };
            puff(cx);
            if (cx - r < 20) puff(cx + 320);
            else if (cx + r > 220) puff(cx - 320);
        }
    }
}

// Fat pixel size matching avatar grid (3x3 blocks)
static constexpr int16_t BIRD_PX = 3;

static inline int16_t birdSnap(int16_t v) {
    return (v >= 0) ? (v / BIRD_PX) * BIRD_PX : ((v - 2) / BIRD_PX) * BIRD_PX;
}

void drawBirds(M5Canvas& canvas, uint16_t colorFG) {
    (void)colorFG;
    const bool flash = isThunderFlashing();
    const bool retro = seasonIsRetro();

    for (int i = 0; i < 2; i++) {
        if (!birds[i].active) continue;
        const SkyBird& b = birds[i];

        if (!b.falling) {
            int16_t bx = birdSnap((int16_t)b.x);
            int bob = (b.sinePhase & 0x08) ? 1 : 0;
            int16_t bodyY = birdSnap(b.y + bob);
            bool wingsUp = (b.sinePhase & 0x04) != 0;
            bool right = b.vx > 0;

            if (b.kind == 2) {
                // Butterfly: colorful wings flap (gray in RETRO)
                uint16_t wing = flash ? 0xFFFF : (retro ? 0xC618 : ((i & 1) ? 0xFD1F : 0xFBE0));
                uint16_t body = flash ? 0xFFFF : (retro ? 0x9CF3 : 0xFDB2);
                int wy = wingsUp ? -2 : 1;
                canvas.fillRect(bx, bodyY + wy, BIRD_PX, BIRD_PX, wing);
                canvas.fillRect(bx + 2 * BIRD_PX, bodyY + wy, BIRD_PX, BIRD_PX, wing);
                canvas.fillRect(bx + BIRD_PX, bodyY, BIRD_PX, BIRD_PX, body);
            } else if (b.kind == 1) {
                // Seagull: white body + grey wings V
                uint16_t body = flash ? 0xFFFF : 0xFFFF;
                uint16_t wing = flash ? 0xFFFF : (retro ? 0x8410 : 0xBDF7);
                int16_t wingY = wingsUp ? (bodyY - BIRD_PX) : (bodyY + 1);
                canvas.fillRect(bx, wingY, BIRD_PX, BIRD_PX, wing);
                canvas.fillRect(bx + 2 * BIRD_PX, wingY, BIRD_PX, BIRD_PX, wing);
                canvas.fillRect(bx + BIRD_PX, bodyY, BIRD_PX, BIRD_PX, body);
                int16_t beakX = right ? (bx + 3 * BIRD_PX) : (bx - BIRD_PX);
                canvas.fillRect(beakX, bodyY, BIRD_PX, BIRD_PX, flash ? 0xFFFF : (retro ? 0xAD55 : 0xFDA0));
            } else {
                // Sparrow: warm brown + cream + V wings
                uint16_t body = flash ? 0xFFFF : (retro ? 0x7BEF : 0x9AC8);
                uint16_t wing = flash ? 0xFFFF : (retro ? 0x632C : 0x82A6);
                uint16_t belly = flash ? 0xFFFF : (retro ? 0xC618 : 0xF6B2);
                int16_t wingY = wingsUp ? (bodyY - BIRD_PX) : (bodyY + BIRD_PX);
                canvas.fillRect(bx, wingY, BIRD_PX, BIRD_PX, wing);
                canvas.fillRect(bx + 2 * BIRD_PX, wingY, BIRD_PX, BIRD_PX, wing);
                canvas.fillRect(bx + BIRD_PX, bodyY, BIRD_PX, BIRD_PX, body);
                canvas.fillRect(bx + BIRD_PX, bodyY + 1, BIRD_PX, 1, belly);
                int16_t beakX = right ? (bx + 3 * BIRD_PX) : (bx - BIRD_PX);
                canvas.fillRect(beakX, bodyY, BIRD_PX, BIRD_PX, flash ? 0xFFFF : (retro ? 0x9CF3 : 0xFD20));
            }
        } else {
            int16_t fx = birdSnap((int16_t)b.fallX);
            int16_t fy = birdSnap((int16_t)b.fallY);
            uint16_t body = flash ? 0xFFFF : (retro ? 0x7BEF : 0x9AC8);
            uint16_t wing = flash ? 0xFFFF : (retro ? 0x632C : 0x82A6);
            if (fy >= 0 && fy < 107) {
                canvas.fillRect(fx, fy, BIRD_PX, BIRD_PX, body);
                canvas.fillRect(fx + BIRD_PX, fy, BIRD_PX, BIRD_PX, wing);
            }
        }
    }

    // Sparks / explosions / impacts — warm orange/yellow (gray in RETRO)
    const uint16_t FX_COL = isThunderFlashing() ? 0xFFFF : (retro ? 0xC618 : 0xFD20);
    for (int s = 0; s < 6; s++) {
        if (sparks[s].life == 0) continue;
        if (sparks[s].life < 4 && (sparks[s].life % 2 == 0)) continue;
        int16_t sx = birdSnap((int16_t)sparks[s].x);
        int16_t sy = birdSnap((int16_t)sparks[s].y);
        if (sx >= 0 && sx < 240 && sy >= 0 && sy < 107) {
            canvas.fillRect(sx, sy, BIRD_PX, BIRD_PX, FX_COL);
        }
    }

    for (int e = 0; e < 2; e++) {
        if (!explosions[e].active) continue;
        if (explosions[e].life < 4 && (explosions[e].life % 2 == 0)) continue;
        int16_t cx = birdSnap((int16_t)explosions[e].x);
        int16_t cy = birdSnap((int16_t)explosions[e].y);
        int16_t r = (int16_t)explosions[e].radius;
        const int16_t pts[][2] = {
            {0, (int16_t)(-r)}, {0, r}, {(int16_t)(-r), 0}, {r, 0},
            {(int16_t)(r * 7 / 10), (int16_t)(-r * 7 / 10)},
            {(int16_t)(-r * 7 / 10), (int16_t)(-r * 7 / 10)},
            {(int16_t)(r * 7 / 10), (int16_t)(r * 7 / 10)},
            {(int16_t)(-r * 7 / 10), (int16_t)(r * 7 / 10)}
        };
        for (int p = 0; p < 8; p++) {
            int16_t px = birdSnap(cx + pts[p][0]);
            int16_t py = birdSnap(cy + pts[p][1]);
            if (px >= 0 && px < 240 && py >= 0 && py < 107) {
                canvas.fillRect(px, py, BIRD_PX, BIRD_PX, FX_COL);
            }
        }
    }

    for (int s = 0; s < 6; s++) {
        if (!impactSplashes[s].active) continue;
        if (impactSplashes[s].life < 4 && (impactSplashes[s].life % 2 == 0)) continue;
        int16_t sx = birdSnap((int16_t)impactSplashes[s].x);
        int16_t sy = birdSnap((int16_t)impactSplashes[s].y);
        if (sx >= 0 && sx < 240 && sy >= 0 && sy < 107) {
            canvas.fillRect(sx, sy, BIRD_PX, BIRD_PX, FX_COL);
        }
    }
}

void draw(M5Canvas& canvas, uint16_t colorFG, uint16_t colorBG) {
    (void)colorFG; (void)colorBG;
    // Bright rain streaks — visible over blue/grey sky (not navy-on-navy)
    const bool flash = isThunderFlashing();
    const uint16_t RAIN_TIP  = flash ? 0xFFFF : 0xEFFF;
    const uint16_t RAIN_MID  = flash ? 0xFFFF : 0xAEDC;
    const uint16_t RAIN_TAIL = flash ? 0xFFFF : 0x7D5F;
    const uint16_t WIND_COL  = flash ? 0xFFFF : 0xDEDB;
    const uint16_t SPLASH    = flash ? 0xFFFF : 0xC6FF;
    // Snow banks / leaves / tumbleweed / butterflies → SeasonalFx::draw()

    if (rainActive) {
        if (seasonIsRetro()) {
            // Old film: blocky pixels falling (world dissolving into noise)
            const uint16_t PX_LO = flash ? 0xFFFF : 0x8410;  // mid gray
            const uint16_t PX_HI = flash ? 0xFFFF : 0xC618;  // light gray
            const uint16_t PX_WH = 0xFFFF;
            for (int i = 0; i < SNOW_FLAKE_COUNT; i++) {
                int16_t rx = ((int16_t)rainDrops[i].x / 3) * 3;  // snap to 3px grid
                int16_t ry = ((int16_t)rainDrops[i].y / 3) * 3;
                if (ry < -6 || ry >= 104) continue;
                uint8_t sz = (rainDrops[i].len <= 1) ? 2 : 3;
                uint16_t c = (i & 2) ? PX_HI : PX_LO;
                if ((i + (int)ry) & 4) c = PX_WH;
                canvas.fillRect(rx, ry, sz, sz, c);
                // trailing ghost pixel (film grain trail)
                if (ry + 6 < 104)
                    canvas.fillRect(rx + ((i & 1) ? 3 : -3), ry + 6, 2, 2, PX_LO);
            }
        } else if (seasonIsWinter()) {
            // Fat-pixel snowflakes (3px blocks, not 1px dust)
            for (int i = 0; i < SNOW_FLAKE_COUNT; i++) {
                int16_t rx = (int16_t)rainDrops[i].x;
                int16_t ry = (int16_t)rainDrops[i].y;
                if (ry < -6 || ry >= 104) continue;
                uint8_t kind = rainDrops[i].len;
                if (kind < 1) kind = 1;
                if (kind > 2) kind = 2;
                drawFatSnowflake(canvas, rx, ry, kind);
            }
        } else {
            // Seasonal rain colors: summer warmer/lighter, spring cooler, autumn dull
            uint16_t tip = RAIN_TIP, mid = RAIN_MID, tail = RAIN_TAIL;
            if (seasonIsSummer()) {
                tip = flash ? 0xFFFF : 0xBDF7;
                mid = flash ? 0xFFFF : 0x7CF7;
                tail = flash ? 0xFFFF : 0x54DF;
            } else if (seasonIsAutumn()) {
                tip = flash ? 0xFFFF : 0x9CD3;
                mid = flash ? 0xFFFF : 0x6B6D;
                tail = flash ? 0xFFFF : 0x4208;
            }
            for (int i = 0; i < RAIN_DROP_COUNT; i++) {
                int16_t rx = (int16_t)rainDrops[i].x;
                int16_t ry = (int16_t)rainDrops[i].y;
                if (ry < 0 || ry >= 104) continue;
                int len = rainDrops[i].len;
                if (len < 2) len = 2;

                for (int s = 0; s < len; s++) {
                    int16_t sy = ry + s * 3;
                    int16_t sx = rx + s / 2;
                    if (sy >= 104) break;
                    uint16_t c = (s == len - 1) ? tip : (s == 0 ? tail : mid);
                    canvas.drawFastVLine(sx, sy, 3, c);
                }
                if (ry + len * 3 >= 98) {
                    canvas.drawPixel(rx - 2, 103, SPLASH);
                    canvas.drawPixel(rx + 2, 103, SPLASH);
                    canvas.drawPixel(rx, 102, SPLASH);
                }
            }
        }
    }

    // Draw wind particles as grid-snapped fat pixels (shrink from multi-block to single)
    if (windActive) {
        for (int i = 0; i < 6; i++) {
            if (!windParticles[i].active) continue;
            int16_t wx = birdSnap((int16_t)windParticles[i].x);
            int16_t wy = birdSnap((int16_t)windParticles[i].y);
            if (wx < -BIRD_PX || wx > DISPLAY_W + BIRD_PX) continue;

            // Block count shrinks over travel distance: 3→2→1 blocks
            float dist = windParticles[i].x - windParticles[i].spawnX;
            if (dist < 0) dist = -dist;
            float progress = dist / windParticles[i].maxTravel;
            if (progress > 1.0f) progress = 1.0f;
            int blocks = (int)((float)windParticles[i].baseSize * (1.0f - progress) + 0.5f);
            if (blocks < 1) continue;

            // Horizontal streak of fat pixels
            for (int b = 0; b < blocks; b++) {
                int16_t bx = windParticles[i].dirRight ? (wx + b * BIRD_PX) : (wx - b * BIRD_PX);
                if (bx >= 0 && bx < DISPLAY_W && wy >= 0 && wy < 107) {
                    canvas.fillRect(bx, wy, BIRD_PX, BIRD_PX, WIND_COL);
                }
            }
        }
    }
}

}  // namespace Weather
