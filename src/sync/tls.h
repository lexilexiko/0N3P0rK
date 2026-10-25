#pragma once
#include <stddef.h>
#include <FS.h>
class WiFiClientSecure;

// Same idea as M5PORKCHOP: lend the screen buffer to mbedTLS so
// handshake RAM does not come from a shredded heap (that hang).
namespace Tls {
void arenaBegin(void* buf, size_t size);
void arenaEnd();
// True while mbedTLS's calloc/free are routed into the arena buffer —
// callers deciding "is there enough heap for a TLS handshake" need to
// know this, because once the arena is active the big mbedTLS buffers
// come from there, not from the general heap.
bool arenaActive();
bool streamFile(WiFiClientSecure& client, File& file, size_t fileSize,
                char* outError, size_t outErrorLen);
}
