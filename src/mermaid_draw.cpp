#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "mermaid_model.h"
#include "png_write.h"

namespace mermaid {

namespace {

// Raster parameters, all derived from the cell size.
struct Metrics {
    int cw, ch;
    int s;         // stroke width, round(cw / 9)
    int hs;        // half the stroke width
    int headLen;   // arrowhead length
    int headHalf;  // arrowhead half-width
    uint32_t rgb;

    explicit Metrics(const Env& env)
        : cw(std::max(1, env.cellW)),
          ch(std::max(1, env.cellH)),
          s(std::max(1, (2 * cw + 9) / 18)),
          hs(s / 2),
          headLen(std::max(6, cw)),
          headHalf(std::max(3, (45 * cw + 50) / 100)),  // round(0.45 * cw)
          rgb(env.darkTheme ? 0xc8c8c8u : 0x505050u) {}

    int px(int hu) const { return hu * cw / 2; }  // half-cell units -> pixels
    int py(int hu) const { return hu * ch / 2; }
};

// The raster: straight-alpha RGBA, all pixels the stroke colour, with alpha 0 where nothing is
// drawn. Coordinates are in pixels; half-open ranges.
struct Raster {
    int w, h;
    std::vector<uint8_t> alpha;

    Raster(int w_, int h_)
        : w(w_), h(h_), alpha(static_cast<size_t>(w_) * static_cast<size_t>(h_), 0) {}

    void plot(int x, int y, int a) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t& p =
            alpha[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
        if (a > p) p = static_cast<uint8_t>(a);
    }

    void rect(int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) plot(x, y, 255);
    }

    // A filled triangle with its tip at (tipX8, tipY8), in eighths of a pixel, pointing down
    // (`down`) or right. Its base lies `len` px behind the tip and is 2*half px wide; each pixel is
    // sampled 4x4.
    void arrowhead(int tipX8, int tipY8, bool down, int len, int half) {
        int span = len + half + 2;
        int cx = tipX8 / 8, cy = tipY8 / 8;
        int x0 = down ? cx - half - 1 : cx - span;
        int x1 = down ? cx + half + 2 : cx + 1;
        int y0 = down ? cy - span : cy - half - 1;
        int y1 = down ? cy + 1 : cy + half + 2;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                int hits = 0;
                for (int j = 0; j < 4; ++j) {
                    for (int i = 0; i < 4; ++i) {
                        int ex = 8 * x + 2 * i + 1, ey = 8 * y + 2 * j + 1;
                        int depth = down ? tipY8 - ey : tipX8 - ex;
                        int cross = std::abs(down ? ex - tipX8 : ey - tipY8);
                        if (depth > 0 && depth <= len * 8 && cross * len <= half * depth) ++hits;
                    }
                }
                if (hits) plot(x, y, (hits * 255 + 8) / 16);
            }
        }
    }
};

// ---- Strokes -------------------------------------------------------------------------------

std::vector<Stroke> strokesOf(const Graph& g, const Layout& lay) {
    std::vector<Stroke> strokes;
    for (const CellBox& f : lay.nodeFoot) {
        Stroke st;
        st.kind = Stroke::Outline;
        st.pts = {{2 * f.col + 1, 2 * f.row + 1},
                  {2 * (f.col + f.cols) - 1, 2 * (f.row + f.rows) - 1}};
        strokes.push_back(st);
    }
    for (size_t k = 0; k < g.edges.size(); ++k) {
        const std::vector<Pt>& path = lay.edgePath[k];
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            Stroke st;
            st.kind = Stroke::Segment;
            st.line = g.edges[k].line;
            st.pts = {path[i], path[i + 1]};
            strokes.push_back(st);
        }
        if (g.edges[k].head == End::Arrow && path.size() >= 2) {
            Stroke st;
            st.kind = Stroke::Arrowhead;
            st.line = g.edges[k].line;
            st.pts = {path.back(), path[path.size() - 2]};
            strokes.push_back(st);
        }
    }
    return strokes;
}

// ---- Rasterizer ----------------------------------------------------------------------------

// Draw the strokes. Arrows run from one outline to the next; a shaft stops where its arrowhead
// begins, and the head's tip touches the target's outline.
bool rasterize(const std::vector<Stroke>& strokes,
               const Metrics& m,
               Raster& ras,
               std::string& reason) {
    const int s = m.s, hs = m.hs;
    for (const Stroke& st : strokes) {
        if (st.kind == Stroke::Outline) {
            int l = m.px(st.pts[0].first), t = m.py(st.pts[0].second);
            int r = m.px(st.pts[1].first), b = m.py(st.pts[1].second);
            ras.rect(l - hs, t - hs, r - hs + s, t - hs + s);
            ras.rect(l - hs, b - hs, r - hs + s, b - hs + s);
            ras.rect(l - hs, t - hs, l - hs + s, b - hs + s);
            ras.rect(r - hs, t - hs, r - hs + s, b - hs + s);
        } else if (st.kind == Stroke::Segment) {
            Pt a = st.pts[0], b = st.pts[1];
            bool tipped = std::any_of(strokes.begin(), strokes.end(), [&](const Stroke& h) {
                return h.kind == Stroke::Arrowhead && h.pts[0] == b;
            });
            bool vertical = a.first == b.first;
            int cross = vertical ? m.px(a.first) : m.py(a.second);
            int from = vertical ? m.py(a.second) : m.px(a.first);
            int to = vertical ? m.py(b.second) : m.px(b.first);
            if (to < from) {
                reason = "unsupported stroke direction";
                return false;
            }
            // The shaft clears the source outline and ends at the target's outer edge, or where the
            // head begins.
            int end = tipped ? to - hs - m.headLen : to - hs + s;
            if (tipped && end < from - hs + s) {
                reason = "arrow too short";
                return false;
            }
            if (vertical)
                ras.rect(cross - hs, from - hs, cross - hs + s, end);
            else
                ras.rect(from - hs, cross - hs, end, cross - hs + s);
        } else if (st.kind == Stroke::Arrowhead) {
            Pt tip = st.pts[0], base = st.pts[1];
            bool down = tip.first == base.first;
            if (down ? tip.second < base.second : tip.first < base.first) {
                reason = "unsupported stroke direction";
                return false;
            }
            int x = 8 * (m.px(tip.first) - hs), y = 8 * (m.py(tip.second) - hs);
            if (down)
                x += 4 * s;
            else
                y += 4 * s;
            ras.arrowhead(x, y, down, m.headLen, m.headHalf);
        }
    }
    return true;
}

// ---- Occupancy grid and validation ---------------------------------------------------------

struct Grid {
    int cols, rows;
    std::vector<Occ> cells;

    Grid(int c, int r) : cols(c), rows(r), cells(static_cast<size_t>(c) * static_cast<size_t>(r)) {}

    Occ& at(int c, int r) {
        return cells[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)];
    }
    Occ get(int c, int r) const {
        if (c < 0 || r < 0 || c >= cols || r >= rows) return Occ::Empty;
        return cells[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)];
    }
};

// Call fn(col, row) for each cell that an axis-aligned hu segment touches.
template <class F>
void segmentCells(Pt a, Pt b, F fn) {
    int c0 = std::min(a.first, b.first) / 2, c1 = std::max(a.first, b.first) / 2;
    int r0 = std::min(a.second, b.second) / 2, r1 = std::max(a.second, b.second) / 2;
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c) fn(c, r);
}

// Call fn(col, row) for each cell on the border of an outline box.
template <class F>
void outlineCells(Pt tl, Pt br, F fn) {
    segmentCells(tl, {br.first, tl.second}, fn);
    segmentCells({tl.first, br.second}, br, fn);
    segmentCells(tl, {tl.first, br.second}, fn);
    segmentCells({br.first, tl.second}, br, fn);
}

bool inBox(const CellBox& b, int c, int r) {
    return c >= b.col && c < b.col + b.cols && r >= b.row && r < b.row + b.rows;
}

bool onOutline(const CellBox& f, Pt p) {
    int l = 2 * f.col + 1, t = 2 * f.row + 1, r = 2 * (f.col + f.cols) - 1,
        b = 2 * (f.row + f.rows) - 1;
    bool insideX = p.first >= l && p.first <= r, insideY = p.second >= t && p.second <= b;
    return (insideX && (p.second == t || p.second == b)) ||
        (insideY && (p.first == l || p.first == r));
}

// Whether the (collinear or not) axis-aligned segments a and b overlap along a stretch.
bool overlap(Pt a0, Pt a1, Pt b0, Pt b1) {
    auto span = [](int p, int q, int& lo, int& hi) {
        lo = std::min(p, q);
        hi = std::max(p, q);
    };
    int alx, ahx, aly, ahy, blx, bhx, bly, bhy;
    span(a0.first, a1.first, alx, ahx);
    span(a0.second, a1.second, aly, ahy);
    span(b0.first, b1.first, blx, bhx);
    span(b0.second, b1.second, bly, bhy);
    bool aVert = alx == ahx, bVert = blx == bhx;
    if (aVert != bVert) return false;
    if (aVert) return alx == blx && std::max(aly, bly) < std::min(ahy, bhy);
    return aly == bly && std::max(alx, blx) < std::min(ahx, bhx);
}

// Turn layout bugs into fallbacks instead of misleading pictures. Reads the occupancy grid and the
// stroke list, not the pixels.
bool validate(const Graph& g,
              const Layout& lay,
              const std::vector<Stroke>& strokes,
              const Grid& grid,
              std::string& reason) {
    // No edge passes through a node or label, except at its own endpoints.
    for (size_t k = 0; k < g.edges.size(); ++k) {
        const CellBox& a = lay.nodeFoot[static_cast<size_t>(g.edges[k].from)];
        const CellBox& b = lay.nodeFoot[static_cast<size_t>(g.edges[k].to)];
        const std::vector<Pt>& path = lay.edgePath[k];
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            bool ok = true;
            segmentCells(path[i], path[i + 1], [&](int c, int r) {
                Occ o = grid.get(c, r);
                if (o == Occ::Label || (o == Occ::Node && !inBox(a, c, r) && !inBox(b, c, r)))
                    ok = false;
            });
            if (!ok) {
                reason = "edge crosses a node";
                return false;
            }
        }
    }
    // No two edges with different sources and different targets share a segment.
    for (size_t j = 0; j < g.edges.size(); ++j) {
        for (size_t k = j + 1; k < g.edges.size(); ++k) {
            if (g.edges[j].from == g.edges[k].from || g.edges[j].to == g.edges[k].to) continue;
            const std::vector<Pt>&pj = lay.edgePath[j], &pk = lay.edgePath[k];
            for (size_t a = 0; a + 1 < pj.size(); ++a) {
                for (size_t b = 0; b + 1 < pk.size(); ++b) {
                    if (overlap(pj[a], pj[a + 1], pk[b], pk[b + 1])) {
                        reason = "edges share a segment";
                        return false;
                    }
                }
            }
        }
    }
    // Every arrowhead touches its target outline.
    for (size_t k = 0; k < g.edges.size(); ++k) {
        if (g.edges[k].head != End::Arrow || lay.edgePath[k].empty()) continue;
        if (!onOutline(lay.nodeFoot[static_cast<size_t>(g.edges[k].to)], lay.edgePath[k].back())) {
            reason = "arrowhead misses its target";
            return false;
        }
    }
    // No stroke falls inside a label cell. An arrowhead reaches back one cell from its tip.
    bool ok = true;
    auto check = [&](int c, int r) {
        if (grid.get(c, r) == Occ::Label) ok = false;
    };
    for (const Stroke& st : strokes) {
        if (st.kind == Stroke::Outline) {
            outlineCells(st.pts[0], st.pts[1], check);
        } else if (st.kind == Stroke::Segment) {
            segmentCells(st.pts[0], st.pts[1], check);
        } else if (st.kind == Stroke::Arrowhead) {
            Pt tip = st.pts[0], base = st.pts[1];
            Pt back = tip.first == base.first ? Pt{tip.first, tip.second - 2}
                                              : Pt{tip.first - 2, tip.second};
            segmentCells(back, tip, check);
        }
    }
    if (!ok) {
        reason = "stroke over label";
        return false;
    }
    return true;
}

}  // namespace

bool drawDiagram(
    const Graph& g, const Layout& lay, const Env& env, Diagram& out, std::string& reason) {
    const Metrics m(env);
    std::vector<Stroke> strokes = strokesOf(g, lay);

    Grid grid(lay.cols, lay.rows);
    for (const CellBox& f : lay.nodeFoot)
        for (int r = f.row; r < f.row + f.rows; ++r)
            for (int c = f.col; c < f.col + f.cols; ++c) grid.at(c, r) = Occ::Node;
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        const CellBox& l = lay.nodeLabel[i];
        for (int r = l.row; r < l.row + l.rows; ++r)
            for (int c = l.col; c < l.col + l.cols; ++c) grid.at(c, r) = Occ::Label;
    }
    for (const Stroke& st : strokes) {
        if (st.kind != Stroke::Segment) continue;
        segmentCells(st.pts[0], st.pts[1], [&](int c, int r) {
            if (grid.get(c, r) == Occ::Empty) grid.at(c, r) = Occ::Edge;
        });
    }

    if (!validate(g, lay, strokes, grid, reason)) return false;

    Raster ras(lay.cols * m.cw, lay.rows * m.ch);
    if (!rasterize(strokes, m, ras, reason)) return false;

    // The text layer: labels on each node's middle row, spaces elsewhere.
    out.cols = lay.cols;
    out.rows = lay.rows;
    out.textRows.assign(static_cast<size_t>(lay.rows), std::string());
    std::vector<std::vector<size_t>> byRow(static_cast<size_t>(lay.rows));
    for (size_t i = 0; i < g.nodes.size(); ++i)
        byRow[static_cast<size_t>(lay.nodeLabel[i].row)].push_back(i);
    for (int r = 0; r < lay.rows; ++r) {
        std::vector<size_t>& nodes = byRow[static_cast<size_t>(r)];
        std::sort(nodes.begin(), nodes.end(), [&](size_t a, size_t b) {
            return lay.nodeLabel[a].col < lay.nodeLabel[b].col;
        });
        std::string& line = out.textRows[static_cast<size_t>(r)];
        int at = 0;
        for (size_t i : nodes) {
            line.append(static_cast<size_t>(lay.nodeLabel[i].col - at), ' ');
            line += g.nodes[i].label.lines[0];
            at = lay.nodeLabel[i].col + lay.nodeLabel[i].cols;
        }
        line.append(static_cast<size_t>(lay.cols - at), ' ');
    }

    std::vector<uint32_t> pixels(ras.alpha.size());
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = (m.rgb << 8) | ras.alpha[i];
    out.png = encodePngRgba(ras.w, ras.h, pixels);
    return true;
}

}  // namespace mermaid
