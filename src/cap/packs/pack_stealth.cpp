// "QUIET" pack - zero deauth. Only PMKID probe (open auth + assoc). Radio
// equivalent of DO NO HAM: sit, watch EAPOL, gently knock for PMKID.
// No scoring, no depth hold, no jitter - pure passive + soft probe.
#include "pack_ctx.h"

namespace Cap {
namespace Packs {

static const Preset kStealthPreset{
    /* bidirKick     */ false,  // no deauth/disassoc
    /* eapolTx       */ false,
    /* pmkidProbe    */ true,   // the whole point
    /* csaHerd       */ false,
    /* authFlood     */ false,
    /* kickBurst     */ 1,      // ignored (no kick)
    /* pauseMs       */ 2000,
    /* lockMs        */ 8000,
    /* hopMs         */ 400,    // sweep channels
    /* jitterMs      */ 0,
    /* cooldownSec   */ 0,
    /* scoreThr      */ 0,
    /* hsDepth       */ 0,      // any pair is a win
    /* dataAct       */ 0,
    /* strictLock    */ true,   // if HS starts, stay put
    /* depthHoldSec  */ 0,
};

// nullptr method = leave HS METHOD on AUTO / user's choice (quiet works
// with ALL or FOCUS; both honor the no-TX stealth guard).
CAP_PACK_REGISTER(stealth, "QUIET", nullptr, kStealthPreset)

} // namespace Packs
} // namespace Cap
