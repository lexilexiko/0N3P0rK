// LIGHT preset — 1.2.5-style: hop, light kick, short patience. Knobs only.
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
    /* hopMs         */ 300,
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
