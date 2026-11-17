#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <set>

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
    int ri;        // inner radius of a rounded corner, at most
    uint32_t rgb;

    explicit Metrics(const Env& env)
        : cw(std::max(1, env.cellW)),
          ch(std::max(1, env.cellH)),
          s(std::max(1, (2 * cw + 9) / 18)),
          hs(s / 2),
          headLen(std::max(6, cw)),
          headHalf(std::max(3, (45 * cw + 50) / 100)),  // round(0.45 * cw)
          ri(std::max(2, std::min(cw / 2, ch / 4))),
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

    // The part of a ring around the pixel corner (ox, oy), between radii ri and ro px, that lies
    // toward (qx, qy), each +1 or -1. Each pixel is sampled 4x4.
    void arc(int ox, int oy, int qx, int qy, int ri, int ro) {
        for (int y = oy - ro - 1; y < oy + ro + 2; ++y) {
            for (int x = ox - ro - 1; x < ox + ro + 2; ++x) {
                int hits = 0;
                for (int j = 0; j < 4; ++j) {
                    for (int i = 0; i < 4; ++i) {
                        long dx = 8L * x + 2 * i + 1 - 8L * ox, dy = 8L * y + 2 * j + 1 - 8L * oy;
                        long d2 = dx * dx + dy * dy;
                        if (dx * qx > 0 && dy * qy > 0 && d2 >= 64L * ri * ri &&
                            d2 <= 64L * ro * ro)
                            ++hits;
                    }
                }
                if (hits) plot(x, y, (hits * 255 + 8) / 16);
            }
        }
    }

    // A filled disc centred at (cx8, cy8), in eighths of a pixel, of radius r8 eighths.
    void disc(int cx8, int cy8, int r8) {
        for (int y = cy8 / 8 - r8 / 8 - 1; y <= cy8 / 8 + r8 / 8 + 1; ++y) {
            for (int x = cx8 / 8 - r8 / 8 - 1; x <= cx8 / 8 + r8 / 8 + 1; ++x) {
                int hits = 0;
                for (int j = 0; j < 4; ++j) {
                    for (int i = 0; i < 4; ++i) {
                        long dx = 8L * x + 2 * i + 1 - cx8, dy = 8L * y + 2 * j + 1 - cy8;
                        if (dx * dx + dy * dy <= static_cast<long>(r8) * r8) ++hits;
                    }
                }
                if (hits) plot(x, y, (hits * 255 + 8) / 16);
            }
        }
    }

    // A filled triangle with its tip at (tipX8, tipY8), in eighths of a pixel, pointing the way
    // (ux, uy) says: one of them is +1 or -1 and the other 0. Its base lies `len` px behind the tip
    // and is 2*half px wide; each pixel is sampled 4x4.
    void arrowhead(int tipX8, int tipY8, int ux, int uy, int len, int half) {
        int span = len + half + 2;
        int cx = tipX8 / 8, cy = tipY8 / 8;
        for (int y = cy - span; y < cy + span + 2; ++y) {
            for (int x = cx - span; x < cx + span + 2; ++x) {
                int hits = 0;
                for (int j = 0; j < 4; ++j) {
                    for (int i = 0; i < 4; ++i) {
                        int ex = 8 * x + 2 * i + 1, ey = 8 * y + 2 * j + 1;
                        int depth = (tipX8 - ex) * ux + (tipY8 - ey) * uy;
                        int cross = std::abs(ux ? ey - tipY8 : ex - tipX8);
                        if (depth > 0 && depth <= len * 8 && cross * len <= half * depth) ++hits;
                    }
                }
                if (hits) plot(x, y, (hits * 255 + 8) / 16);
            }
        }
    }
};

// ---- Strokes -------------------------------------------------------------------------------

std::vector<Stroke> strokesOf(const Graph& g, const Layout& lay, const std::vector<Pt>& crossings) {
    std::vector<Stroke> strokes;
    for (const CellBox& f : lay.nodeFoot) {
        Stroke st;
        st.kind = Stroke::Outline;
        st.pts = {{2 * f.col + 1, 2 * f.row + 1},
                  {2 * (f.col + f.cols) - 1, 2 * (f.row + f.rows) - 1}};
        strokes.push_back(st);
    }

    // Where edges bend, and which bends are junctions. Edges that leave one port, or arrive at one,
    // share runs, so a bend of one edge can lie on another's straight run. A bend where exactly two
    // directions meet is a corner and is rounded. Where three or more meet, a bus splits or merges:
    // the ends stay square and a dot marks the junction.
    std::vector<std::pair<Pt, Pt>> runs;
    std::set<Pt> bends;
    for (const std::vector<Pt>& path : lay.edgePath) {
        for (size_t i = 0; i + 1 < path.size(); ++i) runs.push_back({path[i], path[i + 1]});
        for (size_t i = 1; i + 1 < path.size(); ++i) bends.insert(path[i]);
    }
    std::set<Pt> corners;
    for (const Pt& v : bends) {
        std::array<bool, 4> dir{};  // up, down, left, right
        for (const auto& run : runs) {
            Pt a = run.first, b = run.second;
            bool vertical = a.first == b.first;
            int fixed = vertical ? a.first : a.second, at = vertical ? v.first : v.second;
            int lo = vertical ? std::min(a.second, b.second) : std::min(a.first, b.first);
            int hi = vertical ? std::max(a.second, b.second) : std::max(a.first, b.first);
            int pos = vertical ? v.second : v.first;
            if (fixed != at || pos < lo || pos > hi) continue;
            if (pos > lo) dir[vertical ? 0 : 2] = true;
            if (pos < hi) dir[vertical ? 1 : 3] = true;
        }
        int count = static_cast<int>(std::count(dir.begin(), dir.end(), true));
        Stroke st;
        if (count >= 3) {
            st.kind = Stroke::Dot;
            st.pts = {v};
        } else {
            st.kind = Stroke::Corner;
            st.pts = {v,
                      {v.first, v.second + (dir[0] ? -1 : 1)},
                      {v.first + (dir[2] ? -1 : 1), v.second}};
            corners.insert(v);
        }
        strokes.push_back(st);
    }

    for (size_t k = 0; k < g.edges.size(); ++k) {
        const std::vector<Pt>& path = lay.edgePath[k];
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            Stroke st;
            st.kind = Stroke::Segment;
            st.line = g.edges[k].line;
            st.pts = {path[i], path[i + 1]};
            st.round = static_cast<unsigned char>((corners.count(path[i]) ? 1 : 0) |
                                                  (corners.count(path[i + 1]) ? 2 : 0));
            if (path[i].second == path[i + 1].second) {
                for (const Pt& x : crossings)
                    if (x.second == path[i].second &&
                        x.first > std::min(path[i].first, path[i + 1].first) &&
                        x.first < std::max(path[i].first, path[i + 1].first))
                        st.breaks.push_back(x.first);
            }
            auto same = [&](const Stroke& o) { return o.kind == st.kind && o.pts == st.pts; };
            if (std::none_of(strokes.begin(), strokes.end(), same)) strokes.push_back(st);
        }
        if (g.edges[k].head == End::Arrow && path.size() >= 2) {
            Stroke st;
            st.kind = Stroke::Arrowhead;
            st.line = g.edges[k].line;
            st.pts = {path.back(), path[path.size() - 2]};
            auto same = [&](const Stroke& o) { return o.kind == st.kind && o.pts[0] == st.pts[0]; };
            if (std::none_of(strokes.begin(), strokes.end(), same)) strokes.push_back(st);
        }
    }
    return strokes;
}

// ---- Rasterizer ----------------------------------------------------------------------------

// The inner radius of the rounded corner at v: the usual one, made smaller where a run between two
// corners is too short for both arcs.
int cornerRadius(const std::vector<Stroke>& strokes, const Metrics& m, Pt v) {
    int r = m.ri;
    for (const Stroke& st : strokes) {
        if (st.kind != Stroke::Segment) continue;
        for (int end = 0; end < 2; ++end) {
            if (st.pts[static_cast<size_t>(end)] != v || !(st.round & (1 << end))) continue;
            Pt far = st.pts[static_cast<size_t>(1 - end)];
            bool farRounded = st.round & (2 >> end);
            int len = std::abs(m.px(far.first) - m.px(v.first)) +
                std::abs(m.py(far.second) - m.py(v.second));
            r = std::min(r, farRounded ? (len - m.s) / 2 : len);
        }
    }
    return r;
}

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
            bool roundA = st.round & 1, roundB = st.round & 2;
            bool vertical = a.first == b.first;
            if (vertical ? b.second < a.second : b.first < a.first) {
                std::swap(a, b);
                std::swap(roundA, roundB);
            }
            // An arrowhead on this segment's end `p`: it points along the segment, away from it
            // (`forward`) or back toward the other end.
            auto headAt = [&](Pt p, bool forward) {
                return std::any_of(strokes.begin(), strokes.end(), [&](const Stroke& h) {
                    if (h.kind != Stroke::Arrowhead || h.pts[0] != p) return false;
                    if ((h.pts[0].first == h.pts[1].first) != vertical) return false;
                    bool ahead = vertical ? h.pts[0].second > h.pts[1].second
                                          : h.pts[0].first > h.pts[1].first;
                    return ahead == forward;
                });
            };
            bool tipped = headAt(b, true), tippedBack = headAt(a, false);
            int cross = vertical ? m.px(a.first) : m.py(a.second);
            int from = vertical ? m.py(a.second) : m.px(a.first);
            int to = vertical ? m.py(b.second) : m.px(b.first);
            // A run starts at its source outline's outer edge, or where a corner's arc ends. It
            // ends where its arrowhead begins, at a corner's arc, or at the far edge of the stroke.
            int roA = roundA ? cornerRadius(strokes, m, a) + s : 0;
            int roB = roundB ? cornerRadius(strokes, m, b) + s : 0;
            if ((roundA && roA <= s) || (roundB && roB <= s)) {
                reason = "segment too short";
                return false;
            }
            int begin = roundA ? from - hs + roA
                : tippedBack   ? from - hs + s + m.headLen
                               : from - hs;
            int end = roundB ? to - hs + s - roB : tipped ? to - hs - m.headLen : to - hs + s;
            if ((tipped || tippedBack) && end < begin + s) {
                reason = "arrow too short";
                return false;
            }
            if (end < begin) {
                reason = "segment too short";
                return false;
            }
            if (vertical) {
                ras.rect(cross - hs, begin, cross - hs + s, end);
            } else {
                // The run breaks for 2s px on either side of each vertical stroke that crosses it.
                std::vector<int> breaks = st.breaks;
                std::sort(breaks.begin(), breaks.end());
                int at = begin;
                for (int x : breaks) {
                    int l = m.px(x) - hs - 2 * s, r = m.px(x) - hs + 3 * s;
                    if (l > at) ras.rect(at, cross - hs, l, cross - hs + s);
                    at = std::max(at, r);
                }
                if (end > at) ras.rect(at, cross - hs, end, cross - hs + s);
            }
        } else if (st.kind == Stroke::Corner) {
            Pt v = st.pts[0];
            int vdir = st.pts[1].second < v.second ? -1 : 1;
            int hdir = st.pts[2].first < v.first ? -1 : 1;
            int ri = cornerRadius(strokes, m, v), ro = ri + s;
            if (ri < 1) {
                reason = "segment too short";
                return false;
            }
            int xv = m.px(v.first) - hs, yh = m.py(v.second) - hs;
            int ox = hdir > 0 ? xv + ro : xv + s - ro;
            int oy = vdir > 0 ? yh + ro : yh + s - ro;
            ras.arc(ox, oy, -hdir, -vdir, ri, ro);
        } else if (st.kind == Stroke::Dot) {
            ras.disc(8 * (m.px(st.pts[0].first) - hs) + 4 * s,
                     8 * (m.py(st.pts[0].second) - hs) + 4 * s,
                     12 * s);
        } else if (st.kind == Stroke::Arrowhead) {
            Pt tip = st.pts[0], base = st.pts[1];
            bool vertical = tip.first == base.first;
            int sign = (vertical ? tip.second > base.second : tip.first > base.first) ? 1 : -1;
            // The tip touches the outer edge of the target's outline stroke.
            int x = 8 * (m.px(tip.first) - hs), y = 8 * (m.py(tip.second) - hs);
            if (vertical) {
                x += 4 * s;
                if (sign < 0) y += 8 * s;
            } else {
                y += 4 * s;
                if (sign < 0) x += 8 * s;
            }
            ras.arrowhead(x, y, vertical ? 0 : sign, vertical ? sign : 0, m.headLen, m.headHalf);
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

// Whether two perpendicular axis-aligned segments touch or cross, and where.
bool meet(Pt a0, Pt a1, Pt b0, Pt b1, Pt& at) {
    bool aVert = a0.first == a1.first;
    if (aVert == (b0.first == b1.first)) return false;
    if (!aVert) {
        std::swap(a0, b0);
        std::swap(a1, b1);
    }
    int x = a0.first, y = b0.second;
    at = {x, y};
    return std::min(a0.second, a1.second) <= y && y <= std::max(a0.second, a1.second) &&
        std::min(b0.first, b1.first) <= x && x <= std::max(b0.first, b1.first);
}

// Turn layout bugs into fallbacks instead of misleading pictures. Reads the occupancy grid and the
// stroke list, not the pixels.
bool validate(const Graph& g,
              const Layout& lay,
              const std::vector<Stroke>& strokes,
              const Grid& grid,
              const std::vector<Pt>& crossings,
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
    // No two edges of different groups share a segment or touch, or the picture would join edges
    // the graph does not. They may cross, where one of them bridges the other.
    for (size_t j = 0; j < g.edges.size(); ++j) {
        for (size_t k = j + 1; k < g.edges.size(); ++k) {
            if (lay.edgeGroup[j] == lay.edgeGroup[k]) continue;
            const std::vector<Pt>&pj = lay.edgePath[j], &pk = lay.edgePath[k];
            for (size_t a = 0; a + 1 < pj.size(); ++a) {
                for (size_t b = 0; b + 1 < pk.size(); ++b) {
                    if (overlap(pj[a], pj[a + 1], pk[b], pk[b + 1])) {
                        reason = "edges share a segment";
                        return false;
                    }
                    Pt at;
                    if (meet(pj[a], pj[a + 1], pk[b], pk[b + 1], at) &&
                        !std::binary_search(crossings.begin(), crossings.end(), at)) {
                        reason = "edges touch";
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
            bool vertical = tip.first == base.first;
            int sign = (vertical ? tip.second > base.second : tip.first > base.first) ? 1 : -1;
            Pt back = vertical ? Pt{tip.first, tip.second - 2 * sign}
                               : Pt{tip.first - 2 * sign, tip.second};
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
    const std::vector<Pt> crossings = crossingPoints(lay);
    std::vector<Stroke> strokes = strokesOf(g, lay, crossings);

    Grid grid(lay.cols, lay.rows);
    for (const CellBox& f : lay.nodeFoot)
        for (int r = f.row; r < f.row + f.rows; ++r)
            for (int c = f.col; c < f.col + f.cols; ++c) grid.at(c, r) = Occ::Node;
    if (lay.titleRows > 0)
        for (int c = 0; c < lay.cols; ++c) grid.at(c, 0) = Occ::Label;
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

    if (!validate(g, lay, strokes, grid, crossings, reason)) return false;

    Raster ras(lay.cols * m.cw, lay.rows * m.ch);
    if (!rasterize(strokes, m, ras, reason)) return false;

    // The text layer: labels on each node's middle row, spaces elsewhere.
    out.cols = lay.cols;
    out.rows = lay.rows;
    out.textRows.assign(static_cast<size_t>(lay.rows), std::string());
    std::vector<std::vector<size_t>> byRow(static_cast<size_t>(lay.rows));
    if (lay.titleRows > 0) {  // bold, centred
        int w = env.width(g.title);
        out.textRows[0] = std::string(static_cast<size_t>((lay.cols - w) / 2), ' ') + "\033[1m" +
            g.title + "\033[22m" +
            std::string(static_cast<size_t>(lay.cols - w - (lay.cols - w) / 2), ' ');
    }
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
