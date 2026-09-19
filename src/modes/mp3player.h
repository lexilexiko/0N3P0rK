// MP3 player — SD music on the Cardputer speaker.
//
// Files:  /0N3P0rK/music/*.mp3  (top level, up to 32 tracks)
// Scene:  cassette + VU + track/time.
// Bar:    1 vol-   2 prev   3 play/stop   4 next   5 vol+
//
// Same on/off + draw contract as FileMgrMode/BadUsbMode so App/Menu wire it
// the same way: start() on enter, stop() on leave, draw() into the main
// canvas, drawBar() into the bottom bar.
#pragma once
#include <Arduino.h>
#include <M5Unified.h>

class Mp3PlayerMode {
public:
    static void start();
    static void stop();
    // Keys + scene state. Call from App::loop().
    static void update();
    // While the window is minimized the farm owns the screen, but the
    // transport keys 1..5 keep working so the song can be steered from there.
    // Returns true when one of those keys was consumed.
    static bool minimizedKey();
    // One-liner for the bottom bar (used when minimized): "PLAY 03/12 01:23".
    static void getStatusLine(char* buf, size_t n);
    // Audio service (decode + feed the speaker). Call from the main loop so
    // music keeps playing while the window is minimized or the screen is off.
    static void tick();
    static void draw(M5Canvas& canvas);
    static void drawBar(M5Canvas& canvas);
    static bool isRunning() { return running; }

private:
    static bool running;
};