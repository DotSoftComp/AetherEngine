// Aether Engine — headless frame capture: read back the default framebuffer
// and write it as a 24-bit BMP (used by --screenshot in both the editor and
// the game runtime for automated verification), plus reference comparison
// (--compare) so agents can assert visual regressions without eyes.
#pragma once
#include <cstdint>
#include <vector>

namespace ae {

bool writeBMP(const char* path, int w, int h, const std::vector<uint8_t>& rgba);
// Reads the backbuffer at w×h and writes it as a BMP (window-backend-agnostic;
// hosts pass their window's width()/height()).
void captureScreenshot(int w, int h, const char* path);

// Reads a BMP written by writeBMP (24-bit uncompressed) back into RGBA.
bool readBMP(const char* path, int& w, int& h, std::vector<uint8_t>& rgba);

// RGB comparison of two same-size RGBA buffers (alpha ignored — BMPs are 24-bit).
struct ImageDiff {
    double psnr = 0.0;    // dB; 99 = identical
    double meanAbs = 0.0; // mean per-channel absolute difference (0..255)
    int maxAbs = 0;       // worst single-channel difference
    double diffPct = 0.0; // % of pixels off by more than 2/255 in any channel
};
ImageDiff compareImages(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                        int w, int h);

// Amplified |a-b| heatmap (x8, red-tinted) so a human or vision model can see
// WHERE two captures diverge.
bool writeDiffBMP(const char* path, const std::vector<uint8_t>& a,
                  const std::vector<uint8_t>& b, int w, int h);

} // namespace ae
