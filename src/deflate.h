#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace deflate {

// Compresses data into a zlib stream (RFC 1950). Used for PNG image data so the
// tool does not depend on zlib being installed.
std::vector<uint8_t> compressZlib(const uint8_t* data, size_t size);

}  // namespace deflate
