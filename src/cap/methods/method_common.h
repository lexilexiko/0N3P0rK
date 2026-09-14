// cap/methods/method_common.h
// Shared target-selection helpers so EVERY method can honestly honor the
// universal RADIO knobs (SCORE THR, DATA ACT, COOLDOWN, STRICT LK) — not
// just FOCUS. Pure helpers; they register no method of their own.
#pragma once
#include "method_ctx.h"

namespace Cap {
namespace Methods {

// Selective mode: when ctx.scoreThr != 0 this returns the index (into
// ctx.beacons) of the single best target for this tick, honoring
// ownAP/pin/skip/rssi/handshake filters plus STRICT LK lock-on-BSSID.
// Returns -1 when scoreThr == 0 (caller should kick everything as usual)
// or when no target passes the filters this tick.
int8_t commonFocusTarget(const Ctx& ctx);

// Universal per-AP cooldown after kick (COOLDOWN knob, seconds). Call
// commonRememberKick(bssid) right after actually kicking a target; later
// commonInCooldown() returns true for it until cooldownSec has elapsed.
bool commonInCooldown(const uint8_t* bssid, uint32_t cooldownSec);
void commonRememberKick(const uint8_t* bssid);
void commonResetCooldown();

}
}