// LIGHT preset — passive channel scan with a 30-second dwell per channel.
#include "pack_ctx.h"

namespace Cap {
namespace Packs {

static const Preset kLightPreset{
    /* bidirKick     */ false,
    /* eapolTx       */ false,
    /* pmkidProbe    */ false,
    /* csaHerd       */ false,
    /* authFlood     */ false,
    /* kickBurst     */ 1,
    /* pauseMs       */ 800,
    /* lockMs        */ 5000,
    /* hopMs         */ 30000,
    /* jitterMs      */ 0,
    /* cooldownSec   */ 0,
    /* scoreThr      */ 0,
    /* hsDepth       */ 0,
    /* dataAct       */ 0,
    /* strictLock    */ true,
    /* depthHoldSec  */ 0,
};

CAP_PACK_REGISTER(light, "LIGHT", nullptr, kLightPreset)

} // namespace Packs
} // namespace Cap
