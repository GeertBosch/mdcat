// Table renderer: GFM table detection, cell splitting, alignment and column layout.
#include "mdcat_table.h"

#include <algorithm>
#include <future>
#include <ostream>
#include <sstream>
#include <string>
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

// Is `line` a GFM table delimiter row, e.g. |---|:--:| ? It must contain only pipes, colons,
// hyphens and spaces, include at least one hyphen, and at least one pipe.
bool isTableDelimiterRow(const std::string& line) {
    bool hasDash = false, hasPipe = false;
    for (char c : line) {
        if (c == '-')
            hasDash = true;
        else if (c == '|')
            hasPipe = true;
        else if (c != ':' && c != ' ' && c != '\t')
            return false;
    }
    return hasDash && hasPipe;
}

// Split a table row on unescaped '|', dropping one optional leading and trailing pipe, and trim
// each resulting cell. Pipes escaped as \| become literal pipes within a cell.
namespace {

std::vector<std::string> splitTableRow(const std::string& raw) {
    std::string row = trim(raw);
    size_t b = 0, e = row.size();
    if (b < e && row[b] == '|') ++b;
    if (e > b && row[e - 1] == '|') --e;
    std::vector<std::string> cells;
    std::string cur;
    for (size_t i = b; i < e; ++i) {
        if (row[i] == '\\' && i + 1 < e && row[i + 1] == '|') {
            cur += '|';
            ++i;
            continue;
        }
        if (row[i] == '|') {
            cells.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur += row[i];
    }
    cells.push_back(trim(cur));
    return cells;
}

// Parse the per-column alignment from a delimiter row. Each cell is one of `---` (left, the
// default), `:--` (left), `--:` (right), or `:-:` (center); a leading colon means "left edge
// pinned", a trailing colon "right edge pinned". Cells are split the same way as data rows.
std::vector<Align> parseTableAlignment(const std::string& delimiter) {
    std::vector<Align> aligns;
    for (const std::string& cell : splitTableRow(delimiter)) {
        std::string c = trim(cell);
        bool left = !c.empty() && c.front() == ':';
        bool right = !c.empty() && c.back() == ':';
        if (left && right)
            aligns.push_back(Align::Center);
        else if (right)
            aligns.push_back(Align::Right);
        else
            aligns.push_back(Align::Left);
    }
    return aligns;
}


}  // namespace

void emitTable(const std::vector<std::string>& rawRows, std::ostream& out) {
    // rawRows[0] is the header, rawRows[1] the delimiter (consumed by the caller's detection and
    // not printed), and the rest are body rows. Cells are inline-rendered.
    std::vector<std::vector<std::string>> rows;  // rendered cells, header + body
    rows.push_back(splitTableRow(rawRows[0]));
    for (size_t i = 2; i < rawRows.size(); ++i) rows.push_back(splitTableRow(rawRows[i]));

    size_t ncols = 0;
    for (auto& r : rows) ncols = std::max(ncols, r.size());
    for (auto& r : rows) r.resize(ncols);  // pad short rows with empty cells
    if (ncols == 0) return;

    // Per-column alignment from the delimiter row, padded to ncols with the Left default for any
    // columns the delimiter did not specify.
    std::vector<Align> aligns = parseTableAlignment(rawRows[1]);
    aligns.resize(ncols, Align::Left);

    const int W = terminalWidth();
    int budget = W - 2 * static_cast<int>(ncols - 1);
    if (budget < static_cast<int>(ncols)) budget = static_cast<int>(ncols);  // at least 1 col each

    // Render every cell, distinguishing image cells (a lone <img> for a local PNG that produced an
    // actual sixel) from text cells. An image cell holds raw sixel bytes that must never reach
    // displayWidth/reflow, so we record its column width and row height separately and carry the
    // bytes through layout untouched. A non-image result (ordinary text, or an <img> fallback to
    // its alt text/filename when graphics are unavailable) is treated as normal reflowable text.
    std::vector<std::vector<bool>> isImage(rows.size(), std::vector<bool>(ncols, false));
    std::vector<std::vector<int>> imgRows(rows.size(), std::vector<int>(ncols, 0));  // image height
    std::vector<std::vector<int>> imgCellW(rows.size(), std::vector<int>(ncols, 0));  // image width
    std::vector<std::vector<std::string>> imgText(rows.size(), std::vector<std::string>(ncols));
    std::vector<int> imgWidth(ncols, 0);  // forced width of an image column, if any
    // Batch-join (ADR 0003): submit every image-tag cell's conversion to the pool up front, then
    // wait on the whole batch before laying out the table — the column-width algorithm needs every
    // image's footprint, so the table can't be emitted until its own cells are measured, but the
    // conversions run in parallel with each other (and overlap any earlier blocks' deferred images
    // already queued). Per-cell results live in stable storage so each job writes its own slot
    // without racing.
    struct CellImg {
        std::string block;
        int iw = 0, ih = 0;
        bool ok = false;
    };
    std::vector<std::vector<CellImg>> cimg(rows.size(), std::vector<CellImg>(ncols));
    // future is move-only, so the row vectors can't be copy-filled; resize each instead.
    std::vector<std::vector<std::future<void>>> cfut(rows.size());
    for (auto& row : cfut) row.resize(ncols);
    std::string fileDir = gFileDir;
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            std::string cellText = trim(rows[i][j]);
            std::map<std::string, std::string> probe;
            if (!parseImgTag(cellText, probe) && !parseMdImage(cellText, probe)) continue;
            imgText[i][j] = cellText;
            CellImg* r = &cimg[i][j];
            // Render at the full content budget so an explicit height is honoured at the image's
            // natural width; a column too narrow to hold it is handled by a re-render after layout.
            cfut[i][j] = imagePool().submit([cellText, budget, fileDir, r] {
                r->ok = renderImageBlock(cellText,
                                         budget,
                                         fileDir,
                                         r->block,
                                         r->iw,
                                         r->ih,
                                         /*forceWidthBound=*/false,
                                         kKittyIdDefer);
            });
        }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            if (cfut[i][j].valid()) cfut[i][j].get();  // join this cell's conversion
            const CellImg& r = cimg[i][j];
            if (r.ok && isImageBlock(r.block)) {
                // The Kitty id is left as timg's default (kKittyIdDefer); the writer renumbers
                // every image in document order when it drains this table's slot.
                rows[i][j] = r.block;
                isImage[i][j] = true;
                imgRows[i][j] = std::max(1, r.ih);
                imgCellW[i][j] = r.iw;
                imgWidth[j] = std::max(imgWidth[j], r.iw);
            } else if (r.ok && !r.block.empty() && r.ih == 1) {
                rows[i][j] = r.block;  // <img> fallback text (alt/filename): reflow as text
            } else {
                imgText[i][j].clear();  // not an image after all; drop the kept tag
                rows[i][j] = renderInline(rows[i][j]);
            }
        }
    }

    // Natural (unwrapped) width of each column = its widest rendered cell. Image cells contribute
    // their recorded width rather than displayWidth (which can't measure a sixel block). A text
    // cell may already hold its own line breaks (from a <br>), so measure its widest line, not the
    // whole string (which would wrongly sum the lines and count the '\n').
    std::vector<int> natural(ncols, 0);
    for (size_t i = 0; i < rows.size(); ++i)
        for (size_t j = 0; j < ncols; ++j) {
            if (isImage[i][j]) {
                natural[j] = std::max(natural[j], imgWidth[j]);
                continue;
            }
            for (auto& ln : splitOnNewlines(rows[i][j]))
                natural[j] = std::max(natural[j], displayWidth(ln));
        }

    // Decide a target width per column with max-min fair sharing of the content budget (which
    // already excludes the two-space separators). A column whose natural width is at most its fair
    // share keeps that natural width; the columns that want more split the leftover budget evenly.
    // We iterate because fixing the columns that fit raises the share available to the rest — so
    // e.g. one narrow column does not let the wide ones starve each other (the earlier
    // left-to-right greedy did).
    std::vector<int> target(ncols, 0);
    std::vector<bool> fixed(ncols, false);
    int remBudget = budget, remCols = static_cast<int>(ncols);
    for (bool changed = true; changed && remCols > 0;) {
        changed = false;
        int fair = remBudget / remCols;
        for (size_t j = 0; j < ncols; ++j) {
            if (fixed[j] || natural[j] > fair) continue;
            target[j] = natural[j];  // fits within its share: keep natural width
            fixed[j] = true;
            remBudget -= natural[j];
            --remCols;
            changed = true;
        }
    }
    if (remCols > 0) {  // over-share columns split the rest evenly
        int base = remBudget / remCols, extra = remBudget % remCols, k = 0;
        for (size_t j = 0; j < ncols; ++j)
            if (!fixed[j]) target[j] = base + (k++ < extra ? 1 : 0);  // remainder to earliest
    }

    // Apply the targets. A text column is reflowed to its target and may end narrower still; an
    // image column takes the smaller of its natural width and the target (it does not wrap), and is
    // re-rendered below if that is narrower than the image it already produced.
    std::vector<int> widths(ncols, 0);
    for (size_t j = 0; j < ncols; ++j) {
        if (imgWidth[j] > 0) {
            widths[j] = std::min(natural[j], target[j]);
        } else {
            int actual = 0;
            for (auto& r : rows) {
                r[j] = reflow(r[j], target[j]);
                for (auto& ln : splitOnNewlines(r[j])) actual = std::max(actual, displayWidth(ln));
            }
            widths[j] = actual < 1 ? (natural[j] == 0 ? 0 : 1) : actual;
        }
    }

    // An image whose painted width exceeds its allocated column (the table could not give it the
    // room its requested height implied) is re-rendered WIDTH-BOUND to exactly that column width,
    // so it never overflows into the next column or past the table. forceWidthBound is required
    // because the image's true (real-cell) footprint can exceed the column even when the
    // nominal-cell estimate does not, so renderImageBlock's own heuristic would otherwise keep it
    // height-bound and too wide. This is the only case that runs timg twice, and only when columns
    // are tight.
    std::vector<int> allocWidth = widths;  // the budgeted width; tightening must not exceed it
    // The forced width-bound re-renders are submitted as a follow-up batch and joined the same way
    // (ADR 0003), again with kKittyIdDefer + row-major id stamping for serial-identical output.
    std::vector<std::vector<CellImg>> rimg(rows.size(), std::vector<CellImg>(ncols));
    std::vector<std::vector<std::future<void>>> rfut(rows.size());
    for (auto& row : rfut) row.resize(ncols);
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            if (!isImage[i][j] || allocWidth[j] >= imgCellW[i][j]) continue;
            std::string cellText = imgText[i][j];
            int allocW = allocWidth[j];
            CellImg* r = &rimg[i][j];
            rfut[i][j] = imagePool().submit([cellText, allocW, fileDir, r] {
                r->ok = renderImageBlock(cellText,
                                         allocW,
                                         fileDir,
                                         r->block,
                                         r->iw,
                                         r->ih,
                                         /*forceWidthBound=*/true,
                                         kKittyIdDefer);
            });
        }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            if (!rfut[i][j].valid()) continue;
            rfut[i][j].get();
            CellImg& r = rimg[i][j];
            if (r.ok && isImageBlock(r.block)) {
                rows[i][j] = r.block;  // id left deferred; writer renumbers in document order
                imgRows[i][j] = std::max(1, r.ih);
                imgCellW[i][j] = r.iw;
            }
        }
    }

    // Tighten each image column to the width its image actually paints (aspect-fit and rounding can
    // make it narrower than the allocated cap), so a column hugs its image and the gap between
    // columns stays the intended two spaces rather than padding out to the cap. Never go below the
    // column's widest TEXT cell (a header wider than the image would overflow and misalign later
    // columns) and never ABOVE the budgeted width (which would overflow the table — a re-render can
    // round a cell or two wide).
    for (size_t j = 0; j < ncols; ++j) {
        if (imgWidth[j] <= 0) continue;
        int w = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (isImage[i][j])
                w = std::max(w, imgCellW[i][j]);
            else
                for (auto& ln : splitOnNewlines(rows[i][j])) w = std::max(w, displayWidth(ln));
        }
        if (w > 0) widths[j] = std::min(w, allocWidth[j]);
    }

    // Re-pin each Kitty image's cell footprint (c=/r=) to the FINAL column width. The c=/r= baked
    // in during rendering reflects that render's availWidth cap (the full budget on the first
    // pass), not the width this column ends up with after fair-sharing and tightening — so without
    // this an image can carry a c= wider than its column and overflow into the next cell. We
    // rewrite c to the final width and scale r by the same ratio to keep the aspect ratio the
    // render already chose. (Sixel images carry no c=/r=; kittyRewriteFootprint no-ops on a
    // non-Kitty block, so the guard is just isImage.) imgRows/imgCellW are also corrected so the
    // band reserves the right number of rows.
    for (size_t j = 0; j < ncols; ++j) {
        if (imgWidth[j] <= 0) continue;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!isImage[i][j] || !isKittyImage(rows[i][j]) || imgCellW[i][j] <= 0) continue;
            int newC = widths[j];
            int newR = std::max(1, (imgRows[i][j] * newC + imgCellW[i][j] / 2) / imgCellW[i][j]);
            rows[i][j] = kittyRewriteFootprint(rows[i][j], newC, newR);
            imgCellW[i][j] = newC;
            imgRows[i][j] = newR;
        }
    }

    int total = 0;
    for (int w : widths) total += w;
    total += 2 * static_cast<int>(ncols - 1);  // two-space column separators
    std::string hline = horizontalLine(total);

    if (std::getenv("MDCAT_DEBUG_TABLE")) {
        std::fprintf(stderr, "mdcat: table W=%d budget=%d total=%d widths=[", W, budget, total);
        for (size_t j = 0; j < ncols; ++j) std::fprintf(stderr, "%s%d", j ? "," : "", widths[j]);
        std::fprintf(stderr, "] imgW=[");
        for (size_t j = 0; j < ncols; ++j) std::fprintf(stderr, "%s%d", j ? "," : "", imgWidth[j]);
        std::fprintf(stderr, "]\n");
    }

    // The 1-based starting column of each cell, accounting for the two-space separators. Used to
    // position image sixels during the overlay pass.
    std::vector<int> colOrigin(ncols, 1);
    for (size_t j = 1; j < ncols; ++j) colOrigin[j] = colOrigin[j - 1] + widths[j - 1] + 2;

    // Render one table row. The row occupies a band as tall as the tallest cell — text line count
    // or image row height, whichever is greater. Text is laid out first, top-aligned, one line at a
    // time across the band; image cells contribute blank padding of their column width during this
    // pass (reserving their space). Then, in a second pass, each image's sixel is painted into its
    // reserved column with relative cursor moves: up to the band top, across to the cell's column,
    // replay (which leaves the cursor at column 1 just below the image), then back down to the band
    // bottom. This keeps every image inside its own cell and keeps row separators below the band.
    auto emitRow = [&](size_t rowIdx, bool bold) {
        const std::vector<std::string>& cells = rows[rowIdx];
        std::vector<std::vector<std::string>> cellLines(ncols);
        int bandH = 1;
        for (size_t j = 0; j < ncols; ++j) {
            if (isImage[rowIdx][j]) {
                bandH = std::max(bandH, imgRows[rowIdx][j]);
            } else {
                cellLines[j] = splitOnNewlines(cells[j]);
                bandH = std::max(bandH, static_cast<int>(cellLines[j].size()));
            }
        }
        // Text pass: bandH lines. Image cells reserve their column width with spaces.
        for (int k = 0; k < bandH; ++k) {
            std::string line;
            for (size_t j = 0; j < ncols; ++j) {
                if (j) line += "  ";
                if (isImage[rowIdx][j]) {
                    line.append(static_cast<size_t>(widths[j]), ' ');
                } else {
                    std::string piece = static_cast<size_t>(k) < cellLines[j].size()
                        ? cellLines[j][k]
                        : std::string();
                    if (bold && !piece.empty()) piece = kBoldOn + piece + kBoldOff;
                    line += padAligned(piece, widths[j], aligns[j]);
                }
            }
            out << line << '\n';
        }
        // Overlay pass: paint each image into its reserved space. The cursor starts at column 1 on
        // the row just below the band; every move is relative to that, so the band's earlier scroll
        // (if any) does not matter. Each sixel is bracketed by DECSC/DECRC (in replaySixel), so it
        // restores the cursor to the band-top position regardless of how the terminal advances
        // after a sixel — then we step deterministically back to the band bottom for the next
        // image.
        for (size_t j = 0; j < ncols; ++j) {
            if (!isImage[rowIdx][j]) continue;
            out << "\033[" << bandH << 'A';          // up to the band top
            out << "\033[" << colOrigin[j] << 'G';   // across to this cell's column
            replaySixel(cells[j], out);              // paint; cursor restored to band top
            out << "\033[" << bandH << 'B' << '\r';  // down to the band bottom, column 1
        }
    };

    for (size_t i = 0; i < rows.size(); ++i) {
        emitRow(i, /*bold=*/i == 0);
        if (i == 0)
            out << hline << '\n';  // header underline
        else if (i + 1 < rows.size())
            out << kLightGray << hline << kReset << '\n';  // light-gray row separator
    }
}

}  // namespace mdcat
