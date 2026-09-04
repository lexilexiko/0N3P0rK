// Slim personality config for the Tamagotchi pig.
// WiFi / API keys live in Net (Stamp NVS), not here.
#pragma once

#include <Arduino.h>
#include <stdint.h>

enum class SkyMode : uint8_t {
    AUTO = 0,
    DAY = 1,
    NIGHT = 2
};
static const uint8_t SKY_MODE_COUNT = 3;

enum class PigSkin : uint8_t {
    CLASSIC = 0,
    BLUSH   = 1,
    HOG     = 2,
    ZOMBIE  = 3,
    RETRO   = 4,
    SHADOW  = 5,
    CANDY   = 6,
    GOLD    = 7,
    DIRTY   = 8   // alley / dumpster pig — unlock with CITY (lv 25)
};
static const uint8_t PIG_SKIN_COUNT = 9;

enum class SeasonMode : uint8_t {
    AUTO   = 0,
    SPRING = 1,
    SUMMER = 2,
    AUTUMN = 3,
    WINTER = 4,
    RETRO  = 5,
    NOIR   = 6,
    CITY   = 7,  // urban alley — unlock lv 25
    DESERT = 8   // dunes / palms — unlock lv 30
};
static const uint8_t SEASON_MODE_COUNT = 9;

enum class Season : uint8_t {
    SPRING = 0,
    SUMMER = 1,
    AUTUMN = 2,
    WINTER = 3,
    RETRO  = 4,
    NOIR   = 5,
    CITY   = 6,
    DESERT = 7
};
static const uint8_t SEASON_COUNT = 4;

struct PersonalityConfig {
    char name[32] = "Pig";
    uint8_t soundLevel = 1;
    uint8_t brightness = 80;
    uint8_t dimLevel = 20;
    uint16_t dimTimeout = 30;
    uint8_t skyMode = 0;
    uint8_t pigSkin = 0;
    uint8_t pigSkinAlive = 0;  // last non-zombie skin (restore on 5 hearts)
    uint8_t nightWolfBites = 0;
    bool zombieSkinUnlocked = false;
    uint8_t seasonMode = 0;
    bool animTest = false;
    bool wolfEnabled = true;
    bool propsEnabled = true;  // seasonal daily props (lv35 / P0rkP0rk)
    bool friendEnabled = true; // companion pig (lv 40+)
    bool cardsEnabled = true;  // cards table toggle
    uint8_t scrollSpeed = 9;
    // Seconds between automatic snout monologues. 2..10 (PIG menu TALK SEC).
    uint8_t talkIntervalSec = 5;
    bool fruitTreesAmbient = true;
    bool freeLife = true;  // pig walks/jumps/hides even during functions
    bool wolfEatLoot = true;  // bite at 0 hearts stashes loot; hit/Am-off returns it
};

enum class HopSet : uint8_t { ALL = 0, PRIORITY = 1, CORE = 2 };
static const uint8_t HOP_SET_COUNT = 3;

// OURS = current greedy EAPOL + broadcast kick
// PAN  = extra stack (bidir kick, EAPOL-Start, PMKID probe, optional CSA/flood)
// AUTO = OURS first, then PAN if no pair lands
//
// The enum values are the on-disk format for the radio.hsMethod byte:
//   0      -> AUTO (special, not a real method)
//   1..N   -> Methods::name(idx - 1) from cap/methods/method_registry.cpp
// Adding a new method no longer requires touching this enum — just add a
// row to METHOD_LIST() in method_ctx.h and the new entry shows up in the
// radio settings UI at the next index. Values written by older firmware
// (OURS=1, PAN=2) keep resolving to the same name because the first two
// registry rows are still OURS and PAN in that order.
enum class HsMethod : uint8_t { AUTO = 0, OURS = 1, PAN = 2 };
// Runtime count for the UI is 1 + Cap::Methods::count(); see
// HS_METHOD_COUNT below. Use HS_METHOD_COUNT for legacy code that needs a
// constexpr upper bound (the registry isn't visible from config.h).
static const uint8_t HS_METHOD_COUNT_MAX = 8;

// pack on-disk layout (RadioConfig::pack byte) mirrors hsMethod above, but
// walks the independent Cap::Packs table (cap/packs/), not the Methods one:
//   0      -> STOCK  (factory-default knobs, hsMethod left on AUTO)
//   1..N   -> Cap::Packs::table()[idx-1] - a named knob bundle that can
//              point at any capture method by name (or none, for AUTO).
//              Dropping a pack_yourname.cpp file into src/cap/packs/ (see
//              its README.md) adds a new numbered slot here automatically,
//              same plug-and-play pattern as capture methods.
//   0xFF   -> CUSTOM (fixed sentinel, deliberately NOT N+1 - if it were
//              N+1 it would silently mean a different thing after a
//              firmware update that adds/removes a pack; a fixed byte
//              keeps a saved CUSTOM pack CUSTOM forever)
enum class RadioPack : uint8_t { STOCK = 0, CUSTOM = 0xFF };
static const uint8_t RADIO_PACK_CUSTOM = 0xFF;
// Sanity-clamp bound for NVS load, same role as HS_METHOD_COUNT_MAX above
// (the Packs registry isn't visible from config.h either).
static const uint8_t RADIO_PACK_COUNT_MAX = 8;

// Knobs for LIGHT / AGGRO / EVILPIG — same code, different tune.
struct RadioConfig {
    uint16_t hopMs = 300;      // 50..2000 channel dwell
    uint16_t lockMs = 8000;    // stay on channel after EAPOL (0 = never)
    bool lockOnHs = true;
    bool deauth = true;        // AGGRO / EVILPIG kicks
    bool randomMac = false;
    int8_t minRssi = -85;      // skip weaker APs for kick
    uint8_t hopSet = 0;        // HopSet: ALL / PRI / 1-6-11
    uint8_t hsMethod = 0;      // HsMethod AUTO / OURS / PAN
    uint8_t fallbackSec = 25;  // AUTO: seconds before trying the other method
    uint8_t kickBurst = 2;     // deauth/disassoc rounds per AP
    bool bidirKick = true;     // also spoof client -> AP
    bool eapolTx = true;       // EAPOL-Start / Logoff (works on PMF)
    bool pmkidProbe = true;    // Open-System auth + assoc for PMKID
    bool csaHerd = false;      // spoofed CSA beacon
    bool authFlood = false;    // random-MAC auth flood if no clients
    uint8_t deauthReason = 7;
    uint16_t pauseMs = 1200;   // listen after M1, don't kick
    bool fatPcap = true;       // radiotap with ch / rate / rssi
    uint8_t pack = 0;          // RadioPack last applied
    // Porkchop-style knobs. All default off / safe so existing installs
    // keep their old behavior unless a user opts in.
    uint8_t jitterMs = 0;      // 0..20: random ms between deauth/disassoc (anti-WIDS)
    uint8_t cooldownMs = 0;    // 0..30s: per-AP cooldown after kick (PORKCHOP method)
    int16_t scoreThr = 0;      // -100..200: min score to attack in PORKCHOP method (0 = score all)
    uint16_t dwellMinMs = 120; // 50..600: minimum channel dwell (for PASSIVE-like adaptive hop)
    // How much of the 4-way handshake to insist on before giving up on a
    // target and moving to the next one. M1+M2 is already enough to crack
    // (see Hc22000::hasPair()) - this only controls how patient the lock-
    // on-BSSID logic is about waiting for more before releasing.
    //   0 = PAIR (M1+M2, fastest - default/legacy behavior)
    //   1 = +M3  (also wait for the AP's M3 retransmit)
    //   2 = FULL (wait for the complete M1..M4 exchange)
    uint8_t hsDepth = 0;
    // ----- FOCUS / Porkchop extras (separate RADIO knobs) ----------------
    // DATA ACT: when 1, sniffer counts non-EAPOL data frames per BSSID and
    // FOCUS uses that for the activity term instead of beacon-only bumps.
    // 0 = legacy beacon activity (default, cheaper).
    uint8_t dataAct = 0;
    // STRICT LOCK: when true (default), FOCUS ignores score while a
    // lock-on-BSSID is active and only kicks the locked target. When false,
    // scoring may drift to a higher-scoring neighbor on the same channel.
    bool strictLock = true;
    // DEPTH HOLD: extra seconds to keep the lock after M1+M2 are already
    // on file when hsDepth > 0, so M3/M4 still have a chance to land even
    // if no further EAPOL refreshes the normal lockMs deadline.
    // 0 = off (release on normal lockMs / hasHandshake only).
    uint8_t depthHoldSec = 0;
};

struct BleConfig {
    uint16_t burstMs = 200;    // 50..500 between bursts
    uint16_t advMs = 100;      // 50..200 per advertisement
};

static const uint8_t HOTKEY_COUNT = 16;
// 0-9 old binds; 10-15 empty (user assigns)
struct HotkeyConfig {
    char key[HOTKEY_COUNT] = {
        'a', 'l', 'p', 'e', 'b', 'i', 's', 'h', 'r', 'f',
        0, 0, 0, 0, 0, 0
    };
};
static const uint8_t HOTKEY_RADIO = 8;

struct XferConfig {
    char ssid[33] = "0N3P0rK";
    char pass[65] = "0N3-P0rK";
};

class Config {
public:
    static bool init();
    static bool save();
    static void applyRadioPack(uint8_t pack);
    static void resetRadio();
    // Mark the current radio config as hand-tuned: PACK in the UI flips to
    // CUSTOM, future applyRadioPack() calls from presets stop auto-overwriting
    // the user's knobs. Called by the settings UI whenever any radio knob
    // (other than PACK / HS METHOD) is edited.
    static void markRadioCustom();

    static PersonalityConfig& personality() { return personalityConfig; }
    static RadioConfig& radio() { return radioConfig; }
    static BleConfig& ble() { return bleConfig; }
    static HotkeyConfig& hotkeys() { return hotkeyConfig; }
    static XferConfig& xfer() { return xferConfig; }
    static void setPersonality(const PersonalityConfig& cfg);

    static bool isZombieSkinUnlocked();
    static bool registerNightWolfBite();
    static void becomeZombie();
    static void cureZombie();
    static bool isSDAvailable();

private:
    static PersonalityConfig personalityConfig;
    static RadioConfig radioConfig;
    static BleConfig bleConfig;
    static HotkeyConfig hotkeyConfig;
    static XferConfig xferConfig;
    static bool initialized;
};
