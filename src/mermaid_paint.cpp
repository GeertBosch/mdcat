// The shared painter of the built-in Mermaid renderers: a Scene (strokes, fills and text, all in
// cells and half-cell units) becomes the PNG and the text layer. It knows nothing of flowcharts or
// sequence diagrams, so each of them builds a Scene from its own layout and calls paintScene.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>

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
    // the base of an arrowhead, or the center of a circle or an X.
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
bool rasterize(const std::vector<Stroke>& strokes,
               const Metrics& m,
               Raster& ras,
               std::string& reason) {
    const int s = m.s, hs = m.hs;

    // The nodes that are not plain boxes: an edge meets their outline inside the footprint, at a
    // point that depends on where the edge runs, so the end of a run at such a node is found here.
    struct Shaped {
        shape::Geom geom;
        Pt tl, br;
        const Stroke* outline;
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
                              &st});
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
    // An outline's fill goes under its strokes, and under everything else.
    auto outlineColour = [&](const Stroke& st) {
        return st.colour >= 0 ? static_cast<uint32_t>(st.colour) : m.rgb;
    };
    for (const Stroke& st : strokes) {
        if (st.kind != Stroke::Outline || st.fill < 0) continue;
        ras.cur = static_cast<uint32_t>(st.fill);
        if (shape::isBox(st.shape)) {
            ras.rect(m.px(st.pts[0].first) - hs,
                     m.py(st.pts[0].second) - hs,
                     m.px(st.pts[1].first) - hs + s,
                     m.py(st.pts[1].second) - hs + s);
            continue;
        }
        for (const Shaped& n : shaped) {
            if (n.outline != &st) continue;
            long x0, y0, x1, y1;
            n.geom.bounds(x0, y0, x1, y1);
            ras.cover(
                x0, y0, x1, y1, [&](long x, long y) { return n.geom.in(x, y, n.geom.half()); });
        }
    }
    for (const Shaped& n : shaped) {
        long x0, y0, x1, y1;
        n.geom.bounds(x0, y0, x1, y1);
        ras.cur = outlineColour(*n.outline);
        ras.cover(x0, y0, x1, y1, [&](long x, long y) { return n.geom.ink(x, y); });
    }
    for (const Stroke& st : strokes) {
        const int w = m.width(st.line), hw = w / 2;
        ras.cur = st.kind == Stroke::Outline ? outlineColour(st) : m.rgb;
        if (st.kind == Stroke::Outline) {
            if (!shape::isBox(st.shape)) continue;
            int l = m.px(st.pts[0].first), t = m.py(st.pts[0].second);
            int r = m.px(st.pts[1].first), b = m.py(st.pts[1].second);
            // A subroutine has verticals half a cell inside both sides, a lined box one inside its
            // left side, and a divided one a horizontal a row below its top.
            if (st.shape == Shape::Subroutine || st.shape == Shape::LinedRect)
                ras.rect(l + m.cw / 2 - hs, t - hs, l + m.cw / 2 - hs + s, b - hs + s);
            if (st.shape == Shape::Subroutine)
                ras.rect(r - m.cw / 2 - hs, t - hs, r - m.cw / 2 - hs + s, b - hs + s);
            if (st.shape == Shape::DividedRect)
                ras.rect(l - hs, t + m.ch - hs, r - hs + s, t + m.ch - hs + s);
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
            // Where a stroke goes on beyond the center of the cell next to a label: to a quarter of
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

}  // namespace

bool paintScene(const Scene& scene, const Env& env, Diagram& out, std::string& reason) {
    const Metrics m(env);
    Raster ras(scene.cols * m.cw, scene.rows * m.ch, m.rgb);
    for (const Scene::Fill& f : scene.fills) {
        ras.cur = static_cast<uint32_t>(f.rgb);
        ras.rect(f.box.col * m.cw,
                 f.box.row * m.ch,
                 (f.box.col + f.box.cols) * m.cw,
                 (f.box.row + f.box.rows) * m.ch);
    }
    if (!rasterize(scene.strokes, m, ras, reason)) return false;
    // The image is drawn over the text, so the cells of a filled text must stay transparent for the
    // text to show. Their fill is the background colour of the text instead.
    for (const Scene::Text& t : scene.texts) {
        const CellBox& l = t.box;
        if (t.style.fill < 0 || l.cols <= 0) continue;
        for (int y = l.row * m.ch; y < (l.row + l.rows) * m.ch; ++y)
            for (int x = l.col * m.cw; x < (l.col + l.cols) * m.cw; ++x)
                ras.alpha[static_cast<size_t>(y) * static_cast<size_t>(ras.w) +
                          static_cast<size_t>(x)] = 0;
    }

    // The text layer: each text where the scene put it, spaces elsewhere.
    out.cols = scene.cols;
    out.rows = scene.rows;
    out.textRows.assign(static_cast<size_t>(scene.rows), std::string());
    struct Placed {
        int col, width;
        const std::string* text;
        int color;  // 0xRRGGBB for the text, or -1
    };
    std::vector<std::vector<Placed>> byRow(static_cast<size_t>(scene.rows));
    if (!scene.title.empty()) {  // bold, centred
        const int w = env.width(scene.title);
        out.textRows[0] = std::string(static_cast<size_t>((scene.cols - w) / 2), ' ') + "\033[1m" +
            scene.title + "\033[22m" +
            std::string(static_cast<size_t>(scene.cols - w - (scene.cols - w) / 2), ' ');
    }
    // A text with a fill has the whole of its box written, in the fill as background colour, with
    // U+00A0 for the blanks: plain spaces are skipped on output, which would leave holes in the
    // fill. Bold text is closed at the end of each line, and one that switches bold off in the
    // middle of a line (a Markdown `**x** y`) has it switched on again.
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
    for (const Scene::Text& t : scene.texts) {
        const CellBox& l = t.box;
        if (l.cols <= 0) continue;
        const bool filled = t.style.fill >= 0;
        const int n = filled ? l.rows : static_cast<int>(t.lines.size());
        for (int j = 0; j < n; ++j) {
            std::string line =
                j < static_cast<int>(t.lines.size()) ? t.lines[static_cast<size_t>(j)] : "";
            const int w = env.width(line);
            const int lead = t.align == Scene::Align::Left ? 0 : (l.cols - w) / 2;
            if (t.bold) {
                std::string bold = "\033[1m";
                for (size_t i = 0; i < line.size();) {
                    if (line.compare(i, 5, "\033[22m") == 0) {
                        bold += "\033[22m\033[1m";
                        i += 5;
                    } else {
                        bold += line[i++];
                    }
                }
                line = bold + "\033[22m";
            }
            if (!filled) {
                owned.push_back(line);
                byRow[static_cast<size_t>(l.row + j)].push_back(
                    {l.col + lead, w, &owned.back(), t.style.color});
                continue;
            }
            std::string text = sgr("48", t.style.fill);
            if (t.style.color >= 0) text += sgr("38", t.style.color);
            auto blanks = [&](int count) {
                for (int k = 0; k < count; ++k) text += "\xc2\xa0";
            };
            blanks(lead);
            for (char c : line) {
                if (c == ' ')
                    text += "\xc2\xa0";
                else
                    text += c;
            }
            blanks(l.cols - w - lead);
            text += "\033[49m";
            if (t.style.color >= 0) text += "\033[39m";
            owned.push_back(text);
            byRow[static_cast<size_t>(l.row + j)].push_back({l.col, l.cols, &owned.back(), -1});
        }
    }
    for (int r = 0; r < scene.rows; ++r) {
        std::vector<Placed>& texts = byRow[static_cast<size_t>(r)];
        std::sort(texts.begin(), texts.end(), [](const Placed& a, const Placed& b) {
            return a.col < b.col;
        });
        std::string& line = out.textRows[static_cast<size_t>(r)];
        int at = 0;
        for (const Placed& t : texts) {
            line.append(static_cast<size_t>(t.col - at), ' ');
            if (t.color >= 0) {
                char code[32];
                std::snprintf(code,
                              sizeof code,
                              "\033[38;2;%d;%d;%dm",
                              (t.color >> 16) & 255,
                              (t.color >> 8) & 255,
                              t.color & 255);
                line += code;
            }
            line += *t.text;
            if (t.color >= 0) line += "\033[39m";
            at = t.col + t.width;
        }
        line.append(static_cast<size_t>(scene.cols - at), ' ');
        // Markdown styling must not run on into the next row (or, if a text is cut short, the
        // next one), so a row with any ends by switching it off.
        if (line.find('\033') != std::string::npos) line += "\033[22;23;39;49m";
    }

    std::vector<uint32_t> pixels(ras.alpha.size());
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = (ras.rgb[i] << 8) | ras.alpha[i];
    out.png = encodePngRgba(ras.w, ras.h, pixels);
    return true;
}

}  // namespace mermaid
