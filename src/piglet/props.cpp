// Seasonal daily props — hive, snowman, fox, campfire, city cat, desert skull.
#include "props.h"
#include "weather.h"
#include "avatar.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../ui/display.h"
#include "../audio/sfx.h"
#include <Preferences.h>
#include <M5Cardputer.h>
#include <esp_random.h>
#include <math.h>
#include <time.h>
#include <string.h>

namespace Props {

static Preferences s_prefs;
static uint8_t s_mask = 0;
static bool s_ready = false;

static constexpr uint32_t GAME_DAY_SEC = 360u;
static uint16_t s_lastSpawnDay = 0xFFFF;
static bool s_usedToday = false;

enum class Kind : uint8_t {
    None = 0, Hive, Snowman, Fox, Fire, Cat, Skull
};
enum class Phase : uint8_t { Idle = 0, Live, Action, Gone };

static Kind s_kind = Kind::None;
static Phase s_phase = Phase::Idle;
static int16_t s_worldX = 120;
static int16_t s_scroll = 0;
static uint32_t s_spawnAt = 0;
static uint32_t s_actionAt = 0;
static uint8_t s_breakFrame = 0;
static bool s_announced = false;
static bool s_forceDemo = false;   // ANIM TEST bypass

struct Bee {
    float x, y, ox, oy, ang, spin;
    bool chasing, active;
};
static constexpr int BEE_N = 6;
static Bee s_bees[BEE_N];

// Campfire smoke puffs
struct Smoke {
    float x, y, vy;
    uint8_t life;
    bool active;
};
static constexpr int SMOKE_N = 10;
static Smoke s_smoke[SMOKE_N];

static constexpr int16_t GROUND_Y = 106;

static uint16_t dayId() {
    return (uint16_t)((millis() / 1000u) / GAME_DAY_SEC);
}

static int16_t screenX() {
    int16_t x = (int16_t)(s_worldX + s_scroll);
    while (x > 280) x = (int16_t)(x - 300);
    while (x < -40) x = (int16_t)(x + 300);
    return x;
}

static bool unlockedNow() {
    if (!s_ready) begin();
    if (s_mask != 0) return true;
    if (XP::getLevel() >= 35) return true;
    return false;
}

static bool featureOn() {
    return Config::personality().propsEnabled;
}

void begin() {
    if (s_ready) return;
    s_prefs.begin("pigprops", false);
    s_mask = s_prefs.getUChar("mask", 0);
    s_lastSpawnDay = s_prefs.getUShort("day", 0xFFFF);
    s_usedToday = (s_lastSpawnDay == dayId());
    s_ready = true;
    s_kind = Kind::None;
    s_phase = Phase::Idle;
}

bool anyUnlocked() { return unlockedNow(); }

bool isUnlocked(uint8_t slot) {
    if (!unlockedNow()) return false;
    if (slot >= SLOT_COUNT) return false;
    // Level 35 / full mask opens every seasonal slot
    if (s_mask == 0x3F || XP::getLevel() >= 35) return true;
    return (s_mask & (uint8_t)(1u << slot)) != 0;
}

void unlockSlot(uint8_t slot) {
    if (!s_ready) begin();
    if (slot >= SLOT_COUNT) return;
    s_mask = (uint8_t)(s_mask | (1u << slot));
    s_prefs.putUChar("mask", s_mask);
}

void unlockAllFour() {
    if (!s_ready) begin();
    s_mask = 0x3F;  // all 6 bits
    s_prefs.putUChar("mask", s_mask);
}

void scroll(int8_t dx) {
    if (s_phase == Phase::Idle || s_phase == Phase::Gone) return;
    s_scroll = (int16_t)(s_scroll + dx);
}

static void markUsedToday() {
    if (s_forceDemo) return;  // demos don't burn the daily slot
    s_usedToday = true;
    s_lastSpawnDay = dayId();
    s_prefs.putUShort("day", s_lastSpawnDay);
}

static void clearInstance() {
    s_kind = Kind::None;
    s_phase = Phase::Gone;
    s_forceDemo = false;
    for (int i = 0; i < BEE_N; i++) s_bees[i].active = false;
    for (int i = 0; i < SMOKE_N; i++) s_smoke[i].active = false;
}

static void spawnBees(int16_t hx, int16_t hy) {
    for (int i = 0; i < BEE_N; i++) {
        s_bees[i].active = true;
        s_bees[i].chasing = false;
        s_bees[i].ang = (float)i * 1.05f;
        s_bees[i].spin = 0.06f + (float)(i % 3) * 0.02f;
        s_bees[i].ox = 10.f + (float)(i % 3) * 3.f;
        s_bees[i].oy = 6.f + (float)((i * 3) % 5);
        s_bees[i].x = (float)hx + cosf(s_bees[i].ang) * s_bees[i].ox;
        s_bees[i].y = (float)hy + sinf(s_bees[i].ang) * s_bees[i].oy;
    }
}

static Kind kindForSeason(Season s) {
    switch (s) {
        case Season::SUMMER: return Kind::Hive;
        case Season::WINTER: return Kind::Snowman;
        case Season::AUTUMN: return Kind::Fox;
        case Season::SPRING: return Kind::Fire;   // only when storm rolls
        case Season::CITY:   return Kind::Cat;
        case Season::DESERT: return Kind::Skull;
        default: return Kind::None;
    }
}

static void placeOffscreen() {
    s_scroll = 0;
    bool walkRight = Avatar::isGrassDirectionRight();
    if (walkRight)
        s_worldX = (int16_t)(-70 - (int)(esp_random() % 40));
    else
        s_worldX = (int16_t)(270 + (int)(esp_random() % 40));
}

static void trySpawnNatural() {
    if (!unlockedNow()) return;
    uint16_t today = dayId();
    if (s_lastSpawnDay != today) s_usedToday = false;
    if (s_usedToday) return;
    if (s_phase != Phase::Idle && s_phase != Phase::Gone) return;
    if (millis() < 6000) return;

    Season season = Weather::getActiveSeason();
    Kind k = kindForSeason(season);
    if (k == Kind::None) return;

    // Spring fire: only during storm, chance on thunder flash edge
    if (k == Kind::Fire) {
        if (!Weather::isStorming()) return;
        static bool wasFlash = false;
        bool flash = Avatar::isThunderFlashing() || Weather::isThunderFlashing();
        bool edge = flash && !wasFlash;
        wasFlash = flash;
        if (!edge) return;
        if ((esp_random() % 100) > 28) return;  // ~28% per strike
    } else {
        if (!Avatar::isGrassMoving() && (esp_random() % 100) > 3) return;
        if (Avatar::isGrassMoving() && (esp_random() % 100) > 12) return;
    }

    s_kind = k;
    s_phase = Phase::Live;
    s_breakFrame = 0;
    s_announced = false;
    s_forceDemo = false;
    s_spawnAt = millis();
    s_actionAt = 0;
    placeOffscreen();
    if (k == Kind::Hive) spawnBees(screenX(), GROUND_Y - 28);
    markUsedToday();
}

// --- per-kind update ---
static void updateHive() {
    int16_t hx = screenX();
    int16_t hy = GROUND_Y - 28;
    int pigX = Avatar::getCurrentX() + 20;
    int pigY = GROUND_Y - Avatar::getJumpLiftPx() - 10;
    float dist = fabsf((float)pigX - (float)hx);
    bool aggro = (dist < 48.f) || (s_phase == Phase::Action);
    if (aggro && s_phase == Phase::Live) {
        s_phase = Phase::Action;
        s_actionAt = millis();
        Display::showToast("BUZZ!", 900);
        SFX::play(SFX::OINK_HAPPY);
    }
    for (int i = 0; i < BEE_N; i++) {
        if (!s_bees[i].active) continue;
        if (s_phase == Phase::Action || aggro) {
            // Circle the pig instead of forming a straight trail behind him.
            // Each bee keeps its own phase and height so the swarm reads as
            // animated flight rather than six points stuck to one target.
            s_bees[i].ang += s_bees[i].spin * 1.8f;
            float radius = 15.f + (float)(i % 3) * 5.f;
            float tx = (float)pigX + cosf(s_bees[i].ang) * radius;
            float ty = (float)pigY - 5.f + sinf(s_bees[i].ang * 1.35f) * (6.f + (i % 2) * 3.f);
            s_bees[i].x += (tx - s_bees[i].x) * 0.14f;
            s_bees[i].y += (ty - s_bees[i].y) * 0.14f;
        } else {
            s_bees[i].ang += s_bees[i].spin;
            s_bees[i].x = (float)hx + cosf(s_bees[i].ang) * s_bees[i].ox;
            s_bees[i].y = (float)hy + sinf(s_bees[i].ang) * s_bees[i].oy * 0.7f;
        }
    }
    if (s_phase == Phase::Action && (millis() - s_actionAt) > 12000)
        clearInstance();
}

static void updateSnowman() {
    int16_t sx = screenX();
    int pigX = Avatar::getCurrentX() + 20;
    int dist = abs(pigX - (int)sx);
    bool airborne = Avatar::isJumping() && Avatar::getJumpLiftPx() > 3;
    if (s_phase == Phase::Live && airborne && dist < 32) {
        s_phase = Phase::Action;
        s_actionAt = millis();
        Display::showToast("CRACK!", 1000);
        SFX::play(SFX::LEVEL_UP);
        XP::addXP(12);
        Avatar::triggerSparkles(6);
    }
    if (s_phase == Phase::Action && (millis() - s_actionAt) > 1800)
        clearInstance();
}

// Fox / cat: no interact — leave when player walks far / off-screen long
static void updatePassiveLeave() {
    int16_t sx = screenX();
    int pigX = Avatar::getCurrentX() + 20;
    int dist = abs(pigX - (int)sx);
    // Off-screen or walked away after having been seen
    if (s_announced && (sx < -20 || sx > 260 || dist > 110)) {
        if (s_actionAt == 0) s_actionAt = millis();
        else if ((millis() - s_actionAt) > 2500)
            clearInstance();
    } else {
        s_actionAt = 0;
    }
}

static void updateFire() {
    // Half game-day burn = 180s (or shorter in forceDemo)
    uint32_t life = s_forceDemo ? 25000u : (GAME_DAY_SEC / 2) * 1000u;
    if ((millis() - s_spawnAt) > life) {
        clearInstance();
        return;
    }
    // smoke
    for (int i = 0; i < SMOKE_N; i++) {
        if (!s_smoke[i].active) {
            if ((esp_random() % 100) < 20) {
                s_smoke[i].active = true;
                s_smoke[i].x = (float)screenX() + (float)((int)(esp_random() % 5) - 2);
                s_smoke[i].y = (float)(GROUND_Y - 22);
                s_smoke[i].vy = -0.35f - (float)(esp_random() % 20) / 100.f;
                s_smoke[i].life = (uint8_t)(40 + (esp_random() % 30));
            }
            continue;
        }
        s_smoke[i].y += s_smoke[i].vy;
        s_smoke[i].x += ((int)(esp_random() % 3) - 1) * 0.15f;
        if (s_smoke[i].life) s_smoke[i].life--;
        else s_smoke[i].active = false;
    }
}

static void updateSkull() {
    // Ambient decoration ~40s then fade (or until leave view)
    if ((millis() - s_spawnAt) > (s_forceDemo ? 20000u : 45000u))
        clearInstance();
}

static void maybeAnnounce() {
    if (s_announced) return;
    if (s_phase != Phase::Live && s_phase != Phase::Action) return;
    int16_t sx = screenX();
    if (sx < 8 || sx > 232) return;
    s_announced = true;
    const char* msg = nullptr;
    switch (s_kind) {
        case Kind::Hive:    msg = "HIVE!"; spawnBees(sx, GROUND_Y - 28); break;
        case Kind::Snowman: msg = "SNOWMAN!"; break;
        case Kind::Fox:     msg = "FOX Zzz"; break;
        case Kind::Fire:    msg = "CAMPFIRE!"; break;
        case Kind::Cat:     msg = "STRAY CAT"; break;
        case Kind::Skull:   msg = "SKULL"; break;
        default: break;
    }
    if (msg) {
        Display::showToast(msg, 1200);
        SFX::play(SFX::MENU_CLICK);
    }
}

void update() {
    if (!s_ready) begin();
    if (!featureOn() && !s_forceDemo) return;
    if (!unlockedNow() && !s_forceDemo) return;

    if (s_lastSpawnDay != dayId()) s_usedToday = false;

    if (!s_forceDemo && (s_phase == Phase::Idle || s_phase == Phase::Gone))
        trySpawnNatural();

    switch (s_kind) {
        case Kind::Hive:    if (s_phase == Phase::Live || s_phase == Phase::Action) updateHive(); break;
        case Kind::Snowman: if (s_phase == Phase::Live || s_phase == Phase::Action) updateSnowman(); break;
        case Kind::Fox:
        case Kind::Cat:     if (s_phase == Phase::Live) updatePassiveLeave(); break;
        case Kind::Fire:    if (s_phase == Phase::Live) updateFire(); break;
        case Kind::Skull:   if (s_phase == Phase::Live) updateSkull(); break;
        default: break;
    }
    maybeAnnounce();
}

// --- draw ---
// Classic chunky pixel props (readable silhouettes)

static constexpr int16_t P = 3;
static void px(M5Canvas& c, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col) {
    c.fillRect(x, y, w * P, h * P, col);
}
static void p1(M5Canvas& c, int16_t x, int16_t y, uint16_t col) {
    c.fillRect(x, y, P, P, col);
}

// SUMMER: beehive — stacked dome bands + dark entrance + bees
static void drawHive(M5Canvas& canvas, int16_t yOff) {
    int16_t x = screenX();
    int16_t y = GROUND_Y + yOff;
    // ground shadow
    px(canvas, x - 6 * P, y - P, 12, 1, 0x5A00);
    // body bands (wide → slightly narrower), gold/amber stripes
    static const int halfs[] = { 6, 6, 5, 5, 4, 4, 3 };
    static const uint16_t bands[] = {
        0xFE60, 0xC408, 0xFE60, 0xA300, 0xFE60, 0xC408, 0xFE60
    };
    for (int i = 0; i < 7; i++) {
        int h = halfs[i];
        int16_t yy = y - (i + 1) * 2 * P;
        px(canvas, x - h * P, yy, h * 2, 2, bands[i]);
        // dark rim under each band
        canvas.drawFastHLine(x - h * P, yy + 2 * P - 1, h * 2 * P, 0x8200);
    }
    // peaked lid
    px(canvas, x - 3 * P, y - 16 * P, 6, 1, 0xA300);
    px(canvas, x - 2 * P, y - 17 * P, 4, 1, 0x8200);
    p1(canvas, x - P, y - 18 * P, 0x5A00);
    // entrance hole
    px(canvas, x - P, y - 3 * P, 2, 2, 0x2104);
    p1(canvas, x, y - 3 * P, 0x4208);
    // Bees — small readable pixel sprites rather than yellow rectangles.
    const bool wingsUp = ((millis() / 90u) & 1u) != 0;
    for (int i = 0; i < BEE_N; i++) {
        if (!s_bees[i].active) continue;
        int bx = (int)s_bees[i].x;
        int by = (int)s_bees[i].y + yOff;
        if (bx < -6 || bx > 246 || by < 0 || by > 125) continue;
        const uint16_t outline = 0x2104;
        const uint16_t wing = 0xDDFB;
        const uint16_t wingShade = 0x9CF3;
        const uint16_t gold = 0xFD80;

        // Two-pixel wings flap around a compact outlined body.
        if (wingsUp) {
            canvas.fillRect(bx + 1, by - 2, 3, 2, wing);
            canvas.fillRect(bx + 6, by - 2, 3, 2, wingShade);
        } else {
            canvas.fillRect(bx + 1, by + 4, 3, 2, wingShade);
            canvas.fillRect(bx + 6, by + 4, 3, 2, wing);
        }
        canvas.fillRect(bx + 1, by, 7, 5, outline);
        canvas.fillRect(bx + 2, by + 1, 5, 3, gold);
        canvas.fillRect(bx + 4, by + 1, 1, 3, 0x2104);
        canvas.drawPixel(bx + 7, by + 1, 0x0000); // eye
        canvas.drawPixel(bx + 8, by + 3, outline); // stinger
        canvas.drawPixel(bx, by + 2, outline);
    }
}

// WINTER: snowman — 3 balls, coal face, carrot, scarf, stick arms
static void drawSnowman(M5Canvas& canvas, int16_t yOff) {
    int16_t x = screenX();
    int16_t y = GROUND_Y + yOff;
    if (s_phase == Phase::Action) {
        int tt = (int)(millis() - s_actionAt);
        // Shards inherit the prop pixel scale, but drift apart smoothly.
        px(canvas, x - 16 - tt / 3, y - 12 - tt / 18, 3, 2, 0xFFFF);
        px(canvas, x + 12 + tt / 4, y - 8 - tt / 22, 2, 2, 0xFFFF);
        px(canvas, x + (tt / 18) % 10, y - 24 - tt / 20, 2, 2, 0xE7FF);
        px(canvas, x - 3 + (tt / 14) % 8, y - 18 - tt / 28, 2, 1, 0xFFFF);
        return;
    }
    const uint16_t snow = 0xFFFF, shade = 0xE7FF, outline = 0xC618;
    // Three stepped white snowballs with only a narrow cool edge.
    px(canvas, x - 6 * P, y - 6 * P, 12, 6, outline);
    px(canvas, x - 5 * P, y - 7 * P, 10, 6, snow);
    px(canvas, x - 5 * P, y - P, 10, P, shade);
    px(canvas, x - 5 * P, y - 11 * P, 10, 5, outline);
    px(canvas, x - 4 * P, y - 12 * P, 8, 5, snow);
    px(canvas, x - 4 * P, y - 7 * P, 8, P, shade);
    px(canvas, x - 4 * P, y - 16 * P, 8, 5, outline);
    px(canvas, x - 3 * P, y - 17 * P, 6, 5, snow);
    px(canvas, x - 3 * P, y - 12 * P, 6, P, shade);
    // Hat, scarf, face and carrot.
    px(canvas, x - 3 * P, y - 18 * P, 6, 1, outline);
    px(canvas, x - 2 * P, y - 21 * P, 4, 3, 0x0000);
    px(canvas, x - 3 * P, y - 19 * P, 6, 1, 0xF800);
    p1(canvas, x - 2 * P, y - 15 * P, 0x2104);
    p1(canvas, x + P, y - 15 * P, 0x2104);
    px(canvas, x + P, y - 13 * P, 3, 1, 0xFD20);
    p1(canvas, x + 3 * P, y - 13 * P, 0xFBE0);
    p1(canvas, x - 2 * P, y - 12 * P, 0x2104);
    p1(canvas, x - P, y - 11 * P, 0x2104);
    p1(canvas, x, y - 12 * P, 0x2104);
    px(canvas, x - 4 * P, y - 11 * P, 8, 1, 0xF800);
    px(canvas, x + 3 * P, y - 10 * P, 2, 3, 0xC000);
    p1(canvas, x - P, y - 9 * P, 0x2104);
    p1(canvas, x - P, y - 7 * P, 0x2104);
    p1(canvas, x - P, y - 4 * P, 0x2104);
    // Branch arms with small forked hands.
    canvas.drawLine(x - 6 * P, y - 10 * P, x - 10 * P - 5, y - 13 * P, 0x8200);
    canvas.drawLine(x - 10 * P - 5, y - 13 * P, x - 13 * P - 5, y - 12 * P, 0x8200);
    canvas.drawLine(x + 6 * P, y - 10 * P, x + 10 * P + 5, y - 13 * P, 0x8200);
    canvas.drawLine(x + 10 * P + 5, y - 13 * P, x + 13 * P + 5, y - 12 * P, 0x8200);
}

// AUTUMN: sleeping fox — loaf body, ears, bushy tail, Zzz
static void drawFox(M5Canvas& canvas, int16_t yOff) {
    int16_t x = screenX();
    int16_t y = GROUND_Y + yOff;
    const uint16_t fur = 0xE2C4, dark = 0xA943, cream = 0xFFD7;
    px(canvas, x - 7 * P, y - P, 15, 1, 0x6A40);
    // Low-poly loaf silhouette with a clean pixel outline.
    px(canvas, x - 3 * P, y - 5 * P, 10, 5, dark);
    px(canvas, x - 2 * P, y - 4 * P, 8, 3, fur);
    px(canvas, x - P, y - 3 * P, 6, 2, cream);
    // Head, muzzle and triangular ears.
    px(canvas, x - 7 * P, y - 8 * P, 6, 5, dark);
    px(canvas, x - 6 * P, y - 7 * P, 5, 3, fur);
    px(canvas, x - 8 * P, y - 6 * P, 3, 2, cream);
    p1(canvas, x - 9 * P, y - 5 * P, 0x2104);
    p1(canvas, x - 7 * P, y - 10 * P, dark);
    p1(canvas, x - 6 * P, y - 11 * P, fur);
    p1(canvas, x - 5 * P, y - 10 * P, 0xFCB2);
    p1(canvas, x - 3 * P, y - 10 * P, dark);
    p1(canvas, x - 2 * P, y - 11 * P, fur);
    p1(canvas, x - 3 * P, y - 10 * P, 0xFCB2);
    // Sleeping face and curled tail.
    px(canvas, x - 6 * P, y - 7 * P, 2, 1, 0x2104);
    px(canvas, x + 6 * P, y - 6 * P, 4, 4, dark);
    px(canvas, x + 8 * P, y - 8 * P, 3, 3, fur);
    px(canvas, x + 10 * P, y - 7 * P, 2, 2, cream);
    p1(canvas, x - P, y - P, dark);
    p1(canvas, x + 2 * P, y - P, dark);
    // Zzz
    uint32_t ph = (millis() / 400) % 3;
    canvas.setTextColor(0x8410);
    canvas.setTextSize(1);
    const char* zz[] = { "z", "Z", "Zz" };
    canvas.drawString(zz[ph], x - P, y - 14 * P - (int)ph * 2);
}

// SPRING: campfire — crossed logs + animated flame + smoke
static void drawFire(M5Canvas& canvas, int16_t yOff) {
    int16_t x = screenX();
    int16_t y = GROUND_Y + yOff;
    // Stone ring and two highlighted crossed logs.
    px(canvas, x - 5 * P, y - 2 * P, 10, 2, 0x6B6D);
    p1(canvas, x - 5 * P, y - 3 * P, 0x9CF3);
    p1(canvas, x + 4 * P, y - 3 * P, 0x8410);
    for (int i = 0; i < 7; i++) {
        p1(canvas, x - 4 * P + i * P, y - 3 * P - i / 3, (i & 1) ? 0x9A40 : 0x8200);
        p1(canvas, x - 3 * P + i * P, y - 2 * P + i / 3, 0x5A00);
    }
    // flame — classic tall teardrop, animated
    uint32_t f = (millis() / 70) % 5;
    static const uint16_t FC[] = { 0xF800, 0xFA00, 0xFD20, 0xFFE0, 0xFFFF };
    int h = 7 + (int)(f % 3);
    for (int i = 0; i < h; i++) {
        int half = (i < 2) ? 3 : (i < 5) ? 2 : 1;
        if (i == h - 1) half = 1;
        px(canvas, x - half * P, y - 4 * P - i * P, half * 2, 1, FC[(i + f) % 5]);
    }
    px(canvas, x - P, y - 7 * P, 2, 3, 0xFFE0);
    p1(canvas, x, y - 9 * P, 0xFFFF);
    // smoke puffs
    for (int i = 0; i < SMOKE_N; i++) {
        if (s_smoke[i].life <= 0) continue;
        int sx = (int)s_smoke[i].x;
        int sy = (int)s_smoke[i].y + yOff;
        uint16_t c = (s_smoke[i].life > 20) ? 0x9CF3 : 0x6B6D;
        px(canvas, sx, sy, 2, 2, c);
    }
}

// CITY: cardboard box + cat
static void drawCat(M5Canvas& canvas, int16_t yOff) {
    int16_t x = screenX();
    int16_t y = GROUND_Y + yOff;
    const uint16_t box = 0xC408, edge = 0x8200, cat = 0x8410;
    px(canvas, x - 8 * P, y - P, 16, 1, 0x4208);
    px(canvas, x - 7 * P, y - 6 * P, 14, 6, box);
    canvas.drawRect(x - 7 * P, y - 6 * P, 14 * P, 6 * P, edge);
    px(canvas, x - 7 * P, y - 9 * P, 4, 3, 0xD4A0);
    px(canvas, x + 3 * P, y - 9 * P, 4, 3, 0xBCA6);
    px(canvas, x - P, y - 6 * P, 2, 1, 0xC618);
    px(canvas, x - 3 * P, y - 8 * P, 7, 4, cat);
    px(canvas, x - 4 * P, y - 11 * P, 5, 4, cat);
    p1(canvas, x - 4 * P, y - 12 * P, cat);
    p1(canvas, x - P, y - 12 * P, cat);
    p1(canvas, x - 4 * P, y - 11 * P, 0xF81F);
    p1(canvas, x - 3 * P, y - 10 * P, 0x07FF);
    p1(canvas, x - P, y - 10 * P, 0x07FF);
    p1(canvas, x - 2 * P, y - 9 * P, 0xF81F);
    canvas.drawFastHLine(x - 8 * P, y - 8 * P, 3 * P, 0xC618);
    canvas.drawFastHLine(x + P, y - 8 * P, 3 * P, 0xC618);
    px(canvas, x + 4 * P, y - 5 * P, 3, 2, 0x6B6D);
    p1(canvas, x + 7 * P, y - 6 * P, cat);
}

// DESERT: sun-bleached skull on sand
static void drawSkull(M5Canvas& canvas, int16_t yOff) {
    int16_t x = screenX();
    int16_t y = GROUND_Y + yOff;
    // sand mound
    px(canvas, x - 8 * P, y - 2 * P, 16, 2, 0xD4A0);
    px(canvas, x - 6 * P, y - 3 * P, 12, 1, 0xE5C0);
    // Cranium with stepped corners, cracks and a warm highlight.
    px(canvas, x - 4 * P, y - 12 * P, 8, 1, 0xEF5D);
    px(canvas, x - 5 * P, y - 11 * P, 10, 6, 0xEF5D);
    canvas.drawRect(x - 5 * P, y - 11 * P, 10 * P, 6 * P, 0x9CF3);
    px(canvas, x - 3 * P, y - 11 * P, 6, 1, 0xFFFF);
    // eye sockets (deep)
    px(canvas, x - 3 * P, y - 9 * P, 2, 2, 0x2104);
    px(canvas, x + P, y - 9 * P, 2, 2, 0x2104);
    // nasal cavity
    px(canvas, x - P / 2, y - 7 * P, 1, 2, 0x4208);
    // jaw
    px(canvas, x - 4 * P, y - 4 * P, 8, 2, 0xDEFB);
    // teeth
    for (int i = 0; i < 5; i++)
        p1(canvas, x - 3 * P + i * P, y - 3 * P, 0xFFFF);
    // cheek cracks
    canvas.drawLine(x - 4 * P, y - 6 * P, x - 3 * P, y - 5 * P, 0x9CF3);
    canvas.drawLine(x + 4 * P, y - 6 * P, x + 3 * P, y - 5 * P, 0x9CF3);
}

void draw(M5Canvas& canvas, int16_t yOffset) {
    if (!featureOn() && !s_forceDemo) return;
    if (!unlockedNow() && !s_forceDemo) return;
    if (s_phase != Phase::Live && s_phase != Phase::Action) return;
    switch (s_kind) {
        case Kind::Hive:    drawHive(canvas, yOffset); break;
        case Kind::Snowman: drawSnowman(canvas, yOffset); break;
        case Kind::Fox:     drawFox(canvas, yOffset); break;
        case Kind::Fire:    drawFire(canvas, yOffset); break;
        case Kind::Cat:     drawCat(canvas, yOffset); break;
        case Kind::Skull:   drawSkull(canvas, yOffset); break;
        default: break;
    }
}

void forceDemo(uint8_t which) {
    if (!s_ready) begin();
    if (which >= 6) {
        clearInstance();
        s_phase = Phase::Idle;
        Display::showToast("PROP CLEAR", 900);
        return;
    }
    static const Kind map[] = {
        Kind::Hive, Kind::Snowman, Kind::Fox, Kind::Fire, Kind::Cat, Kind::Skull
    };
    s_kind = map[which];
    s_phase = Phase::Live;
    s_forceDemo = true;
    s_announced = true;  // already in view
    s_scroll = 0;
    s_worldX = 160;
    s_spawnAt = millis();
    s_actionAt = 0;
    s_breakFrame = 0;
    for (int i = 0; i < SMOKE_N; i++) s_smoke[i].active = false;
    if (s_kind == Kind::Hive) spawnBees(screenX(), GROUND_Y - 28);
    static const char* names[] = {
        "HIVE", "SNOWMAN", "FOX", "FIRE", "CAT", "SKULL"
    };
    Display::showToast(names[which], 1000);
    SFX::play(SFX::MENU_CLICK);
}

}  // namespace Props
