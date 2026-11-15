#include <algorithm>

#include "mermaid_model.h"

namespace mermaid {

bool layoutChain(const Graph& g, const Env& env, Layout& lay, std::string& reason) {
    const size_t n = g.nodes.size();
    if (g.edges.empty()) {
        reason = "not a simple path";
        return false;
    }
    if (static_cast<int>(n) > tune::kMaxNodes) {
        reason = "too many nodes";
        return false;
    }
    const bool td = g.dir == Dir::TB;
    if (!td && g.dir != Dir::LR) {
        reason = "unsupported direction";
        return false;
    }

    // Every node has at most one in-edge and one out-edge, there is exactly one start node, and
    // following the out-edges from it reaches every node.
    std::vector<int> out(n, -1), in(n, -1);
    for (const Edge& e : g.edges) {
        size_t a = static_cast<size_t>(e.from), b = static_cast<size_t>(e.to);
        if (a == b || out[a] >= 0 || in[b] >= 0) {
            reason = "not a simple path";
            return false;
        }
        out[a] = e.to;
        in[b] = e.from;
    }
    int start = -1;
    for (size_t i = 0; i < n; ++i) {
        if (in[i] < 0) {
            if (start >= 0) {
                reason = "not a simple path";
                return false;
            }
            start = static_cast<int>(i);
        }
    }
    std::vector<size_t> path;
    for (int i = start; i >= 0; i = out[static_cast<size_t>(i)])
        path.push_back(static_cast<size_t>(i));
    if (path.size() != n) {
        reason = "not a simple path";
        return false;
    }

    // Footprints, in cells: (label width + 4) x 3. For top-down the width is made odd so that
    // every footprint's centre lies on a whole column and the arrows are exactly vertical.
    lay.nodeFoot.assign(n, CellBox());
    lay.nodeLabel.assign(n, CellBox());
    int maxW = 0;
    for (size_t i : path) {
        int labelW = env.width(g.nodes[i].label.lines[0]);
        int w = labelW + tune::kNodePad;
        if (td && w % 2 == 0) ++w;
        lay.nodeFoot[i].cols = w;
        lay.nodeFoot[i].rows = tune::kNodeRows;
        lay.nodeLabel[i].cols = labelW;
        lay.nodeLabel[i].rows = 1;
        maxW = std::max(maxW, w);
    }
    if (td) {
        int row = 0;
        for (size_t i : path) {
            CellBox& f = lay.nodeFoot[i];
            f.col = (maxW - f.cols) / 2;
            f.row = row;
            row += f.rows + tune::kGapTd;
        }
        lay.cols = maxW;
        lay.rows = row - tune::kGapTd;
    } else {
        int col = 0;
        for (size_t i : path) {
            CellBox& f = lay.nodeFoot[i];
            f.col = col;
            f.row = 0;
            col += f.cols + tune::kGapLr;
        }
        lay.cols = col - tune::kGapLr;
        lay.rows = tune::kNodeRows;
    }
    for (size_t i : path) {
        const CellBox& f = lay.nodeFoot[i];
        lay.nodeLabel[i].col = f.col + (f.cols - lay.nodeLabel[i].cols) / 2;
        lay.nodeLabel[i].row = f.row + 1;
    }

    // Edges leave one outline and arrive at the next along the flow axis, through the footprint
    // centre. Outlines are inset half a cell from the footprint.
    lay.edgeLabel.assign(g.edges.size(), CellBox());
    lay.edgePath.assign(g.edges.size(), std::vector<Pt>());
    for (size_t k = 0; k < g.edges.size(); ++k) {
        const CellBox& a = lay.nodeFoot[static_cast<size_t>(g.edges[k].from)];
        const CellBox& b = lay.nodeFoot[static_cast<size_t>(g.edges[k].to)];
        if (td) {
            int x = 2 * a.col + a.cols;
            lay.edgePath[k] = {{x, 2 * (a.row + a.rows) - 1}, {x, 2 * b.row + 1}};
        } else {
            int y = 2 * a.row + a.rows;
            lay.edgePath[k] = {{2 * (a.col + a.cols) - 1, y}, {2 * b.col + 1, y}};
        }
    }
    return true;
}

}  // namespace mermaid
