// The geometry of node shapes, shared by the layout (which sizes a footprint so that its label fits
// inside the outline) and the drawer (which strokes the outline and finds where an edge meets it).
// Everything is integer arithmetic in eighths of a pixel, so the picture does not depend on the
// compiler's floating point.
#pragma once

#include <algorithm>
#include <cstdlib>

#include "mermaid_model.h"

namespace mermaid {
namespace shape {

// The width of a stroke in pixels, for cells cw pixels wide.
inline int strokeWidth(int cw) {
    return std::max(1, (2 * cw + 9) / 18);
}

// Whether the shape's outline is the border of its footprint, so that an edge meets it at the
// footprint's edge and needs no geometry to find the point.
inline bool isBox(Shape s) {
    return s == Shape::Rect || s == Shape::Subroutine;
}

class Geom {
public:
    // The outline's box (the top-left and bottom-right of the strokes' centre lines, as the
    // drawer's Stroke::Outline has them) in pixels, for cells cw x ch pixels and strokes s wide.
    Geom(Shape sh, int left, int top, int right, int bottom, int cw, int ch, int s)
        : shape(sh),
          l(8L * (left - s / 2) + 4L * s),
          t(8L * (top - s / 2) + 4L * s),
          r(8L * (right - s / 2) + 4L * s),
          b(8L * (bottom - s / 2) + 4L * s),
          hw(4L * s),
          gap(16L * s),
          cap(4L * ch) {
        const long w = r - l, h = b - t;
        rad = sh == Shape::Stadium ? h / 2 : std::min(4L * ch, std::min(w, h) / 2);
        cap = std::min(cap, h / 4);
        point = std::min(8L * ch, w / 4);
        slant = std::min(8L * cw, w / 4);
    }

    long half() const { return hw; }

    // Whether (x, y), in eighths, is inside the shape's centre line grown by g (shrunk if g is
    // negative).
    bool in(long x, long y, long g) const {
        const long cx = (l + r) / 2, cy = (t + b) / 2;
        switch (shape) {
        case Shape::Rect:
        case Shape::Subroutine: return x >= l - g && x <= r + g && y >= t - g && y <= b + g;
        case Shape::Round:
        case Shape::Stadium: return rounded(x, y, g);
        case Shape::Circle:
        case Shape::DoubleCircle: return ellipse(x, y, cx, cy, (r - l) / 2 + g, (b - t) / 2 + g);
        case Shape::Cylinder:
            return (x >= l - g && x <= r + g && y >= t + cap && y <= b - cap) ||
                ellipse(x, y, cx, t + cap, (r - l) / 2 + g, cap + g) ||
                ellipse(x, y, cx, b - cap, (r - l) / 2 + g, cap + g);
        case Shape::Odd: {
            // The box less a wedge that opens on its left side; the wedge is cut off far enough
            // to the left that it never ends inside the grown box.
            if (!(x >= l - g && x <= r + g && y >= t - g && y <= b + g)) return false;
            long wx[3], wy[3];
            wedge(wx, wy);
            return !polygonIn(wx, wy, 3, x, y, -g);
        }
        default: break;
        }
        long px[6], py[6];
        const int n = polygon(px, py);
        return n > 0 && polygonIn(px, py, n, x, y, g);
    }

    // Whether (x, y) is on the shape's strokes.
    bool ink(long x, long y) const {
        if (in(x, y, hw) && !in(x, y, -hw)) return true;
        const long cx = (l + r) / 2, cy = (t + b) / 2;
        switch (shape) {
        case Shape::DoubleCircle: {
            const long a = (r - l) / 2 - gap, c = (b - t) / 2 - gap;
            return ellipse(x, y, cx, cy, a + hw, c + hw) && !ellipse(x, y, cx, cy, a - hw, c - hw);
        }
        case Shape::Cylinder: {  // the front edge of the lid
            const long a = (r - l) / 2;
            return y >= t + cap && ellipse(x, y, cx, t + cap, a + hw, cap + hw) &&
                !ellipse(x, y, cx, t + cap, a - hw, cap - hw);
        }
        default: return false;
        }
    }

    // Where the line across the shape through `cross` (an x if `vertical`, else a y) enters and
    // leaves its outer silhouette, along the line. False if the line misses the shape.
    bool extent(bool vertical, long cross, long& lo, long& hi) const {
        auto at = [&](long e) { return vertical ? in(cross, e, hw) : in(e, cross, hw); };
        const long c = vertical ? (t + b) / 2 : (l + r) / 2;
        const long span = (vertical ? b - t : r - l) / 2 + hw + 16;
        const long step = 8;
        // Walk in from outside until the line is inside the shape, then bisect for the edge. The
        // line may miss the middle of a slanted or notched shape and still cross it nearer an end.
        auto edge = [&](long from, long dir, long& found) {
            long out = from;
            for (long d = 0; d <= 2 * span; d += step) {
                const long e = from + dir * d;
                if (at(e)) {
                    long inside = e;
                    while (std::labs(inside - out) > 1) {
                        const long m = out + (inside - out) / 2;
                        (at(m) ? inside : out) = m;
                    }
                    found = inside;
                    return true;
                }
                out = e;
            }
            return false;
        };
        return edge(c - span, 1, lo) && edge(c + span, -1, hi);
    }

    // The bounding box of the strokes, in eighths.
    void bounds(long& x0, long& y0, long& x1, long& y1) const {
        x0 = l - hw - 8;
        y0 = t - hw - 8;
        x1 = r + hw + 8;
        y1 = b + hw + 8;
    }

private:
    static bool ellipse(long x, long y, long cx, long cy, long a, long c) {
        if (a <= 0 || c <= 0) return false;
        const long dx = x - cx, dy = y - cy;
        return dx * dx * c * c + dy * dy * a * a <= a * a * c * c;
    }

    // A rectangle with corners of radius rad, grown by g: with q the distance beyond the straight
    // parts (all doubled, to keep the centre integral), a corner is a circle.
    bool rounded(long x, long y, long g) const {
        if (rad + g <= 0) return false;
        const long qx = std::max(std::labs(2 * x - (l + r)) - (r - l) + 2 * rad, 0L);
        const long qy = std::max(std::labs(2 * y - (t + b)) - (b - t) + 2 * rad, 0L);
        const long d = 2 * (rad + g);
        return qx * qx + qy * qy <= d * d;
    }

    // The vertices of a polygonal shape, clockwise on the screen, and how many there are.
    int polygon(long* px, long* py) const {
        const long cx = (l + r) / 2, cy = (t + b) / 2;
        if (shape == Shape::Rhombus) {
            const long xs[4] = {cx, r, cx, l}, ys[4] = {t, cy, b, cy};
            std::copy(xs, xs + 4, px);
            std::copy(ys, ys + 4, py);
            return 4;
        }
        if (shape == Shape::Hexagon) {
            const long xs[6] = {l + point, r - point, r, r - point, l + point, l};
            const long ys[6] = {t, t, cy, b, b, cy};
            std::copy(xs, xs + 6, px);
            std::copy(ys, ys + 6, py);
            return 6;
        }
        if (shape == Shape::LeanR || shape == Shape::LeanL) {
            const long lt = shape == Shape::LeanR ? slant : 0, lb = slant - lt;
            const long xs[4] = {l + lt, r - lb, r - lt, l + lb};
            const long ys[4] = {t, t, b, b};
            std::copy(xs, xs + 4, px);
            std::copy(ys, ys + 4, py);
            return 4;
        }
        if (shape == Shape::TrapB || shape == Shape::TrapT) {
            const long at = shape == Shape::TrapB ? slant : 0, ab = slant - at;
            const long xs[4] = {l + at, r - at, r - ab, l + ab};
            const long ys[4] = {t, t, b, b};
            std::copy(xs, xs + 4, px);
            std::copy(ys, ys + 4, py);
            return 4;
        }
        return 0;
    }

    // The wedge that a notched shape cuts out of its left side: its tip, then the two points where
    // its edges, run on to the left, are far outside the box (clockwise on the screen).
    void wedge(long* wx, long* wy) const {
        const long cy = (t + b) / 2, h = std::max(cy - t, 1L);
        const long far = (r - l) + 8 * h;  // how far left of the box the wedge is cut
        const long rise = far * h / std::max(slant, 1L);
        wx[0] = l + slant;
        wy[0] = cy;
        wx[1] = l - far;
        wy[1] = b + rise;
        wx[2] = l - far;
        wy[2] = t - rise;
    }

    // Whether the point is inside the convex polygon grown by g: within g of it if g is positive,
    // at least -g inside it if g is negative.
    static bool polygonIn(const long* px, const long* py, int n, long x, long y, long g) {
        bool inside = true, deep = true;
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            const long ex = px[j] - px[i], ey = py[j] - py[i];
            const long cross = ex * (y - py[i]) - ey * (x - px[i]);
            if (cross < 0) {
                inside = false;
            } else if (cross * cross < g * g * (ex * ex + ey * ey)) {
                deep = false;
            }
        }
        if (g <= 0) return inside && (g == 0 || deep);
        if (inside) return true;
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            const long ex = px[j] - px[i], ey = py[j] - py[i];
            const long ax = x - px[i], ay = y - py[i];
            const long len2 = ex * ex + ey * ey, dot = ax * ex + ay * ey;
            if (dot < 0) {
                if (ax * ax + ay * ay <= g * g) return true;
            } else if (dot > len2) {
                const long bx = x - px[j], by = y - py[j];
                if (bx * bx + by * by <= g * g) return true;
            } else {
                const long cross = ex * ay - ey * ax;
                if (cross * cross <= g * g * len2) return true;
            }
        }
        return false;
    }

    Shape shape;
    long l, t, r, b;  // the centre lines' box
    long hw;          // half a stroke
    long gap;         // between the two outlines of a double circle
    long cap;         // the vertical radius of a cylinder's lid
    long rad = 0;     // the corner radius of a rounded box
    long point = 0;   // how far a hexagon's points reach in
    long slant = 0;   // how far a slanted side or a notch reaches in: a column
};

struct Footprint {
    int cols, rows;
    int labelRow;  // the first row of the label, in the footprint
};

// Whether a footprint of cols x rows cells holds the label inside the outline, with a cell to
// spare on either side.
inline bool labelFits(Shape sh, const Footprint& f, int labelW, int lines, int cw, int ch) {
    const int s = strokeWidth(cw);
    const Geom g(
        sh, cw / 2, ch / 2, (2 * f.cols - 1) * cw / 2, (2 * f.rows - 1) * ch / 2, cw, ch, s);
    const long x0 = 8L * ((f.cols - labelW) / 2 - 1) * cw;
    const long x1 = 8L * ((f.cols - labelW) / 2 + labelW + 1) * cw;
    const long y0 = 8L * f.labelRow * ch, y1 = 8L * (f.labelRow + lines) * ch;
    const long grow = -g.half() - (sh == Shape::DoubleCircle ? 16L * s : 0);
    return g.in(x0, y0, grow) && g.in(x1, y0, grow) && g.in(x0, y1, grow) && g.in(x1, y1, grow);
}

// The size of a node of the given shape around a label of labelW columns and `lines` lines, for
// cells cw x ch pixels. For a top-down graph the width is odd, so that the footprint's centre lies
// on a whole column and arrows can be exactly vertical.
inline Footprint footprint(
    Shape sh, int labelW, int lines, bool td, int cw, int ch, int nodePad = tune::kNodePad) {
    Footprint f{labelW + nodePad, lines + tune::kNodeFrame, 1};
    switch (sh) {
    case Shape::Stadium:
    case Shape::Subroutine:
    case Shape::Hexagon:
    case Shape::LeanR:
    case Shape::LeanL:
    case Shape::TrapB:
    case Shape::TrapT: f.cols += 2; break;
    case Shape::Odd: f.cols += 1; break;
    case Shape::Cylinder:
        // The lid is half a row tall whatever the width, so a narrow cylinder would get a lid
        // that looks far deeper than a wide one's. Give it at least the width of a middling label,
        // of the label's parity so that the label is exactly centred.
        f.cols = std::max(labelW, tune::kMinCylinderLabel + labelW % 2) + nodePad;
        f.rows += 1;
        f.labelRow = 2;
        break;
    case Shape::Rhombus:
        f.rows = lines + 4;
        f.labelRow = 2;
        break;
    default: break;
    }
    // A cylinder's label is centred whatever its parity, even if that puts the footprint's centre
    // half a column off the edges' line: its lid is flat enough there to hide it.
    if (td && f.cols % 2 == 0 && sh != Shape::Cylinder) ++f.cols;
    if (sh == Shape::Circle || sh == Shape::DoubleCircle || sh == Shape::Rhombus)
        while (f.cols < 400 && !labelFits(sh, f, labelW, lines, cw, ch)) f.cols += td ? 2 : 1;
    return f;
}

}  // namespace shape
}  // namespace mermaid
