#include "task_manager.h"
#include "loot_menu.h"
#include "display.h"
#include "keys.h"
#include "../cap/sniffer.h"
#include "../cap/hc22000.h"
#include "../modes/badusb.h"
#include "../modes/blepig.h"
#include "../modes/evilpig.h"
#include "../modes/filemgr.h"
#include "../modes/inspectorpig.h"
#include "../modes/irport.h"
#include "../modes/pigpass.h"
#include "../modes/spectrum.h"
#include "../modes/usbsd.h"
#include "../modes/xfer.h"
#include "../modes/mp3player.h"
#include "../storage/littlefs_ops.h"
#include "../sync/pwncrack.h"
#include "../sync/wpasec.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <stdio.h>

namespace TaskManager {

namespace {

enum Row : uint8_t {
    RADIO, WIFI, BLE, IR, EVILPIG, PIGPASS, SPECTRUM, USBSD, FILEMGR, XFER,
    BADUSB, WPA_SYNC, PWN_SYNC, LOOT, MP3, INSPECTOR, STOP_ALL, ROW_COUNT
};

static constexpr uint8_t VISIBLE_ROWS = 5;
bool s_running = false;
uint8_t s_selected = 0;
uint8_t s_scroll = 0;
bool s_keyWas = false;
uint32_t s_lastStatsMs = 0;
size_t s_freeHeap = 0;
size_t s_largestBlock = 0;
size_t s_minFreeHeap = 0;
size_t s_internalFree = 0;
size_t s_sdFree = 0;
bool s_sdAvailable = false;

const char* rowName(Row row) {
    static const char* const names[] = {
        "RADIO", "WIFI", "BLE", "IR", "EVILPIG", "PIGPASS", "SPECTRUM",
        "USB SD", "FILES", "XFER", "BADUSB", "WPA-SEC", "PWNCRACK",
        "LOOT", "MP3", "INSPECT", "STOP ALL"
    };
    return names[row];
}

bool rowActive(Row row) {
    switch (row) {
        case STOP_ALL: return false;
        case RADIO: return Cap::isRunning();
        case WIFI: return WiFi.getMode() != WIFI_OFF;
        case BLE: return BlePigMode::isRunning();
        case IR: return IrPortMode::isRunning();
        case EVILPIG: return EvilPigMode::isRunning();
        case PIGPASS: return PigpassMode::isRunning();
        case SPECTRUM: return SpectrumMode::isRunning();
        case USBSD: return UsbSdMode::isRunning();
        case FILEMGR: return FileMgrMode::isRunning();
        case XFER: return XferMode::isRunning();
        case BADUSB: return BadUsbMode::isRunning();
        case WPA_SYNC: return WPASec::isBusy();
        case PWN_SYNC: return Pwncrack::isBusy();
        case LOOT: return LootMenu::isActive();
        case MP3: return Mp3PlayerMode::isRunning();
        case INSPECTOR: return InspectorPig::isRunning();
        case ROW_COUNT: return false;
    }
    return false;
}

bool networkSyncActive() {
    return WPASec::isBusy() || Pwncrack::isBusy();
}

void refreshMemoryStats() {
    uint32_t now = millis();
    if (s_lastStatsMs && (uint32_t)(now - s_lastStatsMs) < 500) return;
    s_lastStatsMs = now;
    s_freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    s_largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    s_minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    s_internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_sdAvailable = Storage::available();
    if (s_sdAvailable) {
        size_t total = SD.totalBytes();
        size_t used = SD.usedBytes();
        s_sdFree = total > used ? total - used : 0;
    } else {
        s_sdFree = 0;
    }
}

uint8_t activeCount() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < ROW_COUNT; ++i)
        if (rowActive((Row)i)) ++count;
    return count;
}

void rowInfo(Row row, char* out, size_t n) {
    if (!out || !n) return;
    out[0] = '\0';
    if (!rowActive(row)) {
        snprintf(out, n, "OFF");
        return;
    }
    switch (row) {
        case RADIO: {
            const char* mode = Cap::runMode() == Cap::RunMode::Light ? "LIGHT" :
                               Cap::runMode() == Cap::RunMode::Aggressive ? "AGGR" : "PIN";
            const Cap::Counters& c = Cap::counters();
            snprintf(out, n, "%s CH%02u HS%u", mode, (unsigned)c.currentChannel,
                     (unsigned)Hc22000::pairCount());
            break;
        }
        case WIFI: {
            wifi_mode_t mode = WiFi.getMode();
            if (mode == WIFI_AP) snprintf(out, n, "AP %u CLIENT", (unsigned)WiFi.softAPgetStationNum());
            else if (mode == WIFI_STA) snprintf(out, n, "STA");
            else if (mode == WIFI_AP_STA)
                snprintf(out, n, "AP+STA %u", (unsigned)WiFi.softAPgetStationNum());
            else snprintf(out, n, "ON");
            break;
        }
        case BLE:
            snprintf(out, n, "BURSTS %lu", (unsigned long)BlePigMode::getBursts());
            break;
        case PIGPASS:
            snprintf(out, n, "%s", PigpassMode::getCurrentTry()[0] ? "CRACKING" : "ACTIVE");
            break;
        case EVILPIG:
            snprintf(out, n, "%s", EvilPigMode::getStatus());
            break;
        case WPA_SYNC:
        case PWN_SYNC:
            snprintf(out, n, "NETWORK SYNC");
            break;
        case LOOT:
            snprintf(out, n, "FILE SYNC / BROWSE");
            break;
        case MP3:
            snprintf(out, n, "PLAYER ACTIVE");
            break;
        case STOP_ALL:
            snprintf(out, n, "ACTION");
            break;
        default:
            snprintf(out, n, "ACTIVE");
            break;
    }
}

void stopRow(Row row) {
    switch (row) {
        case RADIO: Cap::stop(); break;
        case WIFI:
            WiFi.disconnect(true, false);
            WiFi.mode(WIFI_OFF);
            break;
        case BLE: if (BlePigMode::isRunning()) BlePigMode::stop(); break;
        case IR: if (IrPortMode::isRunning()) IrPortMode::stop(); break;
        case EVILPIG: if (EvilPigMode::isRunning()) EvilPigMode::stop(); break;
        case PIGPASS: if (PigpassMode::isRunning()) PigpassMode::stop(); break;
        case SPECTRUM: if (SpectrumMode::isRunning()) SpectrumMode::stop(); break;
        case USBSD: if (UsbSdMode::isRunning()) UsbSdMode::stop(); break;
        case FILEMGR: if (FileMgrMode::isRunning()) FileMgrMode::stop(); break;
        case XFER: if (XferMode::isRunning()) XferMode::stop(); break;
        case BADUSB: if (BadUsbMode::isRunning()) BadUsbMode::stop(); break;
        case WPA_SYNC:
        case PWN_SYNC:
            break;
        case LOOT: if (LootMenu::isActive()) LootMenu::hide(); break;
        case MP3: if (Mp3PlayerMode::isRunning()) Mp3PlayerMode::stop(); break;
        case INSPECTOR: if (InspectorPig::isRunning()) InspectorPig::stop(); break;
        case STOP_ALL:
            Cap::stop();
            if (BlePigMode::isRunning()) BlePigMode::stop();
            if (IrPortMode::isRunning()) IrPortMode::stop();
            if (EvilPigMode::isRunning()) EvilPigMode::stop();
            if (PigpassMode::isRunning()) PigpassMode::stop();
            if (SpectrumMode::isRunning()) SpectrumMode::stop();
            if (UsbSdMode::isRunning()) UsbSdMode::stop();
            if (FileMgrMode::isRunning()) FileMgrMode::stop();
            if (XferMode::isRunning()) XferMode::stop();
            if (BadUsbMode::isRunning()) BadUsbMode::stop();
            if (Mp3PlayerMode::isRunning()) Mp3PlayerMode::stop();
            if (InspectorPig::isRunning()) InspectorPig::stop();
            if (LootMenu::isActive()) LootMenu::hide();
            if (!networkSyncActive()) {
                WiFi.disconnect(true, false);
                WiFi.mode(WIFI_OFF);
            }
            break;
        case ROW_COUNT:
            break;
    }
}

}  // namespace

void begin() {
    s_running = false;
    s_selected = s_scroll = 0;
    s_keyWas = false;
}

void start() {
    s_running = true;
    s_selected = s_scroll = 0;
    s_keyWas = true;
    s_lastStatsMs = 0;
}

void stop() {
    s_running = false;
}

bool isRunning() {
    return s_running;
}

void update() {
    if (!s_running || !keyNewPress(s_keyWas)) return;
    if (keyEsc()) {
        stop();
        return;
    }
    if (keyMin()) {
        stop();
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (s_selected > 0) --s_selected;
        if (s_selected < s_scroll) s_scroll = s_selected;
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (s_selected + 1 < ROW_COUNT) ++s_selected;
        if (s_selected >= s_scroll + VISIBLE_ROWS)
            s_scroll = (uint8_t)(s_selected - VISIBLE_ROWS + 1);
        return;
    }
    if (M5Cardputer.Keyboard.keysState().enter) {
        Row selected = (Row)s_selected;
        stopRow(selected);
        if (selected == STOP_ALL && networkSyncActive())
            Display::showToast("STOPPED EXCEPT SYNC", 1200);
        else if (selected == WPA_SYNC || selected == PWN_SYNC)
            Display::showToast("CLOSE SYNC IN LOOT", 1200);
        else if (selected == STOP_ALL)
            Display::showToast("ALL STOPPED", 900);
        else
            Display::showToast(rowActive(selected) ? "STOP FAILED" : "STOPPED", 700);
    }
}

void draw(M5Canvas& canvas) {
    refreshMemoryStats();
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setTextWrap(false);

    canvas.setTextColor(UiStyle::TITLE);
    canvas.setTextSize(2);
    canvas.drawString("TASKS", 7, 1);
    canvas.setTextSize(1);
    canvas.setTextColor(UiStyle::CYAN);
    char active[24];
    snprintf(active, sizeof(active), "ON %u/%u",
             (unsigned)activeCount(), (unsigned)ROW_COUNT);
    canvas.setTextDatum(top_right);
    canvas.drawString(active, DISPLAY_W - 7, 7);
    canvas.setTextDatum(top_left);
    canvas.drawFastHLine(6, 18, DISPLAY_W - 12, UiStyle::PANEL);

    char mem[48];
    canvas.setTextColor(UiStyle::TEXT);
    snprintf(mem, sizeof(mem), "FREE %uK   BIG %uK",
             (unsigned)(s_freeHeap / 1024),
             (unsigned)(s_largestBlock / 1024));
    canvas.drawString(mem, 7, 21);
    canvas.setTextColor(UiStyle::DIM);
    if (s_sdAvailable) {
        snprintf(mem, sizeof(mem), "MIN %uK INT %uK SD %uMB",
                 (unsigned)(s_minFreeHeap / 1024),
                 (unsigned)(s_internalFree / 1024),
                 (unsigned)(s_sdFree / (1024 * 1024)));
    } else {
        snprintf(mem, sizeof(mem), "MIN %uK INT %uK SD OFF",
                 (unsigned)(s_minFreeHeap / 1024),
                 (unsigned)(s_internalFree / 1024));
    }
    canvas.drawString(mem, 7, 31);
    canvas.drawFastHLine(6, 41, DISPLAY_W - 12, UiStyle::PANEL);

    for (uint8_t i = 0; i < VISIBLE_ROWS; ++i) {
        uint8_t index = (uint8_t)(s_scroll + i);
        if (index >= ROW_COUNT) break;
        const int y = 44 + i * 11;
        const Row row = (Row)index;
        const bool selected = index == s_selected;
        const bool activeRow = rowActive(row);
        if (selected) canvas.fillRoundRect(4, y - 1, DISPLAY_W - 8, 11, 2, UiStyle::PINK);
        canvas.setTextColor(selected ? UiStyle::BG : UiStyle::TEXT);
        canvas.drawString(rowName(row), 8, y);

        char info[32];
        rowInfo(row, info, sizeof(info));
        canvas.setTextColor(selected ? UiStyle::BG :
                            activeRow ? UiStyle::GREEN : UiStyle::DIM);
        canvas.setTextDatum(top_right);
        canvas.drawString(info, DISPLAY_W - 8, y);
        canvas.setTextDatum(top_left);
    }

    if (s_scroll > 0) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString("^", DISPLAY_W - 7, 43);
    }
    if (s_scroll + VISIBLE_ROWS < ROW_COUNT) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString("v", DISPLAY_W - 7, 88);
    }
}

}  // namespace TaskManager
