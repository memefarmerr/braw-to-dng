#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace image_writer {

// Writes an 8-bit RGBA PNG.
bool writePng(const std::filesystem::path& path,
              uint32_t width,
              uint32_t height,
              const std::vector<uint8_t>& rgba,
              std::string& error);

// A linear (scene referred) RGB image that is stored as a LinearRaw DNG.
struct DngImage {
    uint32_t width = 0;
    uint32_t height = 0;
    const uint16_t* rgb = nullptr;  // interleaved RGB, 3 samples per pixel
    uint16_t whiteLevel = 65535;
    std::string make;
    std::string model;
    std::string uniqueCameraModel;
    double colorMatrix1[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // XYZ (D65) -> camera native
};

bool writeDng(const std::filesystem::path& path, const DngImage& image, std::string& error);

}  // namespace image_writer
