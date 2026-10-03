#include "irport.h"
#include "ir_power/ir_power_tx.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include "../audio/sfx.h"
#include "../piglet/avatar.h"
#include "../core/config.h"
#include "../core/app.h"
#include "../storage/littlefs_ops.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <string.h>
#include <stdlib.h>

bool IrPortMode::running = false;
IrPortMode::Phase IrPortMode::phase = IrPortMode::Phase::REGION;
IrPortMode::Pack IrPortMode::pack = IrPortMode::Pack::BUILTIN;
IrPortMode::Code IrPortMode::codes[MAX_CODES] = {};
uint8_t IrPortMode::codeCount = 0;
uint8_t IrPortMode::blastIndex = 0;
uint8_t IrPortMode::blastTotal = 0;
uint32_t IrPortMode::nextSendMs = 0;
char IrPortMode::packName[28] = "POWER NA";
char IrPortMode::statusMsg[40] = "";
char IrPortMode::fileNames[MAX_FILES][28] = {{0}};
uint8_t IrPortMode::fileCount = 0;
uint8_t IrPortMode::fileSel = 0;
uint8_t IrPortMode::fileScroll = 0;
uint8_t IrPortMode::regionSel = 0;
bool IrPortMode::keyLatch = false;

void IrPortMode::loadBuiltin() {
    pack = Pack::BUILTIN;
    codeCount = 0;
    blastTotal = IrPower::getCodeCount();
    snprintf(packName, sizeof(packName), "POWER %s",
             IrPower::getRegion() == IR_REGION_EU ? "EU" : "NA");
}

bool IrPortMode::loadFile(const char* path) {
    if (!Config::isSDAvailable()) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    codeCount = 0;
    pack = Pack::CUSTOM;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(packName, base, sizeof(packName) - 1);
    packName[sizeof(packName) - 1] = '\0';

    char line[96];
    while (f.available() && codeCount < MAX_CODES) {
        size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        char* cr = strchr(line, '\r');
        if (cr) *cr = '\0';
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        char proto[16] = {0}, aStr[16] = {0}, cStr[16] = {0}, bStr[16] = {0}, name[24] = {0};
        int got = sscanf(p, "%15s %15s %15s %15s %23s", proto, aStr, cStr, bStr, name);
        if (got < 2) continue;

        Proto pr = Proto::NEC;
        if (strcasecmp(proto, "NEC") == 0) pr = Proto::NEC;
        else if (strcasecmp(proto, "SAMSUNG") == 0 || strcasecmp(proto, "SAM") == 0) pr = Proto::SAMSUNG;
        else if (strcasecmp(proto, "SONY") == 0) pr = Proto::SONY;
        else continue;

        uint16_t addr = (uint16_t)strtoul(aStr, nullptr, 0);
        uint16_t cmd = 0;
        uint8_t bits = 0;
        if (pr == Proto::SONY) {
            cmd = addr;
            addr = 0;
            bits = 12;
            if (got >= 3 && cStr[0] >= '0' && cStr[0] <= '9')
                bits = (uint8_t)strtoul(cStr, nullptr, 0);
            if (got >= 3 && cStr[0] && cStr[0] > '9') strncpy(name, cStr, sizeof(name) - 1);
        } else {
            if (got >= 3) cmd = (uint16_t)strtoul(cStr, nullptr, 0);
            if (got >= 4 && bStr[0] && !(bStr[0] >= '0' && bStr[0] <= '9'))
                strncpy(name, bStr, sizeof(name) - 1);
        }

        Code& x = codes[codeCount++];
        x.proto = pr;
        x.addr = addr;
        x.cmd = cmd;
        x.bits = bits;
        if (name[0]) strncpy(x.name, name, sizeof(x.name) - 1);
        else snprintf(x.name, sizeof(x.name), "%s#%u", proto, (unsigned)codeCount);
    }
    f.close();
    blastTotal = codeCount;
    return codeCount > 0;
}

void IrPortMode::scanFiles() {
    fileCount = 0;
    fileSel = 0;
    fileScroll = 0;
    if (!Config::isSDAvailable()) return;
    Storage::ensureDir(Storage::DIR_IR);

    auto addFile = [](const char* base) {
        if (fileCount >= MAX_FILES) return;
        for (uint8_t i = 0; i < fileCount; i++)
            if (strcmp(fileNames[i], base) == 0) return;
        strncpy(fileNames[fileCount], base, sizeof(fileNames[0]) - 1);
        fileNames[fileCount][sizeof(fileNames[0]) - 1] = '\0';
        fileCount++;
    };

    auto scanDir = [&](const char* dir) {
        File d = SD.open(dir);
        if (!d || !d.isDirectory()) { if (d) d.close(); return; }
        File e = d.openNextFile();
        while (e && fileCount < MAX_FILES) {
            if (!e.isDirectory()) {
                const char* nm = e.name();
                const char* base = strrchr(nm, '/');
                base = base ? base + 1 : nm;
                size_t len = strlen(base);
                if (len > 3) {
                    const char* ext3 = base + len - 3;
                    const char* ext4 = (len > 4) ? base + len - 4 : "";
                    if (strcasecmp(ext3, ".ir") == 0 || strcasecmp(ext4, ".txt") == 0)
                        addFile(base);
                }
            }
            e.close();
            e = d.openNextFile();
        }
        d.close();
    };
    scanDir(Storage::DIR_IR);
    scanDir("/ir");
}

void IrPortMode::start() {
    running = true;
    phase = Phase::REGION;
    blastIndex = 0;
    regionSel = 0;
    keyLatch = true;  // swallow the menu ENT that opened us
    IrPower::setRegion(IR_REGION_NA);
    loadBuiltin();
    strncpy(statusMsg, "PICK REGION", sizeof(statusMsg) - 1);
    SFX::setMuted(false);
    SFX::stop();
    SFX::play(SFX::MODE_ENTER);
    Avatar::setState(AvatarState::HUNTING);
    pinMode(IrPower::IR_TX_PIN, OUTPUT);
    digitalWrite(IrPower::IR_TX_PIN, HIGH);
}

void IrPortMode::stop() {
    running = false;
    phase = Phase::READY;
    digitalWrite(IrPower::IR_TX_PIN, HIGH);
    SFX::setMuted(false);
    SFX::stop();
    Avatar::setSparkleStorm(false);
    Avatar::setState(AvatarState::NEUTRAL);
    Avatar::waveRipple(WaveMode::NONE);
}

void IrPortMode::startBlast() {
    blastTotal = (pack == Pack::BUILTIN) ? IrPower::getCodeCount() : codeCount;
    if (blastTotal == 0) {
        strncpy(statusMsg, "NO CODES", sizeof(statusMsg) - 1);
        return;
    }
    SFX::setMuted(false);
    SFX::stop();
    SFX::play(SFX::IR_FIRE);
    phase = Phase::BLAST;
    blastIndex = 0;
    nextSendMs = millis() + 280;
    strncpy(statusMsg, "FIRING...", sizeof(statusMsg) - 1);
    Avatar::setState(AvatarState::EXCITED);
    Avatar::setSparkleStorm(true);
    Avatar::setGrassMoving(false);
    Avatar::waveRipple(WaveMode::NONE);
}

void IrPortMode::handleInput() {
    if (!keyNewPress(keyLatch)) return;

    if (keyEsc()) {
        if (phase == Phase::FILE_PICK || phase == Phase::READY || phase == Phase::DONE) {
            if (phase == Phase::FILE_PICK || phase == Phase::DONE) {
                phase = Phase::READY;
                strncpy(statusMsg, "SPC FIRE  E FILE  R NA/EU", sizeof(statusMsg) - 1);
                return;
            }
            phase = Phase::REGION;
            strncpy(statusMsg, "PICK REGION", sizeof(statusMsg) - 1);
            return;
        }
        if (phase == Phase::BLAST) {
            phase = Phase::DONE;
            snprintf(statusMsg, sizeof(statusMsg), "STOP %u/%u",
                     (unsigned)blastIndex, (unsigned)blastTotal);
            Avatar::setSparkleStorm(false);
            SFX::setMuted(false);
            return;
        }
        stop();
        return;
    }

    if (phase == Phase::REGION) {
        if (M5Cardputer.Keyboard.isKeyPressed(';') ||
            M5Cardputer.Keyboard.isKeyPressed('.')) {
            regionSel = regionSel ? 0 : 1;
            SFX::play(SFX::MENU_CLICK);
            return;
        }
        if (M5Cardputer.Keyboard.keysState().enter ||
            M5Cardputer.Keyboard.isKeyPressed(' ') ||
            M5Cardputer.Keyboard.isKeyPressed('/')) {
            IrPower::setRegion(regionSel ? IR_REGION_EU : IR_REGION_NA);
            loadBuiltin();
            phase = Phase::READY;
            strncpy(statusMsg, "SPC FIRE  E FILE  R NA/EU", sizeof(statusMsg) - 1);
            Display::showToast(regionSel ? "EU" : "NA", 800);
            SFX::play(SFX::CONFIRM);
        }
        return;
    }

    if (phase == Phase::FILE_PICK) {
        if (M5Cardputer.Keyboard.isKeyPressed(';')) {
            if (fileSel > 0) fileSel--;
            if (fileSel < fileScroll) fileScroll = fileSel;
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('.')) {
            if (fileCount && fileSel + 1 < fileCount) fileSel++;
            if (fileSel >= fileScroll + 5) fileScroll = (uint8_t)(fileSel - 4);
            return;
        }
        if (M5Cardputer.Keyboard.keysState().enter ||
            M5Cardputer.Keyboard.isKeyPressed(' ')) {
            if (!fileCount) return;
            char path[80];
            snprintf(path, sizeof(path), "%s/%s", Storage::DIR_IR, fileNames[fileSel]);
            if (!SD.exists(path)) snprintf(path, sizeof(path), "/ir/%s", fileNames[fileSel]);
            if (loadFile(path)) {
                phase = Phase::READY;
                snprintf(statusMsg, sizeof(statusMsg), "LOADED %u", (unsigned)codeCount);
                SFX::play(SFX::CONFIRM);
            } else {
                strncpy(statusMsg, "LOAD FAIL", sizeof(statusMsg) - 1);
            }
        }
        return;
    }

    if (phase == Phase::BLAST) {
        if (M5Cardputer.Keyboard.isKeyPressed('x') ||
            M5Cardputer.Keyboard.isKeyPressed('X')) {
            phase = Phase::DONE;
            snprintf(statusMsg, sizeof(statusMsg), "STOP %u/%u",
                     (unsigned)blastIndex, (unsigned)blastTotal);
            Avatar::setSparkleStorm(false);
            SFX::setMuted(false);
        }
        return;
    }

    if (M5Cardputer.Keyboard.isKeyPressed(' ') ||
        M5Cardputer.Keyboard.keysState().enter) {
        startBlast();
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('e') ||
        M5Cardputer.Keyboard.isKeyPressed('E')) {
        phase = Phase::FILE_PICK;
        scanFiles();
        strncpy(statusMsg, fileCount ? "PICK FILE  ENT" : "NO /0N3P0rK/ir",
                sizeof(statusMsg) - 1);
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('r') ||
        M5Cardputer.Keyboard.isKeyPressed('R')) {
        uint8_t r = IrPower::getRegion();
        IrPower::setRegion(r == IR_REGION_NA ? IR_REGION_EU : IR_REGION_NA);
        if (pack == Pack::BUILTIN) loadBuiltin();
        snprintf(statusMsg, sizeof(statusMsg), "REGION %s  N=%u",
                 IrPower::getRegion() == IR_REGION_EU ? "EU" : "NA",
                 (unsigned)IrPower::getCodeCount());
        SFX::play(SFX::CLICK);
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('b') ||
        M5Cardputer.Keyboard.isKeyPressed('B')) {
        loadBuiltin();
        phase = Phase::READY;
        strncpy(statusMsg, "POWER PACK", sizeof(statusMsg) - 1);
    }
}

void IrPortMode::update() {
    if (!running) return;
    if (!App::windowHidden()) handleInput();
    if (!running) return;
    if (phase != Phase::BLAST) return;
    if (millis() < nextSendMs) return;

    if (blastIndex >= blastTotal) {
        phase = Phase::DONE;
        snprintf(statusMsg, sizeof(statusMsg), "DONE %u", (unsigned)blastTotal);
        Avatar::setSparkleStorm(false);
        Avatar::setState(AvatarState::HAPPY);
        SFX::setMuted(false);
        SFX::stop();
        SFX::play(SFX::CONFIRM);
        return;
    }

    SFX::setMuted(true);
    SFX::stop();
    M5.Speaker.stop();

    if (pack == Pack::BUILTIN) {
        IrPower::sendCode(blastIndex);
        nextSendMs = millis() + 28;
    } else {
        const Code& c = codes[blastIndex];
        switch (c.proto) {
            case Proto::NEC:     IrPower::sendNEC(c.addr, (uint8_t)c.cmd, 1); break;
            case Proto::SAMSUNG: IrPower::sendSamsung(c.addr, c.cmd, 1); break;
            case Proto::SONY:    IrPower::sendSony(c.cmd ? c.cmd : c.addr,
                                                  c.bits ? c.bits : 12, 2); break;
        }
        nextSendMs = millis() + 100;
    }
    if ((blastIndex % 12) == 0) Avatar::setState(AvatarState::EXCITED);
    blastIndex++;
}

void IrPortMode::getStatusLine(char* buf, size_t n) {
    if (!buf || !n) return;
    if (phase == Phase::REGION)
        snprintf(buf, n, "IR REGION  ^/v  ENT");
    else if (phase == Phase::BLAST)
        snprintf(buf, n, "IR %u/%u  X STOP", (unsigned)blastIndex, (unsigned)blastTotal);
    else if (phase == Phase::FILE_PICK)
        snprintf(buf, n, "IR FILE %u/%u", fileCount ? fileSel + 1 : 0, fileCount);
    else if (pack == Pack::BUILTIN)
        snprintf(buf, n, "IR %s N:%u  SPC E R",
                 IrPower::getRegion() == IR_REGION_EU ? "EU" : "NA",
                 (unsigned)IrPower::getCodeCount());
    else
        snprintf(buf, n, "IR CUST N:%u  SPC E B", (unsigned)codeCount);
}

void IrPortMode::draw(M5Canvas& canvas) {
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    if (phase == Phase::BLAST) {
        int barW = 180;
        int fill = blastTotal ? (barW * (int)blastIndex / (int)blastTotal) : 0;
        canvas.fillRoundRect(28, 2, barW + 4, 12, 2, UiStyle::BG);
        canvas.drawRoundRect(28, 2, barW + 4, 12, 2, UiStyle::RED);
        if (fill > 0) canvas.fillRect(30, 4, fill, 8, UiStyle::RED);
        canvas.setTextColor(UiStyle::TEXT);
        char t[24];
        snprintf(t, sizeof(t), "%u/%u", (unsigned)blastIndex, (unsigned)blastTotal);
        canvas.setTextDatum(top_center);
        canvas.drawString(t, 120, 3);
        canvas.setTextDatum(top_left);
        return;
    }

    canvas.fillRoundRect(4, 14, 232, 88, 3, UiStyle::BG);
    canvas.drawRoundRect(4, 14, 232, 88, 3, UiStyle::CYAN);
    canvas.setTextColor(UiStyle::CYAN);
    canvas.drawString("IR PORT", 10, 18);
    canvas.setTextColor(UiStyle::TEXT);
    if (phase == Phase::REGION) {
        canvas.drawString("CHOOSE REGION", 10, 34);
        auto row = [&](int y, bool on, const char* lab, const char* sub) {
            if (on) canvas.fillRect(8, y - 1, 224, 16, UiStyle::PINK);
            canvas.setTextColor(on ? UiStyle::BG : UiStyle::TEXT);
            canvas.drawString(lab, 14, y + 2);
            canvas.setTextColor(on ? UiStyle::BG : UiStyle::DIM);
            canvas.drawString(sub, 90, y + 2);
        };
        row(52, regionSel == 0, "NA", "US / ASIA");
        row(70, regionSel == 1, "EU", "EU / AU / ME");
        return;
    }
    if (phase == Phase::FILE_PICK) {
        canvas.drawString("/0N3P0rK/ir", 10, 32);
        if (!fileCount) {
            canvas.setTextColor(UiStyle::RED);
            canvas.drawString("NO .ir / .txt", 10, 50);
        } else {
            for (uint8_t i = fileScroll; i < fileCount && i < fileScroll + 5; i++) {
                canvas.setTextColor(i == fileSel ? UiStyle::PINK : UiStyle::TEXT);
                canvas.drawString(fileNames[i], 10, 44 + (i - fileScroll) * 10);
            }
        }
        return;
    }
    canvas.drawString(packName, 10, 32);
    canvas.setTextColor(UiStyle::GOLD);
    canvas.drawString(statusMsg, 10, 48);
    canvas.setTextColor(UiStyle::DIM);
    char nbuf[32];
    snprintf(nbuf, sizeof(nbuf), "CODES %u",
             (unsigned)((pack == Pack::BUILTIN) ? IrPower::getCodeCount() : codeCount));
    canvas.drawString(nbuf, 10, 64);
    canvas.drawString("SPC fire  E file  R region", 10, 80);
}
