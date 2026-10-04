// "SOFT" pairs with ALL for passive capture: it disables kick, EAPOL TX,
// PMKID probing and CSA. The sniffer still listens and saves captured data.
#include "pack_ctx.h"

namespace Cap {
namespace Packs {

static const Preset kOursPreset{
    /* bidirKick     */ false,
    /* eapolTx       */ false,
    /* pmkidProbe    */ false,
    /* csaHerd       */ false,
    /* authFlood     */ false,
    /* kickBurst     */ 2,
    /* pauseMs       */ 1200,
    /* lockMs        */ 8000,
    /* hopMs         */ 300,
    /* jitterMs      */ 0,
    /* cooldownSec   */ 0,
    /* scoreThr      */ 0,
    /* hsDepth       */ 0,      // M1+M2; deeper capture is user-selected
    /* dataAct       */ 0,
    /* strictLock    */ true,
    /* depthHoldSec  */ 0,
};

CAP_PACK_REGISTER(ours, "SOFT", "ALL", kOursPreset)

} // namespace Packs
} // namespace Cap
