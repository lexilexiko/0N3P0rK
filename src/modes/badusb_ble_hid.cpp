// BLE HID only — do not include USBHIDKeyboard.h here
#include "badusb_hid.h"

#if __has_include(<BleKeyboard.h>)
#include <BleKeyboard.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include "esp_gap_ble_api.h"
#define BADUSB_BLE 1
static BleKeyboard* s_kb = nullptr;

// BleKeyboard 0.3.x defaults to ESP_LE_AUTH_REQ_SC_MITM_BOND → phone asks for PIN.
// Bruce-style: bond without MITM (Just Works) — no passkey prompt.
static void applyBruceStyleSecurity() {
    // No static passkey — Just Works
    uint8_t iocap = ESP_IO_CAP_NONE;
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));

    // Bond only, no MITM (same idea as people patching BleKeyboard for Android/iOS)
    uint8_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));

    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));

    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
}
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
        applyBruceStyleSecurity();
        delay(200); // wait HID service register (Bruce does the same)
        s_kb->releaseAll();
    }
    return true;
#else
    (void)name;
    (void)mfg;
    return false;
#endif
}

uint32_t blePasskey() {
    // Bruce-style: no fixed PIN
    return 0;
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
