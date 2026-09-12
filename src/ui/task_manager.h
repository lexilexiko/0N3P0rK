#pragma once

#include <M5Unified.h>

namespace TaskManager {

void begin();
void start();
void stop();
void update();
void draw(M5Canvas& canvas);
bool isRunning();

}  // namespace TaskManager
