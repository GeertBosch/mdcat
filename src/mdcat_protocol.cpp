#include "mdcat_protocol.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace mdcat {

// A per-run monotonic Kitty image id. timg assigns the SAME id to every render of a given file, so
// two images in one document would collide — a later transmit replaces the earlier image and
// re-lays its placement. Handing out a fresh id per image keeps them independent. Atomic so
// parallel image workers (ADR 0003) can mint ids concurrently without colliding or racing on the
// counter.
uint32_t nextKittyId() {
    static std::atomic<uint32_t> id{1000};
    return ++id;
}

// Set the i= id on the FIRST Kitty controls segment of `apc` to `id` (byte-exact, leaving the
// base64 payload and all chunk continuations untouched). An existing i= value is rewritten;
// when timg omitted it (older timg — Debian's 1.4.3 emits a bare `a=T,f=100,m=0` with neither
// i= nor q=), i=<id> is INSERTED right after `a=T`. Without an id the terminal assigns the image
// the default id 0, so every image in a document would collide on 0 (a later transmit replaces
// the earlier image) — and its OK reply reports `i=0`. Inserting a unique id fixes both.
std::string kittyRewriteId(const std::string& apc, uint32_t id) {
    size_t g = apc.find("\033_G");
    if (g == std::string::npos) return apc;
    size_t keyStart = g + 3;
    size_t semi = apc.find(';', keyStart);  // controls run up to the first ';'
    if (semi == std::string::npos) return apc;
    size_t ip = apc.find("i=", keyStart);
    if (ip != std::string::npos && ip < semi) {  // rewrite an existing i=
        size_t numStart = ip + 2;
        size_t numEnd = numStart;
        while (numEnd < semi && std::isdigit((unsigned char)apc[numEnd])) ++numEnd;
        return apc.substr(0, numStart) + std::to_string(id) + apc.substr(numEnd);
    }
    // No i= present: insert it at the head of the controls run (controls always start with a=T
    // on a real first chunk, so a leading "i=<id>," is well-formed).
    return apc.substr(0, keyStart) + "i=" + std::to_string(id) + "," + apc.substr(keyStart);
}

// Whether the Kitty controls run in [ctrlStart, ctrlEnd) is an image's FIRST chunk — i.e. it
// carries the action key `a=` (a=T transmit+display, a=t transmit-only). Continuation chunks
// carry only `m=` (and, with some timg builds, `q=`), so `a=` is the reliable first-chunk marker.
// Keying off `a=` rather than `i=` matters because older timg emits no i= at all (see
// kittyRewriteId).
static bool kittyIsFirstChunk(const std::string& s, size_t ctrlStart, size_t ctrlEnd) {
    // Search only the controls run: an unbounded find() on a continuation chunk (which has no
    // a=) would scan the rest of the buffer's base64, making renumbering quadratic in image size.
    static const char key[] = "a=";
    return std::search(s.begin() + static_cast<std::ptrdiff_t>(ctrlStart),
                       s.begin() + static_cast<std::ptrdiff_t>(ctrlEnd),
                       key,
                       key + 2) != s.begin() + static_cast<std::ptrdiff_t>(ctrlEnd);
}

// Renumber the i= id of every Kitty image in `bytes`, in order, with successive ids from
// nextKittyId(). Each image's FIRST chunk carries the action key (a=T); continuation chunks (m=)
// carry no a= and are left alone, so exactly one fresh id is minted per image. Used by the
// writer thread (ADR 0003) to stamp ids in document (slot) order, regardless of the order parallel
// workers produced the bytes — keeping output byte-identical to the serial renderer, which minted
// ids in document order on one thread. `bytes` may hold zero, one, or many images (a single
// deferred <img>, or a whole table row).
std::string kittyRenumberAll(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size());
    size_t pos = 0;
    for (;;) {
        size_t g = bytes.find("\033_G", pos);
        if (g == std::string::npos) {
            out.append(bytes, pos, std::string::npos);
            break;
        }
        size_t semi = bytes.find(';', g + 3);
        out.append(bytes, pos, g - pos);  // bytes before this APC
        if (semi == std::string::npos || !kittyIsFirstChunk(bytes, g + 3, semi)) {
            // No action key in this controls run: a continuation chunk (or not an image). Copy
            // "\033_G" only and continue scanning just past it.
            out.append("\033_G");
            pos = g + 3;
            continue;
        }
        // An image's first chunk: set its i= to the next document-order id (inserting i= when
        // timg emitted none) and copy through the ';'.
        out.append(kittyRewriteId(bytes.substr(g, semi - g + 1), nextKittyId()));
        pos = semi + 1;
    }
    return out;
}

// Inject (or replace) the cell-footprint controls c=<cols>,r=<rows> on the FIRST Kitty controls
// segment of `apc`, byte-exact. timg emits no c=/r=, so the terminal would otherwise derive the
// footprint by dividing the PNG's pixel size by ITS OWN cell size — which differs from the ~9px
// cell timg assumed when it sized the image (it has no terminal to query on a pipe). That mismatch
// makes a width-bound table image paint wider than the column mdcat reserved, overlapping the next
// cell. With c=/r= the terminal scales the image into EXACTLY that cell box, so the painted
// footprint always equals mdcat's reservation. (iTerm requires BOTH c and r and honours them
// exactly — verified via tools/probe-kitty-aspect.sh — so we always set both; aspect is preserved
// because cols and rows are computed from the same painted pixels and the same real cell ratio.)
std::string kittyRewriteFootprint(const std::string& apc, int cols, int rows) {
    if (cols <= 0 || rows <= 0) return apc;
    size_t g = apc.find("\033_G");
    if (g == std::string::npos) return apc;
    size_t semi = apc.find(';', g + 3);  // controls run up to the first ';'
    if (semi == std::string::npos) return apc;
    // Strip any existing c=/r= keys from the controls run so we never duplicate them.
    std::string ctrls = apc.substr(g + 3, semi - (g + 3));
    std::string kept;
    size_t i = 0;
    while (i < ctrls.size()) {
        size_t comma = ctrls.find(',', i);
        std::string kv =
            ctrls.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        if (kv.compare(0, 2, "c=") != 0 && kv.compare(0, 2, "r=") != 0) {
            if (!kept.empty()) kept += ',';
            kept += kv;
        }
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    std::string footprint = ",c=" + std::to_string(cols) + ",r=" + std::to_string(rows);
    return apc.substr(0, g + 3) + kept + footprint + apc.substr(semi);
}

// Ensure every Kitty APC in `bytes` suppresses ALL terminal replies with q=2. For each control
// section: rewrite an existing q=<n> to q=2, and on an image's first chunk that carries NO q= at
// all, insert one. Two cases matter. (1) The missing-q= case — older timg (Debian's 1.4.3) emits
// a bare `a=T,...` with no quiet flag, so the terminal defaults to q=0 (always reply). (2) The
// wrong-level case — iTerm2 ignores q=1 (which per spec only suppresses success replies) and
// still sends `ESC_Gi=0;OK ESC\` back; only q=2 silences it (see gmore_emulator.cpp). Over SSH
// those replies pile up in the remote pty's input buffer and spill onto the shell prompt as
// garbage after mdcat exits. mdcat never reads Kitty replies, so q=2 is always the right level.
std::string kittyQ2(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size() + 16);
    size_t pos = 0;
    for (;;) {
        size_t found = bytes.find("\033_G", pos);
        if (found == std::string::npos) {
            out.append(bytes, pos, std::string::npos);
            break;
        }
        out.append(bytes, pos, found - pos);  // bytes before this APC
        size_t ctrlStart = found + 3;
        size_t semi = bytes.find(';', ctrlStart);
        size_t st = bytes.find("\033\\", ctrlStart);
        size_t ctrlEnd = std::min(semi != std::string::npos ? semi : bytes.size(),
                                  st != std::string::npos ? st : bytes.size());
        // Bounded to the controls run (see kittyIsFirstChunk): an unbounded find() on a chunk
        // lacking q= would scan the rest of the payload, making this quadratic.
        static const char qkey[] = "q=";
        auto qit = std::search(bytes.begin() + static_cast<std::ptrdiff_t>(ctrlStart),
                               bytes.begin() + static_cast<std::ptrdiff_t>(ctrlEnd),
                               qkey,
                               qkey + 2);
        size_t q = qit == bytes.begin() + static_cast<std::ptrdiff_t>(ctrlEnd)
            ? std::string::npos
            : static_cast<size_t>(qit - bytes.begin());
        if (q != std::string::npos && q + 2 < ctrlEnd) {
            // Existing q=<n>: copy through it, force the value to 2, skip the old digit.
            out.append(bytes, found, (q + 2) - found);
            out += '2';
            pos = q + 3;  // past "q=" + one value digit
        } else {
            // No q= in THIS control section — insert q=2. This must happen on EVERY chunk,
            // not just the first: iTerm2 keys its ;OK reply off the FINAL chunk (m=0), which
            // in Debian timg 1.4.3's output is a bare `m=0` with no q= (→ q=0, always reply)
            // and no i= (so the reply is the tell-tale `ESC_Gi=0;OK ESC\`). macOS timg 1.6.3
            // happens to stamp q= on every chunk, which is why the first-chunk-only rewrite
            // masked this on macOS but not over SSH to the Ubuntu box.
            out.append("\033_Gq=2");
            if (ctrlStart != ctrlEnd) out += ',';  // empty controls (`ESC_G;…`): no trailing comma
            pos = ctrlStart;
        }
    }
    return out;
}

// Read a PNG's pixel width/height from the IHDR (big-endian uint32s at byte offsets 16 and 20,
// right after the 8-byte signature + 4-byte length + "IHDR"). `png` is the raw PNG bytes. Returns
// false if the buffer is too short or not a PNG.
bool pngSize(const std::string& png, int& w, int& h) {
    if (png.size() < 24) return false;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i)
        if (static_cast<unsigned char>(png[i]) != sig[i]) return false;
    auto u32 = [&](size_t o) {
        return (static_cast<unsigned char>(png[o]) << 24) |
            (static_cast<unsigned char>(png[o + 1]) << 16) |
            (static_cast<unsigned char>(png[o + 2]) << 8) | static_cast<unsigned char>(png[o + 3]);
    };
    w = u32(16);
    h = u32(20);
    return w > 0 && h > 0;
}

// Decode the base64 PNG payload out of a Kitty APC (concatenating all chunks) and read its pixel
// size, so the caller can compute the exact cell footprint timg's downscaled image will occupy.
// Returns false if no PNG could be recovered. Only a small prefix is needed (the IHDR), but timg's
// first chunk already contains it, so we decode just the first chunk's payload.
bool kittyImageSize(const std::string& apc, int& pw, int& ph) {
    size_t g = apc.find("\033_G");
    if (g == std::string::npos) return false;
    size_t semi = apc.find(';', g);
    if (semi == std::string::npos) return false;
    size_t end = apc.find("\033\\", semi);
    if (end == std::string::npos) return false;
    std::string b64 = apc.substr(semi + 1, end - semi - 1);
    // Minimal base64 decode (standard alphabet, no whitespace in Kitty payloads).
    static const std::string tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char c : b64) {
        if (c == '=') break;
        size_t pos = tbl.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) | static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
        if (out.size() >= 24) break;  // enough for the IHDR
    }
    return pngSize(out, pw, ph);
}

// Read the painted pixel size out of a sixel data stream's raster attributes. A sixel begins
// ESC P <params> q "Pan;Pad;Ph;Pv ... where Ph/Pv are the graphic's pixel width/height. We scan for
// the '"' that introduces them and parse the last two of the four numbers. Returns false if absent
// (older sixel without raster attributes), in which case callers fall back to the requested size.
bool sixelPixelSize(const std::string& sixel, int& pw, int& ph) {
    size_t q = sixel.find("\033P");
    if (q == std::string::npos) return false;
    size_t quote = sixel.find('"', q);
    if (quote == std::string::npos) return false;
    int pan = 0, pad = 0, w = 0, h = 0;
    if (std::sscanf(sixel.c_str() + quote + 1, "%d;%d;%d;%d", &pan, &pad, &w, &h) == 4 && w > 0 &&
        h > 0) {
        pw = w;
        ph = h;
        return true;
    }
    return false;
}

// Whether `s` is sixel image data rather than a text fallback: a sixel contains a DCS introducer
// (ESC P), which rendered inline text never does (it uses only ESC[ for SGR and ESC] for OSC 8).
bool isSixelImage(const std::string& s) {
    return s.find("\033P") != std::string::npos;
}

// Whether `s` is Kitty graphics image data: a Kitty APC begins ESC _ G.
bool isKittyImage(const std::string& s) {
    return s.find("\033_G") != std::string::npos;
}

// Whether `s` is an inline image (either protocol) rather than a one-line text fallback.
bool isImageBlock(const std::string& s) {
    return isSixelImage(s) || isKittyImage(s);
}

std::string base64Encode(const std::string& in) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        size_t n = std::min<size_t>(3, in.size() - i);
        uint32_t v = static_cast<uint32_t>(static_cast<unsigned char>(in[i])) << 16;
        if (n > 1) v |= static_cast<uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8;
        if (n > 2) v |= static_cast<uint32_t>(static_cast<unsigned char>(in[i + 2]));
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += n > 1 ? kAlphabet[(v >> 6) & 63] : '=';
        out += n > 2 ? kAlphabet[v & 63] : '=';
    }
    return out;
}

// A Kitty APC that transmits and displays `png` (f=100) in a `cols` x `rows` cell box, written by
// mdcat itself rather than by timg (ADR 0004). C=1 leaves the cursor where it was. Draw the image
// over the text. Images must use transparency where the text needs to be visible. The payload
// is split into chunks of at most 4096 base64 bytes, and every chunk carries q=2 (suppress all
// replies; see kittyQ2).
std::string kittyApcFromPng(const std::string& png, int cols, int rows, uint32_t id, int xOffset) {
    constexpr size_t kChunk = 4096;
    std::string b64 = base64Encode(png);
    std::string apc;
    apc.reserve(b64.size() + b64.size() / kChunk * 16 + 96);
    for (size_t pos = 0; pos < b64.size() || pos == 0; pos += kChunk) {
        bool first = pos == 0;
        bool more = pos + kChunk < b64.size();
        apc += "\033_G";
        if (first)
            apc += "a=T,f=100,i=" + std::to_string(id) + ",q=2,C=1,c=" + std::to_string(cols) +
                ",r=" + std::to_string(rows) + ",z=0" +
                (xOffset > 0 ? ",X=" + std::to_string(xOffset) : std::string()) +
                (more ? ",m=1" : "");
        else
            apc += std::string("q=2,m=") + (more ? "1" : "0");
        apc += ';';
        apc.append(b64, pos, kChunk);
        apc += "\033\\";
    }
    return apc;
}

}  // namespace mdcat
