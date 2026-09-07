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
    // Cardputer RGB LED (WS2812)
    bool ledEnabled = true;
    uint8_t ledBright = 40;   // 0..100
};

enum class HopSet : uint8_t { ALL = 0, PRIORITY = 1, CORE = 2 };
static const uint8_t HOP_SET_COUNT = 3;

// Capture methods (OURS/PAN/FOCUS/...) removed — Light/Aggro + knobs only.
// Packs = preset knobs. PRO radio page = write/debug fine-tuning.

// Legacy NVS clamp only (methods removed).
static const uint8_t HS_METHOD_COUNT_MAX = 16;

struct RadioConfig {

    uint16_t hopMs = 300;      // 50..60000 channel dwell
    uint16_t lockMs = 8000;    // stay on channel after EAPOL (0 = never)
    bool lockOnHs = true;
    bool deauth = true;        // AGGRO / EVILPIG kicks
    bool randomMac = false;
    int8_t minRssi = -85;      // skip weaker APs for kick
    uint8_t hopSet = 0;        // HopSet: ALL / PRI / 1-6-11
    uint8_t hsMethod = 0;      // DEPRECATED unused (NVS compat, always 0)
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
    // ----- PRO: capture write / ring (sniffer v2) -----------------------
    // Ring depth in the Wi-Fi callback path (compile max 32). Higher =
    // fewer drops under load, more RAM.
    uint8_t ringSlots = 12;     // 8..32
    // Flush SD every N successful packet writes (1 = every packet).
    uint8_t flushEvery = 8;     // 1..32
    // Retry count if a single write returns short (0 = no retry).
    uint8_t writeRetry = 1;     // 0..3
    // On open: refuse / quarantine if magic is not classic pcap.
    bool magicCheck = true;
    // After write: compare File::size() with expected s_fileSize (slower).
    bool sizeVerify = false;
    // Never SD.remove() a pcap that already has a full header (keep data).
    bool protectPcap = true;
    // Allow HIDDEN_*.pcap -> SSID_*.pcap rename when name is learned.
    bool learnRename = true;
    // Fold legacy name variants into preferred path on open.
    bool migrateNames = true;
    bool     autoRepair = true;   // repair an incomplete PCAP tail before append
    bool     rollbackWrite = true; // repair a partial packet after a short write
    // PRO debug
    bool logSd = false;      // extra Serial [CAP] lines
    bool showDrops = false;  // bottom bar shows framesDropped
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
