// sync/ohc.cpp
#include "ohc.h"
#include "../storage/littlefs_ops.h"
#include "../net/ap_sta.h"
#include "net_io.h"
#include "tls.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vector>
#include <esp_heap_caps.h>

static const char*    OHC_HOST = "api.onlinehashcrack.com";
static const uint16_t OHC_PORT = 443;
// The public WPA endpoint is the host root.
static const char*    OHC_UPLOAD_PATH = "/";
// 48-byte ids: capture file names ("AA-BB-CC-DD-EE-FF_SSID.pcap") plus BSSID hex.
static const uint8_t  OHC_ID_MAX = 48;
static const size_t   OHC_MAX_CACHE = 256;
static const char*    OHC_PENDING = "/0N3P0rK/ohc/_pending.txt";
// Per-run report. The device has no console in the field, so every candidate
// and its outcome is written here and can be opened from FILEMGR or pulled over
// XFER.
static const char*    OHC_LASTLOG = "/0N3P0rK/ohc/last.log";

namespace OHC {

// One entry per capture the API accepted: either the file name on SD or the
// BSSID hex, whichever the caller used to identify the capture.
struct UploadedEntry {
    char id[OHC_ID_MAX];
};

// Uploaded-id cache. The captures themselves stay on SD.
static bool   s_cacheLoaded = false;
static char   s_lastError[64] = "";
static volatile bool s_busy = false;
static std::vector<UploadedEntry> s_uploaded;

// ---------------------------------------------------------------------------
// Reply parsing
// ---------------------------------------------------------------------------
// The documented reply shape is fixed and small:
//   {"accepted":{"count":1,"hashes":["WPA*02*..."]},
//    "skipped":{"count":0,"reason":"already_sent","hashes":[]},
//    "rejected":{"count":0,"hashes":[],"reason":"no_hash_found","message":"..."}}
// Request-level errors are {"success":false,"message":"..."}.
// The API page carries a compatibility promise for exactly the key names
// accepted / skipped / rejected / count / reason / message, so a small scanner
// is enough here. Pulling in a full JSON library for six fields would grow the
// image and force a library download during an offline Termux build.
static const char* findKey(const char* json, const char* key) {
    if (!json || !key || !key[0]) return nullptr;
    char pat[40];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) return nullptr;
    const char* p = strstr(json, pat);
    return p ? p + n : nullptr;
}

// `afterKey` is the position right behind the key's closing quote.
static bool jsonUintAt(const char* afterKey, uint16_t* out) {
    if (!afterKey || !out) return false;
    const char* p = strchr(afterKey, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (!isdigit((unsigned char)*p)) return false;
    unsigned v = 0;
    while (isdigit((unsigned char)*p) && v < 1000000u)
        v = v * 10u + (unsigned)(*p++ - '0');
    *out = (uint16_t)v;
    return true;
}

static bool jsonStringAt(const char* afterKey, char* out, size_t outLen) {
    if (!afterKey || !out || outLen < 2) return false;
    out[0] = '\0';
    const char* p = strchr(afterKey, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outLen) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool jsonUintIn(const char* json, const char* objKey, const char* field,
                       uint16_t* out) {
    const char* obj = findKey(json, objKey);
    if (!obj) return false;
    return jsonUintAt(findKey(obj, field), out);
}

static bool jsonStringIn(const char* json, const char* objKey, const char* field,
                         char* out, size_t outLen) {
    const char* obj = findKey(json, objKey);
    if (!obj) return false;
    return jsonStringAt(findKey(obj, field), out, outLen);
}

// Top-level {"success":false,"message":"..."}
static bool jsonTopMessage(const char* json, char* out, size_t outLen) {
    return jsonStringAt(findKey(json, "message"), out, outLen);
}

// Only .cap / .pcap / .pcapng are accepted; .22000 uploads are rejected
// server-side as an unsupported file type.
static bool endsWithPcap(const char* name) {
    if (!name) return false;
    size_t n = strlen(name);
    if (n > 7 && strcasecmp(name + n - 7, ".pcapng") == 0) return true;
    if (n > 5 && strcasecmp(name + n - 5, ".pcap") == 0) return true;
    if (n > 4 && strcasecmp(name + n - 4, ".cap") == 0) return true;
    return false;
}

struct OhcPendCtx {
    File*    out;
    uint16_t count;
};

static void ohcCollect(const char* name, size_t size, void* raw) {
    OhcPendCtx* ctx = (OhcPendCtx*)raw;
    if (!ctx || !ctx->out || !name || name[0] == '\0' || size == 0) return;
    if (!endsWithPcap(name)) return;
    if (OHC::isUploaded(name)) return;
    ctx->out->println(name);
    ctx->count++;
}

// ---------------------------------------------------------------------------
// Account — the public WPA API has no key; the account email is the credential
// ---------------------------------------------------------------------------
bool isValidEmail(const char* email) {
    if (!email || !email[0]) return false;
    size_t n = strlen(email);
    if (n < 6 || n > 64) return false;            // a@b.co is the practical floor
    uint8_t atCount = 0;
    for (size_t i = 0; email[i]; i++) {
        unsigned char c = (unsigned char)email[i];
        if (c <= 0x20 || c >= 0x7F) return false; // no spaces, no non-ASCII
        if (c == '@') atCount++;
    }
    if (atCount != 1) return false;
    const char* at = strchr(email, '@');
    if (at == email || at - email > 60) return false;
    const char* dot = strrchr(at + 1, '.');
    if (!dot || dot == at + 1) return false;      // host needs a dot
    if (dot[1] == '\0') return false;             // TLD must be non-empty
    return true;
}

bool hasAccount(const char* email) { return isValidEmail(email); }
bool hasAccount() { return hasAccount(Net::cfg().ohcEmail); }

bool canSync() {
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint32_t freeH = ESP.getFreeHeap();
    if (largest < 14000 || freeH < 22000) {
        snprintf(s_lastError, sizeof(s_lastError), "low heap %u/%uK",
                 (unsigned)(largest / 1024), (unsigned)(freeH / 1024));
        return false;
    }
    s_lastError[0] = '\0';
    return true;
}

const char* getLastError() { return s_lastError; }

// ---------------------------------------------------------------------------
// Uploaded tracking — /0N3P0rK/ohc/uploaded.txt
// ---------------------------------------------------------------------------
void freeCacheMemory() {
    // MEMFIX: swap-with-empty releases the capacity. shrink_to_fit would be a
    // realloc, and a failed realloc on ESP32 aborts the firmware.
    std::vector<UploadedEntry>().swap(s_uploaded);
    s_cacheLoaded = false;
}

static bool findUploaded(const char* id) {
    if (!id || !id[0]) return false;
    for (const auto& e : s_uploaded) {
        if (strcasecmp(e.id, id) == 0) return true;
    }
    return false;
}

static void pushUploaded(const char* id) {
    if (!id || !id[0]) return;
    if (strlen(id) >= OHC_ID_MAX) return;
    if (findUploaded(id)) return;
    if (s_uploaded.size() >= OHC_MAX_CACHE) return;
    UploadedEntry e{};
    strncpy(e.id, id, sizeof(e.id) - 1);
    s_uploaded.push_back(e);
}

static bool loadUploadedList() {
    s_uploaded.clear();
    if (!Storage::fileExists(Storage::FILE_OHC_UPLOADED)) return true;
    File f = SD.open(Storage::FILE_OHC_UPLOADED, "r");
    if (!f) return false;
    char line[64];
    while (f.available() && s_uploaded.size() < OHC_MAX_CACHE) {
        size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
        if (n == 0) continue;
        pushUploaded(line);
    }
    f.close();
    return true;
}

static bool saveUploadedList() {
    Storage::ensureDir(Storage::DIR_OHC);
    const char* tmp = "/0N3P0rK/ohc/uploaded.tmp";
    File f = SD.open(tmp, "w");
    if (!f) return false;
    for (const auto& e : s_uploaded) {
        if (e.id[0]) f.println(e.id);
    }
    f.flush();
    f.close();
    SD.remove(Storage::FILE_OHC_UPLOADED);
    if (!SD.rename(tmp, Storage::FILE_OHC_UPLOADED)) {
        SD.remove(tmp);
        return false;
    }
    Serial.printf("[OHC] saved %u sent\n", (unsigned)s_uploaded.size());
    return true;
}

bool loadCache() {
    if (s_cacheLoaded) return true;
    loadUploadedList();
    s_cacheLoaded = true;
    return true;
}

bool isUploaded(const char* nameOrBssid) {
    if (!s_cacheLoaded) loadCache();
    if (!nameOrBssid || !nameOrBssid[0]) return false;
    if (findUploaded(nameOrBssid)) return true;
    if (strchr(nameOrBssid, '/')) {
        const char* base = Storage::baseName(nameOrBssid);
        if (base && base[0] && findUploaded(base)) return true;
    }
    return false;
}

void markAsUploaded(const char* nameOrBssid, const char* bssid) {
    if (!s_cacheLoaded) loadCache();
    if (nameOrBssid && nameOrBssid[0]) {
        const char* base = strchr(nameOrBssid, '/')
            ? Storage::baseName(nameOrBssid)
            : nameOrBssid;
        if (base && base[0]) pushUploaded(base);
    }
    if (bssid && bssid[0]) pushUploaded(bssid);
}

uint16_t getUploadedCount() {
    if (!s_cacheLoaded) loadCache();
    return (uint16_t)s_uploaded.size();
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------
// One multipart POST to the root of api.onlinehashcrack.com. The body is
// streamed straight off the SD card through Tls::streamFile(), so a capture
// never has to fit in RAM — the same path WPASec uploads already use.
// Field order matters: the API reads `email` first, then `file`.
static bool uploadCapture(const char* filepath, const char* email,
                          OhcUploadResult* out) {
    if (out) {
        memset(out, 0, sizeof(*out));
        out->error[0] = '\0';
    }
    if (!isValidEmail(email)) {
        snprintf(s_lastError, sizeof(s_lastError), "bad email");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }
    if (!filepath || !filepath[0]) {
        snprintf(s_lastError, sizeof(s_lastError), "no path");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }

    File capFile = SD.open(filepath, "r");
    if (!capFile) {
        snprintf(s_lastError, sizeof(s_lastError), "open fail");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }
    size_t fileSize = capFile.size();
    if (fileSize == 0) {
        capFile.close();
        snprintf(s_lastError, sizeof(s_lastError), "empty file");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }
    if (fileSize > kHsUploadMax) {
        capFile.close();
        snprintf(s_lastError, sizeof(s_lastError), "too big");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }

    // OnlineHashCrack answers "unsupported file type" for anything it cannot
    // recognise as PCAP or PCAPNG. Verify the container ourselves first, so the
    // reason is readable and a doomed file never costs a TLS session.
    uint8_t magic[4] = {0, 0, 0, 0};
    size_t gotMagic = capFile.read(magic, 4);
    capFile.seek(0);
    if (gotMagic != 4) {
        capFile.close();
        snprintf(s_lastError, sizeof(s_lastError), "unreadable");
        if (out) {
            snprintf(out->error, sizeof(out->error), "%s", s_lastError);
            out->badFormat = true;
        }
        return false;
    }
    uint32_t m = ((uint32_t)magic[0] << 24) | ((uint32_t)magic[1] << 16) |
                 ((uint32_t)magic[2] << 8) | (uint32_t)magic[3];
    // 0xD4C3B2A1 = classic pcap little endian (what the sniffer writes),
    // 0xA1B2C3D4 = classic pcap big endian, 0x0A0D0D0A = pcapng.
    const bool isPcap   = (m == 0xD4C3B2A1u) || (m == 0xA1B2C3D4u);
    const bool isPcapng = (m == 0x0A0D0D0Au);
    // A container with no packet record cannot hold a handshake. The capture
    // pipeline writes the 24-byte global header as soon as EAPOL shows up, and
    // a tight hsFileBytes budget can leave the file exactly like that.
    const size_t minEmpty = isPcapng ? 28u : 24u;

    if (out) {
        out->bytes = (uint32_t)fileSize;
        out->magic = m;
    }

    if (!isPcap && !isPcapng) {
        capFile.close();
        snprintf(s_lastError, sizeof(s_lastError), "not a capture (%08X)",
                 (unsigned)m);
        if (out) {
            snprintf(out->error, sizeof(out->error), "%s", s_lastError);
            out->badFormat = true;
        }
        return false;
    }
    if (fileSize <= minEmpty) {
        capFile.close();
        snprintf(s_lastError, sizeof(s_lastError), "no packets (%ub)",
                 (unsigned)fileSize);
        if (out) {
            snprintf(out->error, sizeof(out->error), "%s", s_lastError);
            out->badFormat = true;
        }
        return false;
    }

    const char* filename = Storage::baseName(filepath);
    if (!filename || !filename[0]) filename = "capture.pcap";

    // Unique boundary each POST so capture bytes cannot collide with it.
    char boundary[32];
    snprintf(boundary, sizeof(boundary), "----OHC%08lX", (unsigned long)millis());

    char emailHead[256];
    snprintf(emailHead, sizeof(emailHead),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"email\"\r\n\r\n"
             "%s\r\n",
             boundary, email);

    char fileHead[352];
    snprintf(fileHead, sizeof(fileHead),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n",
             boundary, filename);

    char tail[64];
    snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);

    size_t contentLength = strlen(emailHead) + strlen(fileHead) + fileSize + strlen(tail);

    WiFiClientSecure client;
    if (!ioTlsOpen(client, OHC_HOST, OHC_PORT)) {
        capFile.close();
        client.stop();
        snprintf(s_lastError, sizeof(s_lastError), "tls fail");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }

    client.printf("POST %s HTTP/1.1\r\n", OHC_UPLOAD_PATH);
    client.printf("Host: %s\r\n", OHC_HOST);
    client.printf("User-Agent: 0N3P0rK/" ON3PORK_VERSION " OHC\r\n");
    client.printf("Accept: application/json\r\n");
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
    client.printf("Content-Length: %u\r\n", (unsigned)contentLength);
    client.print("Connection: close\r\n\r\n");
    client.print(emailHead);
    client.print(fileHead);

    client.setTimeout(60000);
    char streamErr[40] = "";
    ioXfer().size = (uint32_t)contentLength;
    ioXferPaint(true);
    // Streams the capture and closes the File handle on every path.
    if (!Tls::streamFile(client, capFile, fileSize, streamErr, sizeof(streamErr))) {
        capFile.close();
        client.stop();
        snprintf(s_lastError, sizeof(s_lastError), "%s",
                 streamErr[0] ? streamErr : "send fail");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }
    capFile.close();

    if (!client.connected()) {
        snprintf(s_lastError, sizeof(s_lastError), "lost after body");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }
    client.flush();
    client.print(tail);

    char status[80] = "";
    bool got = ioReadStatusLine(client, status, sizeof(status), 45000);

    // Drop HTTP headers. The JSON starts after the blank line. Without this
    // the 1 KB scrape filled up with Date/Content-Type plus OHC's long
    // "notice" legal text and never reached "accepted"/"already_sent" —
    // every good upload looked like "not accepted".
    if (got) {
        char hline[160];
        size_t hi = 0;
        bool headersDone = false;
        unsigned long ht = millis();
        while ((uint32_t)(millis() - ht) < 15000 && !headersDone) {
            int avail = client.available();
            if (avail <= 0) {
                if (!client.connected()) break;
                delay(5);
                yield();
                continue;
            }
            int ch = client.read();
            if (ch < 0) continue;
            if (ch == '\n') {
                if (hi == 0) headersDone = true;
                hi = 0;
            } else if (ch != '\r' && hi + 1 < sizeof(hline)) {
                hline[hi++] = (char)ch;
            }
        }
        if (!headersDone) {
            client.stop();
            snprintf(s_lastError, sizeof(s_lastError), "hdr timeout");
            if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
            return false;
        }
    }

    // counts sit before the long WPA* hash strings, so 2 KB is enough after
    // headers are gone. Keep scanning even if the notice is large.
    static char body[2048];
    size_t bl = 0;
    unsigned long t0 = millis();
    while (got && (uint32_t)(millis() - t0) < 20000 && bl + 1 < sizeof(body)) {
        int avail = client.available();
        if (avail <= 0) {
            if (!client.connected()) break;
            delay(8);
            yield();
            continue;
        }
        int n = client.read(reinterpret_cast<uint8_t*>(body) + bl,
                            sizeof(body) - 1 - bl);
        if (n <= 0) continue;
        bl += (size_t)n;
        t0 = millis();
        // We only need the batch counters. Stop once they are in the buffer
        // so a huge hashes[] array cannot push "skipped"/"rejected" out.
        if (bl > 400 && strstr(body, "\"rejected\"")) break;
    }
    body[bl] = '\0';
    ioDrain(client, 3000);
    client.stop();

    if (!got || !status[0]) {
        snprintf(s_lastError, sizeof(s_lastError), "no reply");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }

    uint16_t httpCode = 0;
    const char* sp = strchr(status, ' ');
    if (sp) httpCode = (uint16_t)atoi(sp + 1);

    uint16_t acc = 0, skip = 0, rej = 0;
    jsonUintIn(body, "accepted", "count", &acc);
    jsonUintIn(body, "skipped", "count", &skip);
    jsonUintIn(body, "rejected", "count", &rej);
    if (out) {
        out->acceptedCount = acc;
        out->skippedCount = skip;
        out->rejectedCount = rej;
    }
    Serial.printf("[OHC] %s acc=%u skip=%u rej=%u\n",
                  status, (unsigned)acc, (unsigned)skip, (unsigned)rej);
    yield();

    // Request-level rejections. The API documents these codes explicitly.
    if (httpCode == 401) {
        snprintf(s_lastError, sizeof(s_lastError), "email not in OHC");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }
    if (httpCode == 413) {
        snprintf(s_lastError, sizeof(s_lastError), "over 200MB");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }
    if (httpCode == 400) {
        char msg[48] = "";
        jsonTopMessage(body, msg, sizeof(msg));
        snprintf(s_lastError, sizeof(s_lastError), "%s",
                 msg[0] ? msg : "bad request");
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }
    if (!ioHttpOk(status)) {
        snprintf(s_lastError, sizeof(s_lastError), "http %u", (unsigned)httpCode);
        if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
        return false;
    }

    // HTTP 200 — the batch summary decides what actually happened. A 200 can
    // still carry a rejected item, so the counts are what matter, never the
    // status code alone.
    bool accepted = acc > 0;
    if (out) {
        out->accepted = accepted;
        if (skip > 0) {
            char r[24] = "";
            jsonStringIn(body, "skipped", "reason", r, sizeof(r));
            out->alreadySent = (strcasecmp(r, "already_sent") == 0) ||
                               (r[0] == '\0' && strstr(body, "already_sent") != nullptr);
        } else if (strstr(body, "already_sent")) {
            out->alreadySent = true;
        }
        if (rej > 0) {
            char r[24] = "";
            jsonStringIn(body, "rejected", "reason", r, sizeof(r));
            out->noHashFound = (strcasecmp(r, "no_hash_found") == 0) ||
                               (r[0] == '\0' && strstr(body, "no_hash_found") != nullptr);
            char m[64] = "";
            jsonStringIn(body, "rejected", "message", m, sizeof(m));
            if (m[0]) strncpy(out->error, m, sizeof(out->error) - 1);
        }
    }

    if (accepted) {
        s_lastError[0] = '\0';
        return true;
    }
    if ((out && out->alreadySent) || strstr(body, "already_sent")) {
        if (out) out->alreadySent = true;
        snprintf(s_lastError, sizeof(s_lastError), "already sent");
        return true;   // known to the server: must not be retried
    }
    if ((out && out->noHashFound) || strstr(body, "no_hash_found")) {
        if (out) out->noHashFound = true;
        snprintf(s_lastError, sizeof(s_lastError), "no hash found");
        return false;
    }
    snprintf(s_lastError, sizeof(s_lastError), "not accepted");
    if (out) snprintf(out->error, sizeof(out->error), "%s", s_lastError);
    return false;
}

// ---- public API -----------------------------------------------------------
bool uploadOneCapture(const char* filepath, const char* email, OhcUploadResult* out) {
    if (out) {
        memset(out, 0, sizeof(*out));
        out->error[0] = '\0';
    }
    if (s_busy) {
        snprintf(s_lastError, sizeof(s_lastError), "busy");
        if (out) strncpy(out->error, "busy", sizeof(out->error) - 1);
        return false;
    }
    if (!hasAccount(email)) {
        snprintf(s_lastError, sizeof(s_lastError), "no OHC email");
        if (out) strncpy(out->error, "no OHC email", sizeof(out->error) - 1);
        return false;
    }

    s_busy = true;
    OhcUploadResult res{};
    bool ok = uploadCapture(filepath, email, &res);
    if (ok) {
        const char* base = filepath ? Storage::baseName(filepath) : nullptr;
        markAsUploaded(base && base[0] ? base : filepath, nullptr);
        saveUploadedList();
    }
    if (out) *out = res;
    s_busy = false;
    return ok;
}

OhcSyncResult syncCaptures(const char* email, OhcProgressCallback cb) {
    OhcSyncResult result{};
    result.success = false;
    result.error[0] = '\0';

    if (s_busy) {
        strncpy(result.error, "already syncing", sizeof(result.error) - 1);
        return result;
    }
    if (!isValidEmail(email)) {
        strncpy(result.error, "no OHC email", sizeof(result.error) - 1);
        return result;
    }
    if (!Storage::available()) {
        strncpy(result.error, "no SD", sizeof(result.error) - 1);
        return result;
    }

    s_busy = true;
    loadCache();

    // Queue the captures that were not sent yet. The queue lives on SD so the
    // whole handshake directory never has to fit in RAM - the same trick
    // WPASec uses for its pending list.
    Storage::ensureDir(Storage::DIR_OHC);
    uint16_t queued = 0;
    File pend = SD.open(OHC_PENDING, "w");
    if (pend) {
        OhcPendCtx ctx{&pend, 0};
        Storage::forEachHandshake(ohcCollect, &ctx);
        queued = ctx.count;
        pend.close();
    }
    if (queued == 0) {
        SD.remove(OHC_PENDING);
        result.success = true;
        s_busy = false;
        Serial.println("[OHC] nothing pending");
        return result;
    }

    // Per-run report on SD. The device is normally used with no console
    // attached, so this file is how a partial or failing run can actually be
    // inspected: open it from FILEMGR or pull it over XFER.
    File log = SD.open(OHC_LASTLOG, "w");
    if (log) {
        log.printf("# 0N3P0rK OHC build=%s\n", ON3PORK_VERSION);
        log.printf("# queued=%u email=%s\n", (unsigned)queued, email);
        log.println("# name|bytes|magic|status|acc|skip|rej|detail");
    }

    File in = SD.open(OHC_PENDING, "r");
    uint16_t i = 0;
    uint8_t heapStrikes = 0;
    char line[80];
    char firstErr[48] = "";
    while (in && in.available()) {
        size_t n = in.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
        if (n == 0) continue;
        i++;
        if (cb) cb("OHC up", i, queued);
        ioXferPhase("OHC UP", i, queued);

        // TLS needs one large contiguous block and the previous session leaves
        // the heap shredded. Compact, wait, and then skip only THIS file — the
        // rest of the batch keeps going. Aborting here is what cut runs down to
        // a couple of uploads. Three strikes in a row mean the heap will not
        // come back in this pass, and the remainder is left for the next run.
        if (!canSync()) {
            Storage::brewHeap();
            delay(300);
            yield();
            if (!canSync()) {
                heapStrikes++;
                result.failed++;
                ioXfer().fail++;
                if (firstErr[0] == '\0')
                    snprintf(firstErr, sizeof(firstErr), "%s", s_lastError);
                if (log) log.printf("%s|?|?|heap|0|0|0|%s\n", line, s_lastError);
                if (heapStrikes >= 3) {
                    uint16_t left = (uint16_t)(queued - i);
                    result.failed += left;
                    ioXfer().fail += left;
                    Serial.printf("[OHC] heap stop, %u files left\n", (unsigned)left);
                    if (log) log.printf("# stopped: %u files left\n", (unsigned)left);
                    break;
                }
                continue;
            }
            heapStrikes = 0;
        }

        char path[96];
        snprintf(path, sizeof(path), "%s/%s", Storage::DIR_HANDSHAKES, line);

        OhcUploadResult ur{};
        bool ok = uploadCapture(path, email, &ur);
        if (!ok && !ur.badFormat && !ur.noHashFound) {
            // One immediate retry. Nearly every failure at this point is a
            // torn-down TLS session rather than a bad capture, and a fresh
            // connect right after usually goes through. This is the cheapest
            // way to raise the number of captures that make it per run.
            Storage::brewHeap();
            delay(250);
            yield();
            OhcUploadResult ur2{};
            if (uploadCapture(path, email, &ur2)) {
                ur = ur2;
                ok = true;
            } else if (!ur2.badFormat && !ur2.noHashFound) {
                ur = ur2;   // keep the newer reason for the log
            }
        }
        if (ok) {
            if (ur.alreadySent) result.already++;
            else result.uploaded++;
            ioXfer().ok++;
            markAsUploaded(line, nullptr);
        } else if (ur.noHashFound) {
            // Valid capture, but no usable PMKID/EAPOL inside. There is
            // nothing to retry and nothing to mark, so it stays LOCAL.
            result.empty++;
        } else if (ur.badFormat) {
            // Not something the service would accept: wrong container, or a
            // header-only capture with no packets. Skipped locally, so it never
            // reaches the API and never burns a TLS session. Still reported, so
            // a card full of broken captures cannot look like a clean run.
            result.empty++;
            if (firstErr[0] == '\0')
                snprintf(firstErr, sizeof(firstErr), "%s",
                         ur.error[0] ? ur.error : "bad capture");
        } else {
            result.failed++;
            ioXfer().fail++;
            if (firstErr[0] == '\0')
                snprintf(firstErr, sizeof(firstErr), "%s",
                         ur.error[0] ? ur.error : s_lastError);
        }
        // One line per capture, on SD and on the serial console, carrying the
        // container facts and the server's own counters and message.
        const char* st = ok ? (ur.alreadySent ? "already" : "ok")
                            : (ur.badFormat ? "bad"
                                            : (ur.noHashFound ? "nohash" : "fail"));
        Serial.printf("[OHC] %-34s b=%u m=%08X %s acc=%u skip=%u rej=%u %s\n",
                      line, (unsigned)ur.bytes, (unsigned)ur.magic, st,
                      (unsigned)ur.acceptedCount, (unsigned)ur.skippedCount,
                      (unsigned)ur.rejectedCount,
                      ur.error[0] ? ur.error : "");
        if (log) {
            log.printf("%s|%u|%08X|%s|%u|%u|%u|%s\n",
                       line, (unsigned)ur.bytes, (unsigned)ur.magic, st,
                       (unsigned)ur.acceptedCount, (unsigned)ur.skippedCount,
                       (unsigned)ur.rejectedCount,
                       ur.error[0] ? ur.error : "-");
        }
        // Compact between files so the next TLS handshake gets a clean block.
        Storage::brewHeap();
        ioXferPaint(true);
        delay(60);
        yield();
    }
    if (in) in.close();
    SD.remove(OHC_PENDING);
    if (result.uploaded || result.already) saveUploadedList();

    // OnlineHashCrack has no potfile endpoint: the capture is deleted server
    // side right after hash extraction and the outcome appears in the web
    // dashboard plus by email. So there is deliberately no download step here,
    // unlike WPASec and Pwncrack.
    if (result.failed == 0) {
        result.success = true;
    } else if (result.uploaded || result.already) {
        result.success = true;
        snprintf(result.error, sizeof(result.error), "%s",
                 firstErr[0] ? firstErr : "some failed");
    } else {
        snprintf(result.error, sizeof(result.error), "%s",
                 firstErr[0] ? firstErr
                             : (s_lastError[0] ? s_lastError : "upload failed"));
    }

    if (log) {
        log.printf("# done up=%u already=%u no=%u fail=%u reason=%s\n",
                   (unsigned)result.uploaded, (unsigned)result.already,
                   (unsigned)result.empty, (unsigned)result.failed,
                   result.error[0] ? result.error : "-");
        log.close();
    }

    s_busy = false;
    Serial.printf("[OHC] done up=%u already=%u empty=%u fail=%u\n",
                  (unsigned)result.uploaded, (unsigned)result.already,
                  (unsigned)result.empty, (unsigned)result.failed);
    return result;
}

}  // namespace OHC






/* end of sync/ohc.cpp */
