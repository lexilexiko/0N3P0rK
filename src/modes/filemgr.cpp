#include "filemgr.h"
#include "../storage/littlefs_ops.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include "../audio/sfx.h"
#include "../piglet/avatar.h"
#include "../core/config.h"
#include "../core/app.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

namespace {
char* s_runtimeStorage = nullptr;
}

bool FileMgrMode::running = false;
FileMgrMode::Phase FileMgrMode::phase = FileMgrMode::Phase::BROWSE;
char* FileMgrMode::curPath = nullptr;
FileMgrMode::Entry* FileMgrMode::entries = nullptr;
uint16_t FileMgrMode::entryCount = 0;
uint16_t FileMgrMode::entryCapacity = 0;
bool FileMgrMode::listTruncated = false;
uint16_t FileMgrMode::sel = 0;
uint16_t FileMgrMode::scroll = 0;
bool FileMgrMode::keyLatch = false;
char* FileMgrMode::statusMsg = nullptr;
char* FileMgrMode::buf = nullptr;
uint16_t FileMgrMode::bufLen = 0;
uint16_t FileMgrMode::cursor = 0;
bool FileMgrMode::dirty = false;
char* FileMgrMode::openName = nullptr;
uint16_t FileMgrMode::viewTopLine = 0;
char* FileMgrMode::nameBuf = nullptr;
FileMgrMode::NameAction FileMgrMode::nameAction = FileMgrMode::NameAction::NEW_FILE;
char* FileMgrMode::clipboardPath = nullptr;
FileMgrMode::ClipboardAction FileMgrMode::clipboardAction =
    FileMgrMode::ClipboardAction::EMPTY;

namespace {
static constexpr uint8_t MAX_PATH_DEPTH = 8;
static constexpr uint16_t COPY_CHUNK = 512;

class SDImageData : public lgfx::DataWrapper {
public:
    SDImageData() { need_transaction = true; }
    bool open(const char* path) override {
        file = SD.open(path, FILE_READ);
        return (bool)file;
    }
    int read(uint8_t* data, uint32_t length) override {
        return file.read(data, length);
    }
    void skip(int32_t offset) override { file.seek(offset, fs::SeekCur); }
    bool seek(uint32_t offset) override { return file.seek(offset, fs::SeekSet); }
    void close() override { file.close(); }
    int32_t tell() override { return (int32_t)file.position(); }

private:
    File file;
};

static const char* baseName(const char* path) {
    const char* slash = strrchr(path ? path : "", '/');
    return slash ? slash + 1 : (path ? path : "");
}

static bool validName(const char* name) {
    if (!name || !name[0] || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const char* p = name; *p; ++p) {
        if ((uint8_t)*p < 32 || *p == '/' || *p == '\\' || *p == ':' ||
            *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' ||
            *p == '|') return false;
    }
    return true;
}

static void formatSize(char* out, size_t n, uint32_t bytes) {
    if (bytes < 1024) snprintf(out, n, "%lu B", (unsigned long)bytes);
    else if (bytes < 1024UL * 1024UL)
        snprintf(out, n, "%lu KB", (unsigned long)((bytes + 512) / 1024));
    else snprintf(out, n, "%lu.%lu MB",
                  (unsigned long)(bytes / (1024UL * 1024UL)),
                  (unsigned long)((bytes % (1024UL * 1024UL) * 10) /
                                  (1024UL * 1024UL)));
}

static bool hasExtension(const char* name, const char* ext) {
    const char* dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

}

bool FileMgrMode::allocateRuntimeBuffers() {
    if (s_runtimeStorage) return true;
    constexpr size_t storageSize = (size_t)PATH_BUF * 2 + STATUS_BUF +
                                   (size_t)NAME_BUF * 2;
    s_runtimeStorage = static_cast<char*>(calloc(storageSize, 1));
    if (!s_runtimeStorage) return false;

    char* next = s_runtimeStorage;
    curPath = next;
    next += PATH_BUF;
    statusMsg = next;
    next += STATUS_BUF;
    openName = next;
    next += NAME_BUF;
    nameBuf = next;
    next += NAME_BUF;
    clipboardPath = next;
    buf = nullptr;
    strcpy(curPath, "/");
    return true;
}

void FileMgrMode::freeRuntimeBuffers() {
    freeTextBuffer();
    free(s_runtimeStorage);
    s_runtimeStorage = nullptr;
    free(entries);
    curPath = statusMsg = buf = openName = nameBuf = clipboardPath = nullptr;
    entries = nullptr;
    entryCount = entryCapacity = 0;
    listTruncated = false;
}

bool FileMgrMode::allocateTextBuffer() {
    if (buf) return true;
    buf = static_cast<char*>(malloc(EDIT_CAP));
    if (!buf) return false;
    buf[0] = '\0';
    return true;
}

void FileMgrMode::freeTextBuffer() {
    free(buf);
    buf = nullptr;
    bufLen = cursor = viewTopLine = 0;
    dirty = false;
}

bool FileMgrMode::reserveEntries(uint16_t required) {
    if (required <= entryCapacity) return true;
    size_t capacity = entryCapacity ? (size_t)entryCapacity + 32 : 32;
    if (capacity < required) capacity = required;
    if (capacity > UINT16_MAX) capacity = UINT16_MAX;
    if (capacity < required) return false;

    Entry* resized = static_cast<Entry*>(
        realloc(entries, capacity * sizeof(Entry)));
    if (!resized) return false;
    entries = resized;
    entryCapacity = (uint16_t)capacity;
    return true;
}

void FileMgrMode::buildFullPath(char* out, size_t n, const char* name) {
    if (strcmp(curPath, "/") == 0) snprintf(out, n, "/%s", name);
    else snprintf(out, n, "%s/%s", curPath, name);
}

void FileMgrMode::refreshList() {
    if (!curPath || (!entries && entryCapacity)) return;
    char keepName[NAME_BUF] = "";
    if (sel < entryCount) {
        strncpy(keepName, entries[sel].name, sizeof(keepName) - 1);
        keepName[sizeof(keepName) - 1] = '\0';
    }
    entryCount = sel = scroll = 0;
    listTruncated = false;
    if (!Storage::available() || !Config::isSDAvailable()) return;

    File dir = SD.open(curPath);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    bool truncated = false;
    File entry = dir.openNextFile();
    while (entry) {
        const char* name = baseName(entry.name());
        if (name[0] && strcmp(name, ".") && strcmp(name, "..")) {
            if (entryCount == UINT16_MAX ||
                !reserveEntries((uint16_t)(entryCount + 1))) {
                truncated = true;
                entry.close();
                break;
            }
            Entry& dst = entries[entryCount++];
            strncpy(dst.name, name, sizeof(dst.name) - 1);
            dst.name[sizeof(dst.name) - 1] = '\0';
            dst.isDir = entry.isDirectory();
            dst.size = dst.isDir ? 0 : (uint32_t)entry.size();
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    if (entryCount > 1) {
        qsort(entries, entryCount, sizeof(Entry),
              [](const void* lhs, const void* rhs) {
                  const Entry* a = static_cast<const Entry*>(lhs);
                  const Entry* b = static_cast<const Entry*>(rhs);
                  if (a->isDir != b->isDir) return a->isDir ? -1 : 1;
                  return strcasecmp(a->name, b->name);
              });
    }
    for (uint16_t i = 0; i < entryCount; ++i) {
        if (keepName[0] && !strcmp(entries[i].name, keepName)) {
            sel = i;
            break;
        }
    }
    if (sel >= VIS_ROWS) scroll = (uint16_t)(sel - VIS_ROWS + 1);
    listTruncated = truncated;
    if (listTruncated) Display::showToast("LIST LIMITED: LOW MEM", 1400);
}

void FileMgrMode::enterDir(const char* name) {
    char next[PATH_BUF];
    buildFullPath(next, sizeof(next), name);
    if (strlen(next) >= PATH_BUF) {
        Display::showToast("PATH TOO LONG", 900);
        return;
    }
    strncpy(curPath, next, PATH_BUF - 1);
    curPath[PATH_BUF - 1] = '\0';
    refreshList();
}

void FileMgrMode::goUp() {
    if (!strcmp(curPath, "/")) return;
    char* slash = strrchr(curPath, '/');
    if (slash == curPath) curPath[1] = '\0';
    else if (slash) *slash = '\0';
    refreshList();
}

bool FileMgrMode::isTextFile(const char* name) {
    static const char* const ext[] = {
        ".txt", ".log", ".csv", ".ini", ".md", ".json", ".xml", ".html",
        ".htm", ".css", ".js", ".c", ".cpp", ".h", ".hpp", ".py", ".yaml",
        ".yml", ".conf", ".cfg", ".sh", ".toml", ".properties"
    };
    for (const char* suffix : ext) if (hasExtension(name, suffix)) return true;
    return false;
}

bool FileMgrMode::imageType(const char* name) {
    return hasExtension(name, ".jpg") || hasExtension(name, ".jpeg") ||
           hasExtension(name, ".bmp") || hasExtension(name, ".png");
}

bool FileMgrMode::loadFile(const char* path) {
    if (!allocateTextBuffer()) return false;
    File file = SD.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        freeTextBuffer();
        return false;
    }
    size_t size = file.size();
    if (size > EDIT_CAP - 1) size = EDIT_CAP - 1;
    bufLen = (uint16_t)file.read((uint8_t*)buf, size);
    file.close();
    for (uint16_t i = 0; i < bufLen; ++i) {
        uint8_t ch = (uint8_t)buf[i];
        if (ch && ch < 32 && ch != '\n' && ch != '\r' && ch != '\t') buf[i] = '.';
    }
    buf[bufLen] = '\0';
    cursor = viewTopLine = 0;
    dirty = false;
    return true;
}

bool FileMgrMode::saveFile() {
    char path[PATH_BUF], temp[PATH_BUF], backup[PATH_BUF];
    buildFullPath(path, sizeof(path), openName);
    if (snprintf(temp, sizeof(temp), "%s.fm_tmp", path) >= (int)sizeof(temp) ||
        snprintf(backup, sizeof(backup), "%s.fm_bak", path) >= (int)sizeof(backup)) {
        strncpy(statusMsg, "PATH TOO LONG", STATUS_BUF - 1);
        return false;
    }
    if (SD.exists(temp) && !SD.remove(temp)) return false;
    File file = SD.open(temp, FILE_WRITE);
    if (!file) return false;
    size_t written = file.write((const uint8_t*)buf, bufLen);
    file.flush();
    file.close();
    if (written != bufLen) {
        SD.remove(temp);
        return false;
    }

    bool hadOriginal = SD.exists(path);
    if (hadOriginal) {
        if (SD.exists(backup)) SD.remove(backup);
        if (!SD.rename(path, backup)) {
            SD.remove(temp);
            return false;
        }
    }
    if (!SD.rename(temp, path)) {
        if (hadOriginal) SD.rename(backup, path);
        SD.remove(temp);
        return false;
    }
    if (hadOriginal) SD.remove(backup);
    dirty = false;
    return true;
}

bool FileMgrMode::openSelected() {
    if (!entryCount || sel >= entryCount) return false;
    Entry& entry = entries[sel];
    if (entry.isDir) {
        enterDir(entry.name);
        return true;
    }
    strncpy(openName, entry.name, NAME_BUF - 1);
    openName[NAME_BUF - 1] = '\0';
    if (imageType(entry.name)) {
        phase = Phase::IMAGE;
        return true;
    }
    if (!isTextFile(entry.name)) {
        phase = Phase::INFO;
        return true;
    }
    char path[PATH_BUF];
    buildFullPath(path, sizeof(path), entry.name);
    if (!allocateTextBuffer()) {
        Display::showToast("LOW MEM - TEXT", 1200);
        return false;
    }
    if (!loadFile(path)) {
        Display::showToast("OPEN FAILED", 900);
        return false;
    }
    phase = Phase::VIEW;
    return true;
}

void FileMgrMode::beginNameInput(NameAction action) {
    nameAction = action;
    nameBuf[0] = '\0';
    if (action == NameAction::RENAME && sel < entryCount)
        strncpy(nameBuf, entries[sel].name, NAME_BUF - 1);
    nameBuf[NAME_BUF - 1] = '\0';
    phase = Phase::NAME_INPUT;
}

bool FileMgrMode::commitNameInput() {
    if (!validName(nameBuf)) {
        Display::showToast("INVALID NAME", 900);
        return false;
    }
    if (nameAction == NameAction::NEW_FILE && !allocateTextBuffer()) {
        Display::showToast("LOW MEM - TEXT", 1200);
        return false;
    }
    if (nameAction == NameAction::NEW_FILE && !strchr(nameBuf, '.')) {
        if (strlen(nameBuf) + 4 >= NAME_BUF) {
            Display::showToast("NAME TOO LONG", 900);
            return false;
        }
        strcat(nameBuf, ".txt");
    }
    char target[PATH_BUF];
    buildFullPath(target, sizeof(target), nameBuf);
    if (nameAction == NameAction::RENAME && sel < entryCount) {
        char source[PATH_BUF];
        buildFullPath(source, sizeof(source), entries[sel].name);
        if (strcasecmp(entries[sel].name, nameBuf) == 0) {
            phase = Phase::BROWSE;
            return true;
        }
    }
    if (SD.exists(target)) {
        phase = Phase::CONFIRM_OVERWRITE;
        strncpy(statusMsg, "REPLACE EXISTING?", STATUS_BUF - 1);
        return false;
    }

    bool ok = false;
    if (nameAction == NameAction::NEW_DIR) {
        ok = SD.mkdir(target);
    } else if (nameAction == NameAction::NEW_FILE) {
        File file = SD.open(target, FILE_WRITE);
        ok = (bool)file;
        if (file) file.close();
    } else if (sel < entryCount) {
        char source[PATH_BUF];
        buildFullPath(source, sizeof(source), entries[sel].name);
        ok = SD.rename(source, target);
    }
    if (!ok) {
        Display::showToast("OPERATION FAILED", 1000);
        return false;
    }
    if (nameAction == NameAction::NEW_FILE) {
        strncpy(openName, nameBuf, NAME_BUF - 1);
        openName[NAME_BUF - 1] = '\0';
        bufLen = cursor = 0;
        buf[0] = '\0';
        dirty = false;
        phase = Phase::EDIT;
    } else {
        phase = Phase::BROWSE;
    }
    refreshList();
    Display::showToast("DONE", 700);
    return true;
}

bool FileMgrMode::removePath(const char* path, uint8_t depth) {
    if (depth > MAX_PATH_DEPTH) return false;
    File node = SD.open(path);
    if (!node) return false;
    if (!node.isDirectory()) {
        node.close();
        return SD.remove(path);
    }
    File child = node.openNextFile();
    bool ok = true;
    while (child) {
        char childPath[PATH_BUF];
        const char* name = baseName(child.name());
        child.close();
        if (snprintf(childPath, sizeof(childPath), "%s/%s", path, name) >=
            (int)sizeof(childPath)) {
            ok = false;
            break;
        }
        if (!removePath(childPath, (uint8_t)(depth + 1))) {
            ok = false;
            break;
        }
        child = node.openNextFile();
    }
    node.close();
    if (!ok) return false;
    return SD.rmdir(path);
}

bool FileMgrMode::copyPath(const char* src, const char* dst, uint8_t depth) {
    if (depth > MAX_PATH_DEPTH) return false;
    File input = SD.open(src, FILE_READ);
    if (!input) return false;
    if (input.isDirectory()) {
        input.close();
        if (!SD.exists(dst) && !SD.mkdir(dst)) return false;
        File dir = SD.open(src);
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            return false;
        }
        File child = dir.openNextFile();
        bool ok = true;
        while (child) {
            char childSrc[PATH_BUF], childDst[PATH_BUF];
            const char* name = baseName(child.name());
            child.close();
            if (snprintf(childSrc, sizeof(childSrc), "%s/%s", src, name) >=
                    (int)sizeof(childSrc) ||
                snprintf(childDst, sizeof(childDst), "%s/%s", dst, name) >=
                    (int)sizeof(childDst) ||
                !copyPath(childSrc, childDst, (uint8_t)(depth + 1))) {
                ok = false;
                break;
            }
            child = dir.openNextFile();
        }
        dir.close();
        return ok;
    }

    File output = SD.open(dst, FILE_WRITE);
    if (!output) {
        input.close();
        return false;
    }
    uint8_t chunk[COPY_CHUNK];
    size_t left = input.size();
    bool ok = true;
    while (left) {
        size_t want = left > sizeof(chunk) ? sizeof(chunk) : left;
        size_t got = input.read(chunk, want);
        if (!got || output.write(chunk, got) != got) {
            ok = false;
            break;
        }
        left -= got;
        yield();
    }
    output.flush();
    output.close();
    input.close();
    if (!ok) SD.remove(dst);
    return ok;
}

bool FileMgrMode::pasteClipboard() {
    if (clipboardAction == ClipboardAction::EMPTY || !clipboardPath[0]) {
        Display::showToast("CLIPBOARD EMPTY", 800);
        return false;
    }
    char target[PATH_BUF];
    if (snprintf(target, sizeof(target), "%s%s%s", curPath,
                 strcmp(curPath, "/") ? "/" : "", baseName(clipboardPath)) >=
        (int)sizeof(target)) {
        Display::showToast("PATH TOO LONG", 900);
        return false;
    }
    if (!strcmp(target, clipboardPath)) {
        Display::showToast("SAME LOCATION", 800);
        return false;
    }
    if (SD.exists(target)) {
        phase = Phase::CONFIRM_OVERWRITE;
        strncpy(statusMsg, "REPLACE PASTE TARGET?", STATUS_BUF - 1);
        return false;
    }
    bool ok = false;
    if (clipboardAction == ClipboardAction::MOVE) {
        ok = SD.rename(clipboardPath, target);
        if (ok) clipboardAction = ClipboardAction::EMPTY;
    } else {
        ok = copyPath(clipboardPath, target);
    }
    if (!ok) {
        Display::showToast("PASTE FAILED", 1000);
        return false;
    }
    refreshList();
    Display::showToast("PASTED", 700);
    return true;
}

void FileMgrMode::start() {
    phase = Phase::BROWSE;
    if (!allocateRuntimeBuffers()) {
        running = false;
        Display::showToast("LOW MEM - FILES", 1500);
        return;
    }
    running = true;
    strncpy(curPath, "/", PATH_BUF - 1);
    curPath[PATH_BUF - 1] = '\0';
    keyLatch = true;
    clipboardAction = ClipboardAction::EMPTY;
    clipboardPath[0] = '\0';
    refreshList();
    SFX::setMuted(false);
    SFX::stop();
    SFX::play(SFX::MODE_ENTER);
    Avatar::setState(AvatarState::HUNTING);
}

void FileMgrMode::stop() {
    running = false;
    freeRuntimeBuffers();
    clipboardAction = ClipboardAction::EMPTY;
    phase = Phase::BROWSE;
    SFX::setMuted(false);
    SFX::stop();
    Avatar::setState(AvatarState::NEUTRAL);
}

void FileMgrMode::back() {
    switch (phase) {
        case Phase::NAME_INPUT:
        case Phase::CONFIRM_DEL:
        case Phase::CONFIRM_OVERWRITE:
            phase = Phase::BROWSE;
            return;
        case Phase::EDIT:
            if (dirty) {
                if (!saveFile()) {
                    Display::showToast("SAVE FAILED", 1000);
                    return;
                }
                Display::showToast("SAVED", 700);
            }
            phase = Phase::BROWSE;
            freeTextBuffer();
            refreshList();
            return;
        case Phase::IMAGE:
        case Phase::INFO:
            phase = Phase::BROWSE;
            refreshList();
            return;
        case Phase::VIEW:
            phase = Phase::BROWSE;
            freeTextBuffer();
            refreshList();
            return;
        case Phase::BROWSE:
            if (strcmp(curPath, "/")) goUp();
            else stop();
            return;
    }
}

uint16_t FileMgrMode::cursorLine() {
    uint16_t line = 0;
    for (uint16_t i = 0; i < cursor && i < bufLen; ++i)
        if (buf[i] == '\n') ++line;
    return line;
}

void FileMgrMode::handleBrowseInput() {
    auto& keyboard = M5Cardputer.Keyboard;
    if (keyEsc() || keyMin()) {
        back();
        return;
    }
    if (keyboard.isKeyPressed(';')) {
        if (sel > 0) --sel;
        if (sel < scroll) scroll = sel;
        return;
    }
    if (keyboard.isKeyPressed('.')) {
        if (entryCount && sel + 1 < entryCount) ++sel;
        if (sel >= scroll + VIS_ROWS) scroll = (uint16_t)(sel - VIS_ROWS + 1);
        return;
    }
    if (keyboard.isKeyPressed(',') || keyboard.isKeyPressed('/')) {
        if (keyboard.isKeyPressed(',')) {
            back();
            return;
        }
        if (entryCount) openSelected();
        return;
    }
    if (keyboard.keysState().enter) {
        if (entryCount) openSelected();
        return;
    }
    if (keyboard.isKeyPressed('n') || keyboard.isKeyPressed('N')) {
        beginNameInput(NameAction::NEW_FILE);
        return;
    }
    if (keyboard.isKeyPressed('m') || keyboard.isKeyPressed('M')) {
        beginNameInput(NameAction::NEW_DIR);
        return;
    }
    if ((keyboard.isKeyPressed('r') || keyboard.isKeyPressed('R')) && entryCount) {
        beginNameInput(NameAction::RENAME);
        return;
    }
    if ((keyboard.isKeyPressed('c') || keyboard.isKeyPressed('C')) && entryCount) {
        buildFullPath(clipboardPath, PATH_BUF, entries[sel].name);
        clipboardAction = ClipboardAction::COPY;
        Display::showToast("COPIED TO CLIPBOARD", 800);
        return;
    }
    if ((keyboard.isKeyPressed('v') || keyboard.isKeyPressed('V')) && entryCount) {
        buildFullPath(clipboardPath, PATH_BUF, entries[sel].name);
        clipboardAction = ClipboardAction::MOVE;
        Display::showToast("READY TO MOVE  P", 900);
        return;
    }
    if (keyboard.isKeyPressed('p') || keyboard.isKeyPressed('P')) {
        pasteClipboard();
        return;
    }
    if ((keyboard.isKeyPressed('x') || keyboard.isKeyPressed('X')) && entryCount) {
        phase = Phase::CONFIRM_DEL;
        return;
    }
    if (keyboard.isKeyPressed('t') || keyboard.isKeyPressed('T')) {
        refreshList();
        return;
    }
}

void FileMgrMode::handleViewInput() {
    auto& keyboard = M5Cardputer.Keyboard;
    if (keyEsc() || keyMin()) {
        back();
        return;
    }
    if (keyboard.isKeyPressed(';')) {
        if (viewTopLine) --viewTopLine;
    } else if (keyboard.isKeyPressed('.')) {
        ++viewTopLine;
    } else if (keyboard.isKeyPressed('e') || keyboard.isKeyPressed('E')) {
        cursor = bufLen;
        phase = Phase::EDIT;
    }
}

void FileMgrMode::handleEditInput() {
    auto& keyboard = M5Cardputer.Keyboard;
    auto keys = keyboard.keysState();
    if (keyEsc()) {
        back();
        return;
    }
    if (keyboard.isKeyPressed(KEY_BACKSPACE)) {
        if (cursor) {
            memmove(buf + cursor - 1, buf + cursor, bufLen - cursor);
            --cursor;
            --bufLen;
            buf[bufLen] = '\0';
            dirty = true;
        }
        return;
    }
    if (keyboard.isKeyPressed(',')) { if (cursor) --cursor; return; }
    if (keyboard.isKeyPressed('/')) { if (cursor < bufLen) ++cursor; return; }
    if (keyboard.isKeyPressed(';') || keyboard.isKeyPressed('.')) {
        bool up = keyboard.isKeyPressed(';');
        uint16_t p = cursor;
        if (up) {
            while (p > 0 && buf[p - 1] != '\n') --p;
            if (p > 0) { --p; while (p > 0 && buf[p - 1] != '\n') --p; }
        } else {
            while (p < bufLen && buf[p] != '\n') ++p;
            if (p < bufLen) ++p;
        }
        cursor = p;
        return;
    }
    if (keys.enter) {
        if (bufLen + 1 < EDIT_CAP) {
            memmove(buf + cursor + 1, buf + cursor, bufLen - cursor);
            buf[cursor++] = '\n';
            buf[++bufLen] = '\0';
            dirty = true;
        }
        return;
    }
    for (char ch : keys.word) {
        if (ch < 32 || ch >= 127 || bufLen + 1 >= EDIT_CAP) continue;
        memmove(buf + cursor + 1, buf + cursor, bufLen - cursor);
        buf[cursor++] = ch;
        buf[++bufLen] = '\0';
        dirty = true;
    }
}

void FileMgrMode::update() {
    if (!running || App::windowHidden() || !keyNewPress(keyLatch)) return;
    auto& keyboard = M5Cardputer.Keyboard;

    if (phase == Phase::BROWSE) {
        handleBrowseInput();
    } else if (phase == Phase::VIEW) {
        handleViewInput();
    } else if (phase == Phase::EDIT) {
        handleEditInput();
    } else if (phase == Phase::NAME_INPUT) {
        if (keyEsc()) {
            phase = Phase::BROWSE;
            return;
        }
        if (keyboard.isKeyPressed(KEY_BACKSPACE)) {
            size_t len = strlen(nameBuf);
            if (len) nameBuf[len - 1] = '\0';
            return;
        }
        if (keyboard.keysState().enter) {
            commitNameInput();
            return;
        }
        for (char ch : keyboard.keysState().word) {
            if (ch >= 32 && ch < 127 && strlen(nameBuf) + 1 < NAME_BUF) {
                size_t len = strlen(nameBuf);
                nameBuf[len] = ch;
                nameBuf[len + 1] = '\0';
            }
        }
    } else if (phase == Phase::CONFIRM_DEL) {
        if (keyEsc() || keyMin() || keyboard.isKeyPressed('n') ||
            keyboard.isKeyPressed('N')) {
            phase = Phase::BROWSE;
        } else if (keyboard.isKeyPressed('y') || keyboard.isKeyPressed('Y')) {
            if (sel < entryCount) {
                char path[PATH_BUF];
                buildFullPath(path, sizeof(path), entries[sel].name);
                if (removePath(path)) {
                    Display::showToast("DELETED", 700);
                    refreshList();
                } else Display::showToast("DELETE FAILED", 1000);
            }
            phase = Phase::BROWSE;
        }
    } else if (phase == Phase::CONFIRM_OVERWRITE) {
        if (keyEsc() || keyMin() || keyboard.isKeyPressed('n') ||
            keyboard.isKeyPressed('N')) {
            phase = strstr(statusMsg, "PASTE")
                ? Phase::BROWSE : Phase::NAME_INPUT;
            return;
        }
        if (keyboard.isKeyPressed('y') || keyboard.isKeyPressed('Y')) {
            char target[PATH_BUF];
            if (statusMsg[0] && strstr(statusMsg, "PASTE")) {
                snprintf(target, sizeof(target), "%s%s%s", curPath,
                         strcmp(curPath, "/") ? "/" : "", baseName(clipboardPath));
                if (removePath(target)) {
                    phase = Phase::BROWSE;
                    pasteClipboard();
                } else {
                    phase = Phase::BROWSE;
                    Display::showToast("REPLACE FAILED", 1000);
                }
            } else {
                buildFullPath(target, sizeof(target), nameBuf);
                if (removePath(target)) {
                    phase = Phase::NAME_INPUT;
                    commitNameInput();
                } else {
                    phase = Phase::NAME_INPUT;
                    Display::showToast("REPLACE FAILED", 1000);
                }
            }
        }
    } else if (phase == Phase::IMAGE || phase == Phase::INFO) {
        if (keyEsc() || keyMin()) back();
        else if (phase == Phase::INFO && keyboard.isKeyPressed('e')) {
            phase = Phase::BROWSE;
            openSelected();
        }
    }
}

void FileMgrMode::getStatusLine(char* out, size_t n) {
    if (!out || !n) return;
    const uint8_t hintPhase = (uint8_t)((millis() % 10000u) / 2500u);
    const uint8_t page = hintPhase < 2 ? 0 : (uint8_t)(hintPhase - 1);
    switch (phase) {
        case Phase::BROWSE:
            if (!curPath) snprintf(out, n, "LOW MEMORY");
            else if (listTruncated)
                snprintf(out, n, "PARTIAL LIST %u  LOW MEM", (unsigned)entryCount);
            else if (page == 0) snprintf(out, n, "%u FILES  %s",
                                         (unsigned)entryCount, curPath);
            else if (page == 1) snprintf(out, n, "^/v SELECT  ENT OPEN  BKSP BACK");
            else snprintf(out, n, "N NEW  M FOLDER  R RENAME  X DEL");
            if (page == 2 && clipboardAction != ClipboardAction::EMPTY)
                snprintf(out, n, "%s CLIP  P PASTE",
                         clipboardAction == ClipboardAction::COPY ? "COPY" : "MOVE");
            break;
        case Phase::VIEW:
            snprintf(out, n, "TEXT  ^/v SCROLL  E EDIT  BKSP BACK");
            break;
        case Phase::EDIT:
            snprintf(out, n, "%s  BKSP DEL CHAR  ESC SAVE/BACK",
                     dirty ? "EDIT *" : "EDIT");
            break;
        case Phase::IMAGE:
            snprintf(out, n, "IMAGE PREVIEW  BKSP BACK");
            break;
        case Phase::INFO:
            snprintf(out, n, "FILE INFO  E OPEN  BKSP BACK");
            break;
        case Phase::NAME_INPUT:
            snprintf(out, n, "%s NAME  ENT OK  BKSP DEL  ESC BACK",
                     nameAction == NameAction::NEW_FILE ? "NEW FILE" :
                     nameAction == NameAction::NEW_DIR ? "NEW FOLDER" : "RENAME");
            break;
        case Phase::CONFIRM_DEL:
            snprintf(out, n, "Y DELETE  N / BKSP CANCEL");
            break;
        case Phase::CONFIRM_OVERWRITE:
            snprintf(out, n, "Y REPLACE  N / BKSP CANCEL");
            break;
    }
}

void FileMgrMode::drawBrowse(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setTextWrap(false);
    canvas.setTextColor(UiStyle::TITLE);
    uiDrawMarquee(canvas, curPath, 6, 2, 174);
    canvas.setTextColor(UiStyle::DIM);
    char counts[24];
    snprintf(counts, sizeof(counts), "%u/%u", entryCount ? (unsigned)sel + 1 : 0,
             (unsigned)entryCount);
    canvas.setTextDatum(top_right);
    canvas.drawString(counts, DISPLAY_W - 6, 2);
    canvas.setTextDatum(top_left);
    canvas.drawFastHLine(6, 13, DISPLAY_W - 12, UiStyle::CYAN);

    if (!curPath) {
        canvas.setTextColor(UiStyle::RED);
        canvas.drawString("LOW MEMORY", 8, 32);
        return;
    }
    if (!Storage::available()) {
        canvas.setTextColor(UiStyle::RED);
        canvas.drawString("NO SD CARD", 8, 28);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("INSERT CARD AND RESTART", 8, 42);
        return;
    }
    if (!entryCount) {
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString("(EMPTY FOLDER)", 8, 32);
        canvas.setTextColor(UiStyle::TEXT);
        canvas.drawString("N CREATE TEXT FILE", 8, 48);
        canvas.drawString("M CREATE FOLDER", 8, 61);
    }
    for (uint16_t i = scroll; i < entryCount && i < scroll + VIS_ROWS; ++i) {
        int y = 17 + (i - scroll) * 14;
        bool selected = i == sel;
        if (selected) canvas.fillRoundRect(4, y - 1, DISPLAY_W - 8, 13, 2, UiStyle::PINK);
        canvas.setTextColor(selected ? UiStyle::BG :
                            entries[i].isDir ? UiStyle::GOLD : UiStyle::TEXT);
        char label[56];
        snprintf(label, sizeof(label), "%s %s",
                 entries[i].isDir ? "[D]" :
                 (imageType(entries[i].name) ? "[I]" :
                  (isTextFile(entries[i].name) ? "[T]" : "[F]")),
                 entries[i].name);
        uiDrawMarquee(canvas, label, 8, y + 2, 174);
        if (!entries[i].isDir) {
            char size[12];
            formatSize(size, sizeof(size), entries[i].size);
            canvas.setTextColor(selected ? UiStyle::BG : UiStyle::DIM);
            canvas.setTextDatum(top_right);
            canvas.drawString(size, DISPLAY_W - 8, y + 2);
            canvas.setTextDatum(top_left);
        }
    }

    if (entryCount && sel < entryCount) {
        int y = MAIN_H - 15;
        canvas.fillRect(0, y - 2, DISPLAY_W, 17, UiStyle::PANEL);
        canvas.setTextColor(UiStyle::CYAN);
        canvas.drawString(entries[sel].isDir ? "FOLDER" :
                          imageType(entries[sel].name) ? "IMAGE" :
                          isTextFile(entries[sel].name) ? "TEXT" : "FILE", 6, y);
        char size[16];
        formatSize(size, sizeof(size), entries[sel].size);
        canvas.setTextColor(UiStyle::DIM);
        canvas.setTextDatum(top_right);
        canvas.drawString(size, DISPLAY_W - 6, y);
        canvas.setTextDatum(top_left);
    }
}

void FileMgrMode::drawViewEdit(M5Canvas& canvas) {
    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setTextWrap(false);
    canvas.setTextColor(phase == Phase::EDIT ? UiStyle::PINK : UiStyle::CYAN);
    uiDrawMarquee(canvas, openName, 6, 2, 204);
    if (dirty) canvas.drawString("*", DISPLAY_W - 12, 2);
    canvas.drawFastHLine(6, 13, DISPLAY_W - 12, UiStyle::PANEL);

    uint16_t line = 0, i = 0, drawn = 0;
    uint16_t curLine = phase == Phase::EDIT ? cursorLine() : viewTopLine;
    uint16_t top = phase == Phase::EDIT
        ? (curLine >= 5 ? (uint16_t)(curLine - 4) : 0)
        : viewTopLine;
    while (i <= bufLen && drawn < 6) {
        uint16_t start = i;
        while (i < bufLen && buf[i] != '\n') ++i;
        if (line >= top) {
            uint16_t len = (uint16_t)(i - start);
            if (len > 36) len = 36;
            char text[38];
            memcpy(text, buf + start, len);
            text[len] = '\0';
            int y = 17 + drawn * 13;
            canvas.setTextColor(UiStyle::TEXT);
            canvas.drawString(text, 7, y);
            if (phase == Phase::EDIT && line == curLine) {
                uint16_t col = cursor > start ? (uint16_t)(cursor - start) : 0;
                if (col > len) col = len;
                canvas.fillRect(7 + col * 6, y + 10, 5, 1, UiStyle::PINK);
            }
            ++drawn;
        }
        ++line;
        ++i;
    }
    if (bufLen == EDIT_CAP - 1) {
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString("PREVIEW LIMITED TO 6 KB", 6, MAIN_H - 10);
    }
}

void FileMgrMode::drawImage(M5Canvas& canvas) {
    canvas.fillSprite(0x0000);
    char path[PATH_BUF];
    buildFullPath(path, sizeof(path), openName);
    bool ok = false;
    SDImageData image;
    if (hasExtension(openName, ".jpg") || hasExtension(openName, ".jpeg"))
        ok = canvas.drawJpgFile(&image, path, 0, 0, DISPLAY_W, MAIN_H,
                                0, 0, 0.0f, 0.0f);
    else if (hasExtension(openName, ".bmp"))
        ok = canvas.drawBmpFile(&image, path, 0, 0, DISPLAY_W, MAIN_H,
                                0, 0, 0.0f, 0.0f);
    else
        ok = canvas.drawPngFile(&image, path, 0, 0, DISPLAY_W, MAIN_H,
                                0, 0, 0.0f, 0.0f);
    if (!ok) {
        canvas.fillSprite(UiStyle::BG);
        canvas.setTextColor(UiStyle::RED);
        canvas.drawString("IMAGE DECODE FAILED", 8, 36);
        canvas.setTextColor(UiStyle::DIM);
        canvas.drawString(openName, 8, 50);
    } else {
        canvas.fillRect(0, 0, DISPLAY_W, 12, 0x0000);
        canvas.setTextColor(UiStyle::TEXT);
        uiDrawMarquee(canvas, openName, 5, 2, DISPLAY_W - 10);
    }
}

void FileMgrMode::draw(M5Canvas& canvas) {
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    if (phase == Phase::BROWSE) {
        drawBrowse(canvas);
        return;
    }
    if (phase == Phase::VIEW || phase == Phase::EDIT) {
        drawViewEdit(canvas);
        return;
    }
    if (phase == Phase::IMAGE) {
        drawImage(canvas);
        return;
    }

    canvas.fillSprite(UiStyle::BG);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    if (phase == Phase::INFO && sel < entryCount) {
        const Entry& e = entries[sel];
        char size[16], path[PATH_BUF];
        formatSize(size, sizeof(size), e.size);
        buildFullPath(path, sizeof(path), e.name);
        canvas.setTextColor(UiStyle::TITLE);
        canvas.drawString("FILE DETAILS", 8, 8);
        canvas.setTextColor(UiStyle::TEXT);
        uiDrawMarquee(canvas, e.name, 8, 28, DISPLAY_W - 16);
        canvas.setTextColor(UiStyle::CYAN);
        canvas.drawString(size, 8, 44);
        canvas.setTextColor(UiStyle::DIM);
        uiDrawMarquee(canvas, path, 8, 60, DISPLAY_W - 16);
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString("UNSUPPORTED PREVIEW FORMAT", 8, 78);
    } else if (phase == Phase::NAME_INPUT || phase == Phase::CONFIRM_OVERWRITE) {
        canvas.setTextColor(UiStyle::TITLE);
        canvas.drawString(phase == Phase::NAME_INPUT ? "ENTER NAME" : "REPLACE EXISTING?", 8, 15);
        canvas.fillRoundRect(6, 34, DISPLAY_W - 12, 20, 3, UiStyle::PANEL);
        canvas.setTextColor(UiStyle::TEXT);
        const char* text = phase == Phase::NAME_INPUT ? nameBuf : statusMsg;
        uiDrawMarquee(canvas, text, 10, 40, DISPLAY_W - 20);
    } else if (phase == Phase::CONFIRM_DEL && sel < entryCount) {
        canvas.setTextColor(UiStyle::RED);
        canvas.drawString(entries[sel].isDir ? "DELETE FOLDER AND CONTENTS?" : "DELETE FILE?", 8, 28);
        canvas.setTextColor(UiStyle::TEXT);
        uiDrawMarquee(canvas, entries[sel].name, 8, 48, DISPLAY_W - 16);
    }
}
