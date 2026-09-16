// "FOCUS" pack - Porkchop-style tuning paired with the FOCUS capture method:
// score-and-focus single-target bursts, bidirectional kick, EAPOL TX,
// PMKID probe, long lock-on-BSSID, data-frame activity, strict lock, and
// depth hold so M3/M4 still land after the pair.
//
// Recommended setup for handshake hunting (closest to M5PORKCHOP OINK).
#include "pack_ctx.h"

namespace Cap {
namespace Packs {

static const Preset kPorkchopPreset{
    /* bidirKick     */ true,   // AP->STA + STA->AP + deauth + disassoc
    /* eapolTx       */ true,   // pump EAPOL-Start/Logoff
    /* pmkidProbe    */ true,   // try to coax a PMKID
    /* csaHerd       */ false,
    /* authFlood     */ false,
    /* kickBurst     */ 3,
    /* pauseMs       */ 900,
    /* lockMs        */ 15000,  // 15s - ride out EAPOL retries
    /* hopMs         */ 250,
    /* jitterMs      */ 3,      // anti-WIDS spacing on broadcast path
    /* cooldownSec   */ 8,      // per-AP cooldown (FOCUS scoring)
    /* scoreThr      */ 0,      // attack anything that scores
    /* hsDepth       */ 1,      // wait for +M3 when possible
    /* dataAct       */ 1,      // real data frames feed activity score
    /* strictLock    */ true,   // never drift off locked BSSID
    /* depthHoldSec  */ 8,      // hold after pair to collect M3/M4
};

CAP_PACK_REGISTER(porkchop, "FOCUS", "FOCUS", kPorkchopPreset)

} // namespace Packs
} // namespace Cap
