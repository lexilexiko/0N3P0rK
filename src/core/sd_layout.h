#pragma once
#include <Arduino.h>
#include <SD.h>

namespace SDLayout {
inline bool usingNewLayout() { return true; }
inline void setUseNewLayout(bool) {}

inline const char* rootDir() { return "/0N3P0rK"; }
inline const char* handshakesDir() { return "/0N3P0rK/handshakes"; }
inline const char* passworldDir() { return "/0N3P0rK/Passworld"; }
inline const char* pigpassDir() { return "/0N3P0rK/pigpass"; }
inline const char* pigpassResultsPath() { return "/0N3P0rK/pigpass/cracked.txt"; }
inline const char* pigpassCheckpointPath() { return "/0N3P0rK/pigpass/checkpoint.txt"; }
inline const char* pigpassLastWordlistPath() { return "/0N3P0rK/pigpass/last_wl.txt"; }
inline const char* evilpigDir() { return "/0N3P0rK/evilpig"; }
inline const char* evilpigCredsPath() { return "/0N3P0rK/evilpig/creds.csv"; }
inline const char* screenshotsDir() { return "/0N3P0rK/screenshots"; }
inline const char* inspectorDir() { return "/0N3P0rK/inspector"; }

inline void ensureDirs() {
    SD.mkdir("/0N3P0rK");
    SD.mkdir("/0N3P0rK/handshakes");
    SD.mkdir("/0N3P0rK/wpa-sec");
    SD.mkdir("/0N3P0rK/pwncrack");
    SD.mkdir("/0N3P0rK/ohc");
    SD.mkdir("/0N3P0rK/inspector");
    SD.mkdir("/0N3P0rK/pigpass");
    SD.mkdir("/0N3P0rK/Passworld");
    SD.mkdir("/0N3P0rK/evilpig");
    SD.mkdir("/0N3P0rK/ir");
    SD.mkdir("/0N3P0rK/wolf");
    SD.mkdir("/0N3P0rK/talk");
    SD.mkdir("/0N3P0rK/music");
    SD.mkdir("/0N3P0rK/screenshots");
}
}
