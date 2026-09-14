// cap/methods/method_common.cpp
// Implementation of method_common.h (see header for the idea).
#include "method_common.h"
#include "../hc22000.h"
#include <Arduino.h>
#include <string.h>

namespace Cap {
namespace Methods {

// ---- universal per-AP cooldown cache -------------------------------------
static const uint8_t CD_SLOTS = 24;
static struct {
    uint8_t bssid[6];
    uint32_t atMs;
    bool used;
} s_cd[CD_SLOTS];

static int cdSlot(const uint8_t* bssid) {
    for (uint8_t i = 0; i < CD_SLOTS; i++)
        if (s_cd[i].used && memcmp(s_cd[i].bssid, bssid, 6) == 0) return (int)i;
    for (uint8_t i = 0; i < CD_SLOTS; i++)
        if (!s_cd[i].used) {
            memset(&s_cd[i], 0, sizeof(s_cd[i]));
            memcpy(s_cd[i].bssid, bssid, 6);
            return (int)i;
        }
    return -1;
}

bool commonInCooldown(const uint8_t* bssid, uint32_t cooldownSec) {
    if (!cooldownSec || !bssid) return false;
    int i = cdSlot(bssid);
    if (i < 0 || !s_cd[i].used) return false;
    return (millis() - s_cd[i].atMs) < (uint32_t)cooldownSec * 1000u;
}

void commonRememberKick(const uint8_t* bssid) {
    if (!bssid) return;
    int i = cdSlot(bssid);
    if (i < 0) return;
    s_cd[i].atMs = millis();
    s_cd[i].used = true;
}

void commonResetCooldown() {
    memset(s_cd, 0, sizeof(s_cd));
}

// ---- shared target score -------------------------------------------------
// Higher RSSI, more clients and (when DATA ACT on) real data frames win;
// already-captured APs (per HS DEPTH) and PMF-capable APs rank last. Same
// flavour as FOCUS's scorer so turning SCORE THR on keeps the same feel
// in ALL / CLIENTS.
static int32_t commonScore(const Ctx& ctx, const BeaconSlot& b) {
    int32_t s = (int32_t)b.rssi;                        // e.g. -75
    int32_t c = (int32_t)b.clientN; if (c > 10) c = 10;
    s += c * 12;
    if (ctx.dataAct) {
        int32_t r = (int32_t)b.dataRecent; if (r > 40) r = 40;
        s += r;
    } else {
        s += 20;                                        // beacon presence = baseline activity
    }
    if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) s = -100000;
    if (b.pmfCapable) s -= 500;
    return s;
}

int8_t commonFocusTarget(const Ctx& ctx) {
    if (ctx.scoreThr == 0) return -1;                   // kick-all mode
    int8_t best = -1;
    int32_t bestScore = -200000;
    for (uint8_t i = 0; i < ctx.beaconCount; i++) {
        const BeaconSlot& b = ctx.beacons[i];
        if (b.channel != ctx.channel) continue;
        if (ctx.isOwnAp(b.bssid)) continue;
        if (ctx.skipPin(b.bssid)) continue;
        if (ctx.isSkipped && ctx.isSkipped(b.bssid)) continue;
        if (b.rssi < ctx.minRssi) continue;
        if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) continue;
        // STRICT LK: while parked on a locked BSSID with strictLock set,
        // only focus that one.
        if (ctx.strictLock && ctx.lockedBssidActive &&
            memcmp(ctx.lockedBssid, b.bssid, 6) != 0) continue;
        int32_t sc = commonScore(ctx, b);
        if (sc < ctx.scoreThr) continue;                // below threshold
        if (sc > bestScore) { bestScore = sc; best = (int8_t)i; }
    }
    return best;
}

}
}