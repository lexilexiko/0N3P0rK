// modes/default_psk.h — candidate passwords computed from a network's own
// identity, for PigPass.
//
// Why: a lot of home routers ship with a factory PSK that is a pure function of
// the network itself (SSID, BSSID). A short derived-candidate list checks
// those in seconds, while a generic wordlist has to walk millions of unrelated
// lines first. Everything here is offline: no radio, no scanning, just string
// work on data PigPass already parsed out of the handshake.
//
// Deliberately narrow. A generator that invents random strings burns the same
// battery as the mask generator and finds nothing new; these rules only encode
// patterns real hardware actually ships with.
#pragma once

#include <Arduino.h>

namespace DefaultPsk {

// The list is a supplement to a wordlist, not a replacement, and every extra
// candidate costs one PBKDF2 (4096) round on the device.
//
// The table is heap-allocated for the duration of a run and released when the
// run ends, so it costs nothing while PigPass is idle or browsing files. The
// cap is therefore a peak-usage bound during a run (~10 KB at 256 x 40), not a
// permanent firmware cost.
static constexpr size_t MAX_CANDIDATES = 256;
static constexpr size_t MAX_PW         = 40;

// WPA PSK is 8..63 bytes, so anything shorter than 8 is not a possible key and
// is dropped here rather than wasting a PBKDF2 round on it.
static constexpr size_t MIN_PW_LEN = 8;

// The network to derive from. Filled from the selected handshake, so the caller
// only hands over what it already knows.
struct Target {
    char ssid[33];
    char bssid[13];   // 12 hex digits, no separators; empty is fine
};

// Build the candidate table for `t`. Returns how many entries are valid.
// Calling twice for the same target is cheap but pointless; the table is static.
size_t build(const Target& t);

// Total valid candidates after the last build(). Zero means the SSID is missing
// or produced nothing plausible.
size_t count();

// Writes candidate `index` (0-based) into `out`. False when out of range.
bool emit(size_t index, char* out, size_t outSz);

// Name of the rule that produced candidate 0, for the UI line explaining why the
// run is short. Never NULL.
const char* topRuleName();

// Hand the candidate table back. Called when a run ends and when PigPass leaves
// the mode, so the ~10 KB is not held while the device does anything else.
// Safe to call when nothing is allocated. Returns false when a build() call had
// failed to get memory, so the caller can tell "never ran" from "finished".
bool release();

}  // namespace DefaultPsk