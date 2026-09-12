#pragma once

#include <Arduino.h>
#include <M5Unified.h>

class LootMenu {
public:
    static void show();
    static void openWpaSec();
    static void openPwncrack();
    static void hide();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isActive() { return active; }
    static const char* getBottomHint();

private:
    enum class Tab : uint8_t { WPASEC = 0, PWNCRACK = 1 };

    static const uint8_t PAGE_SIZE = 48; // same footprint that already worked fine

    static bool active;
    static bool keyWasPressed;
    static bool detailView;
    static bool syncModal;
    static bool diagModal;
    static Tab tab;
    static uint8_t selected;
    static uint8_t scroll;
    static uint8_t count;       // matches on the CURRENT page only (0..PAGE_SIZE)
    static uint8_t page;        // 0-based page index into the current tab's file listing
    static bool hasMore;        // true if at least one more matching file exists past this page
    static uint16_t totalItems; // cheap total count across all pages, for the summary line

    static void scan();
    static void countTotal();
    static void gotoPage(uint8_t newPage, bool landOnLast);
    static void handleInput();
    static void startSync(bool oneFile = false);
    static void startPullResults();
    static void runDiag();
    static void deleteSelected();
    static void reloadList();
};
