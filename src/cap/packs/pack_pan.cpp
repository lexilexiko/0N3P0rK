// "NORMAL" pack — balanced aggressive tuning with CLIENTS method:
// bidirectional kick, EAPOL-Start/Logoff, PMKID probe. Light jitter and a
// short DEPTH HOLD so M3 has a chance after the pair lands.
#include "pack_ctx.h"

namespace Cap {
namespace Packs {

static const Preset kPanPreset{
    /* bidirKick     */ true,
    /* eapolTx       */ true,
    /* pmkidProbe    */ true,
    /* csaHerd       */ false,
    /* authFlood     */ false,
    /* kickBurst     */ 3,
    /* pauseMs       */ 1500,
    /* lockMs        */ 10000,
    /* hopMs         */ 250,
    /* jitterMs      */ 2,      // mild anti-WIDS spacing
    /* cooldownSec   */ 0,      // CLIENTS does not use per-AP cooldown
    /* scoreThr      */ 0,
    /* hsDepth       */ 0,      // pair is enough for NORMAL
    /* dataAct       */ 0,
    /* strictLock    */ true,
    /* depthHoldSec  */ 5,      // short hold after pair for late M3
};

CAP_PACK_REGISTER(pan, "NORMAL", "CLIENTS", kPanPreset)

} // namespace Packs
} // namespace Cap
