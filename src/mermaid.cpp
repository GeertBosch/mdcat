#include "mermaid.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>

#include "png_write.h"

namespace mermaid {

namespace {

constexpr int kMaxNodes = 60;  // bounds layout cost and clutter

struct Node {
    std::string id;
    std::string label;
    bool hasLabel = false;
    int out = -1;
    int in = -1;
};

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool isIdChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Characters that are syntax in Mermaid labels (shapes, quoting, entities, HTML, markdown strings);
// a label containing one is not plain text.
bool plainLabelChar(char c) {
    switch (c) {
    case '[':
    case ']':
    case '(':
    case ')':
    case '{':
    case '}':
    case '"':
    case '|':
    case '<':
    case '>':
    case '`':
    case '#': return false;
    default: return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f;
    }
}

bool reservedId(const std::string& id) {
    static const char* const kReserved[] = {"end",
                                            "subgraph",
                                            "style",
                                            "class",
                                            "classDef",
                                            "click",
                                            "linkStyle",
                                            "direction",
                                            "graph",
                                            "flowchart"};
    for (const char* r : kReserved)
        if (id == r) return true;
    return false;
}

struct Parser {
    std::vector<Node> nodes;
    std::map<std::string, int> index;
    std::string reason;

    bool fail(const std::string& why) {
        reason = why;
        return false;
    }

    // Parse `id` or `id[text]` at s[p], skipping surrounding spaces; yields the node index.
    bool parseNode(const std::string& s, size_t& p, int& node) {
        while (p < s.size() && s[p] == ' ') ++p;
        size_t b = p;
        while (p < s.size() && isIdChar(s[p])) ++p;
        if (p == b) return fail("unsupported syntax");
        std::string id = s.substr(b, p - b);
        if (reservedId(id)) return fail("unsupported syntax");
        bool hasLabel = false;
        std::string label;
        if (p < s.size() && s[p] == '[') {
            size_t close = s.find(']', p + 1);
            if (close == std::string::npos) return fail("unsupported syntax");
            std::string raw = s.substr(p + 1, close - p - 1);
            for (char c : raw)
                if (!plainLabelChar(c)) return fail("unsupported label");
            // HTML-style collapsing of whitespace runs, as Mermaid shows them.
            raw = trim(raw);
            for (char c : raw) {
                if (c == ' ' || c == '\t') {
                    if (label.empty() || label.back() != ' ') label += ' ';
                } else {
                    label += c;
                }
            }
            if (label.empty()) return fail("unsupported label");
            hasLabel = true;
            p = close + 1;
        }
        while (p < s.size() && s[p] == ' ') ++p;

        auto it = index.find(id);
        if (it == index.end()) {
            it = index.emplace(id, static_cast<int>(nodes.size())).first;
            nodes.push_back(Node{id, id, false, -1, -1});
        }
        Node& n = nodes[static_cast<size_t>(it->second)];
        if (hasLabel) {
            if (n.hasLabel && n.label != label) return fail("conflicting labels");
            n.label = label;
            n.hasLabel = true;
        }
        node = it->second;
        return true;
    }
};

// The raster: straight-alpha RGBA, all pixels the stroke colour, with alpha 0 where nothing is
// drawn. Coordinates are in pixels; half-open ranges.
struct Raster {
    int w, h;
    uint32_t rgb;
    std::vector<uint8_t> alpha;

    Raster(int w_, int h_, uint32_t rgb_)
        : w(w_), h(h_), rgb(rgb_), alpha(static_cast<size_t>(w_) * static_cast<size_t>(h_), 0) {}

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

    bool touched(int x0, int y0, int x1, int y1) const {
        for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(w, x1); ++x)
                if (alpha[static_cast<size_t>(y) * static_cast<size_t>(w) +
                          static_cast<size_t>(x)] != 0)
                    return true;
        return false;
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

struct Foot {
    int col = 0, row = 0, w = 0, h = 3;  // footprint in cells
    int labelCol = 0;                    // label sits on row + 1
    int labelW = 0;
};

}  // namespace

bool render(const std::vector<std::string>& body,
            const Env& env,
            Diagram& out,
            std::string& reason) {
    Parser ps;
    bool td = false;
    bool sawHeader = false;
    int edges = 0;

    for (const std::string& raw : body) {
        std::string line = trim(raw);
        if (line.empty()) continue;
        if (!sawHeader) {
            size_t p = 0;
            while (p < line.size() && std::isalpha(static_cast<unsigned char>(line[p]))) ++p;
            std::string kw = line.substr(0, p);
            if (kw != "graph" && kw != "flowchart") {
                reason = "not a flowchart";
                return false;
            }
            std::string dir = trim(line.substr(p));
            if (p == line.size() || line[p] != ' ') {
                reason = "unsupported direction";
                return false;
            }
            if (dir == "TD" || dir == "TB") {
                td = true;
            } else if (dir != "LR") {
                reason = "unsupported direction";
                return false;
            }
            sawHeader = true;
            continue;
        }
        size_t p = 0;
        int from = 0, to = 0;
        if (!ps.parseNode(line, p, from)) {
            reason = ps.reason;
            return false;
        }
        if (line.compare(p, 3, "-->") != 0) {
            reason = "unsupported syntax";
            return false;
        }
        p += 3;
        if (!ps.parseNode(line, p, to)) {
            reason = ps.reason;
            return false;
        }
        if (p != line.size()) {
            reason = "unsupported syntax";
            return false;
        }
        Node& a = ps.nodes[static_cast<size_t>(from)];
        Node& b = ps.nodes[static_cast<size_t>(to)];
        if (from == to || a.out >= 0 || b.in >= 0) {
            reason = "not a simple path";
            return false;
        }
        a.out = to;
        b.in = from;
        ++edges;
    }

    if (!sawHeader || edges == 0) {
        reason = "not a simple path";
        return false;
    }
    if (static_cast<int>(ps.nodes.size()) > kMaxNodes) {
        reason = "too many nodes";
        return false;
    }

    // Exactly one start node, and following the out-edges from it must reach every node.
    int start = -1;
    for (size_t i = 0; i < ps.nodes.size(); ++i) {
        if (ps.nodes[i].in < 0) {
            if (start >= 0) {
                reason = "not a simple path";
                return false;
            }
            start = static_cast<int>(i);
        }
    }
    std::vector<const Node*> path;
    for (int i = start; i >= 0; i = ps.nodes[static_cast<size_t>(i)].out)
        path.push_back(&ps.nodes[static_cast<size_t>(i)]);
    if (path.size() != ps.nodes.size()) {
        reason = "not a simple path";
        return false;
    }

    // Footprints, in cells: (label width + 4) x 3. For top-down the width is made odd so that
    // every footprint's centre lies on a whole column and the arrows are exactly vertical.
    const size_t k = path.size();
    std::vector<Foot> feet(k);
    int maxW = 0;
    for (size_t i = 0; i < k; ++i) {
        Foot& f = feet[i];
        f.labelW = env.width(path[i]->label);
        f.w = f.labelW + 4;
        if (td && f.w % 2 == 0) ++f.w;
        maxW = std::max(maxW, f.w);
    }
    constexpr int kGapTd = 2;  // rows between footprints
    constexpr int kGapLr = 4;  // columns between footprints
    int cols = 0, rows = 0;
    if (td) {
        int row = 0;
        for (Foot& f : feet) {
            f.col = (maxW - f.w) / 2;
            f.row = row;
            row += f.h + kGapTd;
        }
        cols = maxW;
        rows = row - kGapTd;
    } else {
        int col = 0;
        for (Foot& f : feet) {
            f.col = col;
            f.row = 0;
            col += f.w + kGapLr;
        }
        cols = col - kGapLr;
        rows = 3;
    }
    for (Foot& f : feet) f.labelCol = f.col + (f.w - f.labelW) / 2;

    if (cols > env.availCols) {
        reason = "too wide: " + std::to_string(cols) + " > " + std::to_string(env.availCols);
        return false;
    }

    // Raster parameters, all derived from the cell size.
    const int cw = std::max(1, env.cellW), ch = std::max(1, env.cellH);
    const int s = std::max(1, (2 * cw + 9) / 18);  // stroke width, round(cw / 9)
    const int hs = s / 2;
    const int headLen = std::max(6, cw);
    const int headHalf = std::max(3, (45 * cw + 50) / 100);  // round(0.45 * cw)
    const uint32_t rgb = env.darkTheme ? 0xc8c8c8u : 0x505050u;
    auto px = [&](int hu) { return hu * cw / 2; };  // half-cell units -> pixels
    auto py = [&](int hu) { return hu * ch / 2; };

    Raster ras(cols * cw, rows * ch, rgb);

    // Outlines: a rectangle inset half a cell from each footprint.
    std::vector<int> left(k), right(k), top(k), bottom(k);
    for (size_t i = 0; i < k; ++i) {
        const Foot& f = feet[i];
        left[i] = px(2 * f.col + 1);
        right[i] = px(2 * (f.col + f.w) - 1);
        top[i] = py(2 * f.row + 1);
        bottom[i] = py(2 * (f.row + f.h) - 1);
        ras.rect(left[i] - hs, top[i] - hs, right[i] - hs + s, top[i] - hs + s);
        ras.rect(left[i] - hs, bottom[i] - hs, right[i] - hs + s, bottom[i] - hs + s);
        ras.rect(left[i] - hs, top[i] - hs, left[i] - hs + s, bottom[i] - hs + s);
        ras.rect(right[i] - hs, top[i] - hs, right[i] - hs + s, bottom[i] - hs + s);
    }

    // Arrows run from one outline to the next, along the flow axis through the footprint centre.
    // The shaft stops where the head begins, and the head's tip touches the target's outline.
    for (size_t i = 0; i + 1 < k; ++i) {
        const Foot& f = feet[i];
        if (td) {
            int cx = px(2 * f.col + f.w);
            int y0 = bottom[i] - hs + s;
            int tip = top[i + 1] - hs;
            if (tip - headLen < y0) {
                reason = "arrow too short";
                return false;
            }
            ras.rect(cx - hs, y0, cx - hs + s, tip - headLen);
            ras.arrowhead(8 * (cx - hs) + 4 * s, 8 * tip, true, headLen, headHalf);
        } else {
            int cy = py(2 * f.row + 3);
            int x0 = right[i] - hs + s;
            int tip = left[i + 1] - hs;
            if (tip - headLen < x0) {
                reason = "arrow too short";
                return false;
            }
            ras.rect(x0, cy - hs, tip - headLen, cy - hs + s);
            ras.arrowhead(8 * tip, 8 * (cy - hs) + 4 * s, false, headLen, headHalf);
        }
    }

    // No stroke may touch a label cell.
    for (size_t i = 0; i < k; ++i) {
        const Foot& f = feet[i];
        if (ras.touched(f.labelCol * cw,
                        (f.row + 1) * ch,
                        (f.labelCol + f.labelW) * cw,
                        (f.row + 2) * ch)) {
            reason = "stroke over label";
            return false;
        }
    }

    // The text layer: labels on each node's middle row, spaces elsewhere.
    out.cols = cols;
    out.rows = rows;
    out.textRows.assign(static_cast<size_t>(rows), std::string());
    std::vector<std::vector<size_t>> byRow(static_cast<size_t>(rows));
    for (size_t i = 0; i < k; ++i) byRow[static_cast<size_t>(feet[i].row + 1)].push_back(i);
    for (int r = 0; r < rows; ++r) {
        std::string& line = out.textRows[static_cast<size_t>(r)];
        int at = 0;
        for (size_t i : byRow[static_cast<size_t>(r)]) {
            line.append(static_cast<size_t>(feet[i].labelCol - at), ' ');
            line += path[i]->label;
            at = feet[i].labelCol + feet[i].labelW;
        }
        line.append(static_cast<size_t>(cols - at), ' ');
    }

    std::vector<uint32_t> pixels(ras.alpha.size());
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = (rgb << 8) | ras.alpha[i];
    out.png = encodePngRgba(ras.w, ras.h, pixels);
    return true;
}

}  // namespace mermaid
