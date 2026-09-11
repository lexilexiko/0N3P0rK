// Interface between the orchestrator (cap/sniffer.cpp) and the individual
// capture methods in this folder. sniffer.cpp keeps owning all the session
// state (beacon table, current channel, config knobs, sequence counters) and
// just hands each method a read-only Ctx snapshot every time it's called —
// methods stay free of sniffer.cpp's statics, so they're readable and
// testable on their own, and adding a new one never touches the others.
//
// To add a method: create method_yourname.cpp in this folder implementing
// one function below, add it to platformio's build (picked up automatically,
// same as the other .cpp here), and call it from Cap's dispatch alongside
// ours()/pan(). Nothing else in this folder needs to change.
#pragma once

#include <stdint.h>
#include "beacon_slot.h"

namespace Cap {
namespace Methods {

struct Ctx {
    BeaconSlot*    beacons;
    uint8_t        beaconCount;
    uint8_t        channel;
    int8_t         minRssi;
    uint8_t        kickBurst;
    uint8_t        deauthReason;
    bool           bidirKick;
    bool           eapolTx;
    bool           pmkidProbe;
    bool           csaHerd;
    bool           authFlood;
    const uint8_t* kickBssid;
    const uint8_t* kickSta;
    bool           kickStaOk;
    const uint8_t* bcast;     // FF:FF:FF:FF:FF:FF, passed through for convenience

    bool (*isOwnAp)(const uint8_t* bssid);
    bool (*skipPin)(const uint8_t* bssid);
    // Session skip list (Z key): true => do not attack this BSSID until
    // Cap session restarts. May be nullptr on older callers.
    bool (*isSkipped)(const uint8_t* bssid);
    void (*sendRawMgmt)(uint8_t fc0, const uint8_t* bssid, const uint8_t* dest);

    uint32_t* framesDeauth; // counter to bump on every injected frame

    // ----- Porkchop-style knobs ------------------------------------------
    // All default to 0 (= off / use the legacy behavior baked into OURS,
    // PAN, CSA, PMKID). Existing methods don't read these — they're here
    // so new methods (e.g. PORKCHOP, KARMA, PASSIVE) can opt into the
    // Porkchop tunables from the RADIO menu without each method having
    // to reach back into Config::radio() on its own.
    uint8_t  jitterMs;        // 0..20: random ms between mgmt frames
    uint8_t  cooldownSec;     // 0..30: per-AP cooldown after kick (seconds)
    int16_t  scoreThr;        // -100..200: minimum score to attack
    uint16_t dwellMinMs;      // 50..600: minimum channel dwell
    uint8_t  hsDepth;         // 0=PAIR(M1+M2) 1=+M3 2=FULL(M1-M4) - see Hc22000::hasHandshake()

    // ----- Lock-on-BSSID focus -------------------------------------------
    // When the sniffer is parked on a target BSSID's channel waiting for
    // M2/M3/M4 of a 4-way handshake, methods that pick a target by score
    // (notably PORK) can drift to a higher-scoring neighbor AP and keep
    // kicking it while we wait for OUR target to finish. The original
    // M5PORKCHOP solves this with a single-target focus mode that
    // suppresses the score comparison; we don't have that knob yet, so
    // the sniffer advertises the locked BSSID through Ctx and any
    // scoring method is expected to honor it (treat it as the only
    // candidate for this tick). Methods that don't read these fields
    // (OURS, PAN, CSA, PMKID) keep their existing behavior.
    uint8_t  lockedBssid[6];  // valid only when lockedBssidActive is true
    bool     lockedBssidActive;

    // ----- FOCUS extras (RADIO knobs) ------------------------------------
    // dataAct: true => activity term prefers BeaconSlot::dataRecent (data
    // frames) over beacon-only bumps. false => legacy beacon activity.
    bool     dataAct;
    // strictLock: true => scoring methods MUST only kick lockedBssid while
    // lockedBssidActive (ignore score). false => may drift by score.
    bool     strictLock;
    // depthHoldSec: passed through for methods that care; the sniffer
    // also uses it to extend lock-on-BSSID after a pair lands.
    uint8_t  depthHoldSec;
};

// Greedy broadcast/targeted deauth on every AP on the current channel.
// Cheapest method, no client-list tracking required. Skips PMF-capable APs.
void ours(const Ctx& ctx);

// Fuller stack: targeted per-client bidirectional kick, EAPOL-Start/Logoff,
// auth-flood fallback when nothing responds, optional CSA-herd disruption
// for PMF-capable APs it can't deauth directly.
void pan(const Ctx& ctx);

// Open-System auth + association attempt to pull a PMKID from the AP's
// association response — works even against PMF-capable APs since it's a
// real (unspoofed on our side) association attempt, not a forged deauth.
// Owns small internal state (round-robin index, rate limit); call
// resetPmkidState() when a capture session starts/stops.
void pmkidProbe(const Ctx& ctx);
void resetPmkidState();

// CSA-herd (also registered as its own method "CSA"). Callable from other
// methods (e.g. PORK) when ctx.csaHerd is set, so packs like AGGRO work
// with any targeting method without sniffer changes.
void csaHerd(const Ctx& ctx);
void resetCsaHerdState();

// ---- Registry ------------------------------------------------------------
// Single source of truth for every capture method. sniffer.cpp, settings
// menu and AUTO-rotation all read from this table.
//
// Adding a method now means ONE line at the bottom of a .cpp — no edits
// to this header, no edits to the registry source, no edits to sniffer/
// settings_menu/config. Just drop the file in src/cap/methods/ and the
// static Registrar constructor below adds it before main() runs:
//
//     CAP_METHOD_REGISTER("MYNAME", myname_kick, nullptr, nullptr)
//
// Order in the UI / AUTO rotation is the link order of .cpp files (left
// to right, OURS first because it lives next to the README). Rename the
// file if you ever need a specific slot.
//
// Want the new method selectable as its own "Pack" in the Radio menu too
// (a named knob bundle, not just the bare method)? See pack_ctx.h -
// CAP_PACK_REGISTER() is the same one-line pattern, fully independent of
// this table (a pack can point at any method by name, or none at all).
struct Entry {
    const char* name;                       // 4..7 chars, fits in Counters::methodTag[8]
    void      (*kick) (const Ctx& ctx);     // required, runs every kick tick
    void      (*probe)(const Ctx& ctx);     // optional, nullptr if N/A
    void      (*reset)();                   // optional, nullptr if N/A
};
typedef void (*probe_fn)(const Ctx&);
typedef void (*reset_fn)();

const Entry* table(uint8_t* outCount);      // returns the live table, writes length
uint8_t      count();                       // convenience
const Entry* findByName(const char* name);  // case-sensitive, nullptr if none
const char*  name(uint8_t idx);             // nullptr if idx out of range
void         resetAll();                    // calls each entry's reset() if any

// Used internally by CAP_METHOD_REGISTER(); don't call from app code.
void add(const Entry& e);

// Auto-register macro. Any of KICK/PROBE/RESET can be `nullptr`.
// KICK/PROBE/RESET come in as the raw tokens in the call site. We cast
// them through `uintptr_t` (allowed by the standard, no UB for null
// pointers or function pointers) and then back to the typed slot —
// function-pointer round-trips via integer are the one path the standard
// explicitly blesses. The static `register_##KICK` initialiser runs
// before main(), so the method is in the table by the time Cap::begin()
// runs.
#define CAP_METHOD_REGISTER(NAME, KICK, PROBE, RESET) \
    static int register_##KICK = ( \
        Cap::Methods::add(Cap::Methods::Entry{ \
            (NAME), \
            (Cap::Methods::probe_fn)(uintptr_t)(KICK), \
            (Cap::Methods::probe_fn)(uintptr_t)(PROBE), \
            (Cap::Methods::reset_fn)(uintptr_t)(RESET) }), \
        0);

} // namespace Methods
} // namespace Cap
