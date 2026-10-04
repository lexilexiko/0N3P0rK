#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <cstdint>
#include <vector>

// WiFi Password Brute Force Cracker Mode (PigPass)
enum class PigpassState : uint8_t {
    IDLE = 0,
    SELECT_HANDSHAKE = 1,
    SELECT_WORDLIST = 2,
    SELECT_MASK = 3,   // charset + length generator
    RUNNING = 4,
    PAUSED = 5,
    DONE = 6
};

// Handshake browser tabs (same idea as LOOT WPASEC / PWNCRACK).
enum class PigpassHsTab : uint8_t {
    PCAP = 0,   // .pcap / .cap
    HC22000 = 1 // .22000 / .hc22000
};

struct PigpassFileEntry {
    char name[40];
    char path[72];
};

class PigpassMode {
public:
    // Lifecycle
    static void init();
    static void start();
    static void stop();
    static void update();
    static void draw(M5Canvas& canvas);

    // State query
    static bool isRunning() { return state != PigpassState::IDLE; }
    static PigpassState getState() { return state; }

    // Status
    static uint64_t getAttempts() { return attempts; }
    static double getRate() { return rate; }
    static uint32_t getElapsedSeconds() { return elapsedSeconds; }

    // Results
    static bool isFoundPassword() { return foundPassword; }
    static bool isFromCache() { return fromCache; }
    static const char* getFoundPasswordString() { return foundPw; }
    static const char* getSSID() { return ssid; }
    static const char* getCurrentTry();

    // Bottom-bar status line
    static void getStatusLine(char* out, size_t len);

private:
    static PigpassState state;

    static uint64_t attempts;
    static double rate;
    static uint32_t elapsedSeconds;
    static bool foundPassword;
    static bool fromCache;
    static char foundPw[64];
    static char ssid[33];

    static uint32_t startTime;
    static uint32_t lastUpdateTime;

    static char handshakePath[72];
    static char wordlistPath[72];

    // File browser state
    static std::vector<PigpassFileEntry> files;
    static PigpassHsTab hsTab;   // only used on SELECT_HANDSHAKE
    static uint16_t selectedIndex;
    static uint16_t scrollOffset;
    static bool keyWasPressed;
    static bool uiDirty;

    // Screen shows 5 rows; scroll for the rest. List capacity matches Cap HS limit.
    static constexpr uint8_t VISIBLE_ITEMS = 5;
    static constexpr size_t MAX_FILES = 200;

    // Mask / charset generator (on-the-fly, no SD dictionary file)
    static bool maskMode;
    static uint8_t maskCharsetId;   // 0 digits, 1 lower, 2 lower+dig, 3 alnum, 4 upper+dig
    static uint8_t maskLen;         // 8..12 (WPA min 8)
    static uint64_t maskIndex;      // next candidate index at current length
    static char maskCharset[72];
    static uint8_t maskCharsetLen;

    // Internal methods
    static void drawUI(M5Canvas& canvas);
    static void drawFileBrowser(M5Canvas& canvas, const char* title);
    static void drawMaskSetup(M5Canvas& canvas);
    static void handleInput();
    static void handleMaskInput();
    static void runBrute();
    static void transitionState(PigpassState newState);
    static void scanHandshakeFiles();
    static void scanWordlistFiles();
    static void scanDirForExt(const char* dirPath, const char* const* exts, size_t extCount);
    static void selectCurrentFile();
    static void clearFileList();
    static void markFound(const char* password, bool cached);
    static bool tryLoadCachedResult();
    static void saveCrackedResult(const char* password);

    // Resume after stop / reboot: SD checkpoint for same HS+wordlist
    static void saveCheckpoint();
    static bool loadCheckpoint(const char* hsPath, const char* wlPath,
                               uint32_t* outOffset, uint64_t* outAttempts,
                               uint32_t* outElapsed);
    static void clearCheckpoint();
    static uint32_t wordlistLogicalOffset();

    // Last-used wordlist preference
    static void saveLastWordlist(const char* path);
    static bool loadLastWordlist(char* out, size_t outLen);

    // Mask generator
    static void applyMaskCharset();
    static const char* maskCharsetName();
    static bool nextMaskPassword(char* out, size_t outSz);
    static void beginMaskRun();
    static void beginWordlistRun(const char* path);
};
