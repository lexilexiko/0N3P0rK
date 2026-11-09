#pragma once

#include <M5Unified.h>

namespace MeshMode {

void update();
void draw(M5Canvas& canvas);
void getStatusLine(char* out, size_t size);
bool isTyping();

}
