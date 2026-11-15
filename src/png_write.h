// A minimal PNG encoder, used to hand mdcat's own generated graphics (the built-in Mermaid
// diagrams) to a Kitty-protocol terminal without an external image tool.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Encode a w x h image as an 8-bit RGBA PNG (colour type 6, no interlace). `rgba` holds w*h pixels
// in row-major order, each packed as 0xRRGGBBAA, with straight (non-premultiplied) alpha. The
// deflate stream uses stored blocks, so the output is deterministic and independent of any zlib.
std::string encodePngRgba(int w, int h, const std::vector<uint32_t>& rgba);
