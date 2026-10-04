#pragma once
// Ground layer: turf / pavement / pebbles + world treadmill scroll.
// Trees/bushes → trees.cpp | season FX → seasonal_fx.cpp | pig → avatar.cpp
#include <Arduino.h>
#include <M5Unified.h>
#include <stdint.h>

namespace Ground {

static constexpr uint8_t BLADE_COUNT = 80;
static constexpr int16_t STRIDE = 3;

struct Blade {
    int8_t lean;
    uint8_t height;
    uint8_t width;
    uint8_t kind;
    uint8_t shade;
    uint16_t windPhase;
    bool frontLayer;
};

struct DrawCtx {
    int16_t pigX = 60;
    int16_t pigLift = 0;
    bool pigOnGround = true;
    bool treeColliding = false;
    int16_t treeScreenX = -1;
    bool attackShake = false;
    bool attackShakeStrong = false;
};

void begin();
void resetBlades();
void setSpeed(uint16_t ms);
uint16_t getSpeed();
void updateScroll(bool moving, bool directionRight, int steps);
void draw(M5Canvas& canvas, bool frontLayer, const DrawCtx& ctx);
int16_t offset();

}  // namespace Ground
