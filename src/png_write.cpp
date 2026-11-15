#include "png_write.h"

#include <array>
#include <cstddef>

namespace {

const std::array<uint32_t, 256>& crcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    return table;
}

uint32_t crc32(uint32_t crc, const std::string& s) {
    const std::array<uint32_t, 256>& t = crcTable();
    for (char ch : s) crc = t[(crc ^ static_cast<unsigned char>(ch)) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint32_t adler32(const std::string& s) {
    uint32_t a = 1, b = 0;
    for (char ch : s) {
        a = (a + static_cast<unsigned char>(ch)) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void putU32be(std::string& s, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) s += static_cast<char>((v >> shift) & 0xFF);
}

void appendChunk(std::string& png, const char type[5], const std::string& data) {
    putU32be(png, static_cast<uint32_t>(data.size()));
    std::string body(type, 4);
    body += data;
    png += body;
    putU32be(png, crc32(0xFFFFFFFFu, body) ^ 0xFFFFFFFFu);
}

}  // namespace

std::string encodePngRgba(int w, int h, const std::vector<uint32_t>& rgba) {
    // Scanlines, each preceded by filter type 0 (None).
    std::string raw;
    raw.reserve(static_cast<size_t>(h) * (static_cast<size_t>(w) * 4 + 1));
    for (int y = 0; y < h; ++y) {
        raw += '\0';
        for (int x = 0; x < w; ++x) {
            uint32_t p =
                rgba[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
            for (int shift = 24; shift >= 0; shift -= 8)
                raw += static_cast<char>((p >> shift) & 0xFF);
        }
    }

    // zlib stream: header, stored deflate blocks of at most 65535 bytes, Adler-32.
    std::string z;
    z += '\x78';
    z += '\x01';
    constexpr size_t kMaxStored = 65535;
    size_t pos = 0;
    do {
        size_t n = raw.size() - pos < kMaxStored ? raw.size() - pos : kMaxStored;
        bool last = pos + n == raw.size();
        z += last ? '\x01' : '\x00';
        z += static_cast<char>(n & 0xFF);
        z += static_cast<char>((n >> 8) & 0xFF);
        z += static_cast<char>(~n & 0xFF);
        z += static_cast<char>((~n >> 8) & 0xFF);
        z.append(raw, pos, n);
        pos += n;
    } while (pos < raw.size());
    putU32be(z, adler32(raw));

    std::string ihdr;
    putU32be(ihdr, static_cast<uint32_t>(w));
    putU32be(ihdr, static_cast<uint32_t>(h));
    ihdr += '\x08';  // bit depth
    ihdr += '\x06';  // colour type: RGBA
    ihdr += '\0';    // compression
    ihdr += '\0';    // filter
    ihdr += '\0';    // interlace

    std::string png("\x89PNG\r\n\x1a\n", 8);
    appendChunk(png, "IHDR", ihdr);
    appendChunk(png, "IDAT", z);
    appendChunk(png, "IEND", std::string());
    return png;
}
