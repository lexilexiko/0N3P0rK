#include "settings_menu.h"
#include "display.h"
#include "keys.h"
#include "../core/config.h"
#include "../core/xp.h"
#include "../piglet/props.h"
#include "../core/app.h"
#include "../piglet/scene_layers.h"
#include "../piglet/wolf.h"
#include "../piglet/mood.h"
#include "../storage/littlefs_ops.h"
#include "../audio/sfx.h"
#include "../board/led.h"
#include "../net/ap_sta.h"
#include "../cap/sniffer.h"
#include "../cap/methods/method_ctx.h"
#include "../cap/packs/pack_ctx.h"
#include "../board/board.h"
#include "../build_info.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <string.h>
#include <stdio.h>

namespace SettingsMenu {

enum class Kind : uint8_t { TOGGLE, VALUE, TEXT, BIND, ACTION };
enum class ConnPhase : uint8_t { LIST = 0, PASS = 1 };

struct Item {
    const char* label;
    Kind kind;
    uint8_t id;
    int minV;
    int maxV;
    int step;
};

static const Item SCENE[] = {
    {"NAME",      Kind::TEXT,   0,  0, 0, 0},
    {"SKIN",      Kind::VALUE,  1,  0, PIG_SKIN_COUNT - 1, 1},
    {"SEASON",    Kind::VALUE,  2,  0, SEASON_MODE_COUNT - 1, 1},
    {"SKY",       Kind::VALUE,  3,  0, SKY_MODE_COUNT - 1, 1},
    {"SCROLL",    Kind::VALUE,  4,  1, 10, 1},
    {"LIFE",      Kind::TOGGLE, 5,  0, 1, 1},
    {"ALL LAYERS",Kind::TOGGLE, 6,  0, 1, 1},
    {"WOLF",      Kind::TOGGLE, 7,  0, 1, 1},
    {"PROPS",     Kind::TOGGLE, 18, 0, 1, 1},
    {"FRIEND",    Kind::TOGGLE, 19, 0, 1, 1},
    {"CARDS",     Kind::TOGGLE, 20, 0, 1, 1},
    {"WOLF EAT",  Kind::TOGGLE, 15, 0, 1, 1},
    {"TREES",     Kind::TOGGLE, 8,  0, 1, 1},
    {"WEATHER",   Kind::TOGGLE, 9,  0, 1, 1},
    {"GRASS",     Kind::TOGGLE, 10, 0, 1, 1},
    {"SHOW PIG",  Kind::TOGGLE, 11, 0, 1, 1},
    {"SEASON FX", Kind::TOGGLE, 12, 0, 1, 1},
    {"MOOD",      Kind::TOGGLE, 13, 0, 1, 1},
    {"TALK SEC",  Kind::VALUE,  17, 2, 10, 1},
    {"ANIM TEST", Kind::TOGGLE, 14, 0, 1, 1},
    {"CODE",      Kind::TEXT,   16, 0, 0, 0},
};
static const uint8_t SCENE_N = sizeof(SCENE) / sizeof(SCENE[0]);

static const Item SYSTEM[] = {
    {"BRIGHT",    Kind::VALUE, 0, 10, 100, 10},
    {"SOUND",     Kind::VALUE, 1, 0, 5, 1},
    {"DIM AFTER", Kind::VALUE, 2, 0, 300, 10},
    {"DIM LEVEL", Kind::VALUE, 3, 0, 50, 5},
    {"LED",       Kind::TOGGLE, 4, 0, 1, 1},
    {"LED BRIGHT",Kind::VALUE, 5, 0, 100, 5},
};
static const uint8_t SYSTEM_N = sizeof(SYSTEM) / sizeof(SYSTEM[0]);

static const Item RADIO[] = {
    {"PACK",      Kind::VALUE,  18, 0, 0, 1}, // max resolved at runtime below
    {"HS METHOD", Kind::VALUE,  7,  0, 0, 1}, // max resolved at runtime below
    {"RESET",     Kind::ACTION, 19, 0, 0, 0}, // stock radio — next to method
    {"FALLBACK",  Kind::VALUE,  8,  10, 90, 5},
    {"KICK N",    Kind::VALUE,  9,  1, 6, 1},
    {"BIDIR",     Kind::TOGGLE, 10, 0, 1, 1},
    {"EAPOL TX",  Kind::TOGGLE, 11, 0, 1, 1},
    {"PMKID",     Kind::TOGGLE, 12, 0, 1, 1},
    {"CSA",       Kind::TOGGLE, 13, 0, 1, 1},
    {"AUTH FLOOD",Kind::TOGGLE, 14, 0, 1, 1},
    {"REASON",    Kind::VALUE,  15, 1, 8, 1},
    {"PAUSE MS",  Kind::VALUE,  16, 400, 3000, 200},
    {"FAT PCAP",  Kind::TOGGLE, 17, 0, 1, 1},
    // Porkchop-style knobs. ID 20+ keeps them out of the way of the
    // legacy IDs already on disk; legacy fields stay exactly the same
    // bytes for backwards compatibility with saved NVS configs.
    {"JITTER MS", Kind::VALUE,  20, 0, 20, 1},     // random ms between mgmt frames
    {"COOLDOWN",  Kind::VALUE,  21, 0, 30, 1},     // seconds per-AP after kick
    {"SCORE THR", Kind::VALUE,  22, -100, 200, 10}, // PORKCHOP method: min score to attack
    {"DWL MIN",   Kind::VALUE,  23, 50, 600, 10},  // min channel dwell (PASSIVE-style)
    {"HS DEPTH",  Kind::VALUE,  24, 0, 2, 1},      // 0=PAIR 1=+M3 2=FULL
    {"DATA ACT",  Kind::TOGGLE, 25, 0, 1, 1},      // data-frame activity for FOCUS score
    {"STRICT LK", Kind::TOGGLE, 26, 0, 1, 1},      // ignore score while lock-on-BSSID
    {"DEPTH HOLD",Kind::VALUE,  27, 0, 30, 1},     // extra sec hold after pair (hsDepth>0)
    {"HOP MS",    Kind::VALUE,  0,  50, 2000, 50},
    {"LOCK MS",   Kind::VALUE,  1,  0, 15000, 500},
    {"LOCK HS",   Kind::TOGGLE, 2,  0, 1, 1},
    {"DEAUTH",    Kind::TOGGLE, 3,  0, 1, 1},
    {"RND MAC",   Kind::TOGGLE, 4,  0, 1, 1},
    {"ATK RSSI",  Kind::VALUE,  5,  -90, -50, 5},
    {"HOP SET",   Kind::VALUE,  6,  0, HOP_SET_COUNT - 1, 1},
};

static const uint8_t RADIO_N = sizeof(RADIO) / sizeof(RADIO[0]);

static const Item BLE[] = {
    {"BLE BURST", Kind::VALUE, 0, 50, 500, 50},
    {"ADV TIME",  Kind::VALUE, 1, 50, 200, 25},
};
static const uint8_t BLE_N = sizeof(BLE) / sizeof(BLE[0]);

static const Item KEYS[] = {
    {"AGGRO",    Kind::BIND, 0, 0, 0, 0},
    {"LIGHT",    Kind::BIND, 1, 0, 0, 0},
    {"PIGPASS",  Kind::BIND, 2, 0, 0, 0},
    {"EVILPIG",  Kind::BIND, 3, 0, 0, 0},
    {"BLE",      Kind::BIND, 4, 0, 0, 0},
    {"IR PORT",  Kind::BIND, 5, 0, 0, 0},
    {"SPECTRUM", Kind::BIND, 6, 0, 0, 0},
    {"LOOT",     Kind::BIND, 7, 0, 0, 0},
    {"RADIO",    Kind::BIND, 8, 0, 0, 0},
    {"FILES",    Kind::BIND, 9, 0, 0, 0},
    {"PIG",      Kind::BIND, 10, 0, 0, 0},
    {"XFER",     Kind::BIND, 11, 0, 0, 0},
    {"BADUSB",   Kind::BIND, 12, 0, 0, 0},
    {"USB SD",   Kind::BIND, 13, 0, 0, 0},
    {"WIFI",     Kind::BIND, 14, 0, 0, 0},
    {"STOP",     Kind::BIND, 15, 0, 0, 0},
};
static const uint8_t KEYS_N = sizeof(KEYS) / sizeof(KEYS[0]);

static const char* const H_SCENE[] = {
    "TYPE NAME. ENT SAVE.",
    "SKIN OF THE HOG.",
    "AUTO OR LOCK A SEASON.",
    "AUTO DUSK / DAY / NIGHT.",
    "WALK SPEED AT THE EDGES.",
    "SHE LIVES WHILE YOU WORK.",
    "MASTER: FULL SCENE OR BLANK.",
    "RANDOM WOLF VISITOR.",
    "SEASONAL PROPS ON FARM.",
    "COMPANION PIG ON FARM.",
    "KILL EATS RANDOM HANDSHAKES.",
    "FRUIT TREES AND DROPS.",
    "RAIN SNOW CLOUDS BIRDS.",
    "GRASS / DIRT FLOOR.",
    "DRAW THE PIG BODY.",
    "LEAVES BANKS BUTTERFLIES.",
    "SPEECH BUBBLE ON/OFF.",
    "SEC BETWEEN MONOLOGUES.",
    "-/= CYCLE ANIMS ON FARM.",
    "TYPE CODE. ENT."
};

static const char* const H_SYSTEM[] = {
    "SCREEN GLOW.",
    "0 = MUTE.",
    "0 = NEVER DIM.",
    "0 = SCREEN OFF WHEN DIM.",
    "CARDPUTER RGB ON/OFF.",
    "RGB LED BRIGHTNESS."
};
static const char* const H_RADIO[] = {
    "STOCK / FOCUS / MAX. TUNE=CUST.",
    "AUTO / ALL / CLIENTS / FOCUS / HERD.",
    "ENT = BACK TO STOCK RADIO.",
    "AUTO: SEC THEN NEXT METHOD.",
    "DEAUTH ROUNDS PER AP.",
    "KICK BOTH WAYS AP<->STA.",
    "EAPOL-START / LOGOFF TX.",
    "AUTH+ASSOC FOR PMKID.",
    "SPOOF CSA BEACON TO HERD.",
    "RANDOM AUTH IF NO CLIENTS.",
    "802.11 DEAUTH REASON CODE.",
    "LISTEN AFTER M1, NO KICK.",
    "RICH RADIOTAP CH/RSSI IN PCAP.",
    "ANTI-WIDS GAP BETWEEN MGMT.",
    "SEC COOLDOWN AFTER KICK/AP.",
    "MIN SCORE TO ATTACK (FOCUS).",
    "MIN CHANNEL DWELL MS.",
    "PAIR / +M3 / FULL 4-WAY.",
    "DATA FRAMES FEED FOCUS SCORE.",
    "LOCK: ONLY KICK LOCKED BSSID.",
    "EXTRA SEC HOLD AFTER PAIR.",
    "HOW LONG YOU SIT ON A CH.",
    "HOLD CHANNEL AFTER EAPOL.",
    "LOCK WHEN HANDSHAKE LANDS.",
    "KICK CLIENTS ON AGGRO / EP.",
    "NEW MAC EACH ATTACK START.",
    "SKIP WEAK APS FOR KICK.",
    "ALL / PRI 1-6-11 FIRST / CORE."
};

static const char* const H_BLE[] = {
    "MS BETWEEN BLE BURSTS.",
    "MS EACH ADVERTISEMENT."
};
static const char* const H_KEYS[] = {
    "A = AGGRO HUNT.",
    "L = QUIET SNIFF.",
    "P = WORDLIST / MASK.",
    "E = LAB PORTAL.",
    "B = BLE FRAMES.",
    "I = IR BLAST.",
    "S = 2.4 SWEEP.",
    "H = WPASEC / PWN.",
    "R = RADIO SETTINGS."
};

struct NetRow {
    char ssid[33];
    int8_t rssi;
    bool open;
};

static bool s_active = false;
static bool s_keyWas = false;
static uint32_t s_openMs = 0;
static bool s_editing = false;
static bool s_text = false;
static bool s_bind = false;
static SettingsPage s_page = SettingsPage::SCENE;
static uint8_t s_idx = 0;
static uint8_t s_scroll = 0;
static uint8_t s_statScroll = 0;
static const uint8_t STAT_VIS = 5;
static char s_edit[65];
static const uint8_t VIS = 4;

static ConnPhase s_conn = ConnPhase::LIST;
static NetRow s_nets[16];
static uint8_t s_netN = 0;
static uint8_t s_netIdx = 0;
static uint8_t s_netScroll = 0;
static char s_pickSsid[33] = "";
static bool s_pickOpen = false;
static bool s_scanning = false;

static const Item* items(uint8_t* n) {
    if (s_page == SettingsPage::SYSTEM) { *n = SYSTEM_N; return SYSTEM; }
    if (s_page == SettingsPage::RADIO) { *n = RADIO_N; return RADIO; }
    if (s_page == SettingsPage::BLE) { *n = BLE_N; return BLE; }
    if (s_page == SettingsPage::KEYS) { *n = KEYS_N; return KEYS; }
    if (s_page == SettingsPage::CONNECT) { *n = 0; return nullptr; }
    if (s_page == SettingsPage::STATUS) { *n = 0; return nullptr; }
    *n = SCENE_N;
    return SCENE;
}

static bool allLayersOn() {
    return SceneLayers::pig && SceneLayers::grass && SceneLayers::trees &&
           SceneLayers::sky && SceneLayers::weather && SceneLayers::seasonFx &&
           SceneLayers::mood && SceneLayers::wolf;
}

static const char* skinName(uint8_t s) {
    switch ((PigSkin)s) {
        case PigSkin::CLASSIC: return "CLASSIC";
        case PigSkin::BLUSH:   return "BLUSH";
        case PigSkin::HOG:     return "HOG";
        case PigSkin::ZOMBIE:  return "ZOMBIE";
        case PigSkin::RETRO:   return "RETRO";
        case PigSkin::SHADOW:  return "SHADOW";
        case PigSkin::CANDY:   return "CANDY";
        case PigSkin::GOLD:    return "GOLD";
        case PigSkin::DIRTY:   return "DIRTY";
        default: return "?";
    }
}
static const char* seasonName(uint8_t s) {
    switch ((SeasonMode)s) {
        case SeasonMode::AUTO:   return "AUTO";
        case SeasonMode::SPRING: return "SPRING";
        case SeasonMode::SUMMER: return "SUMMER";
        case SeasonMode::AUTUMN: return "AUTUMN";
        case SeasonMode::WINTER: return "WINTER";
        case SeasonMode::RETRO:  return "RETRO";
        case SeasonMode::NOIR:   return "NOIR";
        case SeasonMode::CITY:   return "CITY";
        case SeasonMode::DESERT: return "DESERT";
        default: return "?";
    }
}
static const char* skyName(uint8_t s) {
    switch ((SkyMode)s) {
        case SkyMode::AUTO:  return "AUTO";
        case SkyMode::DAY:   return "DAY";
        case SkyMode::NIGHT: return "NIGHT";
        default: return "?";
    }
}
static const char* hopSetName(uint8_t s) {
    switch ((HopSet)s) {
        case HopSet::ALL:      return "ALL 1-13";
        case HopSet::PRIORITY: return "PRI 1-6-11";
        case HopSet::CORE:     return "CORE 1-6-11";
        default: return "?";
    }
}
// HS DEPTH (RADIO id 24): how much of the 4-way handshake to insist on
// before the lock-on-BSSID logic gives up on a target and moves on. See
// RadioConfig::hsDepth (config.h) and Hc22000::hasHandshake().
static const char* hsDepthName(uint8_t s) {
    switch (s) {
        case 0:  return "PAIR M1+2";
        case 1:  return "+M3";
        case 2:  return "FULL M1-4";
        default: return "?";
    }
}
// HsMethod layout for the saved value (kept stable across versions so old
// NVS blobs still parse): 0 = AUTO (special), then explicit methods use
// 1..N and resolve to Methods::name(idx-1). Unknown values fall back to
// AUTO so the user can still tweak something without bricking the radio.
static const char* hsMethodName(uint8_t s) {
    if (s == 0) return "AUTO";
    const char* n = Cap::Methods::name((uint8_t)(s - 1));
    return n ? n : "AUTO";
}
// PACK layout mirrors hsMethodName() just above but walks the independent
// Cap::Packs table: 0 = STOCK, 1..N = Packs::name(idx-1), RADIO_PACK_CUSTOM
// (0xFF) = CUSTOM.
static const char* radioPackName(uint8_t s) {
    if (s == (uint8_t)RadioPack::STOCK) return "STOCK";
    if (s == RADIO_PACK_CUSTOM) return "CUSTOM";
    const char* n = Cap::Packs::name((uint8_t)(s - 1));
    return n ? n : "STOCK";
}

static int getValue(const Item& it) {
    PersonalityConfig& p = Config::personality();
    RadioConfig& r = Config::radio();
    BleConfig& b = Config::ble();
    if (s_page == SettingsPage::SCENE) {
        switch (it.id) {
            case 1: return p.pigSkin;
            case 2: return p.seasonMode;
            case 3: return p.skyMode;
            case 4: return p.scrollSpeed;
            case 17: return p.talkIntervalSec;
            case 5: return p.freeLife ? 1 : 0;
            case 6: return allLayersOn() ? 1 : 0;
            case 7: return (p.wolfEnabled && SceneLayers::wolf) ? 1 : 0;
            case 15: return p.wolfEatLoot ? 1 : 0;
            case 8: return (p.fruitTreesAmbient && SceneLayers::trees) ? 1 : 0;
            case 9: return SceneLayers::weather ? 1 : 0;
            case 10: return SceneLayers::grass ? 1 : 0;
            case 11: return SceneLayers::pig ? 1 : 0;
            case 12: return SceneLayers::seasonFx ? 1 : 0;
            case 13: return SceneLayers::mood ? 1 : 0;
            case 14: return p.animTest ? 1 : 0;
            case 18: return p.propsEnabled ? 1 : 0;
            case 19: return p.friendEnabled ? 1 : 0;
            case 20: return p.cardsEnabled ? 1 : 0;
            default: return 0;
        }
    }
    if (s_page == SettingsPage::SYSTEM) {
        switch (it.id) {
            case 0: return p.brightness;
            case 1: return p.soundLevel;
            case 2: return p.dimTimeout;
            case 3: return p.dimLevel;
            case 4: return p.ledEnabled ? 1 : 0;
            case 5: return p.ledBright;
            default: return 0;
        }
    }
    if (s_page == SettingsPage::RADIO) {
        switch (it.id) {
            case 0: return r.hopMs;
            case 1: return r.lockMs;
            case 2: return r.lockOnHs ? 1 : 0;
            case 3: return r.deauth ? 1 : 0;
            case 4: return r.randomMac ? 1 : 0;
            case 5: return r.minRssi;
            case 6: return r.hopSet;
            case 7: return r.hsMethod;
            case 8: return r.fallbackSec;
            case 9: return r.kickBurst;
            case 10: return r.bidirKick ? 1 : 0;
            case 11: return r.eapolTx ? 1 : 0;
            case 12: return r.pmkidProbe ? 1 : 0;
            case 13: return r.csaHerd ? 1 : 0;
            case 14: return r.authFlood ? 1 : 0;
            case 15: return r.deauthReason;
            case 16: return r.pauseMs;
            case 17: return r.fatPcap ? 1 : 0;
            case 18: return r.pack;
            // Porkchop-style knobs (IDs 20..23).
            case 20: return r.jitterMs;
            case 21: return r.cooldownMs;
            case 22: return r.scoreThr;
            case 23: return r.dwellMinMs;
            case 24: return r.hsDepth;
            case 25: return r.dataAct ? 1 : 0;
            case 26: return r.strictLock ? 1 : 0;
            case 27: return r.depthHoldSec;
            default: return 0;
        }
    }
    return it.id == 0 ? b.burstMs : b.advMs;
}

static void formatValue(const Item& it, char* out, size_t len, bool editing) {
    if (it.kind == Kind::TEXT) {
        if (it.id == 16) {
            if (s_text) snprintf(out, len, ">%s", s_edit);
            else snprintf(out, len, XP::allUnlocked() ? "OPEN" : "----");
            return;
        }
        const char* n = s_text ? s_edit : Config::personality().name;
        snprintf(out, len, editing || s_text ? ">%s" : "%s", n);
        return;
    }
    if (it.kind == Kind::BIND) {
        if (s_bind && editing) {
            snprintf(out, len, ">?");
            return;
        }
        char k = Config::hotkeys().key[it.id];
        if (!k) {
            snprintf(out, len, "-");
            return;
        }
        if (k >= 'a' && k <= 'z') k = (char)(k - 'a' + 'A');
        snprintf(out, len, "%c", k);
        return;
    }
    if (it.kind == Kind::ACTION) {
        snprintf(out, len, editing ? "[ENT]" : "ENT");
        return;
    }
    if (it.kind == Kind::TOGGLE) {
        snprintf(out, len, getValue(it) ? "YES" : "NO");
        return;
    }
    char raw[20];
    raw[0] = '\0';
    if (s_page == SettingsPage::SCENE) {
        if (it.id == 1) strncpy(raw, skinName((uint8_t)getValue(it)), sizeof(raw) - 1);
        else if (it.id == 2) strncpy(raw, seasonName((uint8_t)getValue(it)), sizeof(raw) - 1);
        else if (it.id == 3) strncpy(raw, skyName((uint8_t)getValue(it)), sizeof(raw) - 1);
        else snprintf(raw, sizeof(raw), "%d", getValue(it));
    } else if (s_page == SettingsPage::SYSTEM && it.id == 2) {
        int v = getValue(it);
        if (v <= 0) strncpy(raw, "OFF", sizeof(raw) - 1);
        else snprintf(raw, sizeof(raw), "%dS", v);
    } else if (s_page == SettingsPage::RADIO && it.id == 6) {
        strncpy(raw, hopSetName((uint8_t)getValue(it)), sizeof(raw) - 1);
    } else if (s_page == SettingsPage::RADIO && it.id == 7) {
        strncpy(raw, hsMethodName((uint8_t)getValue(it)), sizeof(raw) - 1);
    } else if (s_page == SettingsPage::RADIO && it.id == 18) {
        strncpy(raw, radioPackName((uint8_t)getValue(it)), sizeof(raw) - 1);
    } else if (s_page == SettingsPage::RADIO && it.id == 24) {
        strncpy(raw, hsDepthName((uint8_t)getValue(it)), sizeof(raw) - 1);
    } else if (s_page == SettingsPage::RADIO && it.id == 8) {
        snprintf(raw, sizeof(raw), "%dS", getValue(it));
    } else if (s_page == SettingsPage::RADIO && it.id == 27) {
        int v = getValue(it);
        if (v <= 0) strncpy(raw, "OFF", sizeof(raw) - 1);
        else snprintf(raw, sizeof(raw), "%dS", v);
    } else if (s_page == SettingsPage::RADIO && it.id == 21) {
        int v = getValue(it);
        if (v <= 0) strncpy(raw, "OFF", sizeof(raw) - 1);
        else snprintf(raw, sizeof(raw), "%dS", v);
    } else {
        snprintf(raw, sizeof(raw), "%d", getValue(it));
    }
    raw[sizeof(raw) - 1] = '\0';
    if (editing) snprintf(out, len, "[%s]", raw);
    else strncpy(out, raw, len - 1);
    out[len - 1] = '\0';
}

static bool setValue(const Item& it, int v) {
    PersonalityConfig& p = Config::personality();
    RadioConfig& r = Config::radio();
    BleConfig& b = Config::ble();

    // HS METHOD (RADIO id 7) is the only item whose max grows with the
    // method registry — clamp it explicitly so the rest of the function
    // can keep using a single minV/maxV range.
    if (s_page == SettingsPage::RADIO && it.id == 7) {
        int maxV = (int)Cap::Methods::count(); // AUTO takes slot 0
        if (v < 0) v = maxV;
        if (v > maxV) v = 0;
        r.hsMethod = (uint8_t)v;
        Config::save();
        return true;
    }

    // PACK (RADIO id 18): 0 = STOCK, 1..N = Cap::Packs::name(idx-1) (its
    // own independent registry, see cap/packs/), RADIO_PACK_CUSTOM (0xFF)
    // = CUSTOM. CUSTOM is a fixed sentinel rather than "N+1" on disk (see
    // config.h), so it can't be reached by the usual +/-1 wrap the way
    // HS METHOD's slots are - instead we map the current value to a
    // contiguous logical slot (0..N+1, CUSTOM last), step that, then map
    // back. `v` is `getValue()+/-step` in the RAW on-disk domain, so its
    // direction relative to the current raw value tells us which way the
    // user pressed even when the raw jump (e.g. off of 0xFF) isn't +/-1.
    if (s_page == SettingsPage::RADIO && it.id == 18) {
        int packCount = (int)Cap::Packs::count();
        int lastSlot = packCount + 1; // logical slot for CUSTOM
        int curSlot = (r.pack == RADIO_PACK_CUSTOM) ? lastSlot : (int)r.pack;
        bool wantUp = v > (int)r.pack;
        int nextSlot = curSlot + (wantUp ? 1 : -1);
        if (nextSlot < 0) nextSlot = lastSlot;
        if (nextSlot > lastSlot) nextSlot = 0;
        uint8_t resolved = (nextSlot == lastSlot) ? RADIO_PACK_CUSTOM : (uint8_t)nextSlot;
        Config::applyRadioPack(resolved);
        Display::showToast(radioPackName(resolved), 900);
        return true;
    }

    if (v < it.minV) v = it.maxV;
    if (v > it.maxV) v = it.minV;

    if (s_page == SettingsPage::SCENE) {
        switch (it.id) {
            case 1: {
                if (Mood::getHearts() < 1) {
                    Display::showToast("NEED HEART", 1000);
                    return false;
                }
                const int from = (int)p.pigSkin;
                const int span = it.maxV - it.minV + 1;
                const int fwd = (v - from + span) % span;
                const int back = (from - v + span) % span;
                const int dir = (fwd <= back) ? 1 : -1;
                int skin = v;
                if (XP::isSkinLocked((uint8_t)skin)) {
                    skin = from;
                    for (int i = 0; i < PIG_SKIN_COUNT; i++) {
                        skin += dir;
                        if (skin < it.minV) skin = it.maxV;
                        if (skin > it.maxV) skin = it.minV;
                        if (XP::isSkinLocked((uint8_t)skin)) continue;
                        break;
                    }
                }
                if (skin == (int)PigSkin::ZOMBIE) {
                    Config::becomeZombie();
                    break;
                }
                const bool leavingZombie = (from == (int)PigSkin::ZOMBIE);
                p.pigSkin = (uint8_t)skin;
                if (leavingZombie) p.nightWolfBites = 0;
                break;
            }
            case 2: {
                int from = (int)p.seasonMode;
                int span = it.maxV - it.minV + 1;
                int fwd = (v - from + span) % span;
                int back = (from - v + span) % span;
                int dir = (fwd <= back) ? 1 : -1;
                int sm = v;
                if (XP::isSeasonLocked((uint8_t)sm)) {
                    sm = from;
                    for (int i = 0; i < SEASON_MODE_COUNT; i++) {
                        sm += dir;
                        if (sm < it.minV) sm = it.maxV;
                        if (sm > it.maxV) sm = it.minV;
                        if (XP::isSeasonLocked((uint8_t)sm)) continue;
                        break;
                    }
                }
                p.seasonMode = (uint8_t)sm;
                break;
            }
            case 3: p.skyMode = (uint8_t)v; break;
            case 4: p.scrollSpeed = (uint8_t)v; break;
            case 17: p.talkIntervalSec = (uint8_t)v; break;
            case 5: p.freeLife = v != 0; break;
            case 6:
                SceneLayers::setAll(v != 0);
                if (v == 0) Wolf::reset();
                break;
            case 7:
                p.wolfEnabled = v != 0;
                SceneLayers::wolf = v != 0;
                if (v == 0) Wolf::reset();
                break;
            case 18:
                p.propsEnabled = v != 0;
                if (v == 0) {
                    Props::forceDemo(6);  // clear active prop
                    Display::showToast("PROPS OFF", 900);
                } else {
                    Display::showToast("PROPS ON", 900);
                }
                break;
            case 19:
                p.friendEnabled = v != 0;
                Display::showToast(v ? "FRIEND ON" : "FRIEND OFF", 900);
                break;
            case 20:
                p.cardsEnabled = v != 0;
                Display::showToast(v ? "CARDS ON" : "CARDS OFF", 900);
                break;
            case 15:
                p.wolfEatLoot = v != 0;
                if (v == 0) {
                    uint8_t n = Storage::restoreWolfLoot();
                    if (n) {
                        char msg[28];
                        snprintf(msg, sizeof(msg), "WOLF GAVE %u FILE%s",
                                 (unsigned)n, n == 1 ? "" : "S");
                        Display::showToast(msg, 2200);
                    }
                }
                break;
            case 8:
                p.fruitTreesAmbient = v != 0;
                SceneLayers::trees = v != 0;
                break;
            case 9: SceneLayers::weather = v != 0; break;
            case 10: SceneLayers::grass = v != 0; break;
            case 11: SceneLayers::pig = v != 0; break;
            case 12: SceneLayers::seasonFx = v != 0; break;
            case 13: SceneLayers::mood = v != 0; break;
            case 14:
                p.animTest = v != 0;
                if (v != 0) Display::showToast("ANIM TEST: -/= ON FARM", 1800);
                break;
            default: return false;
        }
        Config::save();
        return true;
    }
    if (s_page == SettingsPage::SYSTEM) {
        switch (it.id) {
            case 0:
                p.brightness = (uint8_t)v;
                Display::resetDimTimer();
                Display::refreshBrightness();
                break;
            case 1: p.soundLevel = (uint8_t)v; break;
            case 2:
                p.dimTimeout = (uint16_t)v;
                Display::resetDimTimer();
                break;
            case 3:
                p.dimLevel = (uint8_t)v;
                Display::resetDimTimer();
                break;
            case 4:
                p.ledEnabled = (v != 0);
                if (!p.ledEnabled) Led::off();
                else Led::applyBrightness();
                break;
            case 5:
                p.ledBright = (uint8_t)v;
                Led::applyBrightness();
                // brief white so user sees level
                if (p.ledEnabled) Led::pulse(255, 255, 255, 200);
                break;
            default: return false;
        }
        Config::save();
        return true;
    }
    if (s_page == SettingsPage::RADIO) {
        // PACK (id 18) is handled above, before the generic minV/maxV clamp.
        switch (it.id) {
            case 0: r.hopMs = (uint16_t)v; break;
            case 1: r.lockMs = (uint16_t)v; break;
            case 2: r.lockOnHs = v != 0; break;
            case 3: r.deauth = v != 0; break;
            case 4: r.randomMac = v != 0; break;
            case 5: r.minRssi = (int8_t)v; break;
            case 6: r.hopSet = (uint8_t)v; break;
            case 7: r.hsMethod = (uint8_t)v; break;
            case 8: r.fallbackSec = (uint8_t)v; break;
            case 9: r.kickBurst = (uint8_t)v; break;
            case 10: r.bidirKick = v != 0; break;
            case 11: r.eapolTx = v != 0; break;
            case 12: r.pmkidProbe = v != 0; break;
            case 13: r.csaHerd = v != 0; break;
            case 14: r.authFlood = v != 0; break;
            case 15: r.deauthReason = (uint8_t)v; break;
            case 16: r.pauseMs = (uint16_t)v; break;
            case 17: r.fatPcap = v != 0; break;
            // Porkchop-style knobs (IDs 20..23) + handshake depth (24).
            case 20: r.jitterMs = (uint8_t)v; break;
            case 21: r.cooldownMs = (uint8_t)v; break;
            case 22: r.scoreThr = (int16_t)v; break;
            case 23: r.dwellMinMs = (uint16_t)v; break;
            case 24: r.hsDepth = (uint8_t)v; break;
            case 25: r.dataAct = (uint8_t)(v != 0 ? 1 : 0); break;
            case 26: r.strictLock = v != 0; break;
            case 27: r.depthHoldSec = (uint8_t)v; break;
            default: return false;
        }
        // Any hand-tuned knob flips PACK to CUSTOM so the UI reflects that
        // the active parameters no longer match a preset. HS METHOD (id=7)
        // is excluded above — switching methods alone isn't "customising".
        Config::markRadioCustom();
        Config::save();
        return true;
    }
    if (it.id == 0) b.burstMs = (uint16_t)v;
    else b.advMs = (uint16_t)v;
    Config::save();
    return true;
}

static void keepVisible(uint8_t n) {
    if (s_idx < s_scroll) s_scroll = s_idx;
    if (s_idx >= s_scroll + VIS) s_scroll = (uint8_t)(s_idx - VIS + 1);
    if (s_scroll + VIS > n && n >= VIS) s_scroll = (uint8_t)(n - VIS);
}

static void scanWifi() {
    s_scanning = true;
    s_netN = 0;
    s_netIdx = 0;
    s_netScroll = 0;
    s_conn = ConnPhase::LIST;
    if (Cap::isRunning()) Cap::stop();
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(60);
    int n = WiFi.scanNetworks(false, true);
    if (n < 0) n = 0;
    for (int i = 0; i < n && s_netN < 16; i++) {
        String ss = WiFi.SSID(i);
        if (ss.length() == 0) continue;
        NetRow& r = s_nets[s_netN];
        strncpy(r.ssid, ss.c_str(), sizeof(r.ssid) - 1);
        r.ssid[sizeof(r.ssid) - 1] = '\0';
        r.rssi = (int8_t)WiFi.RSSI(i);
        wifi_auth_mode_t enc = WiFi.encryptionType(i);
        r.open = (enc == WIFI_AUTH_OPEN);
        s_netN++;
    }
    WiFi.scanDelete();
    s_scanning = false;
}

static void saveHomeWifi(const char* pass) {
    Net::setSta(s_pickSsid, pass ? pass : "");
    Display::showToast("WIFI SAVED", 1000);
}

void show(SettingsPage page) {
    s_active = true;
    s_page = page;
    s_idx = 0;
    s_scroll = 0;
    s_statScroll = 0;
    s_editing = false;
    s_text = false;
    s_bind = false;
    s_keyWas = true;
    s_openMs = millis();
    if (page == SettingsPage::CONNECT) {
        s_conn = ConnPhase::LIST;
        s_edit[0] = '\0';
        Display::showToast("SCAN...", 600);
        scanWifi();
    }
}

bool isTyping() {
    return s_text || s_bind ||
           (s_page == SettingsPage::CONNECT && s_conn == ConnPhase::PASS);
}

void hide() {
    s_active = false;
    s_editing = false;
    s_text = false;
    s_bind = false;
}

bool isActive() { return s_active; }
SettingsPage page() { return s_page; }

const char* bottomHint() {
    if (s_page == SettingsPage::CONNECT) {
        if (s_conn == ConnPhase::PASS) return "type pass  BS erase  ENT";
        return ";/. pick  ENT  R rescan";
    }
    if (s_page == SettingsPage::STATUS) return ";/. scroll  ` back";
    if (s_text) return "type  ENT save  BS erase";
    if (s_bind) return "press a key  ` cancel";
    if (s_page == SettingsPage::KEYS) return "ENT set  BS clear  ` back";
    if (s_editing) return ";/. change  ENT done";
    uint8_t n = 0;
    const Item* it = items(&n);
    if (it && s_idx < n) {
        if (it[s_idx].kind == Kind::TOGGLE) return "ENT yes/no  ;/.  ` back";
        if (it[s_idx].kind == Kind::TEXT)
            return it[s_idx].id == 16 ? "ENT type code" : "ENT type name";
        if (it[s_idx].kind == Kind::ACTION) return "ENT reset radio to STOCK";
        return "ENT edit  ;/.  ` back";
    }
    return ";/.  ENT  ` back";
}

static void updateConnect() {
    auto keys = M5Cardputer.Keyboard.keysState();
    bool up = M5Cardputer.Keyboard.isKeyPressed(';');
    bool down = M5Cardputer.Keyboard.isKeyPressed('.');
    bool tick = M5Cardputer.Keyboard.isKeyPressed('`') ||
                M5Cardputer.Keyboard.isKeyPressed(27);
    bool erase = M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || keys.del;
    bool rescan = M5Cardputer.Keyboard.isKeyPressed('r') ||
                  M5Cardputer.Keyboard.isKeyPressed('R');

    if (s_conn == ConnPhase::PASS) {
        if (keys.enter) {
            if (strlen(s_edit) > 0 && strlen(s_edit) < 8) {
                Display::showToast("PASS MIN 8", 1000);
                return;
            }
            saveHomeWifi(s_edit);
            s_conn = ConnPhase::LIST;
            s_text = false;
            SFX::play(SFX::CONFIRM);
            return;
        }
        if (erase) {
            size_t L = strlen(s_edit);
            if (L) s_edit[L - 1] = '\0';
            return;
        }
        if (tick) {
            s_conn = ConnPhase::LIST;
            s_text = false;
            SFX::play(SFX::BACK_NAV);
            return;
        }
        for (char c : keys.word) {
            if (c < 32 || c >= 127 || c == '`') continue;
            size_t L = strlen(s_edit);
            if (L + 1 < sizeof(s_edit) && L < 63) {
                s_edit[L] = c;
                s_edit[L + 1] = '\0';
            }
        }
        return;
    }

    if (tick) {
        hide();
        return;
    }
    if (rescan) {
        Display::showToast("SCAN...", 500);
        scanWifi();
        SFX::play(SFX::CLICK);
        return;
    }
    if (up && s_netIdx > 0) {
        s_netIdx--;
        if (s_netIdx < s_netScroll) s_netScroll = s_netIdx;
        SFX::play(SFX::MENU_CLICK);
        return;
    }
    if (down && s_netIdx + 1 < s_netN) {
        s_netIdx++;
        if (s_netIdx >= s_netScroll + VIS) s_netScroll = (uint8_t)(s_netIdx - VIS + 1);
        SFX::play(SFX::MENU_CLICK);
        return;
    }
    if (!keys.enter || s_netN == 0) return;
    strncpy(s_pickSsid, s_nets[s_netIdx].ssid, sizeof(s_pickSsid) - 1);
    s_pickSsid[sizeof(s_pickSsid) - 1] = '\0';
    s_pickOpen = s_nets[s_netIdx].open;
    if (s_pickOpen) {
        saveHomeWifi("");
        SFX::play(SFX::CONFIRM);
        return;
    }
    s_edit[0] = '\0';
    s_conn = ConnPhase::PASS;
    s_text = true;
    SFX::play(SFX::MENU_CLICK);
}

void update() {
    if (!s_active) return;
    if (App::windowHidden()) return;
    if (!keyNewPress(s_keyWas)) return;

    if (s_page == SettingsPage::CONNECT) {
        updateConnect();
        return;
    }
    if (s_page == SettingsPage::STATUS) {
        auto keys = M5Cardputer.Keyboard.keysState();
        bool up = M5Cardputer.Keyboard.isKeyPressed(';');
        bool down = M5Cardputer.Keyboard.isKeyPressed('.');
        if (keyEsc()) {
            hide();
            return;
        }
        const uint8_t statN = 9;
        if (up && s_statScroll > 0) {
            s_statScroll--;
            SFX::play(SFX::MENU_CLICK);
        } else if (down && s_statScroll + STAT_VIS < statN) {
            s_statScroll++;
            SFX::play(SFX::MENU_CLICK);
        }
        (void)keys;
        return;
    }

    auto keys = M5Cardputer.Keyboard.keysState();
    bool up = M5Cardputer.Keyboard.isKeyPressed(';');
    bool down = M5Cardputer.Keyboard.isKeyPressed('.');
    bool tick = keyEsc();
    bool erase = M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || keys.del;
    bool esc = tick;

    uint8_t n = 0;
    const Item* list = items(&n);
    if (!list || n == 0) return;
    const Item& cur = list[s_idx < n ? s_idx : 0];

    if (s_text) {
        if (keys.enter) {
            if (cur.id == 16) {
                auto same = [](const char* a, const char* b) {
                    while (*a && *b) {
                        char ca = *a++, cb = *b++;
                        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
                        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
                        if (ca != cb) return false;
                    }
                    return *a == 0 && *b == 0;
                };
                s_text = false;
                if (same(s_edit, "l3xik0")) {
                    XP::unlockAll();
                    SFX::play(SFX::LEVEL_UP);
                    Display::showToast("ALL OPEN", 1600);
                } else if (same(s_edit, "p0rkp0rk") || same(s_edit, "pork4") || same(s_edit, "o1nk4")) {
                    Props::unlockAllFour();
                    SFX::play(SFX::LEVEL_UP);
                    Display::showToast("PROPS OPEN", 1600);
                } else {
                    SFX::play(SFX::ERROR);
                    Display::showToast("NOPE", 900);
                }
                s_edit[0] = '\0';
                return;
            }
            PersonalityConfig& p = Config::personality();
            strncpy(p.name, s_edit, sizeof(p.name) - 1);
            p.name[sizeof(p.name) - 1] = '\0';
            if (!p.name[0]) strncpy(p.name, "Pig", sizeof(p.name) - 1);
            Config::save();
            s_text = false;
            SFX::play(SFX::CONFIRM);
            Display::showToast("NAME SAVED", 900);
            return;
        }
        if (erase) {
            size_t L = strlen(s_edit);
            if (L) s_edit[L - 1] = '\0';
            return;
        }
        if (tick) {
            s_text = false;
            SFX::play(SFX::BACK_NAV);
            return;
        }
        for (char c : keys.word) {
            if (c < 32 || c >= 127 || c == '`') continue;
            size_t L = strlen(s_edit);
            if (L + 1 < sizeof(s_edit) && L < 16) {
                s_edit[L] = c;
                s_edit[L + 1] = '\0';
            }
        }
        return;
    }

    if (s_bind) {
        if (tick) {
            s_bind = false;
            SFX::play(SFX::BACK_NAV);
            return;
        }
        if (erase) {
            Config::hotkeys().key[cur.id] = 0;
            Config::save();
            s_bind = false;
            SFX::play(SFX::CONFIRM);
            Display::showToast("CLEARED", 700);
            return;
        }
        char picked = 0;
        for (char c : keys.word) {
            if (c < 32 || c >= 127) continue;
            picked = c;
            break;
        }
        if (!picked) return;
        if (picked >= 'A' && picked <= 'Z') picked = (char)(picked - 'A' + 'a');
        bool bad = (picked == '`' || picked == '~' || picked == ' ' ||
                    picked == ';' || picked == '.' || picked == ',' || picked == '/');
        if (bad) {
            Display::showToast("KEY TAKEN", 800);
            return;
        }
        HotkeyConfig& hk = Config::hotkeys();
        for (uint8_t i = 0; i < HOTKEY_COUNT; i++) {
            if (i != cur.id && hk.key[i] == picked) hk.key[i] = 0;
        }
        hk.key[cur.id] = picked;
        Config::save();
        s_bind = false;
        SFX::play(SFX::CONFIRM);
        char up = picked;
        if (up >= 'a' && up <= 'z') up = (char)(up - 'a' + 'A');
        char msg[12];
        snprintf(msg, sizeof(msg), "KEY %c", up);
        Display::showToast(msg, 800);
        return;
    }

    if (esc) {
        if (s_editing) {
            s_editing = false;
            SFX::play(SFX::BACK_NAV);
            return;
        }
        hide();
        return;
    }

    if (up || down) {
        if (s_editing && cur.kind == Kind::VALUE) {
            int next = getValue(cur) + (up ? cur.step : -cur.step);
            if (setValue(cur, next)) SFX::play(SFX::CLICK);
            return;
        }
        s_editing = false;
        if (up && s_idx > 0) s_idx--;
        else if (down && s_idx + 1 < n) s_idx++;
        keepVisible(n);
        SFX::play(SFX::MENU_CLICK);
        return;
    }

    if (erase && cur.kind == Kind::BIND) {
        Config::hotkeys().key[cur.id] = 0;
        Config::save();
        SFX::play(SFX::CONFIRM);
        Display::showToast("CLEARED", 700);
        return;
    }

    if (!keys.enter) return;

    if (cur.kind == Kind::BIND) {
        s_bind = true;
        SFX::play(SFX::MENU_CLICK);
        Display::showToast("PRESS KEY", 700);
        return;
    }

    if (cur.kind == Kind::ACTION) {
        if (s_page == SettingsPage::RADIO && cur.id == 19) {
            Config::resetRadio();
            SFX::play(SFX::CONFIRM);
            Display::showToast("RADIO RESET", 1000);
        }
        return;
    }

    if (cur.kind == Kind::TOGGLE) {
        int next = getValue(cur) ? 0 : 1;
        if (setValue(cur, next)) {
            SFX::play(SFX::CONFIRM);
            Display::showToast(next ? "YES" : "NO", 700);
        }
        return;
    }
    if (cur.kind == Kind::TEXT) {
        if (cur.id == 16) s_edit[0] = '\0';
        else {
            strncpy(s_edit, Config::personality().name, sizeof(s_edit) - 1);
            s_edit[sizeof(s_edit) - 1] = '\0';
        }
        s_text = true;
        SFX::play(SFX::MENU_CLICK);
        return;
    }
    s_editing = !s_editing;
    SFX::play(SFX::MENU_CLICK);
}

static void drawConnect(M5Canvas& canvas) {
    const uint16_t UI_BG = 0x2145, UI_PANEL = 0x3A8A, UI_TITLE = 0xFFE0;
    const uint16_t UI_TEXT = 0xEF5D, UI_DIM = 0x9CD3, UI_SEL = 0xFDB6;
    canvas.fillSprite(UI_BG);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);
    canvas.setTextColor(UI_TITLE);
    canvas.drawString("CONNECT", DISPLAY_W / 2, 2);
    canvas.drawLine(10, 20, DISPLAY_W - 10, 20, UI_TITLE);
    canvas.setTextDatum(top_left);
    canvas.setTextSize(1);

    if (s_conn == ConnPhase::PASS) {
        canvas.setTextColor(UI_DIM);
        canvas.drawString("PASS FOR", 8, 26);
        canvas.setTextColor(UI_TITLE);
        canvas.setTextSize(2);
        char sn[18];
        strncpy(sn, s_pickSsid, sizeof(sn) - 1);
        sn[sizeof(sn) - 1] = '\0';
        canvas.drawString(sn, 8, 40);
        canvas.setTextSize(1);
        canvas.fillRect(5, 64, DISPLAY_W - 10, 18, UI_SEL);
        canvas.setTextColor(UI_BG);
        char show[40];
        snprintf(show, sizeof(show), ">%s", s_edit);
        canvas.drawString(show, 10, 68);
        canvas.setTextColor(UI_TITLE);
        canvas.setTextDatum(top_center);
        canvas.drawString("TYPE PASSWORD. ENT SAVE.", DISPLAY_W / 2, MAIN_H - 10);
        return;
    }

    if (s_netN == 0) {
        canvas.setTextColor(UI_TITLE);
        canvas.drawString("NO NETS", 8, 40);
        canvas.setTextColor(UI_DIM);
        canvas.drawString("R = SCAN AGAIN", 8, 56);
        return;
    }

    canvas.setTextSize(2);
    const int y0 = 24;
    const int lh = 18;
    for (uint8_t i = 0; i < VIS && (s_netScroll + i) < s_netN; i++) {
        uint8_t idx = s_netScroll + i;
        int y = y0 + i * lh;
        bool sel = (idx == s_netIdx);
        if (sel) {
            canvas.fillRect(5, y - 2, DISPLAY_W - 10, lh, UI_SEL);
            canvas.setTextColor(UI_BG);
        } else {
            canvas.fillRect(5, y - 1, DISPLAY_W - 10, lh - 2, UI_PANEL);
            canvas.setTextColor(UI_TEXT);
        }
        char name[16];
        strncpy(name, s_nets[idx].ssid, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        canvas.drawString(name, 12, y);
        canvas.setTextDatum(top_right);
        canvas.setTextSize(1);
        char rs[10];
        snprintf(rs, sizeof(rs), "%s%d", s_nets[idx].open ? "OP " : "",
                 (int)s_nets[idx].rssi);
        canvas.drawString(rs, DISPLAY_W - 10, y + 4);
        canvas.setTextDatum(top_left);
        canvas.setTextSize(2);
    }
    canvas.setTextSize(1);
    canvas.setTextColor(UI_TITLE);
    canvas.setTextDatum(top_center);
    canvas.drawString("PICK NET. ONLY TYPE PASS.", DISPLAY_W / 2, MAIN_H - 10);
}

static void drawStatus(M5Canvas& canvas) {
    const uint16_t UI_BG = 0x2145, UI_TITLE = 0xFFE0;
    const uint16_t UI_TEXT = 0xEF5D, UI_DIM = 0x9CD3, UI_GOLD = 0xFE60;
    canvas.fillSprite(UI_BG);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(2);
    canvas.setTextDatum(top_center);
    canvas.setTextColor(UI_TITLE);
    canvas.drawString("STATUS", DISPLAY_W / 2, 2);
    canvas.drawLine(10, 20, DISPLAY_W - 10, 20, UI_TITLE);

    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);

    char lvl[16], xp[16], batt[16], wifi[18], ver[16];
    snprintf(lvl, sizeof(lvl), "%u", (unsigned)XP::getLevel());
    snprintf(xp, sizeof(xp), "%lu/%lu",
             (unsigned long)XP::intoLevel(), (unsigned long)XP::needForNext());
    int32_t blv = M5.Power.getBatteryLevel();
    if (blv < 0) blv = 0;
    if (blv > 100) blv = 100;
    const bool chg = (M5.Power.isCharging() == m5::Power_Class::is_charging);
    snprintf(batt, sizeof(batt), "%d%%%s", (int)blv, chg ? " CHG" : "");
    wifi[0] = '-'; wifi[1] = '-'; wifi[2] = '\0';
    if (Net::hasStaCreds() && Net::cfg().staSsid[0]) {
        strncpy(wifi, Net::cfg().staSsid, sizeof(wifi) - 1);
        wifi[sizeof(wifi) - 1] = '\0';
    }
    snprintf(ver, sizeof(ver), "v%s", ON3PORK_VERSION);

    const char* k[] = { "LVL", "XP", "BOARD", "BATT", "SD", "WIFI", "WPA", "PWN", "VER" };
    const char* v[] = {
        lvl, xp, Board::modelLabel(), batt,
        Config::isSDAvailable() ? "YES" : "NO",
        wifi,
        Net::cfg().wpaSecKey[0] ? "YES" : "NO",
        Net::cfg().pwncrackKey[0] ? "YES" : "NO",
        ver
    };
    const uint8_t statN = 9;
    if (s_statScroll > statN - STAT_VIS) {
        s_statScroll = (statN > STAT_VIS) ? (uint8_t)(statN - STAT_VIS) : 0;
    }

    int y = 24;
    const int lh = 12;
    for (uint8_t i = 0; i < STAT_VIS; i++) {
        uint8_t idx = (uint8_t)(s_statScroll + i);
        if (idx >= statN) break;
        canvas.setTextColor(UI_DIM);
        canvas.drawString(k[idx], 8, y);
        canvas.setTextColor(idx == 8 ? UI_GOLD : UI_TEXT);
        canvas.drawString(v[idx], 78, y);
        if (idx == 1) {
            int barW = 80;
            uint32_t need = XP::needForNext();
            int fill = (need > 0) ? (int)(XP::intoLevel() * (uint32_t)barW / need) : barW;
            if (XP::getLevel() >= 50) fill = barW;
            canvas.fillRect(78, y + 9, barW, 3, UI_DIM);
            if (fill > 0) canvas.fillRect(78, y + 9, fill, 3, UI_GOLD);
        }
        y += lh;
    }

    canvas.setTextColor(UI_DIM);
    if (s_statScroll > 0) canvas.drawString("^", DISPLAY_W - 12, 22);
    if (s_statScroll + STAT_VIS < statN)
        canvas.drawString("v", DISPLAY_W - 12, MAIN_H - 22);

    canvas.setTextColor(UI_TITLE);
    canvas.setTextDatum(top_center);
    canvas.drawString(";/.  ` BACK", DISPLAY_W / 2, MAIN_H - 10);
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::Font0);
}

void draw(M5Canvas& canvas) {
    if (s_page == SettingsPage::CONNECT) {
        drawConnect(canvas);
        return;
    }
    if (s_page == SettingsPage::STATUS) {
        drawStatus(canvas);
        return;
    }

    const uint16_t UI_BG = 0x2145, UI_PANEL = 0x3A8A, UI_TITLE = 0xFFE0;
    const uint16_t UI_TEXT = 0xEF5D, UI_DIM = 0x9CD3, UI_SEL = 0xFDB6;
    canvas.fillSprite(UI_BG);

    const char* title = "PIG";
    if (s_page == SettingsPage::SYSTEM) title = "SYSTEM";
    else if (s_page == SettingsPage::RADIO) title = "RADIO";
    else if (s_page == SettingsPage::BLE) title = "BLE";
    else if (s_page == SettingsPage::KEYS) title = "KEYS";

    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);
    canvas.setTextColor(UI_TITLE);
    canvas.drawString(title, DISPLAY_W / 2, 2);
    canvas.drawLine(10, 20, DISPLAY_W - 10, 20, UI_TITLE);

    uint8_t n = 0;
    const Item* list = items(&n);
    canvas.setTextDatum(top_left);
    canvas.setTextSize(2);
    const int y0 = 24;
    const int lh = 18;
    for (uint8_t i = 0; i < VIS && (s_scroll + i) < n; i++) {
        uint8_t idx = s_scroll + i;
        int y = y0 + i * lh;
        bool sel = (idx == s_idx);
        if (sel) {
            canvas.fillRect(5, y - 2, DISPLAY_W - 10, lh, UI_SEL);
            canvas.fillRect(5, y - 2, 3, lh, UI_TITLE);
            canvas.setTextColor(UI_BG);
        } else {
            canvas.fillRect(5, y - 1, DISPLAY_W - 10, lh - 2, UI_PANEL);
            canvas.setTextColor(UI_TEXT);
        }
        canvas.drawString(list[idx].label, 12, y);
        char val[22];
        formatValue(list[idx], val, sizeof(val), sel && (s_editing || s_bind));
        canvas.setTextDatum(top_right);
        canvas.drawString(val, DISPLAY_W - 10, y);
        canvas.setTextDatum(top_left);
    }

    canvas.setTextSize(1);
    canvas.setTextColor(UI_DIM);
    if (s_scroll > 0) canvas.drawString("^", DISPLAY_W - 12, 22);
    if (s_scroll + VIS < n) canvas.drawString("v", DISPLAY_W - 12, y0 + (VIS - 1) * lh);

    const char* const* hints = H_SCENE;
    if (s_page == SettingsPage::SYSTEM) hints = H_SYSTEM;
    else if (s_page == SettingsPage::RADIO) hints = H_RADIO;
    else if (s_page == SettingsPage::BLE) hints = H_BLE;
    else if (s_page == SettingsPage::KEYS) hints = H_KEYS;
    if (s_idx < n) {
        canvas.setTextColor(UI_TITLE);
        canvas.setTextDatum(top_center);
        canvas.drawString(hints[s_idx], DISPLAY_W / 2, MAIN_H - 10);
        canvas.setTextDatum(top_left);
    }
}

}  // namespace SettingsMenu
