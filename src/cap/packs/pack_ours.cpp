// "SOFT" pack — light/quiet tuning paired with the ALL capture method.
// Minimal TX: broadcast kick only, no bidir/EAPOL/PMKID/CSA. FOCUS extras
// stay off (ALL does not score); strict lock still on so if a handshake
// starts we do not wander.
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
    /* hsDepth       */ 0,      // PAIR is enough
    /* dataAct       */ 0,
    /* strictLock    */ true,
    /* depthHoldSec  */ 0,
};

CAP_PACK_REGISTER(ours, "SOFT", "ALL", kOursPreset)

} // namespace Packs
} // namespace Cap
