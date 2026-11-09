#include "image_writer.h"

#include "deflate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <numeric>
#include <utility>

namespace image_writer {
namespace {

void appendBigEndian32(std::vector<uint8_t>& buffer, uint32_t value) {
    buffer.push_back(static_cast<uint8_t>(value >> 24));
    buffer.push_back(static_cast<uint8_t>(value >> 16));
    buffer.push_back(static_cast<uint8_t>(value >> 8));
    buffer.push_back(static_cast<uint8_t>(value));
}

void appendLittleEndian16(std::vector<uint8_t>& buffer, uint16_t value) {
    buffer.push_back(static_cast<uint8_t>(value));
    buffer.push_back(static_cast<uint8_t>(value >> 8));
}

void appendLittleEndian32(std::vector<uint8_t>& buffer, uint32_t value) {
    buffer.push_back(static_cast<uint8_t>(value));
    buffer.push_back(static_cast<uint8_t>(value >> 8));
    buffer.push_back(static_cast<uint8_t>(value >> 16));
    buffer.push_back(static_cast<uint8_t>(value >> 24));
}

bool writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& data, std::string& error) {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        error = "failed to open '" + path.string() + "' for writing";
        return false;
    }
    const size_t written = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), file);
    const bool closed = std::fclose(file) == 0;
    if (written != data.size() || !closed) {
        error = "failed to write '" + path.string() + "'";
        return false;
    }
    return true;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    static const std::vector<uint32_t> table = [] {
        std::vector<uint32_t> values(256);
        for (uint32_t index = 0; index < 256; ++index) {
            uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            values[index] = value;
        }
        return values;
    }();

    uint32_t result = 0xFFFFFFFFu;
    for (size_t index = 0; index < size; ++index)
        result = table[(result ^ data[index]) & 0xFFu] ^ (result >> 8);
    return result ^ 0xFFFFFFFFu;
}

void appendPngChunk(std::vector<uint8_t>& png,
                    const char type[4],
                    const std::vector<uint8_t>& payload) {
    appendBigEndian32(png, static_cast<uint32_t>(payload.size()));
    const size_t crcStart = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), payload.begin(), payload.end());
    appendBigEndian32(png, crc32(png.data() + crcStart, png.size() - crcStart));
}

int32_t paethPredictor(int32_t left, int32_t above, int32_t aboveLeft) {
    const int32_t estimate = left + above - aboveLeft;
    const int32_t distanceLeft = std::abs(estimate - left);
    const int32_t distanceAbove = std::abs(estimate - above);
    const int32_t distanceAboveLeft = std::abs(estimate - aboveLeft);
    if (distanceLeft <= distanceAbove && distanceLeft <= distanceAboveLeft)
        return left;
    return distanceAbove <= distanceAboveLeft ? above : aboveLeft;
}

// Picks a filter per scanline the same way libpng's default heuristic does: try
// every filter type and keep the one with the smallest sum of absolute values.
std::vector<uint8_t> filterScanlines(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    const size_t stride = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> filtered;
    filtered.reserve((stride + 1) * height);

    std::vector<uint8_t> candidates(5 * stride);
    const uint8_t* previous = nullptr;

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* current = rgba.data() + static_cast<size_t>(row) * stride;

        int bestFilter = 0;
        uint64_t bestScore = std::numeric_limits<uint64_t>::max();
        for (int filter = 0; filter < 5; ++filter) {
            uint8_t* out = candidates.data() + static_cast<size_t>(filter) * stride;
            uint64_t score = 0;
            for (size_t index = 0; index < stride; ++index) {
                const int32_t left = index >= 4 ? current[index - 4] : 0;
                const int32_t above = previous ? previous[index] : 0;
                const int32_t aboveLeft = (previous && index >= 4) ? previous[index - 4] : 0;
                int32_t value = 0;
                switch (filter) {
                    case 0: value = current[index]; break;
                    case 1: value = current[index] - left; break;
                    case 2: value = current[index] - above; break;
                    case 3: value = current[index] - (left + above) / 2; break;
                    default: value = current[index] - paethPredictor(left, above, aboveLeft); break;
                }
                out[index] = static_cast<uint8_t>(value & 0xFF);
                score += static_cast<uint64_t>(std::abs(static_cast<int>(static_cast<int8_t>(out[index]))));
            }
            if (score < bestScore) {
                bestScore = score;
                bestFilter = filter;
            }
        }

        filtered.push_back(static_cast<uint8_t>(bestFilter));
        const uint8_t* best = candidates.data() + static_cast<size_t>(bestFilter) * stride;
        filtered.insert(filtered.end(), best, best + stride);
        previous = current;
    }
    return filtered;
}

std::string timestampForTiff() {
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d:%02d:%02d %02d:%02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return buffer;
}

struct IfdEntry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    std::vector<uint8_t> payload;
    uint32_t payloadOffset = 0;
};

enum TiffType : uint16_t {
    kTypeByte = 1,
    kTypeAscii = 2,
    kTypeShort = 3,
    kTypeLong = 4,
    kTypeRational = 5,
    kTypeSRational = 10,
};

IfdEntry makeShortEntry(uint16_t tag, std::initializer_list<uint16_t> values) {
    IfdEntry entry;
    entry.tag = tag;
    entry.type = kTypeShort;
    for (uint16_t value : values)
        appendLittleEndian16(entry.payload, value);
    return entry;
}

IfdEntry makeLongEntry(uint16_t tag, std::initializer_list<uint32_t> values) {
    IfdEntry entry;
    entry.tag = tag;
    entry.type = kTypeLong;
    for (uint32_t value : values)
        appendLittleEndian32(entry.payload, value);
    return entry;
}

IfdEntry makeByteEntry(uint16_t tag, std::initializer_list<uint8_t> values) {
    IfdEntry entry;
    entry.tag = tag;
    entry.type = kTypeByte;
    entry.payload.assign(values);
    return entry;
}

IfdEntry makeAsciiEntry(uint16_t tag, const std::string& value) {
    IfdEntry entry;
    entry.tag = tag;
    entry.type = kTypeAscii;
    entry.payload.assign(value.begin(), value.end());
    entry.payload.push_back(0);
    return entry;
}

void appendRational(std::vector<uint8_t>& payload, int32_t numerator, int32_t denominator) {
    appendLittleEndian32(payload, static_cast<uint32_t>(numerator));
    appendLittleEndian32(payload, static_cast<uint32_t>(denominator));
}

IfdEntry makeRationalEntry(uint16_t tag, std::initializer_list<std::pair<int32_t, int32_t>> values) {
    IfdEntry entry;
    entry.tag = tag;
    entry.type = kTypeRational;
    for (const auto& value : values)
        appendRational(entry.payload, value.first, value.second);
    return entry;
}

// Converts a floating point value into an exact-enough rational number.
std::pair<int32_t, int32_t> toRational(double value) {
    const int32_t denominator = 1000000;
    const int32_t numerator = static_cast<int32_t>(std::llround(value * denominator));
    const int32_t divisor = std::gcd(std::abs(numerator), denominator);
    return {numerator / divisor, denominator / divisor};
}

IfdEntry makeColorMatrixEntry(uint16_t tag, const double matrix[9]) {
    IfdEntry entry;
    entry.tag = tag;
    entry.type = kTypeSRational;
    for (int index = 0; index < 9; ++index) {
        const std::pair<int32_t, int32_t> rational = toRational(matrix[index]);
        appendRational(entry.payload, rational.first, rational.second);
    }
    return entry;
}

size_t typeSize(uint16_t type) {
    switch (type) {
        case kTypeShort:
            return 2;
        case kTypeLong:
            return 4;
        case kTypeRational:
        case kTypeSRational:
            return 8;
        default:
            return 1;
    }
}

uint32_t alignTo(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t ifdValueAreaOffset(size_t entryCount) {
    return 8 + static_cast<uint32_t>(2 + entryCount * 12 + 4);
}

// Orders the entries the way TIFF requires (ascending tag) and returns the
// offset at which the image samples have to start.
uint32_t sortAndLayoutEntries(std::vector<IfdEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const IfdEntry& left, const IfdEntry& right) { return left.tag < right.tag; });

    uint32_t valueOffset = ifdValueAreaOffset(entries.size());
    for (const IfdEntry& entry : entries) {
        if (entry.payload.size() > 4)
            valueOffset = alignTo(valueOffset, 4) + static_cast<uint32_t>(entry.payload.size());
    }
    return alignTo(valueOffset, 8);
}

// Serializes the TIFF header, the IFD and the out-of-line values. The returned
// buffer ends exactly at `stripOffset`, so the image samples can be appended.
std::vector<uint8_t> buildTiffHeader(std::vector<IfdEntry>& entries, uint32_t stripOffset) {
    uint32_t valueOffset = ifdValueAreaOffset(entries.size());
    for (IfdEntry& entry : entries) {
        entry.count = static_cast<uint32_t>(entry.payload.size() / typeSize(entry.type));
        if (entry.payload.size() <= 4) {
            entry.payload.resize(4, 0);
            continue;
        }
        valueOffset = alignTo(valueOffset, 4);
        entry.payloadOffset = valueOffset;
        valueOffset += static_cast<uint32_t>(entry.payload.size());
    }

    std::vector<uint8_t> header;
    header.reserve(stripOffset);

    header.push_back('I');
    header.push_back('I');
    appendLittleEndian16(header, 42);
    appendLittleEndian32(header, 8);

    appendLittleEndian16(header, static_cast<uint16_t>(entries.size()));
    for (const IfdEntry& entry : entries) {
        appendLittleEndian16(header, entry.tag);
        appendLittleEndian16(header, entry.type);
        appendLittleEndian32(header, entry.count);
        if (entry.payloadOffset != 0)
            appendLittleEndian32(header, entry.payloadOffset);
        else
            header.insert(header.end(), entry.payload.begin(), entry.payload.end());
    }
    appendLittleEndian32(header, 0);  // no further IFDs

    for (const IfdEntry& entry : entries) {
        if (entry.payloadOffset == 0)
            continue;
        header.resize(entry.payloadOffset, 0);
        header.insert(header.end(), entry.payload.begin(), entry.payload.end());
    }

    header.resize(stripOffset, 0);
    return header;
}

}  // namespace

bool writePng(const std::filesystem::path& path,
              uint32_t width,
              uint32_t height,
              const std::vector<uint8_t>& rgba,
              std::string& error) {
    if (width == 0 || height == 0 ||
        rgba.size() != static_cast<size_t>(width) * height * 4) {
        error = "invalid RGBA buffer size for PNG";
        return false;
    }

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> header;
    appendBigEndian32(header, width);
    appendBigEndian32(header, height);
    header.push_back(8);  // bit depth
    header.push_back(6);  // colour type: truecolour with alpha
    header.push_back(0);  // deflate
    header.push_back(0);  // adaptive filtering
    header.push_back(0);  // no interlacing
    appendPngChunk(png, "IHDR", header);

    const std::vector<uint8_t> filtered = filterScanlines(rgba, width, height);
    appendPngChunk(png, "IDAT", deflate::compressZlib(filtered.data(), filtered.size()));
    appendPngChunk(png, "IEND", {});

    return writeFile(path, png, error);
}

bool writeDng(const std::filesystem::path& path, const DngImage& image, std::string& error) {
    if (image.width == 0 || image.height == 0 || image.rgb == nullptr) {
        error = "invalid image data for DNG";
        return false;
    }

    const size_t sampleCount = static_cast<size_t>(image.width) * image.height * 3;
    const uint16_t endianProbe = 1;
    const bool littleEndianHost = *reinterpret_cast<const uint8_t*>(&endianProbe) == 1;

    std::vector<IfdEntry> entries = {
        makeLongEntry(254, {0}),                          // NewSubfileType: full resolution
        makeLongEntry(256, {image.width}),                // ImageWidth
        makeLongEntry(257, {image.height}),               // ImageLength
        makeShortEntry(258, {16, 16, 16}),                // BitsPerSample
        makeShortEntry(259, {1}),                         // Compression: none
        makeShortEntry(262, {34892}),                     // PhotometricInterpretation: LinearRaw
        makeAsciiEntry(271, image.make.empty() ? "Blackmagic Design" : image.make),  // Make
        makeLongEntry(273, {0}),                          // StripOffsets (filled in below)
        makeShortEntry(274, {1}),                         // Orientation
        makeShortEntry(277, {3}),                         // SamplesPerPixel
        makeLongEntry(278, {image.height}),               // RowsPerStrip
        makeLongEntry(279, {static_cast<uint32_t>(sampleCount * 2)}),  // StripByteCounts
        makeRationalEntry(282, {{72, 1}}),                // XResolution
        makeRationalEntry(283, {{72, 1}}),                // YResolution
        makeShortEntry(284, {1}),                         // PlanarConfiguration: chunky
        makeShortEntry(296, {2}),                         // ResolutionUnit: inch
        makeAsciiEntry(305, "braw-to-dng"),               // Software
        makeAsciiEntry(306, timestampForTiff()),          // DateTime
        makeShortEntry(339, {1, 1, 1}),                   // SampleFormat: unsigned integer
        makeByteEntry(50706, {1, 4, 0, 0}),               // DNGVersion: 1.4.0.0
        makeByteEntry(50707, {1, 1, 0, 0}),               // DNGBackwardVersion: 1.1.0.0
        makeAsciiEntry(50708, image.uniqueCameraModel.empty() ? "Blackmagic Design"
                                                             : image.uniqueCameraModel),
        makeShortEntry(50714, {0, 0, 0}),                 // BlackLevel
        makeLongEntry(50717, {image.whiteLevel, image.whiteLevel, image.whiteLevel}),  // WhiteLevel
        makeLongEntry(50719, {0, 0}),                     // DefaultCropOrigin
        makeLongEntry(50720, {image.width, image.height}),  // DefaultCropSize
        makeColorMatrixEntry(50721, image.colorMatrix1),  // ColorMatrix1
        makeRationalEntry(50728, {{1, 1}, {1, 1}, {1, 1}}),  // AsShotNeutral
        makeShortEntry(50778, {21}),                      // CalibrationIlluminant1: D65
        makeLongEntry(50829, {0, 0, image.height, image.width}),  // ActiveArea
    };
    if (!image.model.empty())
        entries.push_back(makeAsciiEntry(272, image.model));  // Model

    const uint32_t stripOffset = sortAndLayoutEntries(entries);
    for (IfdEntry& entry : entries) {
        if (entry.tag == 273) {
            entry.payload = {static_cast<uint8_t>(stripOffset),
                             static_cast<uint8_t>(stripOffset >> 8),
                             static_cast<uint8_t>(stripOffset >> 16),
                             static_cast<uint8_t>(stripOffset >> 24)};
        }
    }

    const std::vector<uint8_t> header = buildTiffHeader(entries, stripOffset);

    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        error = "failed to open '" + path.string() + "' for writing";
        return false;
    }

    bool ok = std::fwrite(header.data(), 1, header.size(), file) == header.size();

    // Samples are written in chunks so large images never need a full byte copy.
    const size_t chunkSamples = 1 << 20;
    std::vector<uint8_t> chunk;
    for (size_t offset = 0; ok && offset < sampleCount; offset += chunkSamples) {
        const size_t count = std::min(chunkSamples, sampleCount - offset);
        if (littleEndianHost) {
            const uint8_t* samples = reinterpret_cast<const uint8_t*>(image.rgb + offset);
            ok = std::fwrite(samples, 2, count, file) == count;
        } else {
            chunk.resize(count * 2);
            for (size_t index = 0; index < count; ++index) {
                const uint16_t value = image.rgb[offset + index];
                chunk[index * 2] = static_cast<uint8_t>(value);
                chunk[index * 2 + 1] = static_cast<uint8_t>(value >> 8);
            }
            ok = std::fwrite(chunk.data(), 1, chunk.size(), file) == chunk.size();
        }
    }

    if (std::fclose(file) != 0)
        ok = false;

    if (!ok)
        error = "failed to write '" + path.string() + "'";
    return ok;
}

}  // namespace image_writer
