// ESP32-S2 USB CDC does not export Serial from Arduino.h.
#pragma once
#if CONFIG_IDF_TARGET_ESP32S2
#include <Arduino.h>
#include <USBCDC.h>
#ifndef Serial
extern USBCDC USBSerial;
#define Serial USBSerial
#endif
#endif
