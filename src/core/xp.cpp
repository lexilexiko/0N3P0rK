#include "xp.h"
#include "config.h"
#include "../ui/display.h"
#include "../piglet/credits.h"
#include "../piglet/avatar.h"
#include "../piglet/mood.h"
#include "../audio/sfx.h"
#include <Preferences.h>
#include <Arduino.h>
#include <stdio.h>

static constexpr uint8_t XP_MAX_LEVEL = 50;

static uint32_t s_xp = 0;
static bool s_all = false;
static Preferences s_prefs;
static bool s_ready = false;
static bool s_dirty = false;
static uint32_t s_lastPersist = 0;
static bool s_allOnDisk = false;

// Early soft ramp (lv 1..7). From lv 8 onward each level costs a flat amount:
//   same XP as old climb 1→8 (9000) + 1000.
// No more +2500 escalation after 10 — late levels stay playable.
static constexpr uint32_t XP_FLAT_LATE = 9000u + 1000u;  // 10000

static uint32_t xpNeed(uint8_t fromLevel) {
    if (fromLevel < 1) fromLevel = 1;
    switch (fromLevel) {
        case 1: return 100;
        case 2: return 250;
        case 3: return 500;
        case 4: return 1000;
        case 5: return 2500;
        case 6: return 5000;
        case 7: return 7500;           // last step of the 1→8 climb
        default:
            return XP_FLAT_LATE;       // every further level ≈ 1→8 + 1000
    }
}

static uint32_t xpToReach(uint8_t level) {
    if (level <= 1) return 0;
    if (level > XP_MAX_LEVEL) level = XP_MAX_LEVEL;
    uint32_t t = 0;
    for (uint8_t l = 1; l < level; l++) t += xpNeed(l);
    return t;
}

// Preferences::put* commits NVS flash on every call — never do that per apple.
static void persist(bool force) {
    if (!s_ready) return;
    if (!force && !s_dirty) return;
    s_prefs.putUInt("xp", s_xp);
    if (s_all != s_allOnDisk) {
        s_prefs.putBool("all", s_all);
        s_allOnDisk = s_all;
    }
    s_dirty = false;
    s_lastPersist = millis();
}

uint8_t XP::getLevel() {
    uint32_t remain = s_xp;
    uint8_t lvl = 1;
    while (lvl < XP_MAX_LEVEL) {
        uint32_t need = xpNeed(lvl);
        if (remain < need) break;
        remain -= need;
        lvl++;
    }
    return lvl;
}

uint32_t XP::getXP() { return s_xp; }

uint32_t XP::needForNext() {
    uint8_t lvl = getLevel();
    if (lvl >= XP_MAX_LEVEL) return xpNeed((uint8_t)(XP_MAX_LEVEL - 1));
    return xpNeed(lvl);
}

uint32_t XP::intoLevel() {
    if (getLevel() >= XP_MAX_LEVEL) return needForNext();
    uint32_t remain = s_xp;
    uint8_t lvl = 1;
    while (lvl < XP_MAX_LEVEL) {
        uint32_t need = xpNeed(lvl);
        if (remain < need) return remain;
        remain -= need;
        lvl++;
    }
    return 0;
}

bool XP::blushUnlocked() { return s_all || getLevel() >= 5; }
bool XP::goldAppleUnlocked() { return s_all || getLevel() >= 10; }
bool XP::retroUnlocked() { return s_all || getLevel() >= 15; }
bool XP::noirUnlocked() { return s_all || getLevel() >= 18; }
bool XP::cityUnlocked() { return s_all || getLevel() >= 25; }
bool XP::desertUnlocked() { return s_all || getLevel() >= 30; }
bool XP::allUnlocked() { return s_all; }

void XP::unlockAll() {
    if (!s_ready) begin();
    s_all = true;
    s_xp = xpToReach(XP_MAX_LEVEL);
    Config::personality().zombieSkinUnlocked = true;
    Config::save();
    s_dirty = true;
    persist(true);
}

bool XP::isSkinLocked(uint8_t skin) {
    if (s_all) return false;
    if (skin == (uint8_t)PigSkin::ZOMBIE)
        return !Config::isZombieSkinUnlocked();
    if (skin == (uint8_t)PigSkin::BLUSH)  return getLevel() < 5;
    if (skin == (uint8_t)PigSkin::SHADOW) return getLevel() < 8;
    if (skin == (uint8_t)PigSkin::CANDY)  return getLevel() < 12;
    if (skin == (uint8_t)PigSkin::RETRO)  return getLevel() < 15;
    if (skin == (uint8_t)PigSkin::GOLD)   return getLevel() < 20;
    if (skin == (uint8_t)PigSkin::DIRTY)  return getLevel() < 25;
    return false;
}

bool XP::isSeasonLocked(uint8_t seasonMode) {
    if (s_all) return false;
    if (seasonMode == (uint8_t)SeasonMode::RETRO) return getLevel() < 15;
    if (seasonMode == (uint8_t)SeasonMode::NOIR)  return getLevel() < 18;
    if (seasonMode == (uint8_t)SeasonMode::CITY)  return getLevel() < 25;
    if (seasonMode == (uint8_t)SeasonMode::DESERT) return getLevel() < 30;
    return false;
}

static void celebrate(uint8_t newLvl) {
    SFX::play(SFX::LEVEL_UP);
    // Spin cancels an in-air jump — skip it so a level-up mid-hop does not yank her.
    if (!Avatar::isJumping())
        Avatar::spin();
    Avatar::triggerSparkles(8);
    Avatar::triggerTailWiggle();
    Mood::setStatusMessage("LVL UP");
    Display::showToast("LVL UP", 1400);
    if (newLvl == 5)
        Display::notify(NoticeKind::REWARD, "SECRET BLUSH", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 8)
        Display::notify(NoticeKind::REWARD, "SECRET SHADOW", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 10)
        Display::notify(NoticeKind::REWARD, "GOLD APPLES", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 12)
        Display::notify(NoticeKind::REWARD, "SECRET CANDY", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 15)
        Display::notify(NoticeKind::REWARD, "SECRET RETRO", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 18)
        Display::notify(NoticeKind::REWARD, "SECRET NOIR", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 20)
        Display::notify(NoticeKind::REWARD, "SECRET GOLD", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 25)
        Display::notify(NoticeKind::REWARD, "SECRET CITY", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 30)
        Display::notify(NoticeKind::REWARD, "SECRET DESERT", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 35)
        Display::notify(NoticeKind::REWARD, "SECRET PROPS", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 40)
        Display::notify(NoticeKind::REWARD, "SECRET FRIEND", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 45)
        Display::notify(NoticeKind::REWARD, "SECRET TABLE", 3200, NoticeChannel::TOP_BAR);
    if (newLvl == 50) {
        Display::notify(NoticeKind::REWARD, "MAX LEVEL", 3200, NoticeChannel::TOP_BAR);
        Credits::start();
    }
}

void XP::begin() {
    if (s_ready) return;
    s_prefs.begin("pigxp", false);
    s_xp = s_prefs.getUInt("xp", 0);
    s_all = s_prefs.getBool("all", false);
    s_allOnDisk = s_all;
    bool bumped = false;
    auto bump = [&](uint32_t need) {
        if (s_xp < need) {
            s_xp = need;
            bumped = true;
        }
    };
    uint8_t sk = Config::personality().pigSkin;
    uint8_t sm = Config::personality().seasonMode;
    if (sk == (uint8_t)PigSkin::BLUSH)  bump(xpToReach(5));
    if (sk == (uint8_t)PigSkin::SHADOW) bump(xpToReach(8));
    if (sk == (uint8_t)PigSkin::CANDY)  bump(xpToReach(12));
    if (sk == (uint8_t)PigSkin::RETRO || sm == (uint8_t)SeasonMode::RETRO) bump(xpToReach(15));
    if (sm == (uint8_t)SeasonMode::NOIR) bump(xpToReach(18));
    if (sk == (uint8_t)PigSkin::GOLD)   bump(xpToReach(20));
    if (sk == (uint8_t)PigSkin::DIRTY || sm == (uint8_t)SeasonMode::CITY) bump(xpToReach(25));
    if (sm == (uint8_t)SeasonMode::DESERT) bump(xpToReach(30));
    s_ready = true;
    if (bumped) {
        s_dirty = true;
        persist(true);
    }
}

void XP::tick() {
    if (!s_dirty) return;
    if ((uint32_t)(millis() - s_lastPersist) < 2000) return;
    persist(true);
}

void XP::addXP(XPEvent event) {
    if (event == XPEvent::PET || event == XPEvent::FEED) {
        static uint32_t lastCare = 0;
        uint32_t now = millis();
        if (now - lastCare < 7000) return;
        lastCare = now;
    }
    uint16_t n = 6;
    switch (event) {
        case XPEvent::FRUIT_PICKED:  n = 8;  break;
        case XPEvent::GOLD_APPLE:    n = 28; break;
        case XPEvent::WOLF_SCARE:    n = 5;  break;
        case XPEvent::WOLF_HIDE:     n = 14; break;
        case XPEvent::PIGPASS_CRACK: n = 80; break;
        case XPEvent::EVILPIG_CATCH: n = 40; break;
        case XPEvent::HANDSHAKE:     n = 35; break;
        case XPEvent::PET:           n = 6;  break;
        case XPEvent::FEED:          n = 5;  break;
        case XPEvent::NIGHT_SURVIVE: n = 18; break;
    }
    addXP(n);
}

void XP::addXP(uint16_t amount) {
    if (amount == 0) return;
    if (!s_ready) begin();
    uint8_t before = getLevel();
    uint32_t cap = xpToReach(XP_MAX_LEVEL);
    if (s_xp >= cap) return;
    uint32_t next = s_xp + amount;
    if (next > cap) next = cap;
    s_xp = next;
    s_dirty = true;
    uint8_t after = getLevel();
    if (after > before) {
        persist(true);  // keep the new level if power drops
        celebrate(after);
    }
}
