// Multi-kind scene trees (fruit / decor / berry bush).

#include "trees.h"
#include "trees_drops.h"
#include "avatar.h"
#include "ground.h"
#include "weather.h"
#include "../ui/display.h"
#include "../audio/sfx.h"
#include "../core/config.h"
#include "../core/xp.h"
#include <esp_random.h>
#include <math.h>

namespace Trees {

int16_t worldWidth() {
    uint16_t width = Config::personality().fieldWidth;
    if (width < 240) return 240;
    if (width > WORLD_SPAN) return WORLD_SPAN;
    return (int16_t)width;
}

static constexpr int16_t PX = 3;
static inline int16_t snapPx(int16_t v) {
    return (v >= 0) ? (v / PX) * PX : ((v - (PX - 1)) / PX) * PX;
}

static void fatLine(M5Canvas& canvas, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color) {
    int dx = abs((int)x2 - (int)x1), dy = abs((int)y2 - (int)y1);
    int sx = (x1 < x2) ? 1 : -1, sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;
    int x = x1, y = y1;
    for (;;) {
        if (x >= 0 && x < 240 && y >= 0 && y < 135) {
            canvas.fillRect(x, y, PX, PX, color);
        }
        if (x == x2 && y == y2) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx)  { err += dx; y += sy; }
    }
}

static bool thunder() { return Avatar::isThunderFlashing(); }
static uint16_t fl(uint16_t c) {
    if (!thunder()) return c;
    uint16_t r = ((c >> 11) + 31) >> 1;
    uint16_t g = (((c >> 5) & 0x3F) + 63) >> 1;
    uint16_t b = ((c & 0x1F) + 31) >> 1;
    return (r << 11) | (g << 5) | b;
}

// Colors — base + seasonal
static constexpr uint16_t C_TRUNK  = 0x8A40;
static constexpr uint16_t C_TRUNK2 = 0x5140;
static constexpr uint16_t C_TRUNK_H= 0xB4E0;
static constexpr uint16_t C_LEAF   = 0x2C00;
static constexpr uint16_t C_LEAF2  = 0x4C20;
static constexpr uint16_t C_LEAF3  = 0x6E40;
// Summer apples — bright red for ripe fruit on green leaves
static constexpr uint16_t C_APPLE_RED    = 0xF800;  // red body
static constexpr uint16_t C_APPLE_RED2   = 0xA000;  // deeper red shade
static constexpr uint16_t C_APPLE_RED_H  = 0xFDE0;  // warm highlight
static constexpr uint16_t C_APPLE_RED_OUT= 0x3000;  // dark red outline
// Autumn apples — golden yellow
static constexpr uint16_t C_APPLE_YEL    = 0xFFE0;
static constexpr uint16_t C_APPLE_YEL2   = 0xC640;
static constexpr uint16_t C_APPLE_YEL_H  = 0xFFFF;
static constexpr uint16_t C_APPLE_YEL_OUT= 0x8400;
// Winter fir cones / oak acorn brown
static constexpr uint16_t C_CONE   = 0x9A40;
static constexpr uint16_t C_CONE2  = 0x6200;
static constexpr uint16_t C_CONE_H = 0xDE20;
// Winter fir
static constexpr uint16_t C_FIR    = 0x0B40;
static constexpr uint16_t C_FIR2   = 0x1C80;
static constexpr uint16_t C_FIR3   = 0x0540;
static constexpr uint16_t C_ORNAM  = 0xF800;  // red ornament
// Spring cherry (sakura) — dark trunk + pink blossom canopy
static constexpr uint16_t C_CHERRY_TRUNK  = 0x5A20;  // warm brown bark
static constexpr uint16_t C_CHERRY_TRUNK2 = 0x3900;  // dark shade
static constexpr uint16_t C_CHERRY_TRUNK_H= 0x8C40;  // lit edge
static constexpr uint16_t C_BLOSSOM1 = 0xC816;  // deep rose (volume)
static constexpr uint16_t C_BLOSSOM2 = 0xFB56;  // mid pink
static constexpr uint16_t C_BLOSSOM3 = 0xFEDB;  // pale blossom tip
static constexpr uint16_t C_SPRING = 0x3D08;  // fresh leaf (bush / accents)
static constexpr uint16_t C_SPRING2= 0x6E8A;
static constexpr uint16_t C_SPRING3= 0xAFE5;
// Spring willow (decorative) — olive bark + lime curtains, yellow catkins
static constexpr uint16_t C_WILLOW_TRUNK  = 0x6B20;
static constexpr uint16_t C_WILLOW_TRUNK2 = 0x4200;
static constexpr uint16_t C_WILLOW_TRUNK_H= 0x8C60;
static constexpr uint16_t C_WILLOW1 = 0x3C80;  // deep lime
static constexpr uint16_t C_WILLOW2 = 0x6E20;  // mid
static constexpr uint16_t C_WILLOW3 = 0xBFE0;  // pale tip
static constexpr uint16_t C_CATKIN  = 0xDE40;  // yellow-green catkin
// Autumn old apple tree — warm fall canopy
static constexpr uint16_t C_OAK1   = 0xD300;  // orange
static constexpr uint16_t C_OAK2   = 0xE4E0;  // yellow
static constexpr uint16_t C_OAK3   = 0x9A20;  // brown-red
static constexpr uint16_t C_BERRY  = 0xB01F;
static constexpr uint16_t C_BERRY2 = 0xF81F;
static constexpr uint16_t C_DECOR_LEAF = 0x1C40;

enum class Phase : uint8_t { HIDDEN = 0, GROWING, ALIVE, COLLAPSING };

// Season look for FRUIT + DECOR (main scenery trees)
// FRUIT:  SPRING=cherry  SUMMER=apple  AUTUMN=old apple  WINTER=fir
// DECOR:  SPRING=willow  SUMMER=apple  AUTUMN=old apple  WINTER=snowy apple
enum class SeasonTree : uint8_t {
    CHERRY     = 0,
    APPLE      = 1,
    OLD_APPLE  = 2,  // autumn "old apple tree" (was oak)
    FIR        = 3,
    WILLOW     = 4,  // spring decorative only — weeping, not a 2nd cherry
    LAMP       = 5,  // noir decorative — street lamp
    STALL      = 6,  // CITY market stall (fruit lane)
    TRASH      = 7,  // CITY trash can (bush lane)
    PALM_FRUIT = 8,  // DESERT palm with bananas
    PALM_DECOR = 9,  // DESERT plain palm
    CACTUS     = 10  // DESERT cactus (bush lane)
};

struct Branch { int16_t x1, y1, x2, y2; uint8_t thickness; };
struct Leaf   { int16_t cx, cy; uint8_t radius; uint8_t shade; };
struct Fruit  { int16_t ox, oy; uint8_t r, bob; Produce produce; };

static constexpr uint8_t MAX_BRANCHES = 24;
static constexpr uint8_t MAX_LEAVES   = 24;
static constexpr uint8_t MAX_FRUITS   = 8;
static constexpr uint16_t GROW_MS     = 2200;
static constexpr uint16_t COLLAPSE_MS = 1100;
static constexpr uint32_t MIN_ALIVE_MS = 60000;

struct Slot {
    Kind kind;
    Phase phase;
    SeasonTree style;   // season look (FRUIT/DECOR)
    float growth;
    uint32_t animStart, aliveStart;
    int16_t baseX, scroll;
    int8_t lean;
    uint8_t trunkH, trunkW, crownR;
    Branch branches[MAX_BRANCHES];
    uint8_t branchCount;
    Leaf leaves[MAX_LEAVES];
    uint8_t leafCount;
    Fruit fruits[MAX_FRUITS];
    uint8_t fruitCount;
    uint32_t seed;
    bool pendingHide, pendingShow;
    uint8_t pendingFruits;
    uint8_t stompHits;
    bool stompArmed;
    int8_t stompShake;
    uint32_t stompShakeUntil;
    uint32_t meteorDamageStart;
    int8_t meteorDamageSide;
};

// Three configurable instances of each flora kind.
static constexpr uint8_t INSTANCES_PER_KIND = 3;
static constexpr uint8_t SLOT_COUNT = 9;
static Slot slots[SLOT_COUNT];
static int pigHintX = 60;
static bool pigHintRight = false;
static bool waveShake = false;
static uint32_t waveShakeStart = 0;
static int16_t drawWorldShift = 0;

static SeasonTree styleFromSeason(Season s) {
    switch (s) {
        case Season::SPRING: return SeasonTree::CHERRY;
        case Season::SUMMER: return SeasonTree::APPLE;
        case Season::AUTUMN: return SeasonTree::OLD_APPLE;
        case Season::WINTER: return SeasonTree::FIR;
        case Season::RETRO:  return SeasonTree::APPLE;  // round canopy, mono colors applied at draw
        case Season::NOIR:   return SeasonTree::APPLE;  // dark canopy; decor becomes a lamp
        case Season::CITY:   return SeasonTree::STALL;  // fruit lane = market stall
        case Season::DESERT: return SeasonTree::PALM_FRUIT;
        default: return SeasonTree::APPLE;
    }
}

static bool isRetroSeason() {
    return Weather::getActiveSeason() == Season::RETRO;
}

// Silver-screen flora palette (old film)
static constexpr uint16_t R_TRUNK  = 0x4208;
static constexpr uint16_t R_TRUNK2 = 0x2104;
static constexpr uint16_t R_TRUNK_H= 0x7BEF;
static constexpr uint16_t R_LEAF   = 0x4A49;
static constexpr uint16_t R_LEAF2  = 0x8410;
static constexpr uint16_t R_LEAF3  = 0xBDF7;
static constexpr uint16_t R_FIR    = 0x632C;
static constexpr uint16_t R_FIR2   = 0x9CF3;
static constexpr uint16_t R_FIR3   = 0x3186;
static constexpr uint16_t R_SNOW   = 0xFFFF;
static constexpr uint16_t R_SNOW2  = 0xC618;

static SeasonTree currentStyle() {
    return styleFromSeason(Weather::getActiveSeason());
}
// One stomp per airborne period across all flora
static bool globalStompArmed = true;

// Season → falling / hanging produce
static Produce produceForSlot(const Slot& t) {
    // Bush always berries (all seasons)
    if (t.kind == Kind::BERRY) return Produce::BERRY;
    if (t.style == SeasonTree::OLD_APPLE) return Produce::GREEN_APPLE;
    if (t.style == SeasonTree::CHERRY) return Produce::CHERRY;
    if (t.style == SeasonTree::FIR) return Produce::CONE;
    if (t.style == SeasonTree::PALM_FRUIT) return Produce::YELLOW_APPLE;
    if (t.style == SeasonTree::CACTUS) return Produce::BERRY;
    return Produce::RED_APPLE;
}

static void rollFruitKinds(Slot& t) {
    for (uint8_t i = 0; i < t.fruitCount; i++) {
        Produce p = produceForSlot(t);
        if (t.kind == Kind::FRUIT && XP::goldAppleUnlocked() &&
            (esp_random() % 100) < 12)
            p = Produce::GOLD_APPLE;
        t.fruits[i].produce = p;
    }
}

static uint32_t lcg(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s;
}
static const int16_t sin_lut[13] = {
    0, 66, 128, 181, 222, 248, 256, 248, 222, 181, 128, 66, 0
};
static int16_t lsin(uint8_t i) { return (i < 13) ? sin_lut[i] : 0; }
static int16_t lcos(uint8_t i) {
    int8_t ci = 6 - (int8_t)i;
    return (ci >= 0) ? sin_lut[ci] : -sin_lut[-ci];
}

static int16_t screenX(const Slot& t) {
    const int16_t width = worldWidth();
    int16_t x = (int16_t)((t.baseX + t.scroll) % width);
    if (x < 0) x += width;
    return (int16_t)(x + drawWorldShift);
}

static Slot& floraSlot(Kind kind, uint8_t instance = 0) {
    return slots[(uint8_t)kind * INSTANCES_PER_KIND + instance];
}
static Slot& fruitSlot(uint8_t instance = 0) { return floraSlot(Kind::FRUIT, instance); }
static Slot& decorSlot(uint8_t instance = 0) { return floraSlot(Kind::DECOR, instance); }
static Slot& berrySlot(uint8_t instance = 0) { return floraSlot(Kind::BERRY, instance); }

static uint8_t configuredCount(Kind kind) {
    const PersonalityConfig& p = Config::personality();
    uint8_t count = kind == Kind::FRUIT ? p.fruitTreeCount
        : kind == Kind::DECOR ? p.decorTreeCount : p.bushCount;
    if (count < 1) return 1;
    return count > INSTANCES_PER_KIND ? INSTANCES_PER_KIND : count;
}

// ---------- generate: full L/R canopy (never one-sided) + leaves + produce ----------
// scalePct: 100 = fruit/decor, ~70 = berry bush (taller than before, still smaller)
// FRUIT/DECOR pick season style: cherry / apple / old apple / fir
static void genClassicTree(Slot& t, uint8_t fruitCount, uint8_t scalePct) {
    t.seed = esp_random();
    t.scroll = 0;
    t.meteorDamageStart = 0;
    t.meteorDamageSide = 0;
    uint32_t s = t.seed;
    if (t.kind != Kind::BERRY) {
        t.style = currentStyle();
        // DECOR never copies the fruit tree
        if (t.kind == Kind::DECOR && t.style == SeasonTree::FIR)
            t.style = SeasonTree::APPLE;
        if (t.kind == Kind::DECOR && t.style == SeasonTree::CHERRY)
            t.style = SeasonTree::WILLOW;
        if (t.kind == Kind::DECOR && Weather::getActiveSeason() == Season::NOIR)
            t.style = SeasonTree::LAMP;
        if (t.kind == Kind::DECOR && Weather::getActiveSeason() == Season::CITY)
            t.style = SeasonTree::LAMP;
        if (t.kind == Kind::FRUIT && Weather::getActiveSeason() == Season::CITY)
            t.style = SeasonTree::STALL;
        if (t.kind == Kind::FRUIT && Weather::getActiveSeason() == Season::DESERT)
            t.style = SeasonTree::PALM_FRUIT;
        if (t.kind == Kind::DECOR && Weather::getActiveSeason() == Season::DESERT)
            t.style = SeasonTree::PALM_DECOR;
    } else {
        if (Weather::getActiveSeason() == Season::CITY)
            t.style = SeasonTree::TRASH;
        else if (Weather::getActiveSeason() == Season::DESERT)
            t.style = SeasonTree::CACTUS;  // bush lane = cactus with tip flowers
        else
            t.style = SeasonTree::APPLE;  // bush shape; produce color from season
    }

    // Position opposite the pig (berry mid-ground)
    if (t.kind == Kind::BERRY) {
        t.baseX = 40 + (int16_t)(lcg(s) % 160);
        if (abs(t.baseX - pigHintX) < 40)
            t.baseX = (pigHintX < 120) ? 160 + (int16_t)(lcg(s) % 40)
                                       : 30 + (int16_t)(lcg(s) % 40);
    } else if (pigHintRight) {
        t.baseX = 25 + (int16_t)(lcg(s) % 30);
    } else {
        t.baseX = 175 + (int16_t)(lcg(s) % 35);
    }
    if (abs(t.baseX - pigHintX) < 60 && t.kind != Kind::BERRY) {
        t.baseX = (pigHintX < 120) ? 180 + (int16_t)(lcg(s) % 30)
                                   : 20 + (int16_t)(lcg(s) % 30);
    }

    // Height by season species
    uint8_t fullH = 66 + fruitCount + (uint8_t)(lcg(s) % 7);
    if (t.kind == Kind::BERRY) fullH = 40 + (uint8_t)(lcg(s) % 10);
    else if (t.style == SeasonTree::FIR) fullH = 70 + (uint8_t)(lcg(s) % 10);   // tall fir
    else if (t.style == SeasonTree::LAMP) fullH = 64 + (uint8_t)(lcg(s) % 6);   // street lamp
    else if (t.style == SeasonTree::STALL) fullH = 52 + (uint8_t)(lcg(s) % 8);
    else if (t.style == SeasonTree::TRASH) fullH = 36 + (uint8_t)(lcg(s) % 6);
    else if (t.style == SeasonTree::PALM_FRUIT || t.style == SeasonTree::PALM_DECOR)
        fullH = 72 + (uint8_t)(lcg(s) % 12);  // a bit taller — crowns above pig
    else if (t.style == SeasonTree::CACTUS) fullH = 52 + (uint8_t)(lcg(s) % 12);
    else if (t.style == SeasonTree::WILLOW) fullH = 66 + (uint8_t)(lcg(s) % 8); // tall weeping willow
    else if (t.style == SeasonTree::OLD_APPLE) fullH = 58 + (uint8_t)(lcg(s) % 8); // stout old apple
    else if (t.style == SeasonTree::CHERRY) fullH = 60 + (uint8_t)(lcg(s) % 10); // elegant cherry
    t.trunkH = (uint8_t)((int)fullH * (int)scalePct / 100);
    if (t.kind == Kind::BERRY && t.trunkH < 28) t.trunkH = 28;
    if (t.trunkH < 14) t.trunkH = 14;
    t.trunkW = 2 + (uint8_t)(lcg(s) % 2);
    if (t.style == SeasonTree::OLD_APPLE) t.trunkW = 3;     // thick gnarled trunk
    if (t.style == SeasonTree::CHERRY) t.trunkW = 2;        // medium cherry trunk
    if (t.style == SeasonTree::WILLOW) t.trunkW = 2;
    if (t.style == SeasonTree::LAMP) t.trunkW = 1;
    if (t.style == SeasonTree::FIR) t.trunkW = 1;
    if (t.kind == Kind::BERRY && t.trunkW > 2) t.trunkW = 2;
    t.lean = (int8_t)(lcg(s) % 7) - 3;
    if (t.style == SeasonTree::FIR) t.lean = (int8_t)(lcg(s) % 3) - 1;  // straighter
    if (t.style == SeasonTree::WILLOW) t.lean = (int8_t)(lcg(s) % 5) - 2;
    t.crownR = 8 + t.trunkH / 5 + (uint8_t)(lcg(s) % 3);

    // --- Spring willow: high arches + hanging curtains (decorative only) ---
    if (t.kind != Kind::BERRY && t.style == SeasonTree::WILLOW) {
        t.branchCount = 0;
        t.leafCount = 0;
        t.fruitCount = 0;
        auto addHang = [&](int8_t side, uint8_t originPct, uint8_t outLen, uint8_t hangLen) {
            if (t.branchCount + 4 > MAX_BRANCHES) return;
            int16_t oy = -(int16_t)(t.trunkH * originPct / 100);
            int16_t ox = (int16_t)t.lean * originPct / 100;
            Branch& br = t.branches[t.branchCount++];
            br.x1 = ox; br.y1 = oy;
            br.x2 = ox + (int16_t)side * (int16_t)outLen;
            br.y2 = oy + 6;
            br.thickness = 2;
            for (int h = 0; h < 3 && t.branchCount < MAX_BRANCHES; h++) {
                int16_t hx = br.x1 + (br.x2 - br.x1) * (h + 1) / 3;
                int16_t hy = br.y1 + (br.y2 - br.y1) * (h + 1) / 3;
                int16_t hl = (int16_t)hangLen + (int16_t)(lcg(s) % 8) - (int16_t)(h * 5);
                if (hl < 14) hl = 14;
                Branch& hg = t.branches[t.branchCount++];
                hg.x1 = hx; hg.y1 = hy;
                hg.x2 = hx + (int16_t)side * (int16_t)(1 + (lcg(s) % 4));
                hg.y2 = hy + hl;
                if (hg.y2 > -6) hg.y2 = -6;
                hg.thickness = 1;
                for (uint8_t k = 0; k < 2 && t.leafCount < MAX_LEAVES; k++) {
                    Leaf& L = t.leaves[t.leafCount++];
                    L.cx = hg.x1 + (hg.x2 - hg.x1) * (int16_t)(k + 1) / 3
                         + (int16_t)((lcg(s) % 3) - 1);
                    L.cy = hg.y1 + (hg.y2 - hg.y1) * (int16_t)(k + 1) / 3
                         + (int16_t)((lcg(s) % 3) - 1);
                    L.radius = 3 + (uint8_t)(lcg(s) % 2);
                    L.shade = (uint8_t)(lcg(s) % 3);
                }
            }
        };
        addHang( 1, 90, 22, 28);
        addHang(-1, 90, 22, 28);
        addHang( 1, 68, 24, 32);
        addHang(-1, 68, 24, 32);
        if (t.branchCount + 3 <= MAX_BRANCHES) {
            int16_t top = -(int16_t)t.trunkH;
            for (int c = -1; c <= 1; c++) {
                Branch& cr = t.branches[t.branchCount++];
                cr.x1 = 0; cr.y1 = top + 4;
                cr.x2 = (int16_t)(c * 8);
                cr.y2 = top - 8;
                cr.thickness = 1;
            }
        }
        t.stompHits = 0;
        t.stompArmed = true;
        return;
    }

    // --- Noir/city props: drawn by dedicated drawers ---
    if (t.style == SeasonTree::LAMP || t.style == SeasonTree::STALL ||
        t.style == SeasonTree::TRASH || t.style == SeasonTree::PALM_FRUIT ||
        t.style == SeasonTree::PALM_DECOR || t.style == SeasonTree::CACTUS) {
        t.branchCount = 0;
        t.leafCount = 0;
        // Stall still drops "fruit" (produce) for gameplay
        if (t.style != SeasonTree::STALL && t.style != SeasonTree::PALM_FRUIT &&
            t.style != SeasonTree::CACTUS) {
            t.fruitCount = 0;
        } else if (t.fruitCount == 0) {
            if (t.style == SeasonTree::STALL) {
                // Produce sits ON the counter (see drawStall counter Y)
                t.fruitCount = (uint8_t)(4 + (lcg(t.seed) % 3));
                static const int8_t slotX[] = { -14, -7, 0, 7, 14, -10, 4 };
                for (uint8_t i = 0; i < t.fruitCount; i++) {
                    t.fruits[i].ox = (int16_t)(slotX[i % 7] + (int)(lcg(t.seed) % 3) - 1);
                    t.fruits[i].oy = 0;  // meaning: on counter (drawer places Y)
                    t.fruits[i].r = 2 + (uint8_t)(lcg(t.seed) % 2);
                    t.fruits[i].bob = (uint8_t)(lcg(t.seed) % 4);
                    uint8_t k = (uint8_t)(lcg(t.seed) % 4);
                    t.fruits[i].produce = (k == 0) ? Produce::RED_APPLE
                                       : (k == 1) ? Produce::YELLOW_APPLE
                                       : (k == 2) ? Produce::GREEN_APPLE
                                       : Produce::CHERRY;
                }
                if (XP::goldAppleUnlocked() && (esp_random() % 100) < 12)
                    t.fruits[0].produce = Produce::GOLD_APPLE;
            } else if (t.style == SeasonTree::PALM_FRUIT) {
                // Banana bunch under the crown (cluster left of trunk)
                t.fruitCount = (uint8_t)(3 + (lcg(t.seed) % 3));
                for (uint8_t i = 0; i < t.fruitCount; i++) {
                    t.fruits[i].ox = (int16_t)(-6 - (int)(i % 3) * 3 + (int)(lcg(t.seed) % 2));
                    t.fruits[i].oy = (int16_t)(4 + (int)(i / 2) * 4);  // below top
                    t.fruits[i].r = 2;
                    t.fruits[i].bob = (uint8_t)i;
                    t.fruits[i].produce = Produce::YELLOW_APPLE;
                }
            } else { // CACTUS flowers at arm tips / top
                t.fruitCount = (uint8_t)(2 + (lcg(t.seed) % 3));
                for (uint8_t i = 0; i < t.fruitCount; i++) {
                    // ox: -1 left arm, 0 top, +1 right arm
                    int arm = (int)(lcg(t.seed) % 3) - 1;
                    t.fruits[i].ox = (int16_t)arm;
                    t.fruits[i].oy = (int16_t)(lcg(t.seed) % 3);
                    t.fruits[i].r = 2;
                    t.fruits[i].bob = (uint8_t)(lcg(t.seed) % 4);
                    t.fruits[i].produce = Produce::BERRY;
                }
            }
        }
        t.lean = 0;
        t.stompHits = 0;
        t.stompArmed = true;
        return;
    }

    // --- Winter fir: no classic branches — tiers stored as horizontal "needles" ---
    if (t.kind != Kind::BERRY && t.style == SeasonTree::FIR) {
        t.branchCount = 0;
        t.leafCount = 0;
        // branches[] as tier rows: y1=y2 = -height, x1=-halfW, x2=+halfW
        uint8_t tiers = 5 + (uint8_t)(lcg(s) % 2);
        for (uint8_t i = 0; i < tiers && t.branchCount < MAX_BRANCHES; i++) {
            float u = (float)(i + 1) / (float)(tiers + 1);
            int16_t ty = -(int16_t)(t.trunkH * (0.25f + u * 0.72f));
            int16_t half = (int16_t)(8 + (1.0f - u) * 22 + (lcg(s) % 4));
            Branch& br = t.branches[t.branchCount++];
            br.x1 = -half; br.y1 = ty;
            br.x2 = half;  br.y2 = ty;
            br.thickness = 2;
        }
        // Ornaments as "fruits" only on FRUIT kind
        if (t.kind == Kind::FRUIT) {
            t.fruitCount = 4 + (uint8_t)(lcg(s) % 4);
            if (t.fruitCount > MAX_FRUITS) t.fruitCount = MAX_FRUITS;
            for (uint8_t i = 0; i < t.fruitCount; i++) {
                const Branch& br = t.branches[lcg(s) % t.branchCount];
                int16_t span = br.x2 - br.x1;
                t.fruits[i].ox = br.x1 + (int16_t)(lcg(s) % (span > 2 ? span : 2));
                t.fruits[i].oy = br.y1 + (int16_t)((lcg(s) % 5) - 2);
                t.fruits[i].r = 2;
                t.fruits[i].bob = (uint8_t)(lcg(s) & 0xFF);
            }
        } else {
            t.fruitCount = 0;
        }
        rollFruitKinds(t);
        t.stompHits = 0;
        t.stompArmed = true;
        return;
    }

    t.branchCount = 0;
    t.leafCount = 0;
    uint8_t subEndpoints[12];
    uint8_t subEndpointCount = 0;

    auto scaleLen = [&](uint8_t len) -> uint8_t {
        uint8_t v = (uint8_t)((int)len * (int)scalePct / 100);
        return v < 5 ? 5 : v;
    };

    // Angle helpers: 0..12 = 0°..180°. Force BOTH sides of trunk.
    // Right fan: 1..5 (15°–75°), Left fan: 7..11 (105°–165°), Up: 5..7
    auto addMain = [&](uint8_t angleLo, uint8_t angleHi, uint8_t originPct, uint8_t lenBase) {
        if (t.branchCount >= MAX_BRANCHES) return;
        uint8_t span = (angleHi > angleLo) ? (uint8_t)(angleHi - angleLo + 1) : 1;
        uint8_t angleIdx = angleLo + (uint8_t)(lcg(s) % span);
        if (angleIdx > 12) angleIdx = 12;
        int16_t originY = -(int16_t)(t.trunkH * originPct / 100);
        int16_t originX = (int16_t)t.lean * originPct / 100;
        uint8_t length = scaleLen(lenBase + (uint8_t)(lcg(s) % 12));
        int16_t dx = (int16_t)((int16_t)length * lcos(angleIdx) / 256);
        int16_t dy = (int16_t)(-(int16_t)length * lsin(angleIdx) / 256);
        Branch& br = t.branches[t.branchCount];
        br.x1 = originX; br.y1 = originY;
        br.x2 = originX + dx; br.y2 = originY + dy;
        br.thickness = 2;
        t.branchCount++;

        // Two subs that fan further LEFT and RIGHT of this main
        for (int sub = 0; sub < 2 && t.branchCount < MAX_BRANCHES; sub++) {
            int16_t mx = br.x1 + (br.x2 - br.x1) * (sub == 0 ? 2 : 1) / 3;
            int16_t my = br.y1 + (br.y2 - br.y1) * (sub == 0 ? 2 : 1) / 3;
            int8_t side = (sub == 0) ? 1 : -1;
            int8_t subAngle = (int8_t)angleIdx + side * (1 + (int8_t)(lcg(s) % 3));
            if (subAngle < 1) subAngle = 1;
            if (subAngle > 11) subAngle = 11;
            uint8_t subLen = scaleLen(10 + (uint8_t)(lcg(s) % 12));
            uint8_t subIdx = t.branchCount;
            Branch& sbr = t.branches[subIdx];
            sbr.x1 = mx; sbr.y1 = my;
            sbr.x2 = mx + (int16_t)((int16_t)subLen * lcos((uint8_t)subAngle) / 256);
            sbr.y2 = my + (int16_t)(-(int16_t)subLen * lsin((uint8_t)subAngle) / 256);
            sbr.thickness = 1;
            t.branchCount++;
            if (subEndpointCount < 12) subEndpoints[subEndpointCount++] = subIdx;
        }
    };

    // Always: RIGHT mid, LEFT mid, RIGHT high, LEFT high, UP crown (both sides guaranteed)
    const uint8_t nMains = (t.kind == Kind::BERRY) ? 6 : 5;
    addMain(1, 4, 55, 16);   // right lower
    addMain(8, 11, 55, 16);  // left lower
    addMain(2, 5, 75, 18);   // right upper
    addMain(7, 10, 75, 18);  // left upper
    addMain(5, 7, 100, 14);  // crown up
    if (nMains >= 6) {
        addMain(1, 3, 40, 12);   // bush: extra low right
        addMain(9, 11, 40, 12);  // bush: extra low left
    }

    // Tertiary twigs — keep parent direction sign so they don't all collapse one way
    for (uint8_t ti = 0; ti < subEndpointCount && t.branchCount < MAX_BRANCHES; ti++) {
        const Branch& parent = t.branches[subEndpoints[ti]];
        int16_t pdx = parent.x2 - parent.x1;
        int8_t side = (pdx >= 0) ? 1 : -1;
        int8_t terAngle = (side > 0)
            ? (int8_t)(2 + (lcg(s) % 4))     // rightish 30–75°
            : (int8_t)(8 + (lcg(s) % 4));    // leftish 120–165°
        if (lcg(s) % 3 == 0) terAngle = 6;   // occasional up
        if (terAngle < 1) terAngle = 1;
        if (terAngle > 11) terAngle = 11;
        uint8_t terLen = scaleLen(7 + (uint8_t)(lcg(s) % 10));
        Branch& tbr = t.branches[t.branchCount];
        tbr.x1 = parent.x2; tbr.y1 = parent.y2;
        tbr.x2 = parent.x2 + (int16_t)((int16_t)terLen * lcos((uint8_t)terAngle) / 256);
        tbr.y2 = parent.y2 + (int16_t)(-(int16_t)terLen * lsin((uint8_t)terAngle) / 256);
        tbr.thickness = 1;
        t.branchCount++;
    }

    // Leaf clusters at tips + mids, with color variants
    for (uint8_t i = 0; i < t.branchCount && t.leafCount < MAX_LEAVES; i++) {
        const Branch& br = t.branches[i];
        Leaf& L = t.leaves[t.leafCount++];
        L.cx = br.x2 + (int16_t)((lcg(s) % 5) - 2);
        L.cy = br.y2 + (int16_t)((lcg(s) % 5) - 2);
        L.radius = scaleLen(4 + (uint8_t)(lcg(s) % 4));
        if (L.radius < 2) L.radius = 2;
        L.shade = (uint8_t)(lcg(s) % 3);
        if (t.leafCount < MAX_LEAVES) {
            Leaf& M = t.leaves[t.leafCount++];
            M.cx = br.x1 + (br.x2 - br.x1) * 2 / 3 + (int16_t)((lcg(s) % 3) - 1);
            M.cy = br.y1 + (br.y2 - br.y1) * 2 / 3 + (int16_t)((lcg(s) % 3) - 1);
            M.radius = scaleLen(3 + (uint8_t)(lcg(s) % 3));
            if (M.radius < 2) M.radius = 2;
            M.shade = (uint8_t)(lcg(s) % 3);
        }
    }

    // Produce: berry bush always; FRUIT by season (apple / cherry / green apple)
    bool withProduce = false;
    if (t.kind == Kind::BERRY) withProduce = true;
    else if (t.kind == Kind::FRUIT) {
        withProduce = (t.style == SeasonTree::APPLE || t.style == SeasonTree::OLD_APPLE ||
                       t.style == SeasonTree::CHERRY);
    }
    if (withProduce) {
        uint8_t want = fruitCount;
        if (t.kind == Kind::BERRY) {
            want = 5 + (uint8_t)(lcg(s) % 4);  // berries always
            if (want > MAX_FRUITS) want = MAX_FRUITS;
        } else if (t.style == SeasonTree::OLD_APPLE) {
            want = 4 + (uint8_t)(lcg(s) % 4);  // soft green apples
            if (want > MAX_FRUITS) want = MAX_FRUITS;
        } else if (t.style == SeasonTree::CHERRY) {
            // cherries hang in pairs across the blossom canopy
            want = 5 + (uint8_t)(lcg(s) % 4);  // 5..8
            if (want > MAX_FRUITS) want = MAX_FRUITS;
        } else if (want > MAX_FRUITS) {
            want = MAX_FRUITS;
        }
        t.fruitCount = want;
        // Base size by kind; each fruit then jittered so they look mixed
        // Apples: r 2–3 only (compact red apples, not watermelons)
        uint8_t rBase = 2, rJitter = 2;
        if (t.kind == Kind::BERRY) { rBase = 2; rJitter = 2; }
        else if (t.style == SeasonTree::FIR) { rBase = 2; rJitter = 2; }
        else if (t.style == SeasonTree::OLD_APPLE) { rBase = 2; rJitter = 2; }  // compact green apples
        else if (t.style == SeasonTree::CHERRY) { rBase = 2; rJitter = 2; } // paired cherries
        else if (t.style == SeasonTree::APPLE) { rBase = 2; rJitter = 2; } // 2..3 compact
        uint8_t endpointCount = t.branchCount > 0 ? t.branchCount : 1;
        for (uint8_t i = 0; i < t.fruitCount; i++) {
            const Branch& br = t.branches[lcg(s) % endpointCount];
            int16_t scatter = (t.kind == Kind::BERRY || t.style == SeasonTree::OLD_APPLE) ? 2 : 3;
            t.fruits[i].ox = br.x2 + (int16_t)((lcg(s) % (scatter * 2 + 1)) - scatter);
            // Cherries hang slightly below blossom tips
            int16_t yOff = (t.style == SeasonTree::CHERRY) ? 3 : 0;
            t.fruits[i].oy = br.y2 + yOff + (int16_t)((lcg(s) % (scatter * 2 + 1)) - scatter);
            uint8_t rr = rBase + (uint8_t)(lcg(s) % rJitter);
            if (rr < 2) rr = 2;
            if (t.style == SeasonTree::APPLE && rr > 3) rr = 3;
            else if (rr > 7) rr = 7;
            t.fruits[i].r = rr;
            t.fruits[i].bob = (uint8_t)(lcg(s) & 0xFF);
        }
    } else {
        t.fruitCount = 0;
    }

    rollFruitKinds(t);
    t.stompHits = 0;
    t.stompArmed = true;
}

// ---------- public spawn ----------
void setPigHint(int pigX, bool pigOnRight) {
    pigHintX = pigX;
    pigHintRight = pigOnRight;
}

void init() {
    reset();
    applyFloraCounts();
}

void reset() {
    for (int i = 0; i < SLOT_COUNT; i++) {
        slots[i].kind = (Kind)(i / INSTANCES_PER_KIND);
        slots[i].phase = Phase::HIDDEN;
        slots[i].growth = 0;
        slots[i].fruitCount = 0;
        slots[i].branchCount = 0;
        slots[i].pendingHide = slots[i].pendingShow = false;
        slots[i].stompHits = 0;
        slots[i].stompArmed = true;
        slots[i].stompShake = 0;
        slots[i].stompShakeUntil = 0;
        slots[i].scroll = 0;
    }
    dropsReset();
    waveShake = false;
    globalStompArmed = true;
}

static int16_t sharedScroll() {
    return (int16_t)-Ground::worldOffset();
}

static bool floraLive(const Slot& t) {
    return t.phase != Phase::HIDDEN && t.phase != Phase::COLLAPSING;
}

static bool anyFloraLive() {
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (floraLive(slots[i])) return true;
    }
    return false;
}

static int16_t wrapDistWorld(int16_t a, int16_t b) {
    const int16_t width = worldWidth();
    a %= width;
    b %= width;
    if (a < 0) a += width;
    if (b < 0) b += width;
    int16_t distance = (int16_t)abs((int)a - (int)b);
    return distance > width / 2 ? (int16_t)(width - distance) : distance;
}

static int16_t floraWorldX(const Slot& t) {
    return t.baseX;
}

static bool isOffscreenWorld(int16_t wx) {
    int16_t sx = (int16_t)(wx - Ground::worldOffset());
    sx %= worldWidth();
    if (sx < 0) sx += worldWidth();
    return sx >= DISPLAY_W;
}

static int16_t pickWorldX(const Slot& self, bool preferOffscreen) {
    const int16_t width = worldWidth();
    if (!anyFloraLive()) {
        const int16_t cameraX = Ground::worldOffset();
        return (int16_t)((cameraX + 28 + (esp_random() % 186u)) % width);
    }

    for (int pass = 0; pass < 2; pass++) {
        const bool needOff = preferOffscreen && pass == 0;
        if (pass == 1 && !preferOffscreen) break;
        int16_t bestX = 0;
        int16_t bestDistance = -1;
        uint16_t ties = 0;
        uint32_t randomState = esp_random() | 1u;
        for (int16_t wx = 0; wx < width; wx++) {
            if (needOff && !isOffscreenWorld(wx)) continue;
            int16_t nearest = width;
            for (uint8_t i = 0; i < SLOT_COUNT; i++) {
                if (&slots[i] == &self || !floraLive(slots[i])) continue;
                const int16_t distance = wrapDistWorld(wx, floraWorldX(slots[i]));
                if (distance < nearest) nearest = distance;
            }
            if (nearest > bestDistance) {
                bestDistance = nearest;
                bestX = wx;
                ties = 1;
            } else if (nearest == bestDistance) {
                randomState ^= randomState << 13;
                randomState ^= randomState >> 17;
                randomState ^= randomState << 5;
                if (randomState % ++ties == 0) bestX = wx;
            }
        }
        if (bestDistance >= 0) return bestX;
    }
    return 0;
}

static void placeInWorld(Slot& t, bool preferOffscreen) {
    t.scroll = sharedScroll();
    int16_t wx = pickWorldX(t, preferOffscreen);
    t.baseX = wx;
}

static uint8_t produceWant(const Slot& t) {
    if (t.kind == Kind::DECOR || t.style == SeasonTree::LAMP ||
        t.style == SeasonTree::STALL || t.style == SeasonTree::TRASH ||
        t.style == SeasonTree::PALM_DECOR) return 0;
    if (t.kind == Kind::BERRY) return 6;
    if (t.style == SeasonTree::FIR) return 5;
    if (t.style == SeasonTree::CHERRY) return 6;
    return 5;
}

static void addOneProduce(Slot& t) {
    if (t.fruitCount >= MAX_FRUITS) return;
    uint32_t s = t.seed + (uint32_t)t.fruitCount * 17u + millis();
    Fruit& f = t.fruits[t.fruitCount];
    if (t.branchCount > 0) {
        const Branch& br = t.branches[lcg(s) % t.branchCount];
        f.ox = br.x2 + (int16_t)((lcg(s) % 5) - 2);
        f.oy = br.y2 + (int16_t)((lcg(s) % 5) - 2);
    } else {
        f.ox = (int16_t)((lcg(s) % 17) - 8);
        f.oy = -(int16_t)(10 + (lcg(s) % 18));
    }
    f.r = 2;
    f.bob = (uint8_t)(lcg(s) & 0xFF);
    t.fruitCount++;
    rollFruitKinds(t);
}

static void refillProduce(Slot& t) {
    if (t.phase != Phase::ALIVE) return;
    uint8_t want = produceWant(t);
    while (t.fruitCount < want) addOneProduce(t);
}

static void hideSlot(Slot& t, bool force) {
    if (t.phase == Phase::HIDDEN || t.phase == Phase::COLLAPSING) return;
    if (!force && t.phase == Phase::GROWING) { t.pendingHide = true; return; }
    if (!force && t.phase == Phase::ALIVE &&
        (millis() - t.aliveStart < MIN_ALIVE_MS)) {
        t.pendingHide = true;
        return;
    }
    t.phase = Phase::COLLAPSING;
    t.animStart = millis();
    t.growth = 1.0f;
}

static void hideExcess(Kind kind, uint8_t count) {
    for (uint8_t i = count; i < INSTANCES_PER_KIND; i++) {
        Slot& t = floraSlot(kind, i);
        t.pendingShow = false;
        hideSlot(t, true);
    }
}

static void dumpAllProduce(Slot& t);

void showFruit(uint8_t fruitCount) {
    if (fruitCount == 0) return;
    const uint8_t count = configuredCount(Kind::FRUIT);
    for (uint8_t instance = 0; instance < count; instance++) {
    Slot& t = fruitSlot(instance);
    t.kind = Kind::FRUIT;
    if (t.phase == Phase::COLLAPSING) {
        t.pendingShow = true;
        t.pendingFruits = fruitCount;
        continue;
    }
    if (t.phase == Phase::GROWING) continue;
    if (t.phase == Phase::ALIVE) {
        // Refresh produce on existing branches (classic scatter)
        uint32_t s = t.seed + fruitCount;
        t.fruitCount = fruitCount > MAX_FRUITS ? MAX_FRUITS : fruitCount;
        uint8_t ec = t.branchCount > 0 ? t.branchCount : 1;
        for (uint8_t i = 0; i < t.fruitCount; i++) {
            const Branch& br = t.branches[lcg(s) % ec];
            int16_t scatter = 3;
            t.fruits[i].ox = br.x2 + (int16_t)((lcg(s) % (scatter * 2 + 1)) - scatter);
            t.fruits[i].oy = br.y2 + (int16_t)((lcg(s) % (scatter * 2 + 1)) - scatter);
            // compact apples only (2..3)
            t.fruits[i].r = 2 + (uint8_t)(lcg(s) % 2);
            t.fruits[i].bob = (uint8_t)(lcg(s) & 0xFF);
        }
        rollFruitKinds(t);
        t.stompHits = 0;
        continue;
    }
    genClassicTree(t, fruitCount, 100);
    placeInWorld(t, false);
    t.phase = Phase::GROWING;
    t.growth = 0;
    t.animStart = millis();
    t.pendingHide = false;
    }
    hideExcess(Kind::FRUIT, count);
}

void showDecor() {
    const uint8_t count = configuredCount(Kind::DECOR);
    for (uint8_t instance = 0; instance < count; instance++) {
        Slot& t = decorSlot(instance);
        t.kind = Kind::DECOR;
        if (t.phase == Phase::ALIVE || t.phase == Phase::GROWING) continue;
        genClassicTree(t, 3, 100);
        placeInWorld(t, false);
        t.phase = Phase::GROWING;
        t.growth = 0;
        t.animStart = millis();
    }
    hideExcess(Kind::DECOR, count);
}

void showBerry() {
    const uint8_t count = configuredCount(Kind::BERRY);
    for (uint8_t instance = 0; instance < count; instance++) {
        Slot& t = berrySlot(instance);
        t.kind = Kind::BERRY;
        if (t.phase == Phase::ALIVE || t.phase == Phase::GROWING) continue;
        genClassicTree(t, 6, 70);
        placeInWorld(t, false);
        t.phase = Phase::GROWING;
        t.growth = 0;
        t.animStart = millis();
    }
    hideExcess(Kind::BERRY, count);
}

void hideFruit() {
    for (uint8_t i = 0; i < INSTANCES_PER_KIND; i++) hideSlot(fruitSlot(i), false);
}
void hideDecor() {
    for (uint8_t i = 0; i < INSTANCES_PER_KIND; i++) hideSlot(decorSlot(i), true);
}
void hideBerry() {
    for (uint8_t i = 0; i < INSTANCES_PER_KIND; i++) hideSlot(berrySlot(i), true);
}
void hideAll() {
    for (uint8_t i = 0; i < SLOT_COUNT; i++) hideSlot(slots[i], true);
}

bool isFruitVisible() {
    for (uint8_t i = 0; i < INSTANCES_PER_KIND; i++)
        if (fruitSlot(i).phase != Phase::HIDDEN) return true;
    return false;
}

void applyFloraCounts() {
    const int16_t width = worldWidth();
    const int16_t worldScroll = (int16_t)-Ground::worldOffset();
    for (uint8_t i = 0; i < SLOT_COUNT; i++) {
        Slot& t = slots[i];
        if ((i % INSTANCES_PER_KIND) >= configuredCount(t.kind)) continue;
        t.scroll = worldScroll;
        if (t.phase == Phase::HIDDEN || t.baseX < width) continue;
        if (t.phase == Phase::COLLAPSING) {
            t.pendingShow = true;
            t.pendingFruits = t.kind == Kind::FRUIT ? 5 : (t.kind == Kind::DECOR ? 3 : 6);
            continue;
        }
        t.pendingShow = true;
        t.pendingFruits = t.kind == Kind::FRUIT ? 5 : (t.kind == Kind::DECOR ? 3 : 6);
        t.phase = Phase::COLLAPSING;
        t.animStart = millis();
        t.growth = 1.0f;
    }
    showFruit(5);
    showDecor();
    showBerry();
}

// ---------- update phases ----------
static void updateSlot(Slot& t) {
    uint32_t now = millis();
    if (t.phase == Phase::ALIVE) {
        if (now - t.aliveStart >= MIN_ALIVE_MS) {
            dumpAllProduce(t);
            t.pendingShow = true;
            t.pendingFruits = t.kind == Kind::FRUIT ? 5 : (t.kind == Kind::DECOR ? 3 : 6);
            t.phase = Phase::COLLAPSING;
            t.animStart = now;
            t.growth = 1.0f;
        }
        return;
    }
    if (t.phase == Phase::HIDDEN) return;
    uint32_t elapsed = now - t.animStart;
    if (t.phase == Phase::GROWING) {
        t.growth = (float)elapsed / (float)GROW_MS;
        if (t.growth >= 1.0f) {
            t.growth = 1.0f;
            if (t.pendingHide) {
                t.pendingHide = false;
                t.phase = Phase::COLLAPSING;
                t.animStart = now;
            } else {
                t.phase = Phase::ALIVE;
                t.aliveStart = now;
            }
        }
    } else if (t.phase == Phase::COLLAPSING) {
        t.growth = 1.0f - (float)elapsed / (float)COLLAPSE_MS;
        if (t.growth <= 0.0f) {
            t.growth = 0;
            if (t.pendingShow) {
                t.pendingShow = false;
                if (t.kind == Kind::FRUIT)
                    genClassicTree(t, t.pendingFruits, 100);
                else if (t.kind == Kind::DECOR)
                    genClassicTree(t, 3, 100);
                else
                    genClassicTree(t, 6, 70);
                placeInWorld(t, true);  // smashed here → grow off-screen
                t.phase = Phase::GROWING;
                t.animStart = now;
                t.stompHits = 0;
                t.stompArmed = true;
            } else {
                t.phase = Phase::HIDDEN;
            }
        }
    }
}

// ---------- drop / collect / stomp ----------
// Drop one produce item from a slot (fruit or berry). Does not auto-collapse.
// World X is NOT clamped to the screen — drops scroll with Trees::scroll().
static bool dropOneFrom(Slot& t) {
    if (t.fruitCount == 0 || t.phase != Phase::ALIVE) return false;
    uint8_t idx = t.fruitCount - 1;
    const Fruit& f = t.fruits[idx];
    int16_t bx = screenX(t);
    uint32_t now = millis();
    int wave = (int)(now % 3000);
    int8_t sway = (wave < 1500) ? (int8_t)(((wave - 750) * PX) / 750)
                                : (int8_t)(((2250 - wave) * PX) / 750);
    int16_t ax = bx + f.ox + sway;
    // oy relative to trunk base (y=106)
    int16_t dy = 106 + f.oy;
    if (dy < 8) dy = 8;
    if (dy > DROP_GROUND_Y - 6) dy = DROP_GROUND_Y - 6;
    // Keep per-fruit size so fallen apples stay mixed sizes
    uint8_t rr = f.r;
    if (rr < 2) rr = 2;
    if (t.kind == Kind::BERRY && rr > 3) rr = 3;
    // Always detach from tree; pool-full just means no visible drop
    (void)dropsSpawn(ax, dy, rr, f.produce);
    t.fruitCount--;
    return true;
}

void dropFruit() {
    const uint8_t count = configuredCount(Kind::FRUIT);
    const uint8_t start = (uint8_t)random(0, count);
    for (uint8_t i = 0; i < count; i++)
        if (dropOneFrom(fruitSlot((start + i) % count))) return;
}

static void dumpAllProduce(Slot& t) {
    while (t.fruitCount > 0 && t.phase == Phase::ALIVE) {
        if (!dropOneFrom(t)) break;
    }
}

static void forceCollapse(Slot& t) {
    t.pendingHide = false;
    if (t.phase != Phase::HIDDEN && t.phase != Phase::COLLAPSING) {
        t.phase = Phase::COLLAPSING;
        t.animStart = millis();
        t.growth = 1.0f;
    }
    t.stompHits = 0;
}

// tryCollectNearbyFruit — implemented in trees_drops.cpp

uint8_t updateAmbient(int pigCenterX, int pigFeetY, int pigHintX_, bool pigOnRight) {
    setPigHint(pigHintX_, pigOnRight);
    uint32_t now = millis();
    const bool fruitAmbient = Config::personality().fruitTreesAmbient;

    if (fruitAmbient) {
        bool needsFruit = false, needsDecor = false, needsBerry = false;
        for (uint8_t i = 0; i < configuredCount(Kind::FRUIT); i++)
            needsFruit |= fruitSlot(i).phase == Phase::HIDDEN;
        for (uint8_t i = 0; i < configuredCount(Kind::DECOR); i++)
            needsDecor |= decorSlot(i).phase == Phase::HIDDEN;
        for (uint8_t i = 0; i < configuredCount(Kind::BERRY); i++)
            needsBerry |= berrySlot(i).phase == Phase::HIDDEN;
        if (needsFruit) showFruit(5);
        if (needsDecor) showDecor();
        if (needsBerry) showBerry();
    }

    static uint32_t nextAmbientDropMs = 0;
    static uint32_t nextBerryDropMs = 0;
    static uint32_t nextRefillMs = 0;
    if (nextAmbientDropMs == 0) nextAmbientDropMs = now + 4000;
    if (nextBerryDropMs == 0) nextBerryDropMs = now + 3500;
    if (nextRefillMs == 0) nextRefillMs = now + 2200;

    bool hasFruit = false, hasBerry = false;
    for (uint8_t i = 0; i < configuredCount(Kind::FRUIT); i++) {
        hasFruit |= fruitSlot(i).phase == Phase::ALIVE &&
                    fruitSlot(i).fruitCount > 0;
    }
    for (uint8_t i = 0; i < configuredCount(Kind::BERRY); i++) {
        hasBerry |= berrySlot(i).phase == Phase::ALIVE &&
                    berrySlot(i).fruitCount > 0;
    }
    if (hasFruit && now >= nextAmbientDropMs) {
        dropFruit();
        nextAmbientDropMs = now + 1400 + (esp_random() % 2200);
    }
    if (hasBerry && now >= nextBerryDropMs) {
        const uint8_t count = configuredCount(Kind::BERRY);
        const uint8_t start = (uint8_t)random(0, count);
        for (uint8_t i = 0; i < count; i++)
            if (dropOneFrom(berrySlot((start + i) % count))) break;
        nextBerryDropMs = now + 1600 + (esp_random() % 2400);
    }
    if (now >= nextRefillMs) {
        for (uint8_t i = 0; i < configuredCount(Kind::FRUIT); i++)
            refillProduce(fruitSlot(i));
        for (uint8_t i = 0; i < configuredCount(Kind::BERRY); i++)
            refillProduce(berrySlot(i));
        nextRefillMs = now + 2000 + (esp_random() % 800);
    }

    // --- Auto-collect fallen fruit/berries near pig ---
    uint8_t got = 0;
    while (got < 4 &&
           tryCollectNearbyFruit(pigCenterX, pigFeetY, got ? 28 : 24)) {
        got++;
    }
    return got;
}

int16_t getFruitTreeScreenX() {
    const int16_t width = worldWidth();
    const int referenceX = Avatar::getCurrentX();
    int16_t bestX = -1;
    int bestDistance = 0x7FFF;
    for (uint8_t i = 0; i < configuredCount(Kind::FRUIT); i++) {
        const Slot& t = fruitSlot(i);
        if (t.phase == Phase::HIDDEN || t.phase == Phase::COLLAPSING) continue;
        int16_t x = screenX(t);
        if (x - referenceX > width / 2) x = (int16_t)(x - width);
        else if (referenceX - x > width / 2) x = (int16_t)(x + width);
        const int distance = abs((int)x - referenceX);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestX = x;
        }
    }
    return bestX;
}

int16_t nearestFloraScreenX(int fromX) {
    int best = -1;
    int bestD = 9999;
    const int16_t width = worldWidth();
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (slots[i].phase == Phase::HIDDEN || slots[i].phase == Phase::COLLAPSING)
            continue;
        int16_t x = screenX(slots[i]);
        if (x - fromX > width / 2) x = (int16_t)(x - width);
        else if (fromX - x > width / 2) x = (int16_t)(x + width);
        int d = wrapDistWorld(x, (int16_t)fromX);
        if (d < bestD) {
            bestD = d;
            best = x;
        }
    }
    return (int16_t)best;
}

void meteorImpact(int16_t impactX) {
    int bestSlot = -1;
    int bestDistance = 48;
    for (int i = 0; i < SLOT_COUNT; i++) {
        const Slot& t = slots[i];
        if (t.phase == Phase::HIDDEN || t.phase == Phase::COLLAPSING ||
            t.growth <= 0.25f || t.style == SeasonTree::LAMP ||
            t.style == SeasonTree::STALL || t.style == SeasonTree::TRASH)
            continue;
        int distance = wrapDistWorld(screenX(t), impactX);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestSlot = i;
        }
    }
    if (bestSlot >= 0) {
        Slot& t = slots[bestSlot];
        t.meteorDamageStart = millis();
        t.meteorDamageSide = (impactX < screenX(t)) ? 1 : -1;
    }
}

// Stomp any flora within range (jump/attack only). 3 hits → dump produce + collapse.
bool tryStompFruitTree(int pigFeetX, bool airborne) {
    if (!airborne) {
        globalStompArmed = true;
        for (int i = 0; i < SLOT_COUNT; i++) slots[i].stompArmed = true;
        return false;
    }
    if (!globalStompArmed) return false;

    // Nearest hittable trunk/bush
    int best = -1;
    int bestDist = 999;
    for (int i = 0; i < SLOT_COUNT; i++) {
        Slot& t = slots[i];
        if (t.phase != Phase::ALIVE && t.phase != Phase::GROWING) continue;
        int16_t bx = screenX(t);
        int dist = wrapDistWorld((int16_t)pigFeetX, bx);
        int maxR = (t.kind == Kind::BERRY) ? 22 : 30;
        if (dist <= maxR && dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    if (best < 0) return false;

    Slot& t = slots[best];
    globalStompArmed = false;
    t.stompArmed = false;
    t.stompHits++;
    t.stompShake = (t.stompHits & 1) ? 3 : -3;
    t.stompShakeUntil = millis() + 180;

    // Shake loose produce on each hit (fruit + berry)
    if (t.phase == Phase::ALIVE && t.fruitCount > 0) {
        dropOneFrom(t);
        if (t.fruitCount > 0 && t.stompHits >= 2) dropOneFrom(t);
    }
    SFX::play(SFX::ATTACK_HOP);

    if (t.stompHits >= 3) {
        dumpAllProduce(t);
        forceCollapse(t);
        t.pendingShow = true;  // grow again, off-screen, elsewhere on the map
        t.pendingFruits = t.kind == Kind::FRUIT ? 5 : (t.kind == Kind::DECOR ? 3 : 6);
        SFX::play(SFX::OINK_HAPPY);
    }
    return true;
}

bool checkFruitPush(int /*pigLeft*/, int /*pigRight*/, int16_t& treeScreenX) {
    // Trees never push the pig
    treeScreenX = -1;
    return false;
}

void scroll(int dir) {
    const int16_t width = worldWidth();
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (slots[i].phase == Phase::HIDDEN) continue;
        int16_t scroll = (int16_t)(slots[i].scroll + (dir > 0 ? 1 : -1));
        if (scroll >= width) scroll -= width;
        else if (scroll < 0) scroll += width;
        slots[i].scroll = scroll;
    }

    // Critical: fallen fruit must ride the same treadmill as trees/grass.
    // Without this, drops freeze at the ~33% camera rail and the pig can't reach them.
    dropsScroll(dir);
}

void shakeFromWave() {
    waveShake = true;
    waveShakeStart = millis();
}

void setStompShake(int8_t s) {
    int best = -1;
    int bestDist = 999;
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (slots[i].phase != Phase::ALIVE && slots[i].phase != Phase::GROWING) continue;
        const int distance = abs((int)screenX(slots[i]) - pigHintX);
        if (distance < bestDist) {
            bestDist = distance;
            best = i;
        }
    }
    if (best < 0) return;
    slots[best].stompShake = s;
    if (s != 0) slots[best].stompShakeUntil = millis() + 180;
}

static void tickStompShake() {
    const uint32_t now = millis();
    for (int i = 0; i < SLOT_COUNT; i++) {
        Slot& t = slots[i];
        if (t.stompShake != 0 && (int32_t)(now - t.stompShakeUntil) >= 0)
            t.stompShake = 0;
    }
}

// ---------- draw helpers ----------
// Classic 2.5D leaf puff — shade 0/1/2 picks dark / mid / lit greens
static void drawLeafPuff(M5Canvas& canvas, int16_t lx, int16_t ly,
                         uint16_t c0, uint16_t c1, uint16_t c2,
                         uint8_t radius, uint8_t shade) {
    lx = snapPx(lx); ly = snapPx(ly);
    // Rotate palette so neighboring leaves differ
    uint16_t a = c0, b = c1, c = c2;
    if (shade == 1) { a = c1; b = c2; c = c0; }
    else if (shade == 2) { a = c2; b = c0; c = c1; }
    canvas.fillRect(lx - PX, ly - PX, PX * 3, PX * 2, a);
    canvas.fillRect(lx, ly, PX * 2, PX, b);
    canvas.fillRect(lx - PX, ly - 2 * PX, PX, PX, c);
    canvas.fillRect(lx, ly - 2 * PX, PX, PX, b);
    canvas.fillRect(lx + PX, ly, PX, PX, a);
    if (radius >= 5) {
        canvas.fillRect(lx - 2 * PX, ly, PX, PX, a);
        canvas.fillRect(lx + PX, ly - PX, PX, PX, b);
        canvas.fillRect(lx, ly + PX, PX, PX, a);
    }
    if (radius >= 6) {
        canvas.fillRect(lx - PX, ly - 3 * PX, PX * 2, PX, c);
        canvas.fillRect(lx + 2 * PX, ly - PX, PX, PX, a);
    }
}

// Winter fir — stacked needle tiers + thin trunk
static void drawFir(M5Canvas& canvas, Slot& t, int16_t yOffset) {
    uint32_t now = millis();
    const int16_t baseY = (int16_t)(106 + yOffset);
    int16_t bx = screenX(t);
    const uint32_t damageAge = now - t.meteorDamageStart;
    const bool meteorDamaged = t.meteorDamageSide != 0 && damageAge < 18000u;
    const float damageRecovery = meteorDamaged
        ? (float)damageAge / 18000.0f : 1.0f;
    int8_t meteorLean = 0;
    if (meteorDamaged) {
        int leanAmount = damageAge < 1600u
            ? 14 - (int)(damageAge * 6u / 1600u)
            : 8 - (int)((damageAge - 1600u) * 8u / 16400u);
        if (leanAmount < 0) leanAmount = 0;
        int shake = damageAge < 2200u
            ? (((damageAge / 90u) & 1u) ? 3 : -3) *
                (2200u - damageAge) / 2200u
            : 0;
        meteorLean = (int8_t)(t.meteorDamageSide * (leanAmount + shake));
    }
    int8_t sway = 0;
    if (t.phase == Phase::ALIVE) {
        int wave = (int)(now % 3200);
        sway = (wave < 1600) ? 1 : -1;
    }
    sway += t.stompShake;
    bool collapsing = (t.phase == Phase::COLLAPSING);
    float g = t.growth;
    if (collapsing) g = t.growth;
    if (g < 0.05f) return;

    // Thin trunk (brown normal / B&W retro)
    const bool retro = isRetroSeason();
    int16_t trunkH = (int16_t)(t.trunkH * g);
    if (collapsing) {
        float ct = 1.0f - t.growth;
        trunkH -= (int16_t)(ct * ct * t.trunkH);
        if (trunkH < 0) trunkH = 0;
    }
    uint16_t bark = fl(retro ? R_TRUNK2 : C_TRUNK2);
    uint16_t barkLite = fl(retro ? R_TRUNK : C_TRUNK);
    uint16_t firD = fl(retro ? R_FIR3 : C_FIR3);
    uint16_t firM = fl(retro ? R_FIR : C_FIR);
    uint16_t firL = fl(retro ? R_FIR2 : C_FIR2);
    uint16_t snow1 = fl(retro ? R_SNOW : 0xFFFF);
    uint16_t snow2 = fl(retro ? R_SNOW2 : 0xDEFB);
    uint16_t snow3 = fl(retro ? R_LEAF2 : 0xEF7D);
    if (trunkH > 0) {
        for (int16_t row = 0; row < trunkH; row += PX) {
            int16_t lean = (int16_t)(meteorLean * (trunkH - row) /
                                     (trunkH ? trunkH : 1));
            const int16_t h = row + PX > trunkH ? trunkH - row : PX;
            canvas.fillRect(bx - PX + sway + lean, baseY - trunkH + row,
                            PX * 2, h, bark);
            canvas.fillRect(bx + sway + lean, baseY - trunkH + row,
                            PX, h, barkLite);
        }
    }

    // Needle tiers (branch rows)
    float bp = collapsing ? 1.0f : (g < 0.2f ? 0 : (g - 0.2f) / 0.6f);
    if (bp > 1) bp = 1;
    if (bp > 0.05f) {
        uint8_t vis = (uint8_t)(t.branchCount * bp + 0.5f);
        if (vis > t.branchCount) vis = t.branchCount;
        if (meteorDamaged)
            vis = (uint8_t)(vis * (0.55f + 0.45f * damageRecovery));
        for (uint8_t i = 0; i < vis; i++) {
            const Branch& br = t.branches[i];
            int16_t ty = baseY + br.y1;
            if (collapsing) {
                float ct = 1.0f - t.growth;
                ty += (int16_t)(ct * ct * (baseY - ty));
                if (ty > baseY) continue;
            }
            const int16_t tierLean = (int16_t)(meteorLean * (baseY - ty) /
                                               (trunkH ? trunkH : 1));
            const int16_t tierX = bx + sway + tierLean;
            int16_t half = (br.x2 - br.x1) / 2;
            // Filled triangle tiers (fat rows)
            for (int16_t row = 0; row < half / 2 + PX; row += PX) {
                int16_t w = half - row;
                if (w < PX) w = PX;
                uint16_t col = (row == 0) ? firD : (((i + row / PX) & 1) ? firM : firL);
                canvas.fillRect(tierX - w, ty + row, w * 2, PX, col);
            }
            // tip
            canvas.fillRect(tierX - PX, ty - PX, PX * 2, PX, firL);
            // Cap / side patches (snow in winter, film grain white in retro)
            int16_t snowW = half > PX ? half : PX * 2;
            canvas.fillRect(tierX - half / 2, ty - PX, snowW, PX, snow1);
            canvas.fillRect(tierX - half / 3, ty - 2 * PX, half / 2 + PX, PX, snow2);
            if (half > PX * 3) {
                canvas.fillRect(tierX - half + PX, ty + PX, PX * 2, PX, snow1);
                canvas.fillRect(tierX + half - PX * 3, ty + PX, PX * 2, PX, snow2);
            }
            if ((i & 1) == 0 && half > PX * 2) {
                canvas.fillRect(tierX - PX, ty + PX * 2, PX * 2, PX, snow3);
            }
        }
        // Base pile at trunk foot
        if (!collapsing && g > 0.5f) {
            canvas.fillRect(bx - PX * 3 + sway, baseY - PX, PX * 6, PX, snow1);
            canvas.fillRect(bx - PX * 2 + sway, baseY - 2 * PX, PX * 4, PX, snow2);
        }
    }

    // Ornaments
    if (t.kind == Kind::FRUIT && t.fruitCount > 0 && (collapsing || g >= 0.75f)) {
        float fp = collapsing ? 1.0f : (g - 0.75f) / 0.25f;
        if (fp > 1) fp = 1;
        uint8_t vis = (uint8_t)(t.fruitCount * fp + 0.5f);
        for (uint8_t i = 0; i < vis; i++) {
            int16_t fx = bx + t.fruits[i].ox + sway;
            int16_t fy = baseY + t.fruits[i].oy;
            if (collapsing) {
                float ct = 1.0f - t.growth;
                fy += (int16_t)(ct * ct * (baseY - fy));
                if (fy > baseY) continue;
            }
            drawProduce(canvas, fx, fy, 2, Produce::CONE);
        }
    }
}





// DESERT palms — living tree, not a stick:
// thick ringed trunk + seed-varied frond "branches" (rachis + leaflets),
// same spirit as spring/summer/autumn canopies.
// withFruit=true  → banana bunches (fruit tree)
// withFruit=false → coconut cluster at hub (decor)
static void drawBanana(M5Canvas& canvas, int16_t cx, int16_t cy, bool tipLeft) {
    uint16_t yel = fl(0xFFE0);
    uint16_t yelD = fl(0xD5A0);
    uint16_t tip = fl(0x5A20);
    if (tipLeft) {
        canvas.fillRect(cx, cy, PX * 2, PX, yel);
        canvas.fillRect(cx - PX, cy + PX, PX * 2, PX, yel);
        canvas.fillRect(cx - PX * 2, cy + PX * 2, PX * 2, PX, yelD);
        canvas.fillRect(cx - PX * 2, cy + PX * 3, PX, PX, tip);
    } else {
        canvas.fillRect(cx - PX, cy, PX * 2, PX, yel);
        canvas.fillRect(cx, cy + PX, PX * 2, PX, yel);
        canvas.fillRect(cx + PX, cy + PX * 2, PX * 2, PX, yelD);
        canvas.fillRect(cx + PX * 2, cy + PX * 3, PX, PX, tip);
    }
}

static void drawCoconut(M5Canvas& canvas, int16_t cx, int16_t cy) {
    uint16_t nut  = fl(0x82C0);
    uint16_t nutD = fl(0x5140);
    uint16_t nutH = fl(0xC4A0);
    canvas.fillRect(cx - PX, cy - PX, PX * 2, PX * 2, nut);
    canvas.fillRect(cx - PX, cy - PX, PX, PX, nutH);
    canvas.drawPixel(cx + PX - 1, cy + PX - 1, nutD);
}

// One palm frond = solid continuous blade (no dotted gaps).
// Step along the rachis with OVERLAPPING blocks so the leaf reads as one piece.
static void drawPalmFrond(M5Canvas& canvas, int16_t x0, int16_t y0,
                          int8_t dx, int8_t dy, int len,
                          uint16_t rachis, uint16_t leaf, uint16_t leafH, uint16_t leafD,
                          uint8_t seedJit) {
    if (len < 4) len = 4;

    // Normalize step so consecutive segments overlap (kills the "dash" look)
    // Move ~PX/2 per sub-step, draw full PX blocks.
    int steps = len * 2;  // denser sampling
    int16_t prevX = x0, prevY = y0;
    for (int s = 0; s <= steps; s++) {
        float t = (float)s / (float)steps;  // 0..1 along frond
        // mild droop toward tip
        int droop = (int)(t * t * 4.f);
        int jx = ((int)(seedJit + s * 2) % 2);  // tiny variety, not sparse holes
        int jy = ((int)(seedJit + s * 3) % 2);

        int16_t px = (int16_t)(x0 + (dx * s * PX) / 4 + jx);
        int16_t py = (int16_t)(y0 + (dy * s * PX) / 4 + droop + jy);

        // width: thicker near base, taper to tip
        int halfW = (s < steps / 3) ? (PX + 1) : (s < (steps * 2) / 3) ? PX : (PX - 1);
        if (halfW < 2) halfW = 2;

        // perpendicular offset for "blade width" (solid leaf body, not dots)
        int pdx = -dy;  // rough perpendicular
        int pdy = dx;
        if (pdx == 0 && pdy == 0) { pdx = 1; pdy = 0; }

        uint16_t cBody = (s < steps / 4) ? rachis : ((s & 2) ? leaf : leafH);
        if (s > (steps * 3) / 4) cBody = leafH;  // bright tip zone

        // solid core
        canvas.fillRect(px - halfW / 2, py - 1, halfW, PX, cBody);
        // fill gap from previous sample (continuous stroke)
        if (s > 0) {
            int midx = (prevX + px) / 2;
            int midy = (prevY + py) / 2;
            canvas.fillRect(midx - halfW / 2, midy - 1, halfW, PX, cBody);
        }

        // solid side leaflets every other step — filled slabs, not single pixels
        if (s > 1 && (s % 2) == 0) {
            int side = PX + (s < steps / 2 ? 1 : 0);
            int16_t lx = (int16_t)(px - pdx * side / 2);
            int16_t ly = (int16_t)(py - pdy * side / 2);
            int16_t rx = (int16_t)(px + pdx * side / 2);
            int16_t ry = (int16_t)(py + pdy * side / 2);
            uint16_t cL = leaf;
            uint16_t cR = leafD;
            // 2x1 / 1x2 blocks so leaflets look like leaf tissue
            canvas.fillRect(lx - 1, ly, PX, PX - 1, cL);
            canvas.fillRect(rx - 1, ry, PX, PX - 1, cR);
            // soft underside
            canvas.drawFastHLine(lx - 1, ly + PX - 2, PX, leafD);
            canvas.drawFastHLine(rx - 1, ry + PX - 2, PX, rachis);
        }

        prevX = px;
        prevY = py;
    }
    // tip highlight blob
    canvas.fillRect(prevX - 1, prevY - 1, PX, PX, leafH);
}

static void drawPalm(M5Canvas& canvas, Slot& t, int16_t yOffset, bool withFruit) {
    const int16_t baseY = (int16_t)(106 + yOffset);
    int16_t bx = screenX(t);
    float g = t.growth;
    if (g < 0.05f) return;
    int16_t h = (int16_t)(t.trunkH * g);
    if (t.phase == Phase::COLLAPSING) {
        float ct = 1.0f - t.growth;
        h -= (int16_t)(ct * ct * t.trunkH);
        if (h < 0) h = 0;
    }
    if (t.kind == Kind::BERRY) {
        h = (int16_t)(h * 88 / 100);
        if (h < PX * 10) h = PX * 10;
    }
    int16_t x = bx + t.stompShake;

    // Seed-driven variety (like seasonal trees)
    uint32_t s = t.seed ? t.seed : 1u;
    auto rnd = [&]() -> uint32_t {
        s = s * 1664525u + 1013904223u;
        return s;
    };

    uint16_t trunk  = fl(0x9A40);
    uint16_t trunkD = fl(0x6180);
    uint16_t trunkH = fl(0xC4A0);
    uint16_t rachis = fl(0x1A40);
    uint16_t leaf   = fl(0x0B60);
    uint16_t leafM  = fl(0x1CE0);
    uint16_t leafH  = fl(0x3E80);
    uint16_t leafD  = fl(0x0440);
    uint16_t hub    = fl(0x82C0);
    uint16_t hubD   = fl(0x5140);

    // Lean unique per tree
    int16_t lean = (int16_t)((int)(rnd() % 7) - 3);
    if (g < 0.5f) lean = 0;
    x += lean;

    // === Thick ringed trunk (base wider — not a stick) ===
    if (h > 0) {
        for (int y = 0; y < h; y += PX) {
            int16_t yy = baseY - y - PX;
            // base ~5px wide, mid ~4, top ~3 (in PX units)
            int16_t w;
            if (y < h / 4)      w = PX * 5;
            else if (y < h / 2) w = PX * 4;
            else if (y < (h * 3) / 4) w = PX * 3 + 1;
            else                w = PX * 3;
            if ((y / PX) & 1) w += 1; // ring offset
            canvas.fillRect(x - w / 2, yy, w, PX, ((y / PX) & 1) ? trunkD : trunk);
            // highlight edge
            canvas.drawFastVLine(x - w / 2, yy, PX, trunkH);
            if (((y / PX) % 3) == 0)
                canvas.drawFastHLine(x - w / 2, yy, w, trunkH);
        }
        // base flare (roots / thicker foot)
        canvas.fillRect(x - PX * 3, baseY - PX, PX * 6, PX, trunkD);
        canvas.fillRect(x - PX * 2, baseY - PX * 2, PX * 4, PX, trunk);
    }
    int16_t top = baseY - h;

    // Crown hub — chunky attachment point
    canvas.fillRect(x - PX * 3, top - PX, PX * 6, PX * 2, hub);
    canvas.fillRect(x - PX * 2, top - PX * 2, PX * 4, PX, hubD);
    canvas.fillRect(x - PX, top - PX, PX * 2, PX, trunkH);

    if (g < 0.25f) return;

    // === Frond set varies by seed (count, angles, lengths) ===
    // Base directions; we pick a subset + jitter so no two palms match
    struct Dir { int8_t dx, dy; };
    static const Dir BASE[] = {
        {-4, -2}, {-3, -3}, {-1, -4}, { 1, -4}, { 3, -3}, { 4, -2},
        {-5,  0}, { 5,  0}, {-5,  1}, { 5,  1},
        {-4,  2}, {-2,  3}, { 2,  3}, { 4,  2},
        {-3,  1}, { 3,  1}, {-2, -1}, { 2, -1},
    };
    const int NBASE = (int)(sizeof(BASE) / sizeof(BASE[0]));
    int nFronds = 10 + (int)(rnd() % 7); // 10..16 fronds
    if (t.kind == Kind::BERRY) nFronds = 8 + (int)(rnd() % 4);
    if (nFronds > NBASE) nFronds = NBASE;

    // Shuffle-ish pick via seed walk
    bool used[18] = {};
    for (int n = 0; n < nFronds; n++) {
        int idx = (int)(rnd() % NBASE);
        for (int tries = 0; tries < NBASE && used[idx]; tries++)
            idx = (idx + 1) % NBASE;
        used[idx] = true;

        int8_t dx = BASE[idx].dx;
        int8_t dy = BASE[idx].dy;
        // angle jitter
        dx = (int8_t)(dx + (int)(rnd() % 3) - 1);
        dy = (int8_t)(dy + (int)(rnd() % 3) - 1);
        if (dx == 0 && dy == 0) dx = 2;

        int len = 5 + (int)(rnd() % 5); // 5..9 steps
        if (t.kind == Kind::BERRY) len = 4 + (int)(rnd() % 3);
        uint8_t jit = (uint8_t)(rnd() & 0xFF);

        drawPalmFrond(canvas, x, top, dx, dy, len, rachis, leaf, leafH, leafD, jit);
        // secondary shorter frond sometimes (fuller canopy)
        if ((rnd() % 100) < 40 && g > 0.6f) {
            int8_t dx2 = (int8_t)(dx + ((rnd() & 1) ? 1 : -1));
            int8_t dy2 = (int8_t)(dy + 1);
            drawPalmFrond(canvas, x, top + PX, dx2, dy2, len - 2,
                          rachis, leafM, leafH, leafD, (uint8_t)(jit + 17));
        }
    }

    if (g < 0.4f || t.phase == Phase::COLLAPSING) return;

    if (withFruit) {
        int16_t bx0 = x - PX * 5;
        int16_t by0 = top + PX;
        uint8_t n = t.fruitCount ? t.fruitCount : 5;
        for (uint8_t i = 0; i < n && i < 6; i++) {
            int16_t fx = bx0 + (int16_t)((i % 3) * PX * 2);
            int16_t fy = by0 + (int16_t)((i / 3) * PX * 2);
            drawBanana(canvas, fx, fy, (i & 1) == 0);
        }
        // second small bunch on the right sometimes
        if ((t.seed & 2) == 0) {
            drawBanana(canvas, x + PX * 2, top + PX * 2, false);
            drawBanana(canvas, x + PX * 4, top + PX * 3, true);
        }
    } else {
        drawCoconut(canvas, x - PX * 3, top + PX);
        drawCoconut(canvas, x + PX, top + PX);
        drawCoconut(canvas, x - PX, top + PX * 2);
        drawCoconut(canvas, x + PX * 2, top + PX * 2);
        if ((t.seed & 1) == 0)
            drawCoconut(canvas, x, top + PX * 3);
    }
}

// DESERT cactus — bigger pixel saguaro; arm layout RANDOM per seed.
static void drawCactus(M5Canvas& canvas, Slot& t, int16_t yOffset) {
    const int16_t baseY = (int16_t)(106 + yOffset);
    int16_t bx = screenX(t);
    float g = t.growth;
    if (g < 0.05f) return;

    uint32_t rs = t.seed ? t.seed : 1u;
    auto rnd = [&]() -> uint32_t { rs = rs * 1664525u + 1013904223u; return rs; };

    int16_t fullH = (int16_t)(48 + (t.trunkH / 2) + (int)(rnd() % 14)); // bigger
    int16_t h = (int16_t)(fullH * g);
    if (t.phase == Phase::COLLAPSING) {
        float ct = 1.0f - t.growth;
        h -= (int16_t)(ct * ct * fullH);
        if (h < 0) h = 0;
    }
    int16_t x = bx + t.stompShake;

    uint16_t outC  = fl(0x1A40);
    uint16_t darkC = fl(0x2C80);
    uint16_t midC  = fl(0x45A0);
    uint16_t litC  = fl(0x6EC0);
    uint16_t topC  = fl(0x8FE0);
    uint16_t sandD = fl(0xC480);
    uint16_t sandM = fl(0xE5C0);
    uint16_t sandL = fl(0xF6E0);
    uint16_t flower = fl(0xF81F);
    uint16_t flowerY = fl(0xFFE0);

    int16_t W = 12 + (int)(rnd() % 4); // 12..15 — chunkier

    auto stem = [&](int16_t cx, int16_t bottom, int16_t height, int16_t width) {
        if (height < 4 || width < 6) return;
        int16_t left = cx - width / 2;
        canvas.fillRect(left, bottom - height, width, height, midC);
        canvas.fillRect(left, bottom - height, 2, height, darkC);
        canvas.fillRect(left + 2, bottom - height, 1, height, outC);
        canvas.fillRect(left + width - 3, bottom - height, 2, height, litC);
        canvas.fillRect(left + width - 1, bottom - height, 1, height, topC);
        canvas.drawFastVLine(cx, bottom - height + 1, height - 2, darkC);
        // pixel-step cap
        canvas.fillRect(left + 1, bottom - height - 2, width - 2, 2, midC);
        canvas.fillRect(left + 2, bottom - height - 3, width - 4, 1, topC);
        canvas.fillRect(left + 3, bottom - height - 4, width - 6, 1, litC);
        canvas.drawFastVLine(left - 1, bottom - height, height, outC);
        canvas.drawFastVLine(left + width, bottom - height, height, outC);
        // pixel nubs on sides (spines as 1px blocks)
        for (int y = 4; y < height; y += 5 + (int)(y & 1)) {
            canvas.drawPixel(left - 1, bottom - y, litC);
            canvas.drawPixel(left + width, bottom - y - 1, darkC);
        }
    };

    // Sand mound
    {
        int16_t mw = W + 14;
        canvas.fillRect(x - mw / 2, baseY - 1, mw, 2, sandD);
        canvas.fillRect(x - mw / 2 + 2, baseY - 2, mw - 4, 1, sandM);
        canvas.fillRect(x - mw / 2 + 4, baseY - 3, mw - 8, 1, sandL);
        canvas.fillRect(x - mw / 2 + 6, baseY - 4, mw - 12, 1, sandL);
    }

    stem(x, baseY - 1, h, W);

    // Random arms: 1..3, each side/height/length from seed
    if (g > 0.35f) {
        int nArms = 1 + (int)(rnd() % 3); // 1..3 arms
        int16_t aw = W * 2 / 3;
        if (aw < 8) aw = 8;

        for (int a = 0; a < nArms; a++) {
            bool left = (rnd() & 1) != 0;
            // random attach height 30%..70% of trunk
            int pct = 30 + (int)(rnd() % 41);
            int16_t armY = baseY - 1 - h * pct / 100;
            int16_t armUp = 10 + (int)(rnd() % (h / 3 + 6));
            armUp = (int16_t)(armUp * ((g - 0.35f) / 0.65f));
            if (armUp < 8) armUp = 8;
            int out = 6 + (int)(rnd() % 5); // how far arm sticks out
            int thick = 4 + (int)(rnd() % 3);

            if (left) {
                int16_t lx = x - W / 2 - out;
                canvas.fillRect(lx, armY, out + 2, thick, midC);
                canvas.fillRect(lx, armY, out + 2, 2, litC);
                canvas.fillRect(lx, armY + thick - 1, out + 2, 1, darkC);
                // elbow pixel step
                canvas.fillRect(lx, armY - 2, 3, 2, midC);
                stem(lx + 2, armY + 1, armUp, aw);
                if (g > 0.55f && t.phase != Phase::COLLAPSING) {
                    canvas.fillRect(lx, armY + 1 - armUp - 4, 4, 4, flower);
                    canvas.fillRect(lx + 1, armY - armUp - 2, 2, 2, flowerY);
                }
            } else {
                int16_t rx = x + W / 2 - 1;
                canvas.fillRect(rx, armY, out + 2, thick, midC);
                canvas.fillRect(rx, armY, out + 2, 2, litC);
                canvas.fillRect(rx, armY + thick - 1, out + 2, 1, darkC);
                canvas.fillRect(rx + out - 1, armY - 2, 3, 2, midC);
                stem(rx + out - 1, armY + 1, armUp, aw);
                if (g > 0.55f && t.phase != Phase::COLLAPSING) {
                    canvas.fillRect(rx + out - 2, armY + 1 - armUp - 4, 4, 4, flower);
                    canvas.fillRect(rx + out - 1, armY - armUp - 2, 2, 2, flowerY);
                }
            }
        }
        // top flower
        if (g > 0.55f && t.phase != Phase::COLLAPSING) {
            canvas.fillRect(x - 2, baseY - 1 - h - 5, 4, 4, flower);
            canvas.fillRect(x - 1, baseY - h - 4, 2, 2, flowerY);
        }
    }
}

// CITY market stall — produce ON the counter top
static void drawStall(M5Canvas& canvas, Slot& t, int16_t yOffset) {
    uint32_t now = millis();
    const int16_t baseY = (int16_t)(106 + yOffset);
    int16_t bx = screenX(t);
    float g = t.growth;
    if (g < 0.05f) return;
    int16_t h = (int16_t)((48 + t.trunkH / 4) * g);
    if (t.phase == Phase::COLLAPSING) {
        float ct = 1.0f - t.growth;
        h -= (int16_t)(ct * ct * 40);
        if (h < 0) h = 0;
    }
    int16_t x = bx + t.stompShake;
    uint16_t wood = fl(0x9A40);
    uint16_t woodD = fl(0x5140);
    uint16_t awn = fl(0xE8C4);
    uint16_t awn2 = fl(0xC986);
    uint16_t shelf = fl(0xC408);

    int16_t counterTop = baseY - h / 2 - PX * 2; // top of counter board

    // legs
    canvas.fillRect(x - PX * 7, baseY - h / 2, PX * 2, h / 2, woodD);
    canvas.fillRect(x + PX * 5, baseY - h / 2, PX * 2, h / 2, woodD);
    // crates under
    canvas.fillRect(x - PX * 5, baseY - h / 2 + PX, PX * 4, h / 2 - PX * 2, woodD);
    canvas.fillRect(x + PX, baseY - h / 2 + PX, PX * 4, h / 2 - PX * 2, woodD);
    // counter
    canvas.fillRect(x - PX * 8, counterTop, PX * 17, PX * 3, wood);
    canvas.fillRect(x - PX * 8, counterTop, PX * 17, PX, shelf);
    // posts + awning
    int16_t top = baseY - h;
    canvas.fillRect(x - PX * 7, top, PX * 2, counterTop - top, woodD);
    canvas.fillRect(x + PX * 5, top, PX * 2, counterTop - top, woodD);
    canvas.fillRect(x - PX * 9, top, PX * 19, PX * 4, awn);
    for (int i = 0; i < 6; i++)
        canvas.fillRect(x - PX * 9 + i * PX * 3, top, PX * 2, PX * 4, awn2);
    canvas.fillRect(x - PX * 9, top + PX * 3, PX * 19, PX, woodD);

    // Fruit ON counter (in front of posts, on the board)
    if (g > 0.45f && t.phase != Phase::COLLAPSING) {
        for (uint8_t i = 0; i < t.fruitCount; i++) {
            int16_t fx = x + t.fruits[i].ox;
            // sit on counter top, slightly above board
            int16_t fy = counterTop - t.fruits[i].r * 2 - 1;
            int bob = 0; // no bob — stays on shelf
            (void)now; (void)bob;
            drawProduce(canvas, fx, fy, t.fruits[i].r, t.fruits[i].produce);
        }
    }
}

static void drawTrash(M5Canvas& canvas, Slot& t, int16_t yOffset) {
    const int16_t baseY = (int16_t)(106 + yOffset);
    int16_t bx = screenX(t);
    float g = t.growth;
    if (g < 0.05f) return;
    int16_t h = (int16_t)((26 + t.trunkH / 3) * g);
    if (t.phase == Phase::COLLAPSING) {
        float ct = 1.0f - t.growth;
        h -= (int16_t)(ct * ct * 22);
        if (h < 0) h = 0;
    }
    int16_t x = bx + t.stompShake;
    uint16_t body = fl(0x4A69);
    uint16_t bodyD = fl(0x3186);
    uint16_t lid = fl(0x6B6D);
    uint16_t rim = fl(0x9CF3);
    // body
    canvas.fillRect(x - PX * 5, baseY - h, PX * 10, h, body);
    canvas.fillRect(x - PX * 4, baseY - h + PX, PX * 8, h - PX * 2, bodyD);
    // vertical ribs
    canvas.fillRect(x - PX * 2, baseY - h + PX, PX, h - PX * 2, body);
    canvas.fillRect(x + PX, baseY - h + PX, PX, h - PX * 2, body);
    // lid
    canvas.fillRect(x - PX * 6, baseY - h - PX * 2, PX * 12, PX * 2, lid);
    canvas.fillRect(x - PX * 6, baseY - h - PX * 2, PX * 12, PX, rim);
    // handle
    canvas.fillRect(x - PX, baseY - h - PX * 3, PX * 2, PX, rim);
    // wheel dots
    canvas.fillRect(x - PX * 4, baseY - PX, PX * 2, PX, 0x2104);
    canvas.fillRect(x + PX * 2, baseY - PX, PX * 2, PX, 0x2104);
}


static void drawLamp(M5Canvas& canvas, Slot& t, int16_t yOffset) {
    uint32_t now = millis();
    const int16_t baseY = (int16_t)(106 + yOffset);
    int16_t bx = screenX(t);
    bool collapsing = (t.phase == Phase::COLLAPSING);
    float g = t.growth;
    if (collapsing) g = t.growth;
    if (g < 0.05f) return;

    int16_t h = (int16_t)(t.trunkH * g);
    if (collapsing) {
        float ct = 1.0f - t.growth;
        h -= (int16_t)(ct * ct * t.trunkH);
        if (h < 0) h = 0;
    }
    int8_t sway = 0;
    if (t.phase == Phase::ALIVE) sway = ((now / 800) & 1) ? 0 : 0;
    sway += t.stompShake;
    int16_t x = bx + sway;
    uint16_t pole = fl(0x3186);
    uint16_t poleHi = fl(0x5AEB);
    uint16_t head = fl(0x2104);
    uint16_t glow = fl(0xFE60);
    uint16_t glow2 = fl(0xC480);
    if (h > 0) {
        canvas.fillRect(x - PX, baseY - h, PX * 2, h, pole);
        canvas.fillRect(x, baseY - h, PX, h, poleHi);
    }
    int16_t top = baseY - h;
    canvas.fillRect(x, top, PX * 8, PX, pole);
    canvas.fillRect(x + PX * 5, top + PX, PX * 4, PX * 3, head);
    canvas.fillRect(x + PX * 6, top + PX, PX * 2, PX * 2, glow);
    if (!collapsing && g > 0.55f) {
        canvas.fillRect(x + PX * 4, top + PX * 3, PX * 6, PX, glow2);
        canvas.fillRect(x + PX * 2, baseY - PX, PX * 10, PX, fl(0x8200));
        if (((now / 220) & 3) != 0) {
            canvas.fillRect(x + PX * 6, top, PX * 2, PX, fl(0xFFF1));
        }
    }
}

// One drawer for FRUIT / DECOR / BERRY — seasonal trunk/leaves/produce

static void drawTreeSlot(M5Canvas& canvas, Slot& t, int16_t yOffset) {
    if (t.phase == Phase::HIDDEN) return;
    const bool isDecor = (t.kind == Kind::DECOR);
    const bool isBerry = (t.kind == Kind::BERRY);

    // Specialty silhouettes
    if (t.style == SeasonTree::LAMP) {
        drawLamp(canvas, t, yOffset);
        return;
    }
    if (t.style == SeasonTree::STALL) {
        drawStall(canvas, t, yOffset);
        return;
    }
    if (t.style == SeasonTree::TRASH) {
        drawTrash(canvas, t, yOffset);
        return;
    }
    if (t.style == SeasonTree::PALM_FRUIT) {
        drawPalm(canvas, t, yOffset, true);
        return;
    }
    if (t.style == SeasonTree::PALM_DECOR) {
        drawPalm(canvas, t, yOffset, false);
        return;
    }
    if (t.style == SeasonTree::CACTUS) {
        drawCactus(canvas, t, yOffset);
        return;
    }
    if (!isBerry && t.style == SeasonTree::FIR) {
        drawFir(canvas, t, yOffset);
        return;
    }

    // Trunk + leaf palettes by species (RETRO = full B&W film stock)
    uint16_t trunkCol, trunkDark, trunkHi;
    uint16_t leafCol, leafLite, leafHi;
    const Season season = Weather::getActiveSeason();
    const bool retro  = (season == Season::RETRO);
    const bool noir   = (season == Season::NOIR);
    const bool winter = (season == Season::WINTER);
    const bool autumn = (season == Season::AUTUMN);
    const bool spring = (season == Season::SPRING);

    if (retro) {
        trunkCol  = fl(R_TRUNK);
        trunkDark = fl(R_TRUNK2);
        trunkHi   = fl(R_TRUNK_H);
        leafCol   = fl(R_LEAF);
        leafLite  = fl(R_LEAF2);
        leafHi    = fl(R_LEAF3);
    } else if (!isBerry && t.style == SeasonTree::CHERRY) {
        trunkCol  = fl(C_CHERRY_TRUNK);
        trunkDark = fl(C_CHERRY_TRUNK2);
        trunkHi   = fl(C_CHERRY_TRUNK_H);
    } else if (!isBerry && t.style == SeasonTree::WILLOW) {
        trunkCol  = fl(C_WILLOW_TRUNK);
        trunkDark = fl(C_WILLOW_TRUNK2);
        trunkHi   = fl(C_WILLOW_TRUNK_H);
    } else if (!isBerry && t.style == SeasonTree::OLD_APPLE) {
        trunkCol  = fl(0x6A00);
        trunkDark = fl(0x4100);
        trunkHi   = fl(0x9B20);
    } else {
        trunkCol  = fl(C_TRUNK);
        trunkDark = fl(C_TRUNK2);
        trunkHi   = fl(C_TRUNK_H);
    }

    if (!retro) {
        // Leaf palettes — always 3 shades (dark / mid / lit) for volume
        if (isBerry) {
            if (winter) {
                leafCol  = fl(0x6B6D);
                leafLite = fl(0xBDF7);
                leafHi   = fl(0xFFFF);
            } else if (autumn) {
                leafCol  = fl(0x0A20);
                leafLite = fl(0x1C60);
                leafHi   = fl(0x45A0);
            } else if (spring) {
                leafCol  = fl(0x1C40);
                leafLite = fl(C_SPRING);
                leafHi   = fl(C_SPRING3);
            } else {
                leafCol  = fl(0x0A20);
                leafLite = fl(0x1C60);
                leafHi   = fl(0x45A0);
            }
        } else if (t.style == SeasonTree::CHERRY) {
            leafCol  = fl(C_BLOSSOM1);
            leafLite = fl(C_BLOSSOM2);
            leafHi   = fl(C_BLOSSOM3);
        } else if (t.style == SeasonTree::WILLOW) {
            leafCol  = fl(C_WILLOW1);
            leafLite = fl(C_WILLOW2);
            leafHi   = fl(C_WILLOW3);
        } else if (t.style == SeasonTree::OLD_APPLE) {
            leafCol  = fl(C_OAK3);
            leafLite = fl(C_OAK1);
            leafHi   = fl(C_OAK2);
        } else if (isDecor) {
            if (winter) {
                leafCol  = fl(0x0A20);
                leafLite = fl(0x1C40);
                leafHi   = fl(0x9CD3);
            } else {
                leafCol  = fl(0x0A00);
                leafLite = fl(0x1C40);
                leafHi   = fl(0x3C80);
            }
        } else {
            leafCol  = fl(0x0A00);
            leafLite = fl(0x1C20);
            leafHi   = fl(0x3C60);
        }
    }

    if (noir && !retro) {
        trunkCol  = fl(0x2104);
        trunkDark = fl(0x1082);
        trunkHi   = fl(0x4A49);
        leafCol   = fl(0x1124);
        leafLite  = fl(0x1A40);
        leafHi    = fl(isBerry ? 0x7810 : 0x4A00);
    }

    uint32_t now = millis();
    const int16_t baseY = (int16_t)(106 + yOffset);
    int16_t bx = screenX(t);
    const uint32_t damageAge = now - t.meteorDamageStart;
    const bool meteorDamaged = t.meteorDamageSide != 0 && damageAge < 18000u;
    const float damageRecovery = meteorDamaged
        ? (float)damageAge / 18000.0f : 1.0f;
    int8_t meteorLean = 0;
    if (meteorDamaged) {
        int leanAmount = damageAge < 700u
            ? 8 - (int)(damageAge * 4u / 700u)
            : 4 - (int)((damageAge - 700u) * 4u / 17300u);
        if (leanAmount < 0) leanAmount = 0;
        meteorLean = (int8_t)(t.meteorDamageSide * leanAmount);
    }

    int8_t sway = 0;
    if (t.phase == Phase::ALIVE) {
        int wave = (int)(now % 3000);
        sway = (wave < 1500) ? (int8_t)(((wave - 750) * PX) / 750)
                             : (int8_t)(((2250 - wave) * PX) / 750);
        if (isBerry) sway = (sway > 0) ? 1 : (sway < 0 ? -1 : 0);
    }
    sway += t.stompShake;
    if (waveShake) {
        uint32_t e = now - waveShakeStart;
        if (e < 300) {
            int8_t j = ((e / 33) % 2 == 0) ? PX : -PX;
            if (e > 150) j /= 2;
            sway += j;
        } else waveShake = false;
    }

    bool collapsing = (t.phase == Phase::COLLAPSING);
    float collapseT = collapsing ? (1.0f - t.growth) : 0.0f;
    float collapseT2 = collapseT * collapseT;

    // --- Trunk ---
    float trunkProgress = 1.0f;
    if (!collapsing && t.growth < 0.25f) trunkProgress = t.growth / 0.25f;
    if (trunkProgress > 0.0f) {
        int16_t trunkH = (int16_t)((float)t.trunkH * trunkProgress);
        if (collapsing) {
            trunkH -= (int16_t)(collapseT2 * (float)t.trunkH);
            if (trunkH < 0) trunkH = 0;
        }
        if (trunkH > 0) {
            int16_t trunkTop = baseY - trunkH;
            int8_t lean = t.lean + sway + meteorLean;
            for (int16_t row = 0; row < trunkH; row += PX) {
                float tt = (float)row / (float)(trunkH > 1 ? trunkH - 1 : 1);
                int16_t rowLean = snapPx(lean - (int16_t)((float)lean * tt));
                int hw = (int)t.trunkW;
                if (hw < 1) hw = 1;
                hw = 1 + (int)(tt * (float)hw + 0.5f);
                int16_t h = (row + PX > trunkH) ? (trunkH - row) : PX;
                int16_t cx = snapPx(bx + rowLean);
                for (int dx = -hw; dx <= hw; dx++) {
                    uint16_t tc = trunkCol;
                    if (dx == 0) tc = trunkDark;
                    else if (dx == -hw) tc = trunkHi;
                    else if (dx == hw) tc = trunkDark;
                    canvas.fillRect(cx + dx * PX, trunkTop + row, PX, h, tc);
                }
                // Cherry: soft bark rings (not birch dashes)
                if (!isBerry && t.style == SeasonTree::CHERRY && (row % (PX * 4) == 0)) {
                    canvas.fillRect(cx - hw * PX, trunkTop + row, PX, 1, fl(C_CHERRY_TRUNK2));
                }
            }
        }
    }

    // --- Branches + leaves ---
    if (collapsing || t.growth >= 0.25f) {
        float bp = collapsing ? 1.0f : (t.growth - 0.25f) / 0.5f;
        if (bp > 1) bp = 1;

        for (uint8_t i = 0; i < t.branchCount; i++) {
            const Branch& br = t.branches[i];
            int16_t sx = bx + br.x1 + sway +
                         (meteorLean * -br.y1) / (t.trunkH ? t.trunkH : 1);
            int16_t sy = baseY + br.y1;
            int16_t fex = bx + br.x2 + sway +
                          (meteorLean * -br.y2) / (t.trunkH ? t.trunkH : 1);
            int16_t fey = baseY + br.y2;
            if (collapsing) {
                int16_t dg = baseY - fey; if (dg < 0) dg = 0;
                fey += (int16_t)(collapseT2 * dg);
                sy += (int16_t)(collapseT2 * (baseY - sy) * 0.3f);
                if (fey > baseY || sy > baseY) continue;
            }
            int16_t ex = sx + (int16_t)((fex - sx) * bp);
            int16_t ey = sy + (int16_t)((fey - sy) * bp);
            // Cherry twigs slightly darker brown; willow hangers olive
            uint16_t brCol = trunkCol;
            if (t.style == SeasonTree::CHERRY) brCol = fl(C_CHERRY_TRUNK2);
            if (t.style == SeasonTree::WILLOW) brCol = fl(C_WILLOW_TRUNK2);
            fatLine(canvas, sx, sy, ex, ey, brCol);
            if (bp > 0.85f && (!meteorDamaged || (i & 1u) == 0)) {
                drawLeafPuff(canvas, ex, ey, leafCol, leafLite, leafHi, 4, i % 3);
            }
        }

        if (bp > 0.55f) {
            float lp = collapsing ? 1.0f : (bp - 0.55f) / 0.45f;
            if (lp > 1) lp = 1;
            uint8_t visL = (uint8_t)((float)t.leafCount * lp + 0.5f);
            if (visL > t.leafCount) visL = t.leafCount;
            if (meteorDamaged)
                visL = (uint8_t)(visL * (0.55f + 0.45f * damageRecovery));
            for (uint8_t i = 0; i < visL; i++) {
                const Leaf& L = t.leaves[i];
                int16_t lx = bx + L.cx + sway +
                             (meteorLean * -L.cy) / (t.trunkH ? t.trunkH : 1);
                int16_t ly = baseY + L.cy;
                if (collapsing) {
                    int16_t dg = baseY - ly; if (dg < 0) dg = 0;
                    ly += (int16_t)(collapseT2 * dg);
                    if (ly > baseY) continue;
                }
                // Oak / cherry: fuller puffs so canopy reads
                uint8_t rr = L.radius;
                if (t.style == SeasonTree::OLD_APPLE && rr < 6) rr = 6;
                if (t.style == SeasonTree::CHERRY && rr < 5) rr = 5;
                // Mix a few soft-green leaf accents under pink blossoms
                uint16_t lc = leafCol, ll = leafLite, lh = leafHi;
                if (t.style == SeasonTree::CHERRY && (L.shade == 2) && (i & 3) == 0) {
                    lc = fl(0x1C40);
                    ll = fl(C_SPRING);
                    lh = fl(C_SPRING3);
                }
                drawLeafPuff(canvas, lx, ly, lc, ll, lh, rr, L.shade);
                // Willow: tiny yellow catkins hanging off a few leaves
                if (t.style == SeasonTree::WILLOW && t.phase == Phase::ALIVE && (i & 3) == 0) {
                    canvas.fillRect(snapPx(lx), snapPx(ly) + PX, PX, PX * 2, fl(C_CATKIN));
                    canvas.fillRect(snapPx(lx) - PX, snapPx(ly) + PX * 2, PX, PX, fl(0xC600));
                }
                // Winter snow blobs on foliage (all non-fir trees + bush)
                if (winter && t.phase == Phase::ALIVE && (L.shade == 0 || (i & 1))) {
                    canvas.fillRect(snapPx(lx) - PX, snapPx(ly) - 2 * PX, PX * 2, PX, fl(0xFFFF));
                    canvas.fillRect(snapPx(lx), snapPx(ly) - 3 * PX, PX, PX, fl(0xDEFB));
                }
            }
        }

        // Produce
        if (!isDecor && t.fruitCount > 0 && (collapsing || t.growth >= 0.75f)) {
            float fp = collapsing ? 1.0f : (t.growth - 0.75f) / 0.25f;
            if (fp > 1) fp = 1;
            uint8_t vis = (uint8_t)(t.fruitCount * fp + 0.5f);
            if (vis > t.fruitCount) vis = t.fruitCount;
            for (uint8_t i = 0; i < vis; i++) {
                int16_t fx = bx + t.fruits[i].ox + sway +
                             (meteorLean * -t.fruits[i].oy) /
                                 (t.trunkH ? t.trunkH : 1);
                int16_t fy = baseY + t.fruits[i].oy;
                if (collapsing) {
                    int16_t dg = baseY - fy; if (dg < 0) dg = 0;
                    fy += (int16_t)(collapseT2 * dg * 1.5f);
                    if (fy > baseY) continue;
                }
                if (t.phase == Phase::ALIVE) {
                    uint32_t ph = now + t.fruits[i].bob * 8u;
                    if ((ph % 2000) >= 1000) fy += PX;
                }
                drawProduce(canvas, fx, fy, t.fruits[i].r, t.fruits[i].produce);
            }
        }
    }
}

static void drawWrappedTreeSlot(M5Canvas& canvas, Slot& t, int16_t yOffset) {
    const int16_t x = screenX(t);
    drawWorldShift = 0;
    drawTreeSlot(canvas, t, yOffset);
    int16_t margin = (int16_t)(t.crownR + 12);
    if (margin < 48) margin = 48;
    if (x < margin) {
        drawWorldShift = worldWidth();
        drawTreeSlot(canvas, t, yOffset);
    }
    if (x > worldWidth() - margin) {
        drawWorldShift = (int16_t)-worldWidth();
        drawTreeSlot(canvas, t, yOffset);
    }
    drawWorldShift = 0;
}

void draw(M5Canvas& canvas, int16_t yOffset) {
    // Season change → restyle FRUIT + DECOR + BERRY (keep positions/scroll)
    static Season lastSeason = Season::SUMMER;
    static bool seasonInited = false;
    Season nowSeason = Weather::getActiveSeason();
    if (!seasonInited) {
        lastSeason = nowSeason;
        seasonInited = true;
    } else if (nowSeason != lastSeason && yOffset == 0) {
        lastSeason = nowSeason;
        for (int i = 0; i < SLOT_COUNT; i++) {
            Slot& t = slots[i];
            if (t.phase == Phase::HIDDEN) continue;
            int16_t keepX = t.baseX;
            int16_t keepScroll = t.scroll;
            Phase keepPhase = t.phase;
            float keepG = t.growth;
            uint8_t keepFruits = 5;
            if (t.kind == Kind::FRUIT) keepFruits = t.fruitCount > 0 ? t.fruitCount : 5;
            else if (t.kind == Kind::BERRY) keepFruits = 6;
            else keepFruits = 3;
            if (t.kind == Kind::FRUIT) genClassicTree(t, keepFruits, 100);
            else if (t.kind == Kind::DECOR) genClassicTree(t, 3, 100);
            else genClassicTree(t, 6, 70);
            t.baseX = keepX;
            t.scroll = keepScroll;
            t.phase = keepPhase;
            t.growth = keepG;
            if (keepPhase == Phase::ALIVE) t.aliveStart = millis();
        }
    }

    for (int i = 0; i < SLOT_COUNT; i++) updateSlot(slots[i]);
    tickStompShake();

    // Decor first (behind), then berry bush, then fruit (gameplay in front)
    for (uint8_t i = 0; i < INSTANCES_PER_KIND; i++) {
        if (yOffset == 0 || decorSlot(i).phase != Phase::HIDDEN)
            drawWrappedTreeSlot(canvas, decorSlot(i), yOffset);
    }
    for (uint8_t i = 0; i < INSTANCES_PER_KIND; i++) {
        if (yOffset == 0 || berrySlot(i).phase != Phase::HIDDEN)
            drawWrappedTreeSlot(canvas, berrySlot(i), yOffset);
    }
    for (uint8_t i = 0; i < INSTANCES_PER_KIND; i++) {
        if (yOffset == 0 || fruitSlot(i).phase != Phase::HIDDEN)
            drawWrappedTreeSlot(canvas, fruitSlot(i), yOffset);
    }

    // Falling produce: Trees::drawDropsForeground (trees_drops.cpp) after front grass.
    if (yOffset != 0) return;
}

// drawDropsForeground — implemented in trees_drops.cpp

void drawBarOverflow(M5Canvas& bar) {
    draw(bar, (int16_t)TOP_BAR_H);
}

void drawUnderground(M5Canvas& canvas, uint16_t rootColor,
                     uint16_t rootHighlight) {
    const Season season = Weather::getActiveSeason();
    if (season == Season::CITY) return;

    const int16_t width = worldWidth();
    for (int pass = -1; pass <= 1; pass++) {
      drawWorldShift = (int16_t)(pass * width);
      for (uint8_t i = 0; i < SLOT_COUNT; i++) {
        const Slot& t = slots[i];
        if (t.phase == Phase::HIDDEN || t.growth <= 0.01f) continue;
        if (t.style == SeasonTree::LAMP || t.style == SeasonTree::STALL ||
            t.style == SeasonTree::TRASH)
            continue;

        const int16_t x = screenX(t);
        if (x < -12 || x > DISPLAY_W + 12) continue;

        uint16_t bark = rootColor;
        uint16_t barkDark = rootHighlight;
        uint16_t barkLight = rootColor;
        if (season == Season::RETRO) {
            bark = R_TRUNK;
            barkDark = R_TRUNK2;
            barkLight = R_TRUNK_H;
        } else if (season == Season::NOIR) {
            bark = 0x4208;
            barkDark = 0x2104;
            barkLight = 0x630C;
        } else if (season == Season::DESERT) {
            if (t.style == SeasonTree::CACTUS) {
                bark = 0x2C80;
                barkDark = 0x1A40;
                barkLight = 0x45A0;
            } else {
                bark = 0x9A40;
                barkDark = 0x6180;
                barkLight = 0xC4A0;
            }
        } else if (season == Season::SPRING &&
                   t.style == SeasonTree::CHERRY) {
            bark = C_CHERRY_TRUNK;
            barkDark = C_CHERRY_TRUNK2;
            barkLight = C_CHERRY_TRUNK_H;
        } else if (season == Season::SPRING &&
                   t.style == SeasonTree::WILLOW) {
            bark = C_WILLOW_TRUNK;
            barkDark = C_WILLOW_TRUNK2;
            barkLight = C_WILLOW_TRUNK_H;
        } else if (season == Season::AUTUMN &&
                   t.style == SeasonTree::OLD_APPLE) {
            bark = 0x6A00;
            barkDark = 0x4100;
            barkLight = 0x9B20;
        } else if (season == Season::WINTER) {
            bark = C_TRUNK;
            barkDark = C_TRUNK2;
            barkLight = C_TRUNK_H;
        } else {
            bark = C_TRUNK;
            barkDark = C_TRUNK2;
            barkLight = C_TRUNK_H;
        }

        const float growth = t.growth > 1.0f ? 1.0f : t.growth;
        const int maxDepth = t.kind == Kind::BERRY ? 10 : 14;
        const int depth = (int)(maxDepth * growth);
        if (depth < 4) continue;

        auto rootStroke = [&canvas](int x1, int y1, int x2, int y2,
                                    uint16_t color, uint8_t thickness) {
            for (int offset = 0; offset < thickness; offset++) {
                const int centeredOffset = offset - (int)(thickness / 2);
                canvas.drawLine(x1 + centeredOffset, y1,
                                x2 + centeredOffset, y2, color);
            }
        };

        auto root = [&rootStroke, barkDark, bark](int x1, int y1, int x2, int y2,
                                                  uint8_t thickness) {
            rootStroke(x1, y1, x2, y2, barkDark, thickness + 1);
            rootStroke(x1, y1, x2, y2, bark, thickness);
        };

        uint32_t seed = t.seed ^ ((uint32_t)i * 0x9E3779B9u);
        if (seed == 0) seed = (uint32_t)i + 1u;
        const bool shrub = t.kind == Kind::BERRY;
        const int rootCount = shrub ? 6 + (int)(lcg(seed) % 3u)
                                    : 7 + (int)(lcg(seed) % 4u);
        const int maxSpread = shrub ? 12 : 21;
        const int trunkEdge = ((int)t.trunkW + 1) * PX;

        // The underground collar follows the visible flare at the trunk base.
        canvas.fillRect(x - trunkEdge, 0, trunkEdge * 2 + 1, 2, barkDark);
        canvas.fillRect(x - trunkEdge + 2, 0, trunkEdge * 2 - 3, 1, bark);
        canvas.drawFastVLine(x - trunkEdge + 2, 0, 2, barkLight);

        struct RootTip { int x, y; };
        RootTip tips[10];
        int tipCount = 0;

        // Alternate edge roots with inner roots so the whole trunk base is connected.
        for (int r = 0; r < rootCount; r++) {
            const int side = (r & 1) ? 1 : -1;
            const int order = r / 2;
            const bool fromInnerBase = (r % 4 == 1 || r % 4 == 2);
            const int maxInset = trunkEdge < 4 ? trunkEdge - 1 : 3;
            const int inset = (int)(lcg(seed) % (uint32_t)(maxInset + 1));
            const int startOffset = fromInnerBase
                ? 1 + (int)(lcg(seed) % (uint32_t)(trunkEdge / 2 + 1))
                : trunkEdge - inset;
            const int startX = x + side * startOffset;
            const int startY = (int)(lcg(seed) % 3u);
            int reach = 5 + (int)(lcg(seed) % (uint32_t)(maxSpread - 4));
            reach -= order * (shrub ? 2 : 3);
            const int minReach = trunkEdge - 3 + (shrub ? 3 : 5);
            if (reach < minReach) reach = minReach;
            const int endY = 3 + (int)(lcg(seed) % (uint32_t)(depth - 2));
            const int endX = x + side * reach;
            int bendX = startX + (endX - startX) / 2;
            bendX += (int)(lcg(seed) % 5u) - 2;
            int bendY = startY + (int)(lcg(seed) % 4u);
            if (bendY >= endY) bendY = endY - 1;

            const uint8_t thickness = shrub ? (r < 2 ? 2 : 1)
                                            : (r < 3 ? 2 : 1);
            root(startX, startY, bendX, bendY, thickness);
            root(bendX, bendY, endX, endY, 1);

            if (tipCount < (int)(sizeof(tips) / sizeof(tips[0]))) {
                tips[tipCount].x = endX;
                tips[tipCount].y = endY;
                tipCount++;
            }

            // Smaller lateral roots branch off at varied points along each root.
            const int branches = shrub ? 1 + (int)(lcg(seed) % 2u)
                                       : 1 + (int)(lcg(seed) % 3u);
            for (int b = 0; b < branches; b++) {
                const int branchSide = (lcg(seed) & 1u) ? side : -side;
                const int branchX = startX + (endX - startX) *
                    (25 + (int)(lcg(seed) % 50u)) / 100;
                const int branchY = startY + (endY - startY) *
                    (25 + (int)(lcg(seed) % 50u)) / 100;
                const int branchLen = shrub ? 2 + (int)(lcg(seed) % 5u)
                                            : 3 + (int)(lcg(seed) % 8u);
                const int branchEndX = branchX + branchSide * branchLen;
                int branchEndY = branchY + 1 + (int)(lcg(seed) % 4u);
                if (branchEndY > depth) branchEndY = depth;
                root(branchX, branchY, branchEndX, branchEndY, 1);

                if (depth > 6 && (lcg(seed) & 1u)) {
                    const int hairX = branchX + (branchEndX - branchX) * 2 / 3;
                    const int hairY = branchY + (branchEndY - branchY) * 2 / 3;
                    const int hairSide = (lcg(seed) & 1u) ? 1 : -1;
                    root(hairX, hairY,
                         hairX + hairSide * (1 + (int)(lcg(seed) % 4u)),
                         hairY + 1, 1);
                }
            }
        }

        // Fine feeder roots make the bush root mass denser and the tree roots spread.
        const int rootlets = shrub ? 7 : 5;
        for (int r = 0; r < rootlets && tipCount > 0; r++) {
            const RootTip& tip = tips[lcg(seed) % (uint32_t)tipCount];
            const int side = (lcg(seed) & 1u) ? 1 : -1;
            const int len = 2 + (int)(lcg(seed) % (shrub ? 4u : 7u));
            int endY = tip.y + (int)(lcg(seed) % 3u);
            if (endY > depth) endY = depth;
            root(tip.x, tip.y, tip.x + side * len, endY, 1);
        }
      }
    }
    drawWorldShift = 0;
}

}  // namespace Trees
