// "MAX" pack - loud, fast, focused. Everything on, short pause, fast hop.
// Pairs with FOCUS so score-and-focus still picks ONE target per tick, but
// each kick is a heavy bidirectional burst. Visible to any WIDS.
#include "pack_ctx.h"

namespace Cap {
namespace Packs {

static const Preset kAggroPreset{
    /* bidirKick     */ true,
    /* eapolTx       */ true,
    /* pmkidProbe    */ true,
    /* csaHerd       */ true,   // herd stragglers
    /* authFlood     */ true,   // auth spam fallback
    /* kickBurst     */ 5,
    /* pauseMs       */ 500,
    /* lockMs        */ 10000,
    /* hopMs         */ 150,    // ~7 channels/sec
    /* jitterMs      */ 1,      // minimal spacing - volume over stealth
    /* cooldownSec   */ 4,      // short cooldown, keep pressure
    /* scoreThr      */ -20,    // attack weaker scores too
    /* hsDepth       */ 1,      // still want +M3 when we can
    /* dataAct       */ 1,      // busy APs first
    /* strictLock    */ true,
    /* depthHoldSec  */ 10,     // aggressive hold after pair
};

CAP_PACK_REGISTER(aggro, "MAX", "FOCUS", kAggroPreset)

} // namespace Packs
} // namespace Cap
