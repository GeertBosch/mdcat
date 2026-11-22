// Block-level renderers for paragraphs, headings and thematic breaks.
#include "mdcat_blocks.h"

#include <algorithm>
#include <cstdint>
#include <future>
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "mdcat_image.h"
#include "mdcat_inline.h"
#include "mdcat_math.h"
#include "mdcat_pool.h"
#include "mdcat_protocol.h"
#include "mdcat_slot_sink.h"
#include "mdcat_state.h"
#include "mdcat_style.h"
#include "mdcat_terminal.h"
#include "mdcat_text.h"
#include "unicode_width.h"

namespace mdcat {

void emitParagraph(const std::vector<std::string>& lines, std::ostream& out) {
    // Join the paragraph's source lines (a soft line break becomes a space), render inline markup,
    // then reflow the styled result to the terminal width by display columns.
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) joined += ' ';
        joined += trim(lines[i]);
    }
    // A paragraph that is just an <img> tag for a local PNG renders as an image instead of text.
    // renderImageBlock returns either a multi-row sixel (ih > 1, or ih == 1 with sixel bytes) or a
    // one-line text fallback (ih == 1, plain text — e.g. on a non-graphics terminal). The sixel is
    // placed via the reserve-and-paint helper; the text fallback is emitted as a normal line.
    std::string imgText = trim(joined);
    int availW = terminalWidth();
    std::string fileDir = gFileDir;
    // Quick non-rendering test: is this paragraph an image tag at all? parseImgTag/parseMdImage are
    // pure and cheap, so checking here avoids submitting a job for ordinary paragraphs. (A
    // graphics- capable check is implicit — renderImageBlock falls back to text when graphics are
    // unavailable.)
    {
        std::map<std::string, std::string> probe;
        bool isImgTag = parseImgTag(imgText, probe) || parseMdImage(imgText, probe);
        if (isImgTag) {
            // Deferred slot (non-tty): submit the conversion, reserve an ordered slot for its
            // framed bytes, and return immediately so later blocks/images proceed in parallel. The
            // worker produces the exact bytes this site would have written: framed image, or text
            // fallback.
            if (SlotSink* sink = asSlotSink(out)) {
                // Defer the Kitty id: the worker leaves timg's default id and the writer stamps a
                // document-order id when it drains this slot, so out-of-order completion stays
                // serial- identical (see kKittyIdDefer and SlotSink::deferImage's stampKittyId).
                std::future<std::string> fut = imagePool().submit([imgText, availW, fileDir] {
                    std::string image;
                    int iw = 0, ih = 0;
                    if (!renderImageBlock(imgText,
                                          availW,
                                          fileDir,
                                          image,
                                          iw,
                                          ih,
                                          /*forceWidthBound=*/false,
                                          kKittyIdDefer) ||
                        image.empty())
                        return std::string();
                    if (isImageBlock(image)) return imageParagraphBytes(image, ih);
                    return image + "\n";  // one-line text fallback
                });
                slotDeferImage(sink, std::move(fut));
                return;
            }
            // Synchronous path (tty buffer or recursive ostringstream): render now and write
            // inline.
            std::string image;
            int iw = 0, ih = 0;
            if (imagePool()
                    .submit(
                        [&] { return renderImageBlock(imgText, availW, fileDir, image, iw, ih); })
                    .get()) {
                if (image.empty()) return;
                if (isImageBlock(image))
                    emitImageParagraph(image, ih, out);
                else
                    out << image << '\n';  // one-line text fallback
                return;
            }
        }
    }
    // A paragraph that STARTS with a Markdown image followed by more text, e.g.
    // "![Logo](logo.png) A short blurb", gets the same two-column treatment as a heading (see
    // renderLeadingImageBlock/emitHeading): image on the left, text to its right. If the leading
    // token turns out not to be a renderable image (no graphics support, bad file, etc.) the
    // worker falls back to the ordinary reflowed-paragraph text instead, computed right here
    // (cheap: no timg involved) and captured into the deferred bytes so the slot is unconditional
    // like the whole-image-paragraph case above.
    if (!imgText.empty() && imgText[0] == '!') {
        std::string textFallback;
        {
            std::ostringstream fb;
            std::string reflowed = reflow(renderInline(joined), availW);
            if (!reflowed.empty()) fb << reflowed << '\n';
            textFallback = fb.str();
        }
        if (SlotSink* sink = asSlotSink(out)) {
            std::future<std::string> fut =
                imagePool().submit([imgText, availW, fileDir, textFallback] {
                    std::string withImage = renderLeadingImageBlock(
                        imgText, availW, fileDir, /*bold=*/false, /*rule=*/"", kKittyIdDefer);
                    return withImage.empty() ? textFallback : withImage;
                });
            slotDeferImage(sink, std::move(fut));
            return;
        }
        std::string withImage = imagePool()
                                    .submit([&] {
                                        return renderLeadingImageBlock(imgText,
                                                                       availW,
                                                                       fileDir,
                                                                       /*bold=*/false,
                                                                       /*rule=*/"");
                                    })
                                    .get();
        out << (withImage.empty() ? textFallback : withImage);
        return;
    }
    // A paragraph that is exactly one block-math expression ($$...$$) is centered, the way GitHub
    // (and TeX display math) sets it off on its own centered line. Each rendered line is centered
    // independently within the available width.
    std::string trimmed = trim(joined);
    bool blockMath = trimmed.size() >= 4 && trimmed.compare(0, 2, "$$") == 0 &&
        trimmed.compare(trimmed.size() - 2, 2, "$$") == 0 &&
        trimmed.find("$$", 2) == trimmed.size() - 2;

    if (blockMath) {
        // Display math: try a two-dimensional layout so a top-level \frac stacks numerator over
        // denominator (renderMathStacked). If the expression has no top-level \frac it returns
        // false and we fall back to the flat single-line rendering below. The stacked box's rows
        // are each centered within the terminal, keeping the whole figure horizontally centered.
        std::string tex = trim(trimmed.substr(2, trimmed.size() - 4));
        MathBox box;
        if (renderMathStacked(tex, box)) {
            int pad = std::max(0, (terminalWidth() - box.width) / 2);
            for (const std::string& row : box.rows) {
                // Right-trim the row so a centered blank tail doesn't emit trailing spaces; the
                // constant left pad (same for every row) keeps the columns aligned.
                std::string trimmedRow = row;
                while (!trimmedRow.empty() && trimmedRow.back() == ' ') trimmedRow.pop_back();
                out << std::string(static_cast<size_t>(pad), ' ') << trimmedRow << '\n';
            }
            return;
        }
    }

    std::string reflowed = reflow(renderInline(joined), terminalWidth());
    if (reflowed.empty()) return;
    if (blockMath) {
        for (const std::string& line : splitOnNewlines(reflowed)) {
            int w = displayWidth(line);
            int pad = std::max(0, (terminalWidth() - w) / 2);
            out << std::string(static_cast<size_t>(pad), ' ') << line << '\n';
        }
        return;
    }
    out << reflowed << '\n';
}

std::string renderLeadingImageBlock(const std::string& text,
                                    int availW,
                                    const std::string& fileDir,
                                    bool bold,
                                    const std::string& rule,
                                    uint32_t kittyId) {
    if (text.empty() || text[0] != '!') return "";
    size_t i = 0;
    std::string alt, src;
    if (!scanMdImageAt(text, i, alt, src)) return "";
    std::string rest = trim(text.substr(i));

    std::string image;
    int iw = 0, ih = 0;
    std::string imgTag = text.substr(0, i);
    if (!renderImageBlock(
            imgTag, availW, fileDir, image, iw, ih, /*forceWidthBound=*/false, kittyId) ||
        image.empty() || !isImageBlock(image))
        return "";

    // A logo sized to its own natural width can be arbitrarily tall (e.g. a portrait graphic),
    // which would balloon a single heading line into a huge block. Cap it to a small height by
    // re-rendering height-bound through a synthetic <img height=...> tag (routes through
    // renderImageBlock's existing height-bound geometry path; width follows the image's aspect).
    // The requested pixel height and the measured footprint go through independent roundings
    // (request: nominal cell; footprint: the terminal's real cell), so a single attempt at exactly
    // the cap can still measure one row over. Shrink and retry a few times until it actually fits.
    constexpr int kMaxLeadingImageRows = 3;
    if (ih > kMaxLeadingImageRows) {
        // src/alt travel through an attribute value quoted with '"'; a literal '"' in either
        // (rare, but Markdown does not forbid it) would break the synthetic tag's parse, so drop
        // it rather than escape it (alt/src are cosmetic here — the geometry is what matters).
        std::string safeSrc = src, safeAlt = alt;
        safeSrc.erase(std::remove(safeSrc.begin(), safeSrc.end(), '"'), safeSrc.end());
        safeAlt.erase(std::remove(safeAlt.begin(), safeAlt.end(), '"'), safeAlt.end());
        int targetRows = kMaxLeadingImageRows;
        for (int attempt = 0; attempt < 4 && targetRows >= 1; ++attempt) {
            std::string boundTag = "<img src=\"" + safeSrc + "\" alt=\"" + safeAlt +
                "\" height=\"" + std::to_string(targetRows * CellMetrics::kNominalH) + "px\">";
            std::string boundImage;
            int biw = 0, bih = 0;
            if (!renderImageBlock(boundTag,
                                  availW,
                                  fileDir,
                                  boundImage,
                                  biw,
                                  bih,
                                  /*forceWidthBound=*/false,
                                  kittyId) ||
                boundImage.empty() || !isImageBlock(boundImage))
                break;  // re-render failed; keep whatever the previous attempt produced
            image = boundImage;
            iw = biw;
            ih = bih;
            if (ih <= kMaxLeadingImageRows) break;
            --targetRows;  // still too tall: ask for one fewer row and try again
        }
    }

    std::string styled = renderInline(rest);
    int textW = std::max(1, availW - iw - 2);
    std::vector<std::string> textLines = splitOnNewlines(reflow(styled, textW));
    if (textLines.empty()) textLines.emplace_back();
    int bandH = std::max(ih, static_cast<int>(textLines.size()));

    // Vertically center the text lines within the band (extra padding split top/bottom).
    int topPad = (bandH - static_cast<int>(textLines.size())) / 2;

    std::ostringstream out;
    for (int k = 0; k < bandH; ++k) {
        out << std::string(static_cast<size_t>(iw), ' ') << "  ";
        int textIdx = k - topPad;
        if (textIdx >= 0 && textIdx < static_cast<int>(textLines.size())) {
            if (bold) out << kBoldOn;
            out << textLines[textIdx];
            if (bold) out << kBoldOff;
        }
        out << '\n';
    }
    out << "\033[" << bandH << 'A';          // up to the band top
    replaySixel(image, out);                 // paint; cursor restored to band top
    out << "\033[" << bandH << 'B' << '\r';  // down to the band bottom, column 1
    if (!rule.empty()) out << rule << '\n';
    return out.str();
}

void emitHeading(int level, const std::string& text, std::ostream& out) {
    static const std::vector<std::string> kUnderlines = {"━", "─", "┈", "┄"};
    int availW = terminalWidth();
    std::string rule = horizontalLine(availW, kUnderlines[std::min(level, 4) - 1]);
    // Quick non-rendering test: does this heading even start with an image tag? Avoids submitting
    // a pool job (and, on the sync path, blocking on it) for the common case of a plain heading.
    if (!text.empty() && text[0] == '!') {
        std::string fileDir = gFileDir;
        if (SlotSink* sink = asSlotSink(out)) {
            // Defer the Kitty id: the writer stamps a document-order id when it drains this slot
            // (see kKittyIdDefer), so out-of-order completion stays serial-identical. If the text
            // does not actually start with a renderable image, the worker returns "" and the
            // slot's bytes are the plain heading text instead, rendered synchronously here (cheap:
            // it never involves timg) so it can be captured into the deferred bytes.
            std::string plainFallback;
            {
                std::ostringstream fb;
                std::string styled = renderInline(text);
                fb << kBoldOn << styled << kBoldOff << '\n' << rule << '\n';
                plainFallback = fb.str();
            }
            std::future<std::string> fut =
                imagePool().submit([text, availW, fileDir, rule, plainFallback] {
                    std::string withImage = renderLeadingImageBlock(
                        text, availW, fileDir, /*bold=*/true, rule, kKittyIdDefer);
                    return withImage.empty() ? plainFallback : withImage;
                });
            slotDeferImage(sink, std::move(fut));
            return;
        } else {
            std::string withImage =
                imagePool()
                    .submit([&] {
                        return renderLeadingImageBlock(text, availW, fileDir, /*bold=*/true, rule);
                    })
                    .get();
            if (!withImage.empty()) {
                out << withImage;
                return;
            }
        }
    }
    // Bold heading text, followed by an underline whose character depends on the level, matching
    // the original script's "━─┈┄" for levels 1..4 (deeper levels reuse the level-4 character).
    std::string styled = renderInline(text);
    out << kBoldOn << styled << kBoldOff << '\n' << rule << '\n';
}

void emitThematicBreak(std::ostream& out) {
    out << horizontalLine(terminalWidth()) << '\n';
}

}  // namespace mdcat
