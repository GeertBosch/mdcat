// Pure byte handling for the Kitty and sixel graphics protocols: image ids, APC rewriting, PNG and
// sixel size probes, and image-block predicates. No I/O, no terminal state.
#pragma once

#include <cstdint>
#include <string>

namespace mdcat {

// Next process-wide Kitty image id (monotonic, atomic, thread-safe). One counter per process.
uint32_t nextKittyId();

// Sentinel kittyId for renderImageBlock: "leave timg's default id; the id is stamped later, in
// document order, on the writer thread" (ADR 0003 deferred slots). Distinct from 0 (mint now).
inline constexpr uint32_t kKittyIdDefer = 0xFFFFFFFFu;

// Set (or insert) the i= id on the first Kitty controls segment of `apc`.
std::string kittyRewriteId(const std::string& apc, uint32_t id);

// Renumber the i= id of every Kitty image in `bytes`, in order, minting ids with nextKittyId().
// Linear in the size of `bytes`.
std::string kittyRenumberAll(const std::string& bytes);

// Force the cell footprint (c=/r=) of a Kitty image APC to `cols` x `rows`.
std::string kittyRewriteFootprint(const std::string& apc, int cols, int rows);

// Insert q=2 (suppress terminal replies) into every Kitty chunk of `bytes`. Linear.
std::string kittyQ2(const std::string& bytes);

// Pixel size from a PNG's IHDR; false if `png` is too short or not a PNG.
bool pngSize(const std::string& png, int& w, int& h);

// Pixel size of the PNG carried by a Kitty APC (decodes the first chunk); false if unrecoverable.
bool kittyImageSize(const std::string& apc, int& pw, int& ph);

// Painted pixel size from a sixel stream's raster attributes; false if absent.
bool sixelPixelSize(const std::string& sixel, int& pw, int& ph);

// Whether `s` is sixel data (contains a DCS introducer).
bool isSixelImage(const std::string& s);

// Whether `s` is Kitty graphics data (contains an APC ESC _ G).
bool isKittyImage(const std::string& s);

// Whether `s` is an inline image of either protocol rather than a text fallback.
bool isImageBlock(const std::string& s);

// Standard base64 (with padding) of `in`.
std::string base64Encode(const std::string& in);

// A Kitty APC that transmits and displays `png` in a `cols` x `rows` cell box with image id `id`,
// optionally shifted right by `xOffset` pixels (see imageXOffset).
std::string kittyApcFromPng(
    const std::string& png, int cols, int rows, uint32_t id, int xOffset = 0);

}  // namespace mdcat
