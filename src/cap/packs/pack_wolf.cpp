// "LOUD" pack - predator preset: CLIENTS stack + CSA + auth-flood, short
// pause, fast hop. Depth hold + light jitter so handshakes still complete
// under heavy TX.
#include "pack_ctx.h"

namespace Cap {
namespace Packs {

static const Preset kWolfPreset{
    /* bidirKick     */ true,
    /* eapolTx       */ true,
    /* pmkidProbe    */ true,
    /* csaHerd       */ true,
    /* authFlood     */ true,
    /* kickBurst     */ 5,
    /* pauseMs       */ 600,
    /* lockMs        */ 12000,
    /* hopMs         */ 150,
    /* jitterMs      */ 1,
    /* cooldownSec   */ 3,      // CLIENTS mostly ignores; kept for FOCUS if swapped
    /* scoreThr      */ 0,
    /* hsDepth       */ 1,
    /* dataAct       */ 0,      // CLIENTS does not score; leave off
    /* strictLock    */ true,
    /* depthHoldSec  */ 8,
};

CAP_PACK_REGISTER(wolf, "LOUD", "CLIENTS", kWolfPreset)

} // namespace Packs
} // namespace Cap
