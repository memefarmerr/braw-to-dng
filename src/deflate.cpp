#include "deflate.h"

#include <algorithm>
#include <cstring>

namespace deflate {
namespace {

constexpr int kWindowSize = 32768;
constexpr int kWindowMask = kWindowSize - 1;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kHashSize = 1 << kHashBits;
constexpr int kMaxChainLength = 64;

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& output) : m_output(output) {}

    // Writes bits least-significant first, as required for values and extra bits.
    void writeBits(uint32_t value, int count) {
        m_buffer |= static_cast<uint64_t>(value) << m_bitCount;
        m_bitCount += count;
        while (m_bitCount >= 8) {
            m_output.push_back(static_cast<uint8_t>(m_buffer & 0xFF));
            m_buffer >>= 8;
            m_bitCount -= 8;
        }
    }

    // Huffman codes are packed starting with the most-significant bit.
    void writeCode(uint32_t code, int count) {
        for (int bit = count - 1; bit >= 0; --bit)
            writeBits((code >> bit) & 1u, 1);
    }

    void alignToByte() {
        if (m_bitCount > 0)
            writeBits(0, 8 - m_bitCount);
    }

private:
    std::vector<uint8_t>& m_output;
    uint64_t m_buffer = 0;
    int m_bitCount = 0;
};

// Encodes a literal byte or a length symbol with the fixed Huffman code table.
void writeSymbol(BitWriter& writer, uint32_t symbol) {
    if (symbol <= 143)
        writer.writeCode(0x30 + symbol, 8);
    else if (symbol <= 255)
        writer.writeCode(0x190 + (symbol - 144), 9);
    else if (symbol <= 279)
        writer.writeCode(symbol - 256, 7);
    else
        writer.writeCode(0xC0 + (symbol - 280), 8);
}

struct LengthCode {
    uint16_t baseLength;
    uint8_t extraBits;
    uint16_t symbol;
};

const LengthCode kLengthCodes[] = {
    {3, 0, 257},   {4, 0, 258},   {5, 0, 259},   {6, 0, 260},   {7, 0, 261},
    {8, 0, 262},   {9, 0, 263},   {10, 0, 264},  {11, 1, 265},  {13, 1, 266},
    {15, 1, 267},  {17, 1, 268},  {19, 2, 269},  {23, 2, 270},  {27, 2, 271},
    {31, 2, 272},  {35, 3, 273},  {43, 3, 274},  {51, 3, 275},  {59, 3, 276},
    {67, 4, 277},  {83, 4, 278},  {99, 4, 279},  {115, 4, 280}, {131, 5, 281},
    {163, 5, 282}, {195, 5, 283}, {227, 5, 284}, {258, 0, 285},
};

const uint16_t kDistanceBase[30] = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,    25,    33,    49,    65,     97,     129,
    193,  257,  385,  513,  769,  1025,  1537,  2049,  3073,  4097,  6145,  8193,  12289,  16385,  24577,
};

const uint8_t kDistanceExtraBits[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

void writeMatch(BitWriter& writer, int length, int distance) {
    const LengthCode* lengthCode = &kLengthCodes[0];
    for (const LengthCode& candidate : kLengthCodes) {
        if (length >= candidate.baseLength)
            lengthCode = &candidate;
        else
            break;
    }
    writeSymbol(writer, lengthCode->symbol);
    if (lengthCode->extraBits > 0)
        writer.writeBits(static_cast<uint32_t>(length - lengthCode->baseLength), lengthCode->extraBits);

    int distanceCode = 0;
    while (distanceCode + 1 < 30 && distance >= kDistanceBase[distanceCode + 1])
        ++distanceCode;
    writer.writeCode(static_cast<uint32_t>(distanceCode), 5);
    if (kDistanceExtraBits[distanceCode] > 0)
        writer.writeBits(static_cast<uint32_t>(distance - kDistanceBase[distanceCode]),
                         kDistanceExtraBits[distanceCode]);
}

inline uint32_t hashAt(const uint8_t* data, size_t position) {
    uint32_t value = static_cast<uint32_t>(data[position]) << 16 |
                     static_cast<uint32_t>(data[position + 1]) << 8 |
                     static_cast<uint32_t>(data[position + 2]);
    return (value * 2654435761u) >> (32 - kHashBits);
}

std::vector<uint8_t> compressFixed(const uint8_t* data, size_t size) {
    std::vector<uint8_t> output;
    output.reserve(size / 2 + 64);
    BitWriter writer(output);

    writer.writeBits(1, 1);  // final block
    writer.writeBits(1, 2);  // fixed Huffman codes

    std::vector<int32_t> head(kHashSize, -1);
    std::vector<int32_t> chain(kWindowSize, -1);

    auto insert = [&](size_t position) {
        if (position + kMinMatch > size)
            return;
        uint32_t hash = hashAt(data, position);
        chain[position & kWindowMask] = head[hash];
        head[hash] = static_cast<int32_t>(position);
    };

    size_t position = 0;
    while (position < size) {
        int bestLength = 0;
        int bestDistance = 0;

        if (position + kMinMatch <= size) {
            const size_t maxLength = std::min<size_t>(kMaxMatch, size - position);
            const size_t limit = position > static_cast<size_t>(kWindowSize)
                                     ? position - kWindowSize
                                     : 0;
            int32_t candidate = head[hashAt(data, position)];
            int chainLength = kMaxChainLength;
            while (candidate >= 0 && static_cast<size_t>(candidate) >= limit && chainLength-- > 0) {
                const uint8_t* candidateData = data + candidate;
                const uint8_t* currentData = data + position;
                if (candidateData[bestLength] == currentData[bestLength]) {
                    size_t length = 0;
                    while (length < maxLength && candidateData[length] == currentData[length])
                        ++length;
                    if (static_cast<int>(length) > bestLength) {
                        bestLength = static_cast<int>(length);
                        bestDistance = static_cast<int>(position - static_cast<size_t>(candidate));
                        if (length == maxLength)
                            break;
                    }
                }
                candidate = chain[static_cast<size_t>(candidate) & kWindowMask];
            }
        }

        if (bestLength >= kMinMatch) {
            writeMatch(writer, bestLength, bestDistance);
            for (int offset = 0; offset < bestLength; ++offset)
                insert(position + static_cast<size_t>(offset));
            position += static_cast<size_t>(bestLength);
        } else {
            writeSymbol(writer, data[position]);
            insert(position);
            ++position;
        }
    }

    writeSymbol(writer, 256);  // end of block
    writer.alignToByte();
    return output;
}

uint32_t adler32(const uint8_t* data, size_t size) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t index = 0; index < size; ++index) {
        a = (a + data[index]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

}  // namespace

std::vector<uint8_t> compressZlib(const uint8_t* data, size_t size) {
    std::vector<uint8_t> output;
    output.reserve(size / 2 + 64);
    output.push_back(0x78);  // deflate, 32K window
    output.push_back(0x9C);  // default compression, no dictionary (check bits valid)

    const std::vector<uint8_t> compressed = compressFixed(data, size);
    output.insert(output.end(), compressed.begin(), compressed.end());

    const uint32_t checksum = adler32(data, size);
    output.push_back(static_cast<uint8_t>(checksum >> 24));
    output.push_back(static_cast<uint8_t>(checksum >> 16));
    output.push_back(static_cast<uint8_t>(checksum >> 8));
    output.push_back(static_cast<uint8_t>(checksum));
    return output;
}

}  // namespace deflate
