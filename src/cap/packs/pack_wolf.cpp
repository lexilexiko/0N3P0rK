// "LOUD" pack - predator preset: CLIENTS stack + CSA + auth-flood, short
// pause, fast hop, and light jitter. Deeper handshake capture is controlled
// by HS DEPTH; DEPTH HOLD applies only when the user selects a depth above
// M1+M2.
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
    /* hsDepth       */ 0,
    /* dataAct       */ 0,      // CLIENTS does not score; leave off
    /* strictLock    */ true,
    /* depthHoldSec  */ 8,
};

CAP_PACK_REGISTER(wolf, "LOUD", "CLIENTS", kWolfPreset)

} // namespace Packs
} // namespace Cap
