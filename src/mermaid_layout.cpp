#include <algorithm>
#include <climits>
#include <functional>
#include <set>

#include "mermaid_model.h"

namespace mermaid {

namespace {

int floorDiv(int a, int b) {
    int q = a / b;
    if (a % b != 0 && ((a < 0) != (b < 0))) --q;
    return q;
}

// One thing that occupies a slot in a rank: a real node, or a dummy that an edge spanning several
// ranks passes through.
struct Item {
    int rank = 0;
    int ext = 1;   // extent across the flow, in cells
    int left = 0;  // start across the flow, in cells
};

// Place the items of one rank, in order, as close as possible to their desired starts (in cells)
// without overlapping or reordering: each item is put at its desired position, and neighbours that
// would overlap merge into a block placed at the mean of their desires (pool-adjacent-violators).
void placeRank(std::vector<Item>& items,
               const std::vector<int>& order,
               const std::vector<int>& desire,
               int sep) {
    struct Block {
        long sum;
        long count;
        size_t first;
    };
    std::vector<int> offset(order.size());
    int off = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        offset[i] = off;
        off += items[static_cast<size_t>(order[i])].ext + sep;
    }
    std::vector<Block> blocks;
    for (size_t i = 0; i < order.size(); ++i) {
        blocks.push_back({desire[i] - offset[i], 1, i});
        while (blocks.size() >= 2) {
            const Block& a = blocks[blocks.size() - 2];
            const Block& b = blocks.back();
            if (a.sum * b.count <= b.sum * a.count) break;
            Block merged{a.sum + b.sum, a.count + b.count, a.first};
            blocks.pop_back();
            blocks.back() = merged;
        }
    }
    for (size_t k = 0; k < blocks.size(); ++k) {
        size_t end = k + 1 < blocks.size() ? blocks[k + 1].first : order.size();
        int at = floorDiv(static_cast<int>(2 * blocks[k].sum + blocks[k].count),
                          static_cast<int>(2 * blocks[k].count));
        for (size_t i = blocks[k].first; i < end; ++i)
            items[static_cast<size_t>(order[i])].left = at + offset[i];
    }
}

// Whether the closed intervals [a0, a1] and [b0, b1], each given in either order, share a point.
bool intervalsMeet(int a0, int a1, int b0, int b1) {
    return std::max(std::min(a0, a1), std::min(b0, b1)) <=
        std::min(std::max(a0, a1), std::max(b0, b1));
}

}  // namespace

bool layoutGraph(const Graph& g, const Env& env, Layout& lay, std::string& reason) {
    const size_t n = g.nodes.size();
    if (g.edges.empty()) {
        reason = "no edges";
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

    std::set<std::pair<int, int>> seen;
    for (const Edge& e : g.edges) {
        if (!seen.insert({e.from, e.to}).second) {
            reason = "parallel edges";
            return false;
        }
    }
    std::vector<std::vector<int>> outE(n), inE(n);  // edge indices, in declaration order
    for (size_t k = 0; k < g.edges.size(); ++k) {
        outE[static_cast<size_t>(g.edges[k].from)].push_back(static_cast<int>(k));
        inE[static_cast<size_t>(g.edges[k].to)].push_back(static_cast<int>(k));
    }

    // Ranks: longest path from the sources, so that every edge spans at least its minimum length.
    std::vector<int> rank(n, 0);
    std::vector<size_t> indeg(n), topo;
    for (size_t i = 0; i < n; ++i) {
        indeg[i] = inE[i].size();
        if (indeg[i] == 0) topo.push_back(i);
    }
    for (size_t h = 0; h < topo.size(); ++h) {
        size_t u = topo[h];
        for (int k : outE[u]) {
            const Edge& e = g.edges[static_cast<size_t>(k)];
            size_t v = static_cast<size_t>(e.to);
            rank[v] = std::max(rank[v], rank[u] + e.minlen);
            if (--indeg[v] == 0) topo.push_back(v);
        }
    }
    if (topo.size() != n) {
        reason = "cycle";
        return false;
    }
    // Pull each source down next to its nearest successor, so that it does not hang far above.
    for (size_t i = 0; i < n; ++i) {
        if (!inE[i].empty()) continue;
        int r = INT_MAX;
        for (int k : outE[i]) {
            const Edge& e = g.edges[static_cast<size_t>(k)];
            r = std::min(r, rank[static_cast<size_t>(e.to)] - e.minlen);
        }
        rank[i] = r;
    }
    int lowest = *std::min_element(rank.begin(), rank.end());
    int highest = 0;
    for (int& r : rank) {
        r -= lowest;
        highest = std::max(highest, r);
    }
    if (highest + 1 > tune::kMaxRanks) {
        reason = "too many ranks";
        return false;
    }
    const size_t ranks = static_cast<size_t>(highest) + 1;

    // Footprints of the real nodes, in cells: (label width + 4) x 3. For top-down the width is made
    // odd so that every footprint's centre lies on a whole column and arrows can be exactly
    // vertical.
    std::vector<int> nodeW(n);
    std::vector<Item> items(n);
    for (size_t i = 0; i < n; ++i) {
        int w = env.width(g.nodes[i].label.lines[0]) + tune::kNodePad;
        if (td && w % 2 == 0) ++w;
        nodeW[i] = w;
        items[i].rank = rank[i];
        items[i].ext = td ? w : tune::kNodeRows;
    }

    // Initial order, by depth-first search from the sources of each weakly connected component in
    // turn, visiting children in the order their edges were declared. An edge that spans k ranks
    // gets k - 1 dummy items, one per rank it passes through.
    std::vector<size_t> comp(n);
    for (size_t i = 0; i < n; ++i) comp[i] = i;
    std::function<size_t(size_t)> find = [&](size_t i) {
        return comp[i] == i ? i : comp[i] = find(comp[i]);
    };
    for (const Edge& e : g.edges) {
        size_t a = find(static_cast<size_t>(e.from)), b = find(static_cast<size_t>(e.to));
        if (a != b) comp[std::max(a, b)] = std::min(a, b);
    }
    std::vector<std::vector<int>> order(ranks);
    std::vector<std::vector<int>> chain(g.edges.size());  // items an edge passes through
    std::vector<char> visited(n, 0);
    std::function<void(size_t)> visit = [&](size_t u) {
        visited[u] = 1;
        order[static_cast<size_t>(rank[u])].push_back(static_cast<int>(u));
        for (int k : outE[u]) {
            size_t v = static_cast<size_t>(g.edges[static_cast<size_t>(k)].to);
            std::vector<int>& c = chain[static_cast<size_t>(k)];
            c.push_back(static_cast<int>(u));
            for (int r = rank[u] + 1; r < rank[v]; ++r) {
                Item d;
                d.rank = r;
                items.push_back(d);
                c.push_back(static_cast<int>(items.size()) - 1);
                order[static_cast<size_t>(r)].push_back(static_cast<int>(items.size()) - 1);
            }
            c.push_back(static_cast<int>(v));
            if (!visited[v]) visit(v);
        }
    };
    std::vector<char> done(n, 0);
    for (size_t i = 0; i < n; ++i) {
        size_t root = find(i);
        if (done[root]) continue;
        done[root] = 1;
        for (size_t j = 0; j < n; ++j)
            if (find(j) == root && inE[j].empty()) visit(j);
    }

    // Placement across the flow. Pack each rank, then in one pass from the first rank on, move each
    // item toward the mean centre of the items its edges come from.
    const int sep = td ? tune::kSepTd : tune::kSepLr;
    std::vector<std::vector<int>> preds(items.size());
    for (const std::vector<int>& c : chain)
        for (size_t i = 0; i + 1 < c.size(); ++i)
            preds[static_cast<size_t>(c[i + 1])].push_back(c[i]);
    for (const std::vector<int>& row : order) {
        int at = 0;
        for (int id : row) {
            items[static_cast<size_t>(id)].left = at;
            at += items[static_cast<size_t>(id)].ext + sep;
        }
    }
    auto centre = [&](int id) {  // in hu
        return 2 * items[static_cast<size_t>(id)].left + items[static_cast<size_t>(id)].ext;
    };
    for (size_t r = 1; r < ranks; ++r) {
        const std::vector<int>& row = order[r];
        std::vector<int> desire(row.size(), 0);
        std::vector<char> known(row.size(), 0);
        for (size_t i = 0; i < row.size(); ++i) {
            const std::vector<int>& from = preds[static_cast<size_t>(row[i])];
            if (from.empty()) continue;
            int sum = 0;
            for (int p : from) sum += centre(p);
            int count = static_cast<int>(from.size());
            int mean = floorDiv(2 * sum + count, 2 * count);
            desire[i] = floorDiv(mean - items[static_cast<size_t>(row[i])].ext + 1, 2);
            known[i] = 1;
        }
        // An item with no predecessor (a source pulled down) hugs its neighbour.
        auto firstKnown = std::find(known.begin(), known.end(), 1);
        if (firstKnown == known.end()) {
            for (size_t i = 0; i < row.size(); ++i)
                desire[i] = items[static_cast<size_t>(row[i])].left;
        } else {
            size_t fk = static_cast<size_t>(firstKnown - known.begin());
            for (size_t i = fk; i-- > 0;)
                desire[i] = desire[i + 1] - items[static_cast<size_t>(row[i])].ext - sep;
            for (size_t i = fk + 1; i < row.size(); ++i)
                if (!known[i])
                    desire[i] = desire[i - 1] + items[static_cast<size_t>(row[i - 1])].ext + sep;
        }
        placeRank(items, row, desire, sep);
    }
    int minLeft = INT_MAX, across = 0;
    for (const Item& it : items) minLeft = std::min(minLeft, it.left);
    for (Item& it : items) {
        it.left -= minLeft;
        across = std::max(across, it.left + it.ext);
    }

    // Position along the flow. A rank is as thick as its thickest node (1 when it holds only
    // dummies). Between two ranks lies a gap with at most one track, the line along which edges
    // jog sideways; it is the second cell of the gap. Two edges with different sources and
    // different targets must not have jogs that overlap, or they would need separate tracks.
    std::vector<int> thick(ranks, 1);
    for (size_t i = 0; i < n; ++i) {
        size_t r = static_cast<size_t>(rank[i]);
        thick[r] = std::max(thick[r], td ? tune::kNodeRows : nodeW[i]);
    }
    std::vector<std::vector<std::pair<int, int>>> gapSegs(ranks);
    for (const std::vector<int>& c : chain)
        for (size_t i = 0; i + 1 < c.size(); ++i)
            gapSegs[static_cast<size_t>(items[static_cast<size_t>(c[i])].rank)].push_back(
                {c[i], c[i + 1]});
    std::vector<int> start(ranks, 0);
    for (size_t r = 0; r + 1 < ranks; ++r) {
        const auto& segs = gapSegs[r];
        bool jog = false;
        for (size_t j = 0; j < segs.size(); ++j) {
            int ja = centre(segs[j].first), jb = centre(segs[j].second);
            jog = jog || ja != jb;
            for (size_t k = j + 1; k < segs.size(); ++k) {
                if (segs[j].first == segs[k].first || segs[j].second == segs[k].second) continue;
                if (intervalsMeet(ja, jb, centre(segs[k].first), centre(segs[k].second))) {
                    reason = "needs tracks";
                    return false;
                }
            }
        }
        int gap = !td ? tune::kGapLr : jog ? tune::kGapTdTrack : tune::kGapTd;
        start[r + 1] = start[r] + thick[r] + gap;
    }
    const int along = start[ranks - 1] + thick[ranks - 1];
    auto track = [&](size_t r) { return 2 * (start[r] + thick[r] + 1) + 1; };  // hu
    auto point = [&](int cross, int flow) { return td ? Pt{cross, flow} : Pt{flow, cross}; };

    lay.cols = td ? across : along;
    lay.rows = td ? along : across;
    lay.nodeFoot.assign(n, CellBox());
    lay.nodeLabel.assign(n, CellBox());
    for (size_t i = 0; i < n; ++i) {
        const Item& it = items[i];
        int at = start[static_cast<size_t>(it.rank)];
        CellBox& f = lay.nodeFoot[i];
        f.cols = nodeW[i];
        f.rows = tune::kNodeRows;
        f.col = td ? it.left : at;
        f.row = td ? at : it.left;
        CellBox& l = lay.nodeLabel[i];
        l.cols = env.width(g.nodes[i].label.lines[0]);
        l.rows = 1;
        l.col = f.col + (f.cols - l.cols) / 2;
        l.row = f.row + 1;
    }

    // Each edge leaves its source's outline at the middle of the side facing the flow, jogs along
    // the track of every gap where its ends differ, and arrives at the middle of its target's
    // facing side. All the edges leaving a node share one port, and all those arriving share one.
    lay.edgeLabel.assign(g.edges.size(), CellBox());
    lay.edgePath.assign(g.edges.size(), std::vector<Pt>());
    for (size_t k = 0; k < g.edges.size(); ++k) {
        const std::vector<int>& c = chain[k];
        size_t from = static_cast<size_t>(g.edges[k].from), to = static_cast<size_t>(g.edges[k].to);
        std::vector<Pt> path;
        int leave =
            2 * (start[static_cast<size_t>(rank[from])] + (td ? tune::kNodeRows : nodeW[from])) - 1;
        path.push_back(point(centre(c.front()), leave));
        for (size_t i = 0; i + 1 < c.size(); ++i) {
            int a = centre(c[i]), b = centre(c[i + 1]);
            if (a == b) continue;
            int t = track(static_cast<size_t>(items[static_cast<size_t>(c[i])].rank));
            path.push_back(point(a, t));
            path.push_back(point(b, t));
        }
        path.push_back(point(centre(c.back()), 2 * start[static_cast<size_t>(rank[to])] + 1));
        // Drop the points that lie on a straight run.
        std::vector<Pt>& out = lay.edgePath[k];
        for (const Pt& p : path) {
            while (out.size() >= 2) {
                const Pt &a = out[out.size() - 2], &b = out.back();
                bool straight = (a.first == b.first && b.first == p.first) ||
                    (a.second == b.second && b.second == p.second);
                if (!straight) break;
                out.pop_back();
            }
            out.push_back(p);
        }
    }
    return true;
}

}  // namespace mermaid
