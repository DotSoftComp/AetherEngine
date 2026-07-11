// Aether Engine — headless frame capture: read back the default framebuffer
// and write it as a 24-bit BMP (used by --screenshot in both the editor and
// the game runtime for automated verification).
#pragma once
#include <cstdint>
#include <vector>

namespace ae {

class Window;

bool writeBMP(const char* path, int w, int h, const std::vector<uint8_t>& rgba);
void captureScreenshot(Window& window, const char* path);

} // namespace ae
