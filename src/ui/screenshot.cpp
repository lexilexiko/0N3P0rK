#include "screenshot.h"
#include "display.h"
#include "../core/config.h"
#include "../core/sd_layout.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace Screenshot {

static const int RETRY_N = 3;
static const int RETRY_MS = 10;
static const uint32_t DEBOUNCE_MS = 400;

static bool s_busy = false;
static bool s_keyWas = false;
static uint32_t s_lastMs = 0;

bool busy() { return s_busy; }

static uint16_t nextNumber(const char* dir) {
    uint16_t maxNum = 0;
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return 1;
    File entry;
    while ((entry = d.openNextFile())) {
        const char* name = entry.name();
        entry.close();
        const char* base = strrchr(name, '/');
        base = base ? base + 1 : name;
        size_t n = strlen(base);
        if (n > 14 && strncmp(base, "screenshot", 10) == 0 &&
            strcmp(base + n - 4, ".bmp") == 0) {
            char buf[8];
            size_t len = n - 14;
            if (len < sizeof(buf)) {
                memcpy(buf, base + 10, len);
                buf[len] = '\0';
                uint16_t v = (uint16_t)atoi(buf);
                if (v > maxNum) maxNum = v;
            }
        }
    }
    d.close();
    return (uint16_t)(maxNum + 1);
}

bool take() {
    if (s_busy) return false;
    uint32_t now = millis();
    if (s_lastMs && (now - s_lastMs) < DEBOUNCE_MS) return false;

    if (!Config::isSDAvailable()) {
        Display::setTopBarMessage("NO SD CARD", 2000);
        return false;
    }

    s_busy = true;

    const char* dir = SDLayout::screenshotsDir();
    if (!SD.exists(dir)) SD.mkdir(dir);

    uint16_t num = nextNumber(dir);
    char path[48];
    snprintf(path, sizeof(path), "%s/screenshot%03d.bmp", dir, num);
    Serial.printf("[SNAP] %s\n", path);

    File file;
    for (int i = 0; i < RETRY_N; i++) {
        file = SD.open(path, FILE_WRITE);
        if (file) break;
        delay(RETRY_MS);
    }
    if (!file) {
        Display::setTopBarMessage("SD WRITE FAILED", 2500);
        s_busy = false;
        return false;
    }

    const int w = DISPLAY_W;
    const int h = DISPLAY_H;
    const uint32_t pad = (4 - (3 * w) % 4) % 4;
    uint32_t filesize = 54 + (3 * w + pad) * h;

    unsigned char hdr[54] = {
        'B', 'M',
        0, 0, 0, 0,
        0, 0, 0, 0,
        54, 0, 0, 0,
        40, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        1, 0,
        24, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    for (uint32_t i = 0; i < 4; i++) {
        hdr[2 + i]  = (unsigned char)((filesize >> (8 * i)) & 0xFF);
        hdr[18 + i] = (unsigned char)((w >> (8 * i)) & 0xFF);
        hdr[22 + i] = (unsigned char)((h >> (8 * i)) & 0xFF);
    }
    file.write(hdr, 54);

    static unsigned char line[DISPLAY_W * 3 + 4];
    for (uint32_t i = (uint32_t)w * 3; i < (uint32_t)w * 3 + pad; i++)
        line[i] = 0;

    // Same as M5PORKCHOP: read panel GRAM bottom-up, RGB → BGR.
    for (int y = h - 1; y >= 0; y--) {
        M5.Display.readRectRGB(0, y, w, 1, line);
        for (int x = 0; x < w; x++) {
            unsigned char t = line[x * 3];
            line[x * 3] = line[x * 3 + 2];
            line[x * 3 + 2] = t;
        }
        file.write(line, w * 3 + pad);
    }
    file.close();

    s_lastMs = millis();
    s_busy = false;

    char msg[32];
    snprintf(msg, sizeof(msg), "SNAP! #%d", num);
    Display::setTopBarMessage(msg, 2000);
    Serial.printf("[SNAP] saved %s (%lu bytes)\n", path, (unsigned long)filesize);
    return true;
}

void poll() {
    char k = Config::hotkeys().key[HOTKEY_SLOT];
    if (!k) {
        s_keyWas = false;
        return;
    }
    char up = k;
    if (up >= 'a' && up <= 'z') up = (char)(up - 'a' + 'A');
    bool down = M5Cardputer.Keyboard.isKeyPressed(k) ||
                (up != k && M5Cardputer.Keyboard.isKeyPressed(up));
    if (down && !s_keyWas && !s_busy) take();
    s_keyWas = down;
}

} // namespace Screenshot
