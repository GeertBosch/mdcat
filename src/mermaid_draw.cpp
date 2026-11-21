#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>

#include "mermaid_model.h"
#include "mermaid_shape.h"
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
    int dash;      // length of a dash and of the gap after it, on a dotted link
    uint32_t rgb;

    explicit Metrics(const Env& env)
        : cw(std::max(1, env.cellW)),
          ch(std::max(1, env.cellH)),
          s(shape::strokeWidth(cw)),
          hs(s / 2),
          headLen(std::max(6, cw)),
          headHalf(std::max(3, (45 * cw + 50) / 100)),  // round(0.45 * cw)
          ri(std::max(2, std::min(cw / 2, ch / 4))),
          dash(3 * s),
          rgb(env.darkTheme ? 0xc8c8c8u : 0x505050u) {}

    // The stroke width of a link, and the arrowhead on it: thick links get a larger one.
    int width(Line l) const { return l == Line::Thick ? 2 * s : s; }
    int len(Line l) const { return l == Line::Thick ? (5 * headLen + 2) / 4 : headLen; }
    int half(Line l) const { return l == Line::Thick ? (5 * headHalf + 2) / 4 : headHalf; }
    int circleR() const { return 2 * s; }   // radius of a circle end
    int crossArm() const { return 3 * s; }  // reach of an X end, along each axis

    // How far a marker reaches along its link from the outline it touches to where the shaft stops:
    // the base of an arrowhead, or the centre of a circle or an X.
    int shaftStop(const Stroke& st) const {
        switch (st.kind) {
        case Stroke::Arrowhead: return len(st.line);
        case Stroke::CircleEnd: return circleR();
        case Stroke::CrossEnd: return crossArm();
        default: return -1;
        }
    }

    int px(int hu) const { return hu * cw / 2; }  // half-cell units -> pixels
    int py(int hu) const { return hu * ch / 2; }
};

// The raster: straight-alpha RGBA, the stroke colour unless a node's style says otherwise, with
// alpha 0 where nothing is drawn. Coordinates are in pixels; half-open ranges.
struct Raster {
    int w, h;
    std::vector<uint8_t> alpha;
    std::vector<uint32_t> rgb;  // per pixel, 0xRRGGBB
    uint32_t cur;               // the colour that plot paints in

    Raster(int w_, int h_, uint32_t colour)
        : w(w_),
          h(h_),
          alpha(static_cast<size_t>(w_) * static_cast<size_t>(h_), 0),
          rgb(alpha.size(), colour),
          cur(colour) {}

    // Cover the pixel by a/255 in the current colour. Over ink of the same colour that is the
    // larger coverage; over ink of another, it blends.
    void plot(int x, int y, int a) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
        const int p = alpha[i];
        if (p == 0 || rgb[i] == cur) {
            if (a > p) alpha[i] = static_cast<uint8_t>(a);
            rgb[i] = cur;
            return;
        }
        const int under = p * (255 - a) / 255, out = a + under;
        if (out == 0) return;
        uint32_t mixed = 0;
        for (int sh = 16; sh >= 0; sh -= 8) {
            const int c = static_cast<int>((cur >> sh) & 255) * a +
                static_cast<int>((rgb[i] >> sh) & 255) * under;
            mixed |= static_cast<uint32_t>((c + out / 2) / out) << sh;
        }
        rgb[i] = mixed;
        alpha[i] = static_cast<uint8_t>(out);
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

    // Paint the pixels in the box [x0, x1) x [y0, y1), in eighths, where `ink(x, y)`, a point in
    // eighths, holds; each pixel is sampled 4x4.
    template <class F>
    void cover(long x0, long y0, long x1, long y1, F ink) {
        for (long y = y0 / 8; y <= y1 / 8; ++y) {
            for (long x = x0 / 8; x <= x1 / 8; ++x) {
                int hits = 0;
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 4; ++i)
                        if (ink(8 * x + 2 * i + 1, 8 * y + 2 * j + 1)) ++hits;
                if (hits) plot(static_cast<int>(x), static_cast<int>(y), (hits * 255 + 8) / 16);
            }
        }
    }

    // A line from (ax8, ay8) to (bx8, by8), in eighths of a pixel, w8 eighths wide with round
    // ends; each pixel is sampled 4x4.
    void line(int ax8, int ay8, int bx8, int by8, int w8) {
        const long r2 = static_cast<long>(w8) * w8 / 4;
        const long dx = bx8 - ax8, dy = by8 - ay8, len2 = dx * dx + dy * dy;
        const int pad = w8 / 16 + 2;
        for (int y = std::min(ay8, by8) / 8 - pad; y <= std::max(ay8, by8) / 8 + pad; ++y) {
            for (int x = std::min(ax8, bx8) / 8 - pad; x <= std::max(ax8, bx8) / 8 + pad; ++x) {
                int hits = 0;
                for (int j = 0; j < 4; ++j) {
                    for (int i = 0; i < 4; ++i) {
                        long ex = 8L * x + 2 * i + 1 - ax8, ey = 8L * y + 2 * j + 1 - ay8;
                        long t = len2 == 0 ? 0 : std::clamp(ex * dx + ey * dy, 0L, len2);
                        // The point of the line nearest the sample, scaled by len2.
                        long fx = ex * len2 - t * dx, fy = ey * len2 - t * dy;
                        if (static_cast<double>(fx) * fx + static_cast<double>(fy) * fy <=
                            static_cast<double>(r2) * len2 * len2)
                            ++hits;
                    }
                }
                if (hits) plot(x, y, (hits * 255 + 8) / 16);
            }
        }
    }
};

// ---- Strokes -------------------------------------------------------------------------------

// Dotted runs that overlap or meet along a line, as the edges of one bus do, become one run, so
// that the dashes of the edges line up instead of filling each other's gaps.
void mergeDotted(std::vector<Stroke>& strokes) {
    auto fixedOf = [](const Stroke& st) {
        bool vertical = st.pts[0].first == st.pts[1].first;
        return std::make_pair(vertical, vertical ? st.pts[0].first : st.pts[0].second);
    };
    auto lowEnd = [](const Stroke& st) {  // index of the end with the smaller coordinate
        bool vertical = st.pts[0].first == st.pts[1].first;
        return (vertical ? st.pts[0].second < st.pts[1].second : st.pts[0].first < st.pts[1].first)
            ? 0
            : 1;
    };
    auto coord = [](const Stroke& st, size_t end) {
        bool vertical = st.pts[0].first == st.pts[1].first;
        return vertical ? st.pts[end].second : st.pts[end].first;
    };
    auto dotted = [](const Stroke& st) {
        return st.kind == Stroke::Segment && st.line == Line::Dotted;
    };
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < strokes.size() && !merged; ++i) {
            if (!dotted(strokes[i])) continue;
            for (size_t j = i + 1; j < strokes.size() && !merged; ++j) {
                if (!dotted(strokes[j]) || fixedOf(strokes[i]) != fixedOf(strokes[j])) continue;
                const Stroke &a = strokes[i], &b = strokes[j];
                size_t al = static_cast<size_t>(lowEnd(a)), bl = static_cast<size_t>(lowEnd(b));
                int alo = coord(a, al), ahi = coord(a, 1 - al);
                int blo = coord(b, bl), bhi = coord(b, 1 - bl);
                if (std::max(alo, blo) > std::min(ahi, bhi)) continue;
                // The merged run takes each end, and its rounding, from the stroke that reaches
                // farther, and either one's where they end together.
                auto roundOf = [](const Stroke& st, size_t end) { return (st.round >> end) & 1; };
                int loRound = alo < blo ? roundOf(a, al)
                    : blo < alo         ? roundOf(b, bl)
                                        : roundOf(a, al) | roundOf(b, bl);
                int hiRound = ahi > bhi ? roundOf(a, 1 - al)
                    : bhi > ahi         ? roundOf(b, 1 - bl)
                                        : roundOf(a, 1 - al) | roundOf(b, 1 - bl);
                Stroke u = a;
                u.pts[0] = alo <= blo ? a.pts[al] : b.pts[bl];
                u.pts[1] = ahi >= bhi ? a.pts[1 - al] : b.pts[1 - bl];
                u.round = static_cast<unsigned char>(loRound | (hiRound << 1));
                u.breaks.insert(u.breaks.end(), b.breaks.begin(), b.breaks.end());
                std::sort(u.breaks.begin(), u.breaks.end());
                u.breaks.erase(std::unique(u.breaks.begin(), u.breaks.end()), u.breaks.end());
                strokes[i] = u;
                strokes.erase(strokes.begin() + static_cast<long>(j));
                merged = true;
            }
        }
    }
}

std::vector<Stroke> strokesOf(const Graph& g, const Layout& lay, const std::vector<Pt>& crossings) {
    std::vector<Stroke> strokes;
    for (size_t i = 0; i < lay.nodeFoot.size(); ++i) {
        const CellBox& f = lay.nodeFoot[i];
        Stroke st;
        st.kind = Stroke::Outline;
        st.shape = g.nodes[i].shape;
        st.node = static_cast<int>(i);
        st.pts = {{2 * f.col + 1, 2 * f.row + 1},
                  {2 * (f.col + f.cols) - 1, 2 * (f.row + f.rows) - 1}};
        strokes.push_back(st);
    }

    // Where edges bend, and which bends are junctions. Edges that leave one port, or arrive at one,
    // share runs, so a bend of one edge can lie on another's straight run. A bend where exactly two
    // directions meet is a corner and is rounded. Where three or more meet, a bus splits or merges:
    // the ends stay square and a dot marks the junction.
    std::vector<std::pair<Pt, Pt>> runs;
    std::map<Pt, Line> bends;  // with the style of the edge that bends there
    for (size_t k = 0; k < lay.edgePath.size(); ++k) {
        const std::vector<Pt>& path = lay.edgePath[k];
        for (size_t i = 0; i + 1 < path.size(); ++i) runs.push_back({path[i], path[i + 1]});
        for (size_t i = 1; i + 1 < path.size(); ++i) bends.emplace(path[i], g.edges[k].line);
    }
    std::set<Pt> corners;
    for (const auto& [v, line] : bends) {
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
        st.line = line;
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
            const Pt a = path[i], b = path[i + 1];
            const unsigned char round =
                static_cast<unsigned char>((corners.count(a) ? 1 : 0) | (corners.count(b) ? 2 : 0));
            // A run that goes through the edge's label is two strokes, each stopping a little short
            // of the label: from a to the cell before it, and from the cell after it to b.
            struct Piece {
                Pt from, to;
                unsigned char round, reach;
            };
            std::vector<Piece> pieces = {{a, b, round, 0}};
            const CellBox& lb = lay.edgeLabel[k];
            const bool vertical = a.first == b.first;
            if (lb.cols > 0 && (vertical || a.second == b.second)) {
                const int lo = vertical ? std::min(a.second, b.second) : std::min(a.first, b.first);
                const int hi = vertical ? std::max(a.second, b.second) : std::max(a.first, b.first);
                const int line = vertical ? a.first : a.second;
                const int l0 = 2 * (vertical ? lb.row : lb.col);
                const int l1 = 2 * (vertical ? lb.row + lb.rows : lb.col + lb.cols);
                const int c0 = 2 * (vertical ? lb.col : lb.row);
                const int c1 = 2 * (vertical ? lb.col + lb.cols : lb.row + lb.rows);
                if (line > c0 && line < c1 && lo <= l0 && hi >= l1) {
                    auto at = [&](int along) {
                        return vertical ? Pt{line, along} : Pt{along, line};
                    };
                    const bool forward = vertical ? a.second < b.second : a.first < b.first;
                    const Pt before = at(l0 - 1), after = at(l1 + 1);
                    pieces = {
                        {a, forward ? before : after, static_cast<unsigned char>(round & 1), 2},
                        {forward ? after : before, b, static_cast<unsigned char>(round & 2), 1}};
                }
            }
            for (const Piece& pc : pieces) {
                Stroke st;
                st.kind = Stroke::Segment;
                st.line = g.edges[k].line;
                st.pts = {pc.from, pc.to};
                st.round = pc.round;
                st.reach = pc.reach;
                if (pc.from.second == pc.to.second) {
                    for (const Pt& x : crossings)
                        if (x.second == pc.from.second &&
                            x.first > std::min(pc.from.first, pc.to.first) &&
                            x.first < std::max(pc.from.first, pc.to.first))
                            st.breaks.push_back(x.first);
                }
                auto same = [&](const Stroke& o) { return o.kind == st.kind && o.pts == st.pts; };
                if (std::none_of(strokes.begin(), strokes.end(), same)) strokes.push_back(st);
            }
        }
        // The markers at the two ends of the edge, each pointing from the next point of the path
        // to the end.
        for (int atTail = 0; atTail < 2 && path.size() >= 2; ++atTail) {
            End end = atTail ? g.edges[k].tail : g.edges[k].head;
            if (end == End::None) continue;
            Stroke st;
            st.kind = end == End::Arrow ? Stroke::Arrowhead
                : end == End::Circle    ? Stroke::CircleEnd
                                        : Stroke::CrossEnd;
            st.line = g.edges[k].line;
            st.pts = atTail ? std::vector<Pt>{path.front(), path[1]}
                            : std::vector<Pt>{path.back(), path[path.size() - 2]};
            auto same = [&](const Stroke& o) { return o.kind == st.kind && o.pts[0] == st.pts[0]; };
            if (std::none_of(strokes.begin(), strokes.end(), same)) strokes.push_back(st);
        }
    }
    mergeDotted(strokes);
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
            r = std::min(r, farRounded ? (len - m.width(st.line)) / 2 : len);
            // A marker at the far end takes its length off the run, and the shaft must still reach
            // the arc (see how rasterize sets where a run begins and ends).
            if (farRounded) continue;
            const bool vertical = far.first == v.first;
            const bool farIsLow = vertical ? far.second < v.second : far.first < v.first;
            const int w = m.width(st.line), hw = w / 2;
            for (const Stroke& h : strokes) {
                if (m.shaftStop(h) < 0 || h.pts[0] != far) continue;
                if ((h.pts[0].first == h.pts[1].first) != vertical) continue;
                const int stop = m.shaftStop(h);
                r = std::min(r,
                             farIsLow ? len - stop - hw + m.hs - 2 * m.s
                                      : len - stop - m.hs + hw - w - m.s);
            }
        }
    }
    return r;
}

// Draw the strokes. Arrows run from one outline to the next; a shaft stops where its arrowhead
// begins, and the head's tip touches the target's outline.
bool rasterize(const Graph& g,
               const std::vector<Stroke>& strokes,
               const Metrics& m,
               Raster& ras,
               std::string& reason) {
    const int s = m.s, hs = m.hs;

    // The nodes that are not plain boxes: an edge meets their outline inside the footprint, at a
    // point that depends on where the edge runs, so the end of a run at such a node is found here.
    struct Shaped {
        shape::Geom geom;
        Pt tl, br;
        int node;
    };
    std::vector<Shaped> shaped;
    for (const Stroke& st : strokes)
        if (st.kind == Stroke::Outline && !shape::isBox(st.shape))
            shaped.push_back({shape::Geom(st.shape,
                                          m.px(st.pts[0].first),
                                          m.py(st.pts[0].second),
                                          m.px(st.pts[1].first),
                                          m.py(st.pts[1].second),
                                          m.cw,
                                          m.ch,
                                          m.s),
                              st.pts[0],
                              st.pts[1],
                              st.node});
    // Where the run through hu point p, along the vertical or horizontal, meets the silhouette of
    // the shaped node whose footprint's edge p is on, in eighths along the run. Gives 0 if p is
    // not on such a node, 1 if it is, and -1 if the run misses the shape.
    auto surfaceAt = [&](Pt p, bool vertical, long& at) {
        for (const Shaped& n : shaped) {
            const bool within = p.first >= n.tl.first && p.first <= n.br.first &&
                p.second >= n.tl.second && p.second <= n.br.second;
            const bool edge = p.first == n.tl.first || p.first == n.br.first ||
                p.second == n.tl.second || p.second == n.br.second;
            if (!within || !edge) continue;
            const long cross = 8L * ((vertical ? m.px(p.first) : m.py(p.second)) - hs) + 4L * s;
            long lo, hi;
            if (!n.geom.extent(vertical, cross, lo, hi)) return -1;
            const bool low = vertical ? 2 * p.second < n.tl.second + n.br.second
                                      : 2 * p.first < n.tl.first + n.br.first;
            at = low ? lo : hi;
            return 1;
        }
        return 0;
    };
    // A node's fill goes under its outline, and under everything else.
    auto outlineColour = [&](int node) {
        const int c = node < 0 ? -1 : g.nodes[static_cast<size_t>(node)].style.stroke;
        return c >= 0 ? static_cast<uint32_t>(c) : m.rgb;
    };
    for (const Stroke& st : strokes) {
        if (st.kind != Stroke::Outline) continue;
        const int fill = g.nodes[static_cast<size_t>(st.node)].style.fill;
        if (fill < 0) continue;
        ras.cur = static_cast<uint32_t>(fill);
        if (shape::isBox(st.shape)) {
            ras.rect(m.px(st.pts[0].first) - hs,
                     m.py(st.pts[0].second) - hs,
                     m.px(st.pts[1].first) - hs + s,
                     m.py(st.pts[1].second) - hs + s);
            continue;
        }
        for (const Shaped& n : shaped) {
            if (n.node != st.node) continue;
            long x0, y0, x1, y1;
            n.geom.bounds(x0, y0, x1, y1);
            ras.cover(
                x0, y0, x1, y1, [&](long x, long y) { return n.geom.in(x, y, n.geom.half()); });
        }
    }
    for (const Shaped& n : shaped) {
        long x0, y0, x1, y1;
        n.geom.bounds(x0, y0, x1, y1);
        ras.cur = outlineColour(n.node);
        ras.cover(x0, y0, x1, y1, [&](long x, long y) { return n.geom.ink(x, y); });
    }
    for (const Stroke& st : strokes) {
        const int w = m.width(st.line), hw = w / 2;
        ras.cur = st.kind == Stroke::Outline ? outlineColour(st.node) : m.rgb;
        if (st.kind == Stroke::Outline) {
            if (!shape::isBox(st.shape)) continue;
            int l = m.px(st.pts[0].first), t = m.py(st.pts[0].second);
            int r = m.px(st.pts[1].first), b = m.py(st.pts[1].second);
            if (st.shape == Shape::Subroutine) {  // verticals half a cell inside the sides
                ras.rect(l + m.cw / 2 - hs, t - hs, l + m.cw / 2 - hs + s, b - hs + s);
                ras.rect(r - m.cw / 2 - hs, t - hs, r - m.cw / 2 - hs + s, b - hs + s);
            }
            ras.rect(l - hs, t - hs, r - hs + s, t - hs + s);
            ras.rect(l - hs, b - hs, r - hs + s, b - hs + s);
            ras.rect(l - hs, t - hs, l - hs + s, b - hs + s);
            ras.rect(r - hs, t - hs, r - hs + s, b - hs + s);
        } else if (st.kind == Stroke::Segment) {
            Pt a = st.pts[0], b = st.pts[1];
            bool roundA = st.round & 1, roundB = st.round & 2;
            bool reachA = st.reach & 1, reachB = st.reach & 2;
            bool vertical = a.first == b.first;
            if (vertical ? b.second < a.second : b.first < a.first) {
                std::swap(a, b);
                std::swap(roundA, roundB);
                std::swap(reachA, reachB);
            }
            // How far the shaft stops short of this segment's end `p` because of a marker there:
            // the marker points along the segment, away from it (`forward`) or back toward the
            // other end. -1 if there is none.
            auto markerAt = [&](Pt p, bool forward) {
                for (const Stroke& h : strokes) {
                    if (m.shaftStop(h) < 0 || h.pts[0] != p) continue;
                    if ((h.pts[0].first == h.pts[1].first) != vertical) continue;
                    bool ahead = vertical ? h.pts[0].second > h.pts[1].second
                                          : h.pts[0].first > h.pts[1].first;
                    if (ahead == forward) return m.shaftStop(h);
                }
                return -1;
            };
            auto dotAt = [&](Pt p) {
                return std::any_of(strokes.begin(), strokes.end(), [&](const Stroke& d) {
                    return d.kind == Stroke::Dot && d.pts[0] == p;
                });
            };
            int stopB = markerAt(b, true), stopA = markerAt(a, false);
            long surfA = 0, surfB = 0;
            const int onA = surfaceAt(a, vertical, surfA), onB = surfaceAt(b, vertical, surfB);
            if (onA < 0 || onB < 0) {
                reason = "edge misses its node";
                return false;
            }
            int cross = vertical ? m.px(a.first) : m.py(a.second);
            int from = vertical ? m.py(a.second) : m.px(a.first);
            int to = vertical ? m.py(b.second) : m.px(b.first);
            // A run starts at its source outline's outer edge, or where a corner's arc ends. It
            // ends where its arrowhead begins, at a corner's arc, or at the far edge of the stroke;
            // at a junction dot it reaches across the other strokes.
            int roA = roundA ? cornerRadius(strokes, m, a) + w : 0;
            int roB = roundB ? cornerRadius(strokes, m, b) + w : 0;
            if ((roundA && roA <= w) || (roundB && roB <= w)) {
                reason = "segment too short";
                return false;
            }
            // Where a stroke goes on beyond the centre of the cell next to a label: to a quarter of
            // a cell's height short of the label.
            const int gap = std::max(0, (vertical ? m.ch : m.cw) / 2 - m.ch / 4);
            // A run that ends at a shaped node reaches its silhouette, wherever that is in the
            // footprint, and a marker stops short of it or the run goes on into the stroke.
            const int atA = static_cast<int>(surfA / 8), atB = static_cast<int>((surfB + 7) / 8);
            int begin = reachA ? from - gap
                : roundA       ? from - hw + roA
                : onA          ? (stopA >= 0 ? atA + stopA : atA - s)
                : stopA >= 0   ? from - hs + s + stopA
                : dotAt(a)     ? from - hw
                               : from - hs;
            int end = reachB ? to + gap
                : roundB     ? to - hw + w - roB
                : onB        ? (stopB >= 0 ? atB - stopB : atB + s)
                : stopB >= 0 ? to - hs - stopB
                : dotAt(b)   ? to - hw + w
                             : to - hs + s;
            if ((stopA >= 0 || stopB >= 0) && end < begin + s) {
                reason = "arrow too short";
                return false;
            }
            if (end < begin) {
                reason = "segment too short";
                return false;
            }
            // Paint the stretch [lo, hi) of the run. A horizontal run breaks for 2s px on either
            // side of each vertical stroke that crosses it.
            std::vector<int> breaks = st.breaks;
            std::sort(breaks.begin(), breaks.end());
            auto paint = [&](int lo, int hi) {
                if (vertical) {
                    ras.rect(cross - hw, lo, cross - hw + w, hi);
                    return;
                }
                int at = lo;
                for (int x : breaks) {
                    int l = m.px(x) - hs - 2 * s, r = m.px(x) - hs + 3 * s;
                    if (l > at) ras.rect(at, cross - hw, std::min(l, hi), cross - hw + w);
                    at = std::max(at, r);
                }
                if (hi > at) ras.rect(at, cross - hw, hi, cross - hw + w);
            };
            if (st.line == Line::Dotted) {
                // Dashes of m.dash, a first at the start of the run and a last at its end, with
                // gaps at least as long between them, so each straight run begins and ends on one.
                int run = end - begin, n = (run + m.dash) / (2 * m.dash);
                if (n < 2) {
                    paint(begin, end);
                } else {
                    for (int i = 0; i < n; ++i) {
                        int at = begin + i * (run - m.dash) / (n - 1);
                        paint(at, at + m.dash);
                    }
                }
            } else {
                paint(begin, end);
            }
        } else if (st.kind == Stroke::Corner) {
            Pt v = st.pts[0];
            int vdir = st.pts[1].second < v.second ? -1 : 1;
            int hdir = st.pts[2].first < v.first ? -1 : 1;
            int ri = cornerRadius(strokes, m, v), ro = ri + w;
            if (ri < 1) {
                reason = "segment too short";
                return false;
            }
            int xv = m.px(v.first) - hw, yh = m.py(v.second) - hw;
            int ox = hdir > 0 ? xv + ro : xv + w - ro;
            int oy = vdir > 0 ? yh + ro : yh + w - ro;
            ras.arc(ox, oy, -hdir, -vdir, ri, ro);
        } else if (st.kind == Stroke::Dot) {
            ras.disc(8 * (m.px(st.pts[0].first) - hw) + 4 * w,
                     8 * (m.py(st.pts[0].second) - hw) + 4 * w,
                     12 * w);
        } else if (m.shaftStop(st) >= 0) {
            Pt tip = st.pts[0], base = st.pts[1];
            bool vertical = tip.first == base.first;
            int sign = (vertical ? tip.second > base.second : tip.first > base.first) ? 1 : -1;
            // The tip touches the outer edge of the outline stroke of the node it points at.
            int x = 8 * (m.px(tip.first) - hs), y = 8 * (m.py(tip.second) - hs);
            if (vertical) {
                x += 4 * s;
                if (sign < 0) y += 8 * s;
            } else {
                y += 4 * s;
                if (sign < 0) x += 8 * s;
            }
            int ux = vertical ? 0 : sign, uy = vertical ? sign : 0;
            long surf = 0;
            const int onNode = surfaceAt(tip, vertical, surf);
            if (onNode < 0) {
                reason = "edge misses its node";
                return false;
            }
            if (onNode > 0) (vertical ? y : x) = static_cast<int>(surf);
            if (st.kind == Stroke::Arrowhead) {
                ras.arrowhead(x, y, ux, uy, m.len(st.line), m.half(st.line));
            } else if (st.kind == Stroke::CircleEnd) {
                ras.disc(x - 8 * ux * m.circleR(), y - 8 * uy * m.circleR(), 8 * m.circleR());
            } else {
                int cx = x - 8 * ux * m.crossArm(), cy = y - 8 * uy * m.crossArm();
                int arm = 8 * m.crossArm();
                ras.line(cx - arm, cy - arm, cx + arm, cy + arm, 8 * s);
                ras.line(cx - arm, cy + arm, cx + arm, cy - arm, 8 * s);
            }
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
    // No edge passes through a node or label, except at its own endpoints, and through its own
    // label.
    for (size_t k = 0; k < g.edges.size(); ++k) {
        const CellBox& a = lay.nodeFoot[static_cast<size_t>(g.edges[k].from)];
        const CellBox& b = lay.nodeFoot[static_cast<size_t>(g.edges[k].to)];
        const CellBox& own = lay.edgeLabel[k];
        const std::vector<Pt>& path = lay.edgePath[k];
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            bool ok = true;
            segmentCells(path[i], path[i + 1], [&](int c, int r) {
                Occ o = grid.get(c, r);
                if ((o == Occ::Label && !inBox(own, c, r)) ||
                    (o == Occ::Node && !inBox(a, c, r) && !inBox(b, c, r)))
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
    // Every marker touches the outline of the node it is at.
    for (size_t k = 0; k < g.edges.size(); ++k) {
        if (lay.edgePath[k].empty()) continue;
        if (g.edges[k].head != End::None &&
            !onOutline(lay.nodeFoot[static_cast<size_t>(g.edges[k].to)], lay.edgePath[k].back())) {
            reason = "arrowhead misses its target";
            return false;
        }
        if (g.edges[k].tail != End::None &&
            !onOutline(lay.nodeFoot[static_cast<size_t>(g.edges[k].from)],
                       lay.edgePath[k].front())) {
            reason = "arrowhead misses its source";
            return false;
        }
    }
    // No stroke falls inside a label cell. A marker reaches back one cell from its tip.
    bool ok = true;
    auto check = [&](int c, int r) {
        if (grid.get(c, r) == Occ::Label) ok = false;
    };
    for (const Stroke& st : strokes) {
        if (st.kind == Stroke::Outline) {
            outlineCells(st.pts[0], st.pts[1], check);
        } else if (st.kind == Stroke::Segment) {
            segmentCells(st.pts[0], st.pts[1], check);
        } else if (st.kind == Stroke::Arrowhead || st.kind == Stroke::CircleEnd ||
                   st.kind == Stroke::CrossEnd) {
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
    for (const CellBox& l : lay.edgeLabel) {
        for (int r = l.row; r < l.row + l.rows; ++r) {
            for (int c = l.col; c < l.col + l.cols; ++c) {
                if (grid.get(c, r) != Occ::Empty) {
                    reason = "overlap";
                    return false;
                }
                grid.at(c, r) = Occ::Label;
            }
        }
    }
    for (const Stroke& st : strokes) {
        if (st.kind != Stroke::Segment) continue;
        segmentCells(st.pts[0], st.pts[1], [&](int c, int r) {
            if (grid.get(c, r) == Occ::Empty) grid.at(c, r) = Occ::Edge;
        });
    }

    if (!validate(g, lay, strokes, grid, crossings, reason)) return false;

    Raster ras(lay.cols * m.cw, lay.rows * m.ch, m.rgb);
    if (!rasterize(g, strokes, m, ras, reason)) return false;
    // The image is drawn over the text, so a node's label cells must stay transparent for the text
    // to show. Their fill is the background colour of the text instead (see addLines).
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        const CellBox& l = lay.nodeLabel[i];
        if (env.underText || g.nodes[i].style.fill < 0 || l.cols <= 0) continue;
        for (int y = l.row * m.ch; y < (l.row + l.rows) * m.ch; ++y)
            for (int x = l.col * m.cw; x < (l.col + l.cols) * m.cw; ++x)
                ras.alpha[static_cast<size_t>(y) * static_cast<size_t>(ras.w) +
                          static_cast<size_t>(x)] = 0;
    }

    // The text layer: node labels on each node's middle row, edge labels where the layout put them,
    // spaces elsewhere.
    out.cols = lay.cols;
    out.rows = lay.rows;
    out.textRows.assign(static_cast<size_t>(lay.rows), std::string());
    struct Text {
        int col, width;
        const std::string* text;
        int color;  // 0xRRGGBB for the text, or -1
    };
    std::vector<std::vector<Text>> byRow(static_cast<size_t>(lay.rows));
    if (lay.titleRows > 0) {  // bold, centred
        int w = env.width(g.title);
        out.textRows[0] = std::string(static_cast<size_t>((lay.cols - w) / 2), ' ') + "\033[1m" +
            g.title + "\033[22m" +
            std::string(static_cast<size_t>(lay.cols - w - (lay.cols - w) / 2), ' ');
    }
    // Each line of a label is centred in the label's cells.
    // A node with a fill has the whole of its label cells written, in the fill as background
    // colour, with U+00A0 for the blanks: plain spaces are skipped on output, which would leave
    // holes in the fill.
    std::deque<std::string> owned;
    auto sgr = [](const char* code, int rgb) {
        char b[32];
        std::snprintf(b,
                      sizeof b,
                      "\033[%s;2;%d;%d;%dm",
                      code,
                      (rgb >> 16) & 255,
                      (rgb >> 8) & 255,
                      rgb & 255);
        return std::string(b);
    };
    const std::string none;
    // `margin` cells of the same background go on each side of a filled label, under the image's
    // fill: iTerm shows the terminal's own background at the edge of the transparent label cells
    // when the image sits half a cell off the text grid.
    auto addLines =
        [&](const CellBox& l, const std::vector<std::string>& lines, const Style& st, int margin) {
            if (l.cols <= 0) return;
            const bool filled = st.fill >= 0 && !env.underText;
            const int n = filled ? l.rows : static_cast<int>(lines.size());
            for (int j = 0; j < n; ++j) {
                const std::string& line =
                    j < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(j)] : none;
                const int w = env.width(line);
                if (!filled) {
                    byRow[static_cast<size_t>(l.row + j)].push_back(
                        {l.col + (l.cols - w) / 2, w, &line, st.color});
                    continue;
                }
                std::string text = sgr("48", st.fill);
                if (st.color >= 0) text += sgr("38", st.color);
                auto blanks = [&](int count) {
                    for (int k = 0; k < count; ++k) text += "\xc2\xa0";
                };
                blanks(margin + (l.cols - w) / 2);
                for (char c : line) {
                    if (c == ' ')
                        text += "\xc2\xa0";
                    else
                        text += c;
                }
                blanks(margin + l.cols - w - (l.cols - w) / 2);
                text += "\033[49m";
                if (st.color >= 0) text += "\033[39m";
                owned.push_back(text);
                byRow[static_cast<size_t>(l.row + j)].push_back(
                    {l.col - margin, l.cols + 2 * margin, &owned.back(), -1});
            }
        };
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        // Only where the cell beside the label is inside the outline, on every side: a box, or a
        // shape whose silhouette fills its footprint's rows.
        const CellBox &l = lay.nodeLabel[i], &f = lay.nodeFoot[i];
        const Shape sh = g.nodes[i].shape;
        const bool boxy = sh == Shape::Rect || sh == Shape::Round || sh == Shape::Stadium ||
            sh == Shape::Subroutine;
        const int margin =
            boxy && l.col - 1 >= f.col + 1 && l.col + l.cols <= f.col + f.cols - 2 ? 1 : 0;
        addLines(lay.nodeLabel[i], g.nodes[i].label.lines, g.nodes[i].style, margin);
    }
    for (size_t k = 0; k < g.edges.size(); ++k)
        addLines(lay.edgeLabel[k], g.edges[k].label.lines, Style(), 0);
    for (int r = 0; r < lay.rows; ++r) {
        std::vector<Text>& texts = byRow[static_cast<size_t>(r)];
        std::sort(
            texts.begin(), texts.end(), [](const Text& a, const Text& b) { return a.col < b.col; });
        std::string& line = out.textRows[static_cast<size_t>(r)];
        int at = 0;
        for (const Text& t : texts) {
            line.append(static_cast<size_t>(t.col - at), ' ');
            if (t.color >= 0) {
                char sgr[32];
                std::snprintf(sgr,
                              sizeof sgr,
                              "\033[38;2;%d;%d;%dm",
                              (t.color >> 16) & 255,
                              (t.color >> 8) & 255,
                              t.color & 255);
                line += sgr;
            }
            line += *t.text;
            if (t.color >= 0) line += "\033[39m";
            at = t.col + t.width;
        }
        line.append(static_cast<size_t>(lay.cols - at), ' ');
        // Markdown styling must not run on into the next row (or, if a label is cut short, the
        // next label), so a row with any ends by switching it off.
        if (line.find('\033') != std::string::npos) line += "\033[22;23;39;49m";
    }

    std::vector<uint32_t> pixels(ras.alpha.size());
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = (ras.rgb[i] << 8) | ras.alpha[i];
    out.png = encodePngRgba(ras.w, ras.h, pixels);
    return true;
}

}  // namespace mermaid
