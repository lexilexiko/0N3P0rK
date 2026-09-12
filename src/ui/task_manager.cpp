#include "task_manager.h"

#include "display.h"
#include "keys.h"
#include "../cap/sniffer.h"
#include "../modes/badusb.h"
#include "../modes/blepig.h"
#include "../modes/evilpig.h"
#include "../modes/filemgr.h"
#include "../modes/irport.h"
#include "../modes/pigpass.h"
#include "../modes/spectrum.h"
#include "../modes/usbsd.h"
#include "../modes/xfer.h"
#include "../sync/pwncrack.h"
#include "../sync/wpasec.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <stdio.h>

namespace TaskManager {

namespace {

enum Row : uint8_t {
    RADIO, WIFI, BLE, IR, EVILPIG, PIGPASS, SPECTRUM, USBSD, FILEMGR, XFER,
    BADUSB, WPA_SYNC, PWN_SYNC, STOP_ALL, ROW_COUNT
};

bool s_running = false;
uint8_t s_selected = 0;
bool s_keyWas = false;

const char* rowName(Row row) {
    static const char* const names[] = {
        "RADIO", "WIFI", "BLE", "IR", "EVILPIG", "PIGPASS", "SPECTRUM",
        "USB SD", "FILES", "XFER", "BADUSB", "WPA-SEC", "PWNCRACK", "STOP ALL"
    };
    return names[row];
}

bool rowActive(Row row) {
    switch (row) {
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
        case STOP_ALL: return false;
        default: return false;
    }
}

const char* rowCost(Row row) {
    switch (row) {
        case RADIO: return "48K*";
        case SPECTRUM: return "~10K";
        case PIGPASS: return "~5K";
        case WIFI: return "dyn";
        case WPA_SYNC:
        case PWN_SYNC: return "TLS";
        default: return "-";
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
            // Network sync is deliberately not force-killed: the client owns
            // its socket and must close it through its normal UI flow.
            break;
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
            WiFi.disconnect(true, false);
            WiFi.mode(WIFI_OFF);
            break;
        default:
            break;
    }
}

}  // namespace

void begin() {
    s_running = false;
    s_selected = 0;
    s_keyWas = false;
}

void start() {
    s_running = true;
    s_selected = 0;
    s_keyWas = true;
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
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (s_selected > 0) s_selected--;
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (s_selected + 1 < ROW_COUNT) s_selected++;
        return;
    }
    if (M5Cardputer.Keyboard.keysState().enter) {
        stopRow((Row)s_selected);
        if (s_selected == STOP_ALL) {
            Display::showToast("ALL STOPPED", 900);
        } else {
            Display::showToast(rowActive((Row)s_selected) ? "STOP FAILED" : "STOPPED", 700);
        }
    }
}

void draw(M5Canvas& canvas) {
    const uint16_t bg = 0x2145, panel = 0x3A8A, title = 0xFFE0;
    const uint16_t text = 0xEF5D, dim = 0x9CD3, active = 0xFDB6;
    canvas.fillSprite(bg);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(2);
    canvas.setTextColor(title);
    canvas.drawString("TASK MANAGER", DISPLAY_W / 2, 2);
    canvas.drawLine(8, 20, DISPLAY_W - 8, 20, title);

    char mem[32];
    snprintf(mem, sizeof(mem), "HEAP %uK  BLOCK %uK",
             (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
    canvas.setTextDatum(top_left);
    canvas.setTextSize(1);
    canvas.setTextColor(dim);
    canvas.drawString(mem, 8, 24);

    const int y0 = 38;
    const int lh = 13;
    for (uint8_t i = 0; i < 5; i++) {
        uint8_t row = (uint8_t)((s_selected / 5) * 5 + i);
        if (row >= ROW_COUNT) break;
        int y = y0 + i * lh;
        bool selected = row == s_selected;
        if (selected) canvas.fillRect(6, y - 1, DISPLAY_W - 12, lh, active);
        else canvas.fillRect(6, y, DISPLAY_W - 12, lh - 1, panel);
        canvas.setTextColor(selected ? bg : text);
        canvas.drawString(rowName((Row)row), 12, y + 1);
        canvas.setTextDatum(top_right);
        char state[18];
        snprintf(state, sizeof(state), "%s %s",
                 rowActive((Row)row) ? "ON" : "OFF", rowCost((Row)row));
        canvas.drawString(state, DISPLAY_W - 12, y + 1);
        canvas.setTextDatum(top_left);
    }
    canvas.setTextColor(dim);
    canvas.drawString(";/. select  ENT stop  ` back", 8, MAIN_H - 10);
}

}  // namespace TaskManager
