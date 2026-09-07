// SAFE preset - conservative, append-safe PCAP capture profile.
#include "pack_ctx.h"

namespace Cap {
namespace Packs {

static const Preset kSafePreset{
    /* bidirKick     */ false,
    /* eapolTx       */ false,
    /* pmkidProbe    */ true,
    /* csaHerd       */ false,
    /* authFlood     */ false,
    /* kickBurst     */ 1,
    /* pauseMs       */ 1200,
    /* lockMs        */ 8000,
    /* hopMs         */ 300,
    /* jitterMs      */ 0,
    /* cooldownSec   */ 0,
    /* scoreThr      */ 0,
    /* hsDepth       */ 0,
    /* dataAct       */ 0,
    /* strictLock    */ true,
    /* depthHoldSec  */ 0,
    /* ringSlots     */ 32,
    /* flushEvery    */ 1,
    /* writeRetry    */ 3,
    /* magicCheck    */ true,
    /* sizeVerify    */ true,
    /* protectPcap   */ true,
    /* learnRename   */ true,
    /* migrateNames  */ true,
    /* autoRepair    */ true,
    /* rollbackWrite */ true,
    /* frameLimit    */ 512,
};

CAP_PACK_REGISTER(safe, "SAFE", nullptr, kSafePreset)

} // namespace Packs
} // namespace Cap
