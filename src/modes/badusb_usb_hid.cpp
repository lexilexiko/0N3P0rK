// USB HID only — do not include BleKeyboard.h here
#include "badusb_hid.h"

#if !ARDUINO_USB_MODE
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "tusb.h"
static USBHIDKeyboard s_kb;
static bool s_on = false;
#endif

namespace BadUsbHid {

bool usbBegin() {
#if !ARDUINO_USB_MODE
    if (!s_on) {
        USB.begin();
        s_kb.begin();
        s_on = true;
        delay(200);
        s_kb.releaseAll();
    }
    return true;
#else
    return false;
#endif
}

bool usbWaitHost(uint32_t timeoutMs) {
#if !ARDUINO_USB_MODE
    if (!s_on && !usbBegin()) return false;
    uint32_t t0 = millis();
    while (!tud_mounted()) {
        if (timeoutMs && (millis() - t0) > timeoutMs) return false;
        delay(100);
        yield();
    }
    delay(150);
    s_kb.releaseAll();
    return true;
#else
    (void)timeoutMs;
    return false;
#endif
}

bool usbMounted() {
#if !ARDUINO_USB_MODE
    return s_on && tud_mounted();
#else
    return false;
#endif
}

void usbEnd() {
#if !ARDUINO_USB_MODE
    if (s_on) {
        s_kb.releaseAll();
        s_kb.end();
        s_on = false;
    }
#endif
}

bool usbReady() {
#if !ARDUINO_USB_MODE
    return s_on;
#else
    return false;
#endif
}

void usbPress(uint8_t key) {
#if !ARDUINO_USB_MODE
    if (s_on) s_kb.press(key);
#endif
}

void usbRelease(uint8_t key) {
#if !ARDUINO_USB_MODE
    if (s_on) s_kb.release(key);
#endif
}

void usbReleaseAll() {
#if !ARDUINO_USB_MODE
    if (s_on) s_kb.releaseAll();
#endif
}

void usbPrint(const char* text) {
#if !ARDUINO_USB_MODE
    if (s_on && text) s_kb.print(text);
#endif
}

void usbModifierCombo(bool gui, bool alt, bool ctrl, bool shift, uint8_t key) {
#if !ARDUINO_USB_MODE
    if (!s_on) return;
    if (gui) s_kb.press(KEY_LEFT_GUI);
    if (alt) s_kb.press(KEY_LEFT_ALT);
    if (ctrl) s_kb.press(KEY_LEFT_CTRL);
    if (shift) s_kb.press(KEY_LEFT_SHIFT);
    if (key) {
        s_kb.press(key);
        delay(30);
        s_kb.release(key);
    }
    s_kb.releaseAll();
#endif
}

void usbSetDelay(uint32_t ms) {
#if !ARDUINO_USB_MODE
    // USBHIDKeyboard has no setDelay on all cores — soft delay after strokes in caller
    (void)ms;
#else
    (void)ms;
#endif
}

}  // namespace BadUsbHid
