#pragma once

#include <M5Unified.h>

namespace GpsAntenna {

void init();
void draw(M5Canvas& canvas, bool enabled, bool hasFix);
bool tryStomp(int pigFeetX, bool airborne);

}  // namespace GpsAntenna
