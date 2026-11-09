#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace braw {

enum class OutputFormat { Dng, Png };

struct RenderOptions {
    OutputFormat format = OutputFormat::Dng;
    std::filesystem::path sdkLibraryPath;
    uint64_t frameIndex = 0;
};

struct RenderResult {
    uint32_t width = 0;
    uint32_t height = 0;

    // 8-bit RGBA, only filled for OutputFormat::Png.
    std::vector<uint8_t> rgba;

    // 16-bit linear RGB, only filled for OutputFormat::Dng.
    std::vector<uint16_t> rgb;
    uint16_t whiteLevel = 65535;
    // XYZ (D65) -> native RGB, matching whichever gamut the clip was processed
    // with. Fed straight into the DNG's ColorMatrix1 tag.
    double xyzToCamera[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    // Largest linear sample seen, useful for diagnosing odd exposure scaling.
    float maxLinearValue = 0.0f;

    std::string colorGamut;
    std::string cameraType;
    uint64_t frameCount = 0;
};

// Initialises the platform runtime (COM on Windows); pair with shutdown().
bool initialize(std::string& error);
void shutdown();

// Folder that holds the Blackmagic RAW codec library for this platform.
std::filesystem::path defaultSdkLibraryPath();

// Renders a single frame of a .braw clip into memory.
bool renderFrame(const std::filesystem::path& clipPath,
                 const RenderOptions& options,
                 RenderResult& result,
                 std::string& error);

}  // namespace braw
