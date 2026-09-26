// InspectorPig — handshake inspector.
// Reads the captures in /0N3P0rK/handshakes and reports what is actually
// inside them, the way Wireshark would: container, radiotap, 802.11 frame
// classes, EAPOL message numbers, ESSID, replay counters, RSN parameters.
//
// Why it exists: before trusting a sync (WPASec / Pwncrack / OHC) we want to
// know whether a capture is a real, complete handshake. Every run can be saved
// under /0N3P0rK/inspector/ so a verdict is always reviewable later.
//
// Same on/off + draw contract as FileMgrMode / LootMenu so App wires it the
// same way. This module only READS captures — it never modifies them.

#pragma once

#include <Arduino.h>
#include <M5Unified.h>

class InspectorPig {
public:
    static void start();
    static void stop();
    static void update();
    static void draw(M5Canvas& canvas);
    static bool isRunning() { return running; }
    static void getStatusLine(char* buf, size_t n);

private:
    enum class Phase : uint8_t { LIST, DETAIL };
    enum class Kind : uint8_t { PCAP, HC22000 };

    struct Entry {
        char     name[40];
        uint32_t size;
        Kind     kind;
        bool     checked;     // has a verdict from the last single-file run
        uint8_t  score;       // 0..100 confidence
        char     verdict[14];
    };

    static constexpr uint8_t MAX_ENTRIES = 48;
    static constexpr uint8_t VIS_ROWS    = 5;
    static constexpr uint8_t MAX_LINES   = 72;
    static constexpr uint8_t LINE_LEN    = 46;
    // A capture is read into the heap only while it is being inspected, so the
    // module costs nothing to have loaded. Anything larger is reported as too
    // big instead of being silently truncated.
    static constexpr uint32_t READ_MAX   = 128u * 1024u;

    static bool  running;
    static Phase phase;
    static Entry entries[MAX_ENTRIES];
    static uint8_t entryCount;
    static uint8_t sel;
    static uint8_t scroll;
    static bool  keyLatch;
    static char  statusMsg[40];

    // Report lines hold whatever the current file produced; the same text is
    // written to SD while it is generated.
    static char  lines[MAX_LINES][LINE_LEN];
    static uint8_t lineCount;
    static uint8_t lineScroll;

    // --- report sinks -----------------------------------------------------
    static void reportBegin();                  // reset the line buffer
    static void reportFlush(const char* path);  // write the buffered lines out
    static void emit(const char* fmt, ...);

    // --- helpers ----------------------------------------------------------
    static void macToStr(const uint8_t* mac, char* out);
    static void hexToAscii(const char* hex, char* out, size_t outLen);
    static const char* verdictFor(uint8_t score);

    // --- analysis ---------------------------------------------------------
    static void inspectSelected();
    static void inspectAll();
    static uint8_t analyzePath(const char* path, Kind kind);  // returns score
    static uint8_t analyze22000(const uint8_t* data, size_t len);
    static uint8_t analyzePcap(const uint8_t* data, size_t len);

    // --- list -------------------------------------------------------------
    static void refreshList();
    static void handleInput();
    static void drawList(M5Canvas& canvas);
    static void drawDetail(M5Canvas& canvas);
};
