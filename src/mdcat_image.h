// Image rendering: <img> tags and ![alt](src) images via timg (sixel or Kitty), plus the helpers
// that replay captured image bytes at the cursor.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>

#include "mdcat_pool.h"

namespace mdcat {

// Pixel width of a PNG file, read from its IHDR chunk; 0 if the file is not a readable PNG.
int pngWidth(const std::string& path);

// Scan a markdown image ![alt](src) starting at s[i] (which must be '!'). On success sets alt/src,
// advances i past the closing ')' and returns true.
bool scanMdImageAt(const std::string& s, size_t& i, std::string& alt, std::string& src);

// Parse exactly one markdown image (plain or linked) into "alt"/"src" (and "href") attrs.
bool parseMdImage(const std::string& text, std::map<std::string, std::string>& attrs);

// Parse exactly one HTML <img ...> tag into lowercased attribute names -> values.
bool parseImgTag(const std::string& text, std::map<std::string, std::string>& attrs);

// The process-wide image pool, created on first use (defined in mdcat_image.cpp).
ThreadPool& imagePool();

// If `text` (already trimmed) is a single image, render it and return true, writing the raw image
// bytes to `out` and its cell footprint to `cellWidth` / `cellHeight`. On failure to produce an
// image, the alt text (or literal tag) is returned as a one-line block (cellHeight == 1).
// `availWidth` caps the width in columns. `kittyId`: 0 mints an id now, nonzero is used verbatim,
// kKittyIdDefer leaves timg's id for a deferred slot to stamp later (ADR 0003).
bool renderImageBlock(const std::string& text,
                      int availWidth,
                      const std::string& fileDir,
                      std::string& out,
                      int& cellWidth,
                      int& cellHeight,
                      bool forceWidthBound = false,
                      uint32_t kittyId = 0);

// Replay a captured image (sixel or Kitty APC) at the cursor, leaving the cursor where it started
// (bracketed with DECSC/DECRC).
void replaySixel(const std::string& image, std::ostream& out);

// The framed bytes of an image paragraph `rows` tall: reserve the band, paint, end at its bottom.
std::string imageParagraphBytes(const std::string& image, int rows);

// Write imageParagraphBytes(image, rows) to `out`.
void emitImageParagraph(const std::string& image, int rows, std::ostream& out);

}  // namespace mdcat
