// Interface for the "Pack" registry (Radio menu's PACK item + bottom bar's
// P: tag). Mirrors src/cap/methods/method_ctx.h's plug-and-play pattern
// exactly, but for named general-radio knob bundles instead of capture
// algorithms.
//
// A pack = a display name + which capture method to select (by name, or
// none for AUTO) + a bundle of general-radio knobs (bidir kick, EAPOL TX,
// kick burst, pause/lock/hop timing, FOCUS extras...).
//
// To add a pack: create pack_yourname.cpp in this folder with one
// CAP_PACK_REGISTER() line. Nothing else needs to change — see README.md.
#pragma once

#include <stdint.h>

namespace Cap {
namespace Packs {

// General-radio knobs a pack applies on top of picking a method. Every
// field has a safe, non-aggressive default, so a pack only needs to name
// the fields it actually wants to change.
//
// Note: explicit constructor instead of C++14/17 default member
// initialisers. The ESP32 Arduino toolchain defaults to C++14, where
// default member initialisers disqualify a struct from being an
// aggregate, breaking `Preset{...}` brace-init. A plain constructor
// keeps both `Preset{}` and `Preset{a, b, ...}` working, which the
// auto-register macro relies on.
struct Preset {
    bool     bidirKick;
    bool     eapolTx;
    bool     pmkidProbe;
    bool     csaHerd;
    bool     authFlood;
    uint8_t  kickBurst;
    uint16_t pauseMs;
    uint16_t lockMs;
    uint16_t hopMs;
    // ----- FOCUS / Porkchop extras (same RADIO knobs) --------------------
    uint8_t  jitterMs;       // 0..20 anti-WIDS gap between mgmt frames
    uint8_t  cooldownSec;    // 0..30 per-AP cooldown after kick (FOCUS)
    int16_t  scoreThr;       // -100..200 min score to attack (FOCUS)
    uint8_t  hsDepth;        // 0=PAIR 1=+M3 2=FULL
    uint8_t  dataAct;        // 0=beacon activity 1=data-frame activity (FOCUS)
    bool     strictLock;     // FOCUS: only kick locked BSSID while lock active
    uint8_t  depthHoldSec;   // extra sec hold after pair when hsDepth>0

    constexpr Preset(bool bk = false, bool et = false, bool pp = false,
                     bool ch = false, bool af = false, uint8_t kb = 2,
                     uint16_t pms = 1200, uint16_t lms = 8000,
                     uint16_t hms = 300,
                     uint8_t jit = 0, uint8_t cd = 0, int16_t thr = 0,
                     uint8_t depth = 0, uint8_t dact = 0,
                     bool slock = true, uint8_t dhold = 0) noexcept
        : bidirKick(bk), eapolTx(et), pmkidProbe(pp), csaHerd(ch),
          authFlood(af), kickBurst(kb), pauseMs(pms), lockMs(lms),
          hopMs(hms),
          jitterMs(jit), cooldownSec(cd), scoreThr(thr),
          hsDepth(depth), dataAct(dact),
          strictLock(slock), depthHoldSec(dhold) {}
};

// ---- Registry ------------------------------------------------------------
// Single source of truth for every pack. The Radio menu's PACK item and
// the bottom bar's P: tag both read straight from this table — nothing
// hardcodes pack names anywhere else.
//
// STOCK (all defaults, hsMethod left on AUTO) and CUSTOM (keep the user's
// hand-tuned knobs) are NOT in this table — they're built-in sentinels
// handled directly by Config::applyRadioPack(), since neither one is a
// "bundle of knobs" the way a real pack is. Everything registered here
// fills the slots between them.
struct Entry {
    const char* name;        // display name, 4..8 chars, shown in menu + bar
    const char* methodName;  // Cap::Methods table name to select, or
                              // nullptr/"" to leave hsMethod on AUTO
    Preset      preset;      // general-radio knobs this pack applies
};

const Entry* table(uint8_t* outCount);      // returns the live table, writes length
uint8_t      count();                       // convenience
const Entry* findByName(const char* name);  // case-sensitive, nullptr if none
const char*  name(uint8_t idx);             // nullptr if idx out of range

// Used internally by CAP_PACK_REGISTER(); don't call from app code.
void add(const Entry& e);

// Auto-register macro. TAG must be a bare identifier unique across every
// pack_*.cpp file (used to build a unique static initialiser name) — it
// does not have to match NAME. METHOD_NAME can be nullptr for "leave
// hsMethod on AUTO". PRESET must be an already-constructed
// Cap::Packs::Preset lvalue (e.g. a `static const` or namespace-local
// variable defined right above the macro call) so that Entry{...}
// initialises its `preset` field by copy, not by trying to construct
// Preset in place from a brace-init-list (which fails because Preset
// has no matching constructor). The static `register_pack_##TAG`
// initialiser runs before main(), same timing as CAP_METHOD_REGISTER.
#define CAP_PACK_REGISTER(TAG, NAME, METHOD_NAME, PRESET) \
    static int register_pack_##TAG = ( \
        Cap::Packs::add(Cap::Packs::Entry{ (NAME), (METHOD_NAME), (PRESET) }), \
        0);

} // namespace Packs
} // namespace Cap
