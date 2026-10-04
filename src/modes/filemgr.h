// SD file browser, text editor and image viewer for the Cardputer.
#pragma once
#include <Arduino.h>
#include <M5Unified.h>

class FileMgrMode {
public:
    static void start();
    static void stop();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isRunning() { return running; }
    static void getStatusLine(char* buf, size_t n);
    static bool isTyping() {
        return running && (phase == Phase::EDIT || phase == Phase::NAME_INPUT);
    }

private:
    enum class Phase : uint8_t {
        BROWSE, VIEW, EDIT, IMAGE, INFO, NAME_INPUT, CONFIRM_DEL, CONFIRM_OVERWRITE
    };
    enum class NameAction : uint8_t { NEW_FILE, NEW_DIR, RENAME };
    enum class ClipboardAction : uint8_t { EMPTY, COPY, MOVE };

    struct Entry {
        char name[64];
        bool isDir;
        uint32_t size;
    };

    static constexpr uint16_t EDIT_CAP = 6144;   // internal RAM, no PSRAM on this board
    static constexpr uint16_t PATH_BUF = 192;
    static constexpr uint8_t NAME_BUF = 64;
    static constexpr uint8_t STATUS_BUF = 40;
    static constexpr uint8_t VIS_ROWS = 5;

    static bool running;
    static Phase phase;

    static char* curPath;
    static Entry* entries;
    static uint16_t entryCount;
    static uint16_t entryCapacity;
    static bool listTruncated;
    static uint16_t sel;
    static uint16_t scroll;
    static bool keyLatch;
    static char* statusMsg;

    // view/edit buffer — one file at a time, either volume
    static char* buf;
    static uint16_t bufLen;
    static uint16_t cursor;
    static bool dirty;
    static char* openName;
    static uint16_t viewTopLine;
    static char* nameBuf;
    static NameAction nameAction;
    static char* clipboardPath;
    static ClipboardAction clipboardAction;

    static bool allocateRuntimeBuffers();
    static void freeRuntimeBuffers();
    static bool allocateTextBuffer();
    static void freeTextBuffer();
    static bool reserveEntries(uint16_t required);
    static void refreshList();
    static void enterDir(const char* name);
    static void goUp();
    static bool openSelected();
    static bool loadFile(const char* path);
    static bool saveFile();
    static void buildFullPath(char* out, size_t n, const char* name);
    static bool isTextFile(const char* name);
    static bool imageType(const char* name);
    static void beginNameInput(NameAction action);
    static bool commitNameInput();
    static bool pasteClipboard();
    static bool copyPath(const char* src, const char* dst, uint8_t depth = 0);
    static bool removePath(const char* path, uint8_t depth = 0);
    static void back();
    static uint16_t cursorLine();
    static void handleBrowseInput();
    static void handleViewInput();
    static void handleEditInput();
    static void drawBrowse(M5Canvas& canvas);
    static void drawViewEdit(M5Canvas& canvas);
    static void drawImage(M5Canvas& canvas);
};
