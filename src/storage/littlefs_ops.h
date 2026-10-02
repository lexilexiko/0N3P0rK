// SD card storage - OnePork layout on the Cardputer SD.
// Namespace stays Storage so Stamp cap/sync keep compiling.
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD.h>

namespace Storage {

static const uint8_t FILE_NAME_MAX = 48;

bool begin();
void end();
bool remount();
bool available();

uint32_t sectorSize();
uint32_t numSectors();
bool readSector(uint32_t lba, uint8_t* buf);
bool writeSector(uint32_t lba, const uint8_t* buf);

struct Stats {
    size_t total;
    size_t used;
    size_t free;
    uint16_t handshakes;
    uint16_t results;
};
Stats stats();

const char* baseName(const char* path);

typedef void (*FileVisitor)(const char* name, size_t size, void* ctx);
uint16_t forEachInDir(const char* dir, FileVisitor fn, void* ctx);
uint16_t forEachHandshake(FileVisitor fn, void* ctx);
uint16_t forEachPwn(FileVisitor fn, void* ctx);

uint16_t listHandshakes(char out[][FILE_NAME_MAX], uint16_t max);
uint16_t listResults(char out[][FILE_NAME_MAX], uint16_t max);

// /0N3P0rK — one bag of captures, service folders hold only keys + logs
const char* const DIR_ROOT       = "/0N3P0rK";
const char* const DIR_LOOT       = "/0N3P0rK";
const char* const DIR_HS         = "/0N3P0rK/handshakes";
const char* const DIR_HANDSHAKES = "/0N3P0rK/handshakes";
const char* const DIR_WPASEC     = "/0N3P0rK/wpa-sec";
const char* const DIR_PWNCRACK   = "/0N3P0rK/pwncrack";
const char* const DIR_OHC        = "/0N3P0rK/ohc";
// Reports written by the handshake inspector (InspectorPig).
const char* const DIR_INSPECTOR  = "/0N3P0rK/inspector";
const char* const DIR_RESULTS    = "/0N3P0rK/wpa-sec";
const char* const DIR_EVILPIG    = "/0N3P0rK/evilpig";
const char* const DIR_PIGPASS    = "/0N3P0rK/pigpass";
const char* const DIR_PASSWORLD  = "/0N3P0rK/Passworld";
const char* const DIR_IR         = "/0N3P0rK/ir";
const char* const DIR_WOLF       = "/0N3P0rK/wolf";
const char* const DIR_TALK       = "/0N3P0rK/talk";
const char* const DIR_MUSIC      = "/0N3P0rK/music";

const char* const FILE_WPASEC_KEY        = "/0N3P0rK/wpa-sec/key.txt";
const char* const FILE_WPASEC_RESULTS    = "/0N3P0rK/wpa-sec/results.txt";
const char* const FILE_WPASEC_UPLOADED   = "/0N3P0rK/wpa-sec/uploaded.txt";
const char* const FILE_PWNCRACK_KEY      = "/0N3P0rK/pwncrack/key.txt";
const char* const FILE_PWNCRACK_RESULTS  = "/0N3P0rK/pwncrack/results.txt";
const char* const FILE_PWNCRACK_UPLOADED = "/0N3P0rK/pwncrack/uploaded.txt";
// OnlineHashCrack public WPA API: the account email is the only credential,
// so this file holds one address instead of a key. results.txt is unused —
// the service has no potfile endpoint, results live in the web dashboard.
const char* const FILE_OHC_EMAIL         = "/0N3P0rK/ohc/email.txt";
const char* const FILE_OHC_UPLOADED      = "/0N3P0rK/ohc/uploaded.txt";

bool ensureDir(const char* path);
bool removeFile(const char* path);
bool removeCapture(const char* name);  // handshake file + companion .txt
bool fileExists(const char* path);
size_t fileSize(const char* path);
bool formatStorage();

// Read first line of a key file into dest. Returns true if non-empty.
bool loadKeyFile(const char* path, char* dest, size_t destLen);
void loadKeysIntoNet();
void migrateLegacy();
void brewHeap();

// Wolf-eat: move up to want random capture files into /0N3P0rK/wolf. Never keys.
uint8_t eatRandomLoot(uint8_t want);
// Spit belly back into handshakes (hit wolf / turn Am off).
uint8_t restoreWolfLoot();

// One capture per BSSID: drop HIDDEN_/MAC copies, keep named SSID + handshake.
uint8_t compactLoot();

} // namespace Storage
