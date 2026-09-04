#pragma once
#include <Arduino.h>

namespace BadUsbHid {

bool usbBegin();
bool usbWaitHost(uint32_t timeoutMs); // 0 = wait forever (caller yields)
bool usbMounted();
void usbEnd();
bool usbReady();
void usbPress(uint8_t key);
void usbRelease(uint8_t key);
void usbReleaseAll();
void usbPrint(const char* text);
void usbModifierCombo(bool gui, bool alt, bool ctrl, bool shift, uint8_t key);
void usbSetDelay(uint32_t ms);

bool bleBegin(const char* name, const char* mfg);
void bleEnd();
bool bleConnected();
void blePress(uint8_t key);
void bleRelease(uint8_t key);
void bleReleaseAll();
void blePrint(const char* text);
void bleModifierCombo(bool gui, bool alt, bool ctrl, bool shift, uint8_t key);
void bleSetDelay(uint32_t ms);

#ifndef BADUSB_KEY_CODES
#define BADUSB_KEY_CODES
#define BAD_KEY_LEFT_CTRL   0x80
#define BAD_KEY_LEFT_SHIFT  0x81
#define BAD_KEY_LEFT_ALT    0x82
#define BAD_KEY_LEFT_GUI    0x83
#define BAD_KEY_UP_ARROW    0xDA
#define BAD_KEY_DOWN_ARROW  0xD9
#define BAD_KEY_LEFT_ARROW  0xD8
#define BAD_KEY_RIGHT_ARROW 0xD7
#define BAD_KEY_BACKSPACE   0xB2
#define BAD_KEY_TAB         0xB3
#define BAD_KEY_RETURN      0xB0
#define BAD_KEY_ESC         0xB1
#define BAD_KEY_F1          0xC2
#define BAD_KEY_F2          0xC3
#define BAD_KEY_F3          0xC4
#define BAD_KEY_F4          0xC5
#define BAD_KEY_F5          0xC6
#define BAD_KEY_F6          0xC7
#define BAD_KEY_F7          0xC8
#define BAD_KEY_F8          0xC9
#define BAD_KEY_F9          0xCA
#define BAD_KEY_F10         0xCB
#define BAD_KEY_F11         0xCC
#define BAD_KEY_F12         0xCD
#endif

}  // namespace BadUsbHid
