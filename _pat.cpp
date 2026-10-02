// Standalone pattern test: mirrors the exact class layout of modes/scriptmode.h
#include <cstdint>
class ScriptMode {
public:
    static bool isRunning() { return running; }
    static bool isTyping() { return running && tab == Tab::REPL && phase == Phase::REPL; }
private:
    enum class Tab   : uint8_t { FILES = 0, REPL = 1 };
    enum class Phase : uint8_t { BROWSER, OUTPUT, REPL };
    static bool  running;
    static Tab   tab;
    static Phase phase;
    static void enterTab(Tab t);
};
bool ScriptMode::running = false;
ScriptMode::Tab ScriptMode::tab = ScriptMode::Tab::FILES;
ScriptMode::Phase ScriptMode::phase = ScriptMode::Phase::BROWSER;
void ScriptMode::enterTab(Tab t) { (void)t; }