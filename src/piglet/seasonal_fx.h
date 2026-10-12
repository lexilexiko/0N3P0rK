// Seasonal decorative FX — separate from core weather (rain/snow/clouds/thunder).
// Put snow banks, autumn leaf piles, butterflies, pollen, etc. HERE so weather.cpp stays stable.
//
// Seasons from Weather::getActiveSeason() / Config SEASON setting.
// Call after Weather::update() and draw after Weather::draw() (on top of precip).
#pragma once

#include <M5Unified.h>

namespace SeasonalFx {

// === Lifecycle ===
void init();
void update();
// Start the rare seasonal meteor shower immediately (ANIM TEST hook).
void triggerMeteorShower();
// Foreground season decor (banks, leaves, butterflies)
void draw(M5Canvas& canvas);
// Backdrop season decor — call after clouds, BEFORE tree/pig
// (spring lightning bolts live here so they sit in the sky)
void drawBackdrop(M5Canvas& canvas);

// Scroll world-space FX with grass treadmill (snow banks, etc.)
// Same sign as Trees::scroll: +1 / -1 per grass pixel step.
void scroll(int dir);

// Pig walks through snow banks → trample path (melt under feet)
// pigFeetX = avatar feet X on screen
void trampleSnow(int pigFeetX);

// Optional: clear all FX (mode changes, tests)
void reset();

// Spring  — lightning bolts (backdrop) + thunder storms
// Summer  — butterflies + golden pollen motes (unique heat shimmer)
// Autumn  — dense falling leaves that accumulate into interactive leaf piles
// Winter  — snow banks grow only while snowing; trampling carves a lasting path

}  // namespace SeasonalFx
