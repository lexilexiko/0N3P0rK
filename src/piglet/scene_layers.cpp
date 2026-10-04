#include "scene_layers.h"
#include <Arduino.h>

namespace SceneLayers {

bool pig = true;
bool grassBack = true;
bool grassFront = true;
bool underground = true;
bool trees = true;
bool sky = true;
bool weather = true;
bool seasonFx = true;
bool mood = true;
bool wolf = true;
bool cpuHud = false;

static uint32_t s_frameStartUs = 0;
static uint32_t s_lastFrameUs = 0;
static uint32_t s_avgUs = 0;
static uint8_t s_cpuPct = 0;
static uint16_t s_frameMs = 0;

// ~30 fps budget: 33.3ms = 100% scene load
static constexpr uint32_t kBudgetUs = 33000;

void init() {
    pig = grassBack = grassFront = underground = trees = sky = weather =
        seasonFx = mood = wolf = true;
    cpuHud = false;
    s_avgUs = 0;
    s_cpuPct = 0;
    s_frameMs = 0;
}

void setAll(bool on) {
    pig = grassBack = grassFront = underground = trees = sky = weather =
        seasonFx = mood = wolf = on;
}

void beginFrame() {
    s_frameStartUs = micros();
}

void endFrame() {
    uint32_t now = micros();
    uint32_t dt = now - s_frameStartUs;
    // Handle micros wrap
    if (now < s_frameStartUs) dt = now + (0xFFFFFFFFu - s_frameStartUs) + 1u;
    s_lastFrameUs = dt;
    s_frameMs = (uint16_t)(dt / 1000u);
    if (s_frameMs > 999) s_frameMs = 999;

    // EMA: smooth the meter
    if (s_avgUs == 0) s_avgUs = dt;
    else s_avgUs = (s_avgUs * 7u + dt) / 8u;

    uint32_t pct = (s_avgUs * 100u) / kBudgetUs;
    if (pct > 100u) pct = 100u;
    s_cpuPct = (uint8_t)pct;
}

uint8_t getCpuPct() { return s_cpuPct; }
uint16_t getFrameMs() { return s_frameMs; }

}  // namespace SceneLayers
