// sync/ohc.h
// OnlineHashCrack client — public WPA API.
// https://www.onlinehashcrack.com/how-developpers-public-api-documentation.php
//
// How this differs from WPASec / Pwncrack:
//   * No API key. The credential is the email address of an existing
//     OnlineHashCrack account (unknown addresses answer HTTP 401).
//   * Accepts .cap / .pcap / .pcapng captures, up to 200 MB, on
//     POST https://api.onlinehashcrack.com  (field names: email, file).
//   * WPA captures (hashcat mode 22000) do NOT consume the monthly task
//     quota, so the device can push everything it catches.
//   * There is NO potfile / results endpoint. OnlineHashCrack deletes the
//     uploaded capture right after extracting the hash and publishes the
//     outcome in the web dashboard plus by email. This client is therefore
//     upload-only: a row is LOCAL or SENT, never CRACKED.

#pragma once

#include <Arduino.h>

// Reply of a single upload, as reported by the API batch summary.
struct OhcUploadResult {
    bool     accepted;       // accepted.count > 0
    bool     alreadySent;    // 200 + skipped.reason == "already_sent"
    bool     noHashFound;    // 200 + rejected.reason == "no_hash_found"
    uint16_t acceptedCount;
    uint16_t skippedCount;
    uint16_t rejectedCount;
    char     error[64];
};

struct OhcSyncResult {
    bool     success;
    uint16_t uploaded;   // accepted.count > 0
    uint16_t already;    // already known to the server
    uint16_t empty;      // no PMKID/EAPOL found in the capture
    uint16_t failed;     // transport / HTTP / other rejection
    char     error[64];
};

typedef void (*OhcProgressCallback)(const char* status, uint16_t progress, uint16_t total);

namespace OHC {

// ---- account -----------------------------------------------------------
// An email is all the public WPA API needs. Validate the shape locally so a
// typo fails on the device instead of after a 40 s TLS handshake.
bool isValidEmail(const char* email);
bool hasAccount();
bool hasAccount(const char* email);

// Same heap floor the other sync providers use before opening TLS.
bool canSync();

// ---- upload ------------------------------------------------------------
// Upload one capture from SD.
// Returns true when the server accepted the hash, or when it reported the
// capture as already_sent — in both cases the capture is known to
// OnlineHashCrack and must not be retried. Returns false for transport
// failures and for captures rejected with no_hash_found; inspect `out` to
// tell those two apart (out->noHashFound).
bool uploadOneCapture(const char* filepath, const char* email,
                      OhcUploadResult* out = nullptr);

// Upload every capture in /0N3P0rK/handshakes that was not sent yet.
// There is no results step — OnlineHashCrack has no potfile endpoint.
OhcSyncResult syncCaptures(const char* email, OhcProgressCallback cb = nullptr);

// ---- uploaded tracking -------------------------------------------------
bool loadCache();
void freeCacheMemory();
bool isUploaded(const char* nameOrBssid);
void markAsUploaded(const char* nameOrBssid, const char* bssid = nullptr);
uint16_t getUploadedCount();

const char* getLastError();

}  // namespace OHC
