// "WPASEC" pack — drop-in preset for the WPASEC method. Same Preset fields
// as every other pack_*.cpp; nothing else in the tree needs to change.
#include "pack_ctx.h"

namespace Cap {
namespace Packs {

static const Preset kWpasecPreset{
    /* bidirKick     */ true,
    /* eapolTx       */ true,
    /* pmkidProbe    */ true,
    /* csaHerd       */ false,
    /* authFlood     */ false,
    /* kickBurst     */ 3,
    /* pauseMs       */ 1200,
    /* lockMs        */ 15000,
    /* hopMs         */ 250,
    /* jitterMs      */ 3,
    /* cooldownSec   */ 8,
    /* scoreThr      */ 0,
    /* hsDepth       */ 0,
    /* dataAct       */ 1,
    /* strictLock    */ true,
    /* depthHoldSec  */ 0,
};

CAP_PACK_REGISTER(wpasec, "WPASEC", "WPASEC", kWpasecPreset)

} // namespace Packs
} // namespace Cap
