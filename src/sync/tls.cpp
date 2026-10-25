#include "tls.h"
#include "net_io.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <multi_heap.h>
#include <mbedtls/platform.h>
#include <string.h>
#include <stdlib.h>

namespace {
multi_heap_handle_t s_heap = nullptr;
uint8_t* s_lo = nullptr;
uint8_t* s_hi = nullptr;

void* arenaCalloc(size_t n, size_t size) {
    size_t need = n * size;
    if (size != 0 && need / size != n) return nullptr;
    if (s_heap) {
        void* p = multi_heap_malloc(s_heap, need);
        if (p) {
            memset(p, 0, need);
            return p;
        }
    }
    return calloc(n, size);
}

void arenaFree(void* p) {
    if (!p) return;
    if ((uint8_t*)p >= s_lo && (uint8_t*)p < s_hi) {
        multi_heap_free(s_heap, p);
        return;
    }
    free(p);
}
}

namespace Tls {

void arenaBegin(void* buf, size_t size) {
    if (s_heap || !buf || size < 2048) return;
    s_heap = multi_heap_register(buf, size);
    if (!s_heap) {
        Serial.println("[TLS] arena fail");
        return;
    }
    s_lo = (uint8_t*)buf;
    s_hi = s_lo + size;
    mbedtls_platform_set_calloc_free(arenaCalloc, arenaFree);
    Serial.printf("[TLS] arena %u\n", (unsigned)size);
}

void arenaEnd() {
    if (!s_heap) return;
    mbedtls_platform_set_calloc_free(calloc, free);
    s_heap = nullptr;
    s_lo = s_hi = nullptr;
}

bool arenaActive() { return s_heap != nullptr; }

bool streamFile(WiFiClientSecure& client, File& file, size_t fileSize,
                char* outError, size_t outErrorLen) {
    uint8_t chunk[1460];
    size_t left = fileSize;
    size_t sent = 0;
    ioXfer().sent = 0;
    ioXfer().size = (uint32_t)fileSize;
    ioXferPaint(true);
    while (left > 0) {
        if (ESP.getFreeHeap() < 16000) {
            client.flush();
            uint32_t t0 = millis();
            while (ESP.getFreeHeap() < 16000 && millis() - t0 < 800) {
                if (!client.connected()) break;
                delay(20);
                yield();
            }
            if (ESP.getFreeHeap() < 12000) {
                snprintf(outError, outErrorLen, "heap low");
                file.close();
                client.stop();
                return false;
            }
        }
        if (!client.connected()) {
            snprintf(outError, outErrorLen, "lost @%u", (unsigned)sent);
            file.close();
            client.stop();
            return false;
        }
        size_t n = left > sizeof(chunk) ? sizeof(chunk) : left;
        size_t rd = file.read(chunk, n);
        if (rd == 0) {
            snprintf(outError, outErrorLen, "sd read");
            file.close();
            client.stop();
            return false;
        }
        if (!ioWriteAll(client, chunk, rd)) {
            snprintf(outError, outErrorLen, "tls write");
            file.close();
            client.stop();
            return false;
        }
        sent += rd;
        left -= rd;
        ioXfer().sent = (uint32_t)sent;
        ioXferPaint(false);
        yield();
    }
    file.close();
    ioXfer().sent = (uint32_t)fileSize;
    ioXferPaint(true);
    return true;
}

}  // namespace Tls
