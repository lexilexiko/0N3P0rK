#include "method_ctx.h"

namespace Cap {
namespace Methods {

// Keep the normal lightweight kick behavior, but pair it with the strict
// capture profile so only valid WPA-Key M1/M2 frames reach the PCAP.
void minWpaKick(const Ctx& ctx) {
    ours(ctx);
}

CAP_METHOD_REGISTER_MIN_WPA("MINWPA", minWpaKick, nullptr, nullptr)

} // namespace Methods
} // namespace Cap
