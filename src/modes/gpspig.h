#pragma once
#include <Arduino.h>
#include <M5Unified.h>

// Own-position screen. Grove GPS on the original Cardputer, the GPS UART on
// the ADV LoRa cap. The LoRa radio itself stays off.

namespace GpsMode {

void start();
void stop();
void update();
void draw(M5Canvas& canvas);
bool isRunning();
// UART ring and the trail points. Zero after stop().
size_t heapHeld();
void getStatusLine(char* out, size_t n);

}  // namespace GpsMode
