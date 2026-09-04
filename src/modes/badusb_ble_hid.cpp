// BLE HID only — do not include USBHIDKeyboard.h here
#include "badusb_hid.h"

#if __has_include(<BleKeyboard.h>)
#include <BleKeyboard.h>
#define BADUSB_BLE 1
static BleKeyboard* s_kb = nullptr;
#else
#define BADUSB_BLE 0
#endif

namespace BadUsbHid {

bool bleBegin(const char* name, const char* mfg) {
#if BADUSB_BLE
    if (!s_kb) {
        s_kb = new BleKeyboard(name ? name : "0N3P0rK", mfg ? mfg : "lexilexiko", 100);
        s_kb->setDelay(25);
        s_kb->begin();
        delay(200); // Bruce: wait HID service register
        s_kb->releaseAll();
    }
    return true;
#else
    (void)name;
    (void)mfg;
    return false;
#endif
}

void bleEnd() {
#if BADUSB_BLE
    if (s_kb) {
        s_kb->releaseAll();
        s_kb->end();
        delete s_kb;
        s_kb = nullptr;
    }
#endif
}

bool bleConnected() {
#if BADUSB_BLE
    return s_kb && s_kb->isConnected();
#else
    return false;
#endif
}

void blePress(uint8_t key) {
#if BADUSB_BLE
    if (s_kb) s_kb->press(key);
#endif
}

void bleRelease(uint8_t key) {
#if BADUSB_BLE
    if (s_kb) s_kb->release(key);
#endif
}

void bleReleaseAll() {
#if BADUSB_BLE
    if (s_kb) s_kb->releaseAll();
#endif
}

void blePrint(const char* text) {
#if BADUSB_BLE
    if (s_kb && text) s_kb->print(text);
#endif
}

void bleModifierCombo(bool gui, bool alt, bool ctrl, bool shift, uint8_t key) {
#if BADUSB_BLE
    if (!s_kb) return;
    if (gui) s_kb->press(KEY_LEFT_GUI);
    if (alt) s_kb->press(KEY_LEFT_ALT);
    if (ctrl) s_kb->press(KEY_LEFT_CTRL);
    if (shift) s_kb->press(KEY_LEFT_SHIFT);
    if (key) {
        s_kb->press(key);
        delay(35);
        s_kb->release(key);
    }
    s_kb->releaseAll();
#endif
}

void bleSetDelay(uint32_t ms) {
#if BADUSB_BLE
    if (s_kb) s_kb->setDelay(ms);
#else
    (void)ms;
#endif
}

}  // namespace BadUsbHid
