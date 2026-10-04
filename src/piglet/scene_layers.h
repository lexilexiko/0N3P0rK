// Runtime scene layer toggles — SCENE test lab (not NVS).
// Turn pieces off to measure CPU cost of sky/grass/trees/pig/etc.
#pragma once

#include <stdint.h>

namespace SceneLayers {

// Defaults: all visual layers ON, CPU HUD OFF
extern bool pig;
extern bool grassBack;
extern bool grassFront;
extern bool underground;
extern bool trees;
extern bool sky;       // sky gradient backdrop
extern bool weather;   // rain/snow/clouds/birds/wind
extern bool seasonFx;  // banks / leaves / butterflies / pollen
extern bool mood;      // speech bubble monologue
extern bool wolf;      // visitor (also needs personality.wolfEnabled)
extern bool cpuHud;    // show CPU% + frame ms

void init();           // restore defaults
void setAll(bool on);  // master kill-switch (keeps cpuHud as-is)

// Frame-time CPU estimator (call around Display::update work)
void beginFrame();
void endFrame();
uint8_t getCpuPct();     // 0–100, EMA of frame cost vs 33ms budget
uint16_t getFrameMs();   // last full frame ms

}  // namespace SceneLayers
