// WpsPig — WPS PIN brute-force mode for M5Cardputer ADV (ESP32-S3).
// Uses only standard ESP-IDF esp_wifi WPS API — no external library.
// Lab / authorized networks only.
#pragma once
#include <Arduino.h>
#include <M5Unified.h>

class WpsPigMode {
public:
    static void init();
    static void start();
    static void stop();
    static void update();
    static void draw(M5Canvas& canvas);
    static void getStatusLine(char* out, size_t cap);
    static bool isRunning() { return s_running; }
private:
    static bool s_running;
};
