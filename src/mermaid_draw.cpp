#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <set>

#include "mermaid_model.h"

namespace mermaid {

namespace {

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

// A node with no label whose style gives it no height, as Mermaid users write `SPACER(" ")` with
// `style SPACER height:0px;` to push neighbours apart. Mermaid keeps its room but draws nothing.
bool spacer(const Node& n) {
    if (!n.style.collapsed) return false;
    for (const std::string& line : n.label.lines)
        if (!line.empty()) return false;
    return n.label.markdown.empty();
}

std::vector<Stroke> strokesOf(const Graph& g, const Layout& lay, const std::vector<Pt>& crossings) {
    std::vector<Stroke> strokes;
    // A subgraph's box is an outline under everything else. It has no fill: the text layer over
    // the image would lose the theme's background.
    for (size_t c = 0; c < lay.clusterBox.size(); ++c) {
        const CellBox& b = lay.clusterBox[c];
        Stroke st;
        st.kind = Stroke::Outline;
        st.shape = Shape::Rect;
        st.cluster = static_cast<int>(c);
        st.pts = {{2 * b.col + 1, 2 * b.row + 1},
                  {2 * (b.col + b.cols) - 1, 2 * (b.row + b.rows) - 1}};
        strokes.push_back(st);
    }
    for (size_t i = 0; i < lay.nodeFoot.size(); ++i) {
        const CellBox& f = lay.nodeFoot[i];
        if (spacer(g.nodes[i])) continue;  // keeps its footprint, so that other things avoid it
        Stroke st;
        st.kind = Stroke::Outline;
        st.shape = g.nodes[i].shape;
        st.node = static_cast<int>(i);
        st.colour = g.nodes[i].style.stroke;
        st.fill = g.nodes[i].style.fill;
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

// Whether two boxes of cells share a cell.
bool touches(const CellBox& a, const CellBox& b) {
    return a.col < b.col + b.cols && b.col < a.col + a.cols && a.row < b.row + b.rows &&
        b.row < a.row + a.rows;
}

// Whether the box `inner` lies inside `outer`, clear of its border.
bool inside(const CellBox& inner, const CellBox& outer) {
    return inner.col > outer.col && inner.col + inner.cols < outer.col + outer.cols &&
        inner.row > outer.row && inner.row + inner.rows < outer.row + outer.rows;
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

// Whether cluster `c` is `of` or lies inside it (-1 is a node outside every cluster).
bool clusterWithin(const Graph& g, int c, int of) {
    for (; c >= 0; c = g.clusters[static_cast<size_t>(c)].parent)
        if (c == of) return true;
    return false;
}

// The checks that go with subgraphs. Each box is clear of the others, or strictly inside the one
// that holds it (a blank cell from its border), holds exactly its members, those of the boxes
// inside it too (their footprints, and the labels and paths of the edges between them), and an
// edge with only one end inside it crosses its border, never runs along it; an edge with no end
// inside never touches it.
bool validateClusters(const Graph& g, const Layout& lay, std::string& reason) {
    const size_t count = lay.clusterBox.size();
    for (size_t c = 0; c < count; ++c) {
        const CellBox& box = lay.clusterBox[c];
        for (size_t d = c + 1; d < count; ++d) {
            const CellBox& other = lay.clusterBox[d];
            // `d` closes after `c`, so if either holds the other it is `d` that holds `c`.
            const bool held = clusterWithin(g, static_cast<int>(c), static_cast<int>(d));
            const CellBox room{other.col + 1, other.row + 1, other.cols - 2, other.rows - 2};
            if (held ? !inside(box, room) : touches(box, other)) {
                reason = "subgraph layout: boxes";
                return false;
            }
        }
        for (size_t i = 0; i < g.nodes.size(); ++i) {
            const CellBox& f = lay.nodeFoot[i];
            const bool member = clusterWithin(g, g.nodes[i].cluster, static_cast<int>(c));
            if (member ? !inside(f, box) : touches(f, box)) {
                reason = "subgraph layout: node";
                return false;
            }
        }
        // No edge is level with a border, which would make it seem to run into it.
        if (alignedWithBorder(lay, c)) {
            reason = "subgraph layout: edge";
            return false;
        }
        const CellBox& title = lay.clusterLabel[c];
        if (title.cols > 0) {
            // Clear of the border and a blank cell from it on both sides, so that it is read as
            // the box's title.
            const CellBox room{box.col + 1, box.row, box.cols - 2, box.rows};
            if (!inside(title, room)) {
                reason = "subgraph layout: title";
                return false;
            }
            for (const std::vector<Pt>& path : lay.edgePath) {
                for (size_t i = 0; i + 1 < path.size(); ++i) {
                    bool hit = false;
                    segmentCells(path[i], path[i + 1], [&](int x, int y) {
                        if (inBox(title, x, y)) hit = true;
                    });
                    if (hit) {
                        reason = "subgraph layout: title";
                        return false;
                    }
                }
            }
        }
        const int l = 2 * box.col + 1, t = 2 * box.row + 1, r = 2 * (box.col + box.cols) - 1,
                  b = 2 * (box.row + box.rows) - 1;
        const Pt corner[4] = {{l, t}, {r, t}, {r, b}, {l, b}};
        for (size_t k = 0; k < g.edges.size(); ++k) {
            const bool from = clusterWithin(
                g, g.nodes[static_cast<size_t>(g.edges[k].from)].cluster, static_cast<int>(c));
            const bool to = clusterWithin(
                g, g.nodes[static_cast<size_t>(g.edges[k].to)].cluster, static_cast<int>(c));
            const CellBox& label = lay.edgeLabel[k];
            if (label.cols > 0 && (from && to ? !inside(label, box) : touches(label, box))) {
                reason = "subgraph layout: label";
                return false;
            }
            const std::vector<Pt>& path = lay.edgePath[k];
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                const Pt &p = path[i], &q = path[i + 1];
                for (int side = 0; side < 4; ++side) {
                    if (overlap(p, q, corner[side], corner[(side + 1) % 4])) {
                        reason = "subgraph layout: edge";
                        return false;
                    }
                }
                // Both ends inside: every point is inside. No end inside: no point is.
                for (const Pt& pt : {p, q}) {
                    const bool in = pt.first > l && pt.first < r && pt.second > t && pt.second < b;
                    const bool on =
                        pt.first >= l && pt.first <= r && pt.second >= t && pt.second <= b;
                    if ((from && to && !in) || (!from && !to && on)) {
                        reason = "subgraph layout: edge";
                        return false;
                    }
                }
                // An edge between two outsiders may also cross a box without a point in it.
                if (!from && !to) {
                    const int x0 = std::min(p.first, q.first), x1 = std::max(p.first, q.first);
                    const int y0 = std::min(p.second, q.second), y1 = std::max(p.second, q.second);
                    if (x0 <= r && x1 >= l && y0 <= b && y1 >= t) {
                        reason = "subgraph layout: edge";
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

// Turn layout bugs into fallbacks instead of misleading pictures. Reads the occupancy grid and the
// stroke list, not the pixels.
bool validate(const Graph& g,
              const Layout& lay,
              const std::vector<Stroke>& strokes,
              const Grid& grid,
              const std::vector<Pt>& crossings,
              std::string& reason) {
    if (!validateClusters(g, lay, reason)) return false;
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
    for (const CellBox& l : lay.clusterLabel) {
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
    // A box's border may not lie over a node or a label.
    bool borderClear = true;
    for (const Stroke& st : strokes) {
        if (st.kind != Stroke::Outline || st.cluster < 0) continue;
        // A cell is the border of one box only: two borders never share one. (An outline passes
        // its corners twice.)
        std::vector<std::pair<int, int>> cells;
        outlineCells(st.pts[0], st.pts[1], [&](int c, int r) { cells.push_back({c, r}); });
        for (const auto& [c, r] : cells)
            if (grid.get(c, r) != Occ::Empty) borderClear = false;
        for (const auto& [c, r] : cells) grid.at(c, r) = Occ::Cluster;
    }
    if (!borderClear) {
        reason = "subgraph layout: border";
        return false;
    }
    for (const Stroke& st : strokes) {
        if (st.kind != Stroke::Segment) continue;
        segmentCells(st.pts[0], st.pts[1], [&](int c, int r) {
            if (grid.get(c, r) == Occ::Empty) grid.at(c, r) = Occ::Edge;
        });
    }

    if (!validate(g, lay, strokes, grid, crossings, reason)) return false;

    // The scene: the strokes, and the text of the subgraph titles, the nodes and the edge labels,
    // in that order. A node with a fill has its label cells written in it (see paintScene).
    Scene scene;
    scene.cols = lay.cols;
    scene.rows = lay.rows;
    if (lay.titleRows > 0) scene.title = g.title;
    scene.strokes = std::move(strokes);
    auto addText = [&](const CellBox& box,
                       const std::vector<std::string>& lines,
                       const Style& style,
                       bool bold) {
        if (box.cols <= 0) return;
        Scene::Text t;
        t.box = box;
        t.lines = lines;
        t.style = style;
        t.bold = bold;
        scene.texts.push_back(std::move(t));
    };
    for (size_t c = 0; c < g.clusters.size(); ++c)
        addText(lay.clusterLabel[c], g.clusters[c].title.lines, Style(), true);
    for (size_t i = 0; i < g.nodes.size(); ++i)
        addText(lay.nodeLabel[i], g.nodes[i].label.lines, g.nodes[i].style, false);
    for (size_t k = 0; k < g.edges.size(); ++k)
        addText(lay.edgeLabel[k], g.edges[k].label.lines, Style(), false);
    return paintScene(scene, env, out, reason);
}

}  // namespace mermaid
