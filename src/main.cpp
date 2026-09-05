// 0N3P0rK - Tamagotchi pig + barn on M5Cardputer / Cardputer ADV

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "build_info.h"
#include "core/config.h"
#include "core/xp.h"
#include "core/app.h"
#include "ui/display.h"
#include "ui/menu.h"
#include "piglet/avatar.h"
#include "piglet/mood.h"
#include "audio/sfx.h"
#include "storage/littlefs_ops.h"
#include "board/board.h"
#include "net/ap_sta.h"
#include "cap/sniffer.h"
#include "modes/evilpig.h"
#include "modes/pigpass.h"
#include "board/led.h"

static void preInitWiFiDriverEarly() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, false);
    WiFi.setSleep(false);
    delay(50);
}

static void setupHeapLayout() {
    size_t beforeLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    Serial.printf("[BOOT] pre-fence largest=%u\n", (unsigned)beforeLargest);

    static constexpr size_t kFenceSize = 80000;
    void* fence = heap_caps_malloc(kFenceSize, MALLOC_CAP_8BIT);
    preInitWiFiDriverEarly();
    if (fence) heap_caps_free(fence);

    Serial.printf("[BOOT] post-fence free=%u largest=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void setup() {
    Serial.begin(115200);
    delay(120);
    Serial.println();
    Serial.printf(">>> %s v%s build=%s\n", ON3PORK_NAME, ON3PORK_VERSION, ON3PORK_BUILD);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    pinMode(0, INPUT_PULLUP);

    setupHeapLayout();
    yield();

    if (!Storage::begin()) {
        Serial.println("[BOOT] LittleFS failed - continuing");
    }
    Board::startKeyboard();
    Serial.printf("[BOOT] board=%s\n", Board::modelLabel());

    Config::init();
    XP::begin();
    M5.Display.setBrightness(Config::personality().brightness * 255 / 100);

    Display::init();
    SFX::init();
    Avatar::init();
    Display::showBootSplash();
    Display::refreshBrightness();
    Mood::init();
    Menu::begin();
    App::begin();
    yield();

    Net::begin();
    Storage::loadKeysIntoNet();
    Cap::begin();
    Led::begin();
    EvilPigMode::init();
    PigpassMode::init();

    Net::Status s = Net::status();
    Serial.printf("[BOOT] wifi mode ssid=%s ip=%s\n", s.ssid, s.ip);
    Serial.printf("[BOOT] pig=%s ready\n", Config::personality().name);
}

static unsigned long s_lastHeapLog = 0;

void loop() {
    M5Cardputer.update();
    App::loop();
    Display::update();
    SFX::update();
    XP::tick();
    Cap::loop();
    Led::update();

    if (millis() - s_lastHeapLog > 30000) {
        s_lastHeapLog = millis();
        Serial.printf("[LOOP] heap=%u largest=%u cap=%s\n",
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      Cap::runMode() == Cap::RunMode::Aggressive ? "AGG" :
                      (Cap::isRunning() ? "LITE" : "off"));
    }
    delay(5);
    yield();
}
