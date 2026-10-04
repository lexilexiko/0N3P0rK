// SD card as a USB disk on the PC. Same idea as Launcher USB MSC.
// Host often sends READ10/WRITE10 with a byte offset inside the first LBA.
// Returning -1 there makes Windows drop the volume (flash appears, then gone).
#include "usbsd.h"
#include "../storage/littlefs_ops.h"
#include "../cap/sniffer.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include "../core/app.h"
#include "../piglet/avatar.h"
#include "../audio/sfx.h"
#include <M5Cardputer.h>
#include <string.h>
#include <stdio.h>

#if !ARDUINO_USB_MODE
#include "USB.h"
#include "USBMSC.h"
#include "tusb.h"
#endif

namespace UsbSdMode {

static bool s_run = false;
static bool s_ok = false;
static bool s_eject = false;
static bool s_keyWas = false;
static volatile uint32_t s_reads = 0;
static volatile uint32_t s_writes = 0;
static volatile uint32_t s_lastIo = 0;
static volatile uint32_t s_fail = 0;
static uint32_t s_sectors = 0;
static uint16_t s_secSz = 512;
static uint8_t s_scratch[512];

#if !ARDUINO_USB_MODE
static USBMSC s_msc;

static bool hostUp() { return tud_mounted(); }

static void usbBounce() {
    Serial.setTxTimeoutMs(0);
    tud_disconnect();
    delay(180);
    tud_connect();
    delay(40);
}

static bool readOne(uint32_t lba, uint8_t* dst) {
    if (lba >= s_sectors) {
        memset(dst, 0, s_secSz);
        return true;
    }
    for (int t = 0; t < 3; t++) {
        if (Storage::readSector(lba, dst)) return true;
        delay(1);
    }
    s_fail++;
    return false;
}

static bool writeOne(uint32_t lba, const uint8_t* src) {
    if (lba >= s_sectors) return true;
    for (int t = 0; t < 3; t++) {
        if (Storage::writeSector(lba, src)) return true;
        delay(1);
    }
    s_fail++;
    return false;
}

// TinyUSB offset is a byte offset inside the first LBA. Serve any slice.
static int32_t xfer(bool wr, uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t bufsize) {
    if (!s_ok || !s_secSz || !buf || !bufsize) return -1;
    if (s_secSz != 512) return -1;
    uint32_t done = 0;
    while (done < bufsize) {
        uint32_t pos = offset + done;
        uint32_t slba = lba + (pos / s_secSz);
        uint32_t soff = pos % s_secSz;
        uint32_t n = s_secSz - soff;
        if (n > bufsize - done) n = bufsize - done;
        if (!wr) {
            if (soff == 0 && n == s_secSz) {
                if (!readOne(slba, buf + done)) return -1;
            } else {
                if (!readOne(slba, s_scratch)) return -1;
                memcpy(buf + done, s_scratch + soff, n);
            }
        } else {
            if (soff == 0 && n == s_secSz) {
                if (!writeOne(slba, buf + done)) return -1;
            } else {
                if (!readOne(slba, s_scratch)) return -1;
                memcpy(s_scratch + soff, buf + done, n);
                if (!writeOne(slba, s_scratch)) return -1;
            }
        }
        done += n;
    }
    s_lastIo = millis();
    if (wr) s_writes++;
    else s_reads++;
    return (int32_t)bufsize;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    return xfer(false, lba, offset, (uint8_t*)buffer, bufsize);
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    return xfer(true, lba, offset, buffer, bufsize);
}

static bool onStartStop(uint8_t, bool start, bool loadEject) {
    if (!start && loadEject) s_eject = true;
    return true;
}
#endif

void start() {
    if (s_run) return;
    if (Cap::isRunning()) Cap::stop();
    Avatar::suspendScene();
    s_run = true;
    s_ok = false;
    s_eject = false;
    s_keyWas = true;
    s_reads = 0;
    s_writes = 0;
    s_fail = 0;
    s_lastIo = 0;

    if (!Storage::available() && !Storage::begin()) {
        Display::showToast("NO SD");
        return;
    }

#if ARDUINO_USB_MODE
    Display::showToast("USB MODE OFF");
    return;
#else
    s_secSz = (uint16_t)Storage::sectorSize();
    s_sectors = Storage::numSectors();
    if (s_secSz != 512 || !s_sectors) {
        Display::showToast("SD RAW FAIL");
        return;
    }

    s_msc.vendorID("0N3P0rK");
    s_msc.productID("SD CARD");
    s_msc.productRevision("1.0");
    s_msc.onRead(onRead);
    s_msc.onWrite(onWrite);
    s_msc.onStartStop(onStartStop);
    if (!s_msc.begin(s_sectors, s_secSz)) {
        Display::showToast("USB MSC FAIL");
        s_msc.mediaPresent(false);
        return;
    }
    s_ok = true;
    s_msc.mediaPresent(true);
    // CDC already owns the bus from boot. Bounce so the host re-reads
    // capacity with media present instead of the empty LUN from startup.
    usbBounce();
    SFX::play(SFX::CONFIRM);
    Serial.printf("[USBSD] disk %u x %u\n", (unsigned)s_sectors, (unsigned)s_secSz);
#endif
}

void stop() {
    if (!s_run) return;
#if !ARDUINO_USB_MODE
    if (s_ok) {
        s_msc.mediaPresent(false);
        delay(40);
        s_msc.end();
        usbBounce();
    }
#endif
    s_ok = false;
    s_run = false;
    Storage::remount();
    Avatar::resumeScene();
    Serial.println("[USBSD] stop");
}

void update() {
    if (!s_run) return;
    if (App::windowHidden()) return;
    if (!keyNewPress(s_keyWas)) return;
    if (keyEsc()) {
        stop();
        return;
    }
}

bool isRunning() { return s_run; }

void getStatusLine(char* out, size_t n) {
    if (!out || !n) return;
    if (!s_ok) {
        snprintf(out, n, "USB SD  NO CARD");
        return;
    }
    if (s_eject) {
        snprintf(out, n, "USB SD  EJECTED");
        return;
    }
#if !ARDUINO_USB_MODE
    const bool up = hostUp();
#else
    const bool up = false;
#endif
    snprintf(out, n, "USB SD  %s R%lu W%lu",
             up ? "ON" : "WAIT",
             (unsigned long)s_reads, (unsigned long)s_writes);
}

void draw(M5Canvas& canvas) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    canvas.fillSprite(bg);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_center);

    canvas.setTextColor(UiStyle::GOLD);
    canvas.drawString("USB SD", 120, 6);

    int bx = 52, by = 26, bw = 90, bh = 36;
    canvas.fillRoundRect(bx, by, bw, bh, 4, UiStyle::CYAN);
    canvas.fillRoundRect(bx + bw - 2, by + 6, 28, bh - 12, 3, UiStyle::DIM);
    canvas.fillRoundRect(bx + bw + 6, by + 10, 10, 6, 1, 0x4208);
    canvas.fillRoundRect(bx + bw + 6, by + 22, 10, 6, 1, 0x4208);
#if !ARDUINO_USB_MODE
    bool live = s_ok && !s_eject && hostUp();
#else
    bool live = false;
#endif
    canvas.fillRoundRect(bx + 8, by + 8, 6, 20, 1,
                         s_eject ? UiStyle::DIM : (live ? UiStyle::GREEN : UiStyle::RED));

    canvas.setTextDatum(top_left);
    canvas.setTextColor(fg);
    if (!Storage::available()) {
        canvas.drawString("NO SD CARD", 8, 72);
    } else if (!s_ok) {
        canvas.drawString("MSC DID NOT START", 8, 72);
    } else if (s_eject) {
        canvas.setTextColor(UiStyle::GREEN);
        canvas.drawString("PC EJECTED", 8, 72);
    } else {
        char line[44];
        uint32_t mb = (s_sectors / 2048);
        snprintf(line, sizeof(line), "DISK %lu MB  PLUG USB", (unsigned long)mb);
        canvas.drawString(line, 8, 70);
        snprintf(line, sizeof(line), "R %lu   W %lu   F %lu",
                 (unsigned long)s_reads, (unsigned long)s_writes,
                 (unsigned long)s_fail);
        canvas.setTextColor(UiStyle::PINK);
        canvas.drawString(line, 8, 82);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString(live ? "GREEN = PC HAS DISK" : "RED = WAIT FOR PC", 8, 94);
    }
}

}  // namespace UsbSdMode
