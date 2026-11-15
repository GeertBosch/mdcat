#include <algorithm>
#include <climits>
#include <functional>
#include <map>
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

// An edge segment between two adjacent ranks: the items at its ends, and where they are across the
// flow, in half-cell units.
struct GapSeg {
    int from, to, xa, xb;
};

// Choose the track each segment of one gap jogs along, numbered from the source rank; a straight
// segment gets -1. Returns the number of tracks.
//
// Segments that jog are grouped into nets. All the jogging segments of a source with two or more
// of them form one net, a bus that splits off a common track. The rest are grouped by target, so
// that a fan-in merges. A net occupies one track, over the span of its ends.
//
// A net whose stub (the vertical run from its source to its track, or from its track to its
// target) lies within another net's span must be on the near side of that net's track, or the stub
// would cross the other net's run. Those constraints are met by longest-path layering of the nets;
// nets that do not constrain each other share a track. Where the constraints form a cycle, one
// net's are reversed, so the crossing is a bridge, not a touch.
int assignTracks(const std::vector<GapSeg>& segs, std::vector<int>& trackOf) {
    const size_t count = segs.size();
    trackOf.assign(count, -1);
    std::vector<int> netOf(count, -1);
    std::map<int, int> perSource, outNet, inNet;
    for (const GapSeg& s : segs)
        if (s.xa != s.xb) ++perSource[s.from];
    int nets = 0;
    for (size_t i = 0; i < count; ++i) {
        if (segs[i].xa == segs[i].xb || perSource[segs[i].from] < 2) continue;
        netOf[i] = outNet.emplace(segs[i].from, nets).first->second;
        nets = std::max(nets, netOf[i] + 1);
    }
    for (size_t i = 0; i < count; ++i) {
        if (segs[i].xa == segs[i].xb || netOf[i] >= 0) continue;
        netOf[i] = inNet.emplace(segs[i].to, nets).first->second;
        nets = std::max(nets, netOf[i] + 1);
    }
    if (nets == 0) return 0;

    const size_t n = static_cast<size_t>(nets);
    std::vector<int> lo(n, INT_MAX), hi(n, INT_MIN);
    std::vector<std::vector<size_t>> members(n);
    std::vector<std::set<int>> sources(n), targets(n);
    for (size_t i = 0; i < count; ++i) {
        if (netOf[i] < 0) continue;
        size_t p = static_cast<size_t>(netOf[i]);
        lo[p] = std::min({lo[p], segs[i].xa, segs[i].xb});
        hi[p] = std::max({hi[p], segs[i].xa, segs[i].xb});
        members[p].push_back(i);
        sources[p].insert(segs[i].from);
        targets[p].insert(segs[i].to);
    }

    // above[p][q] > 0: p must be above q. The weight is 1 where breaking the constraint makes a
    // crossing, and large where it makes two edges touch.
    std::vector<std::vector<int>> above(n, std::vector<int>(n, 0));
    auto cost = [](int x, int l, int h) { return x > l && x < h ? 1 : 1000; };
    for (size_t i = 0; i < count; ++i) {
        if (netOf[i] < 0) continue;
        size_t p = static_cast<size_t>(netOf[i]);
        for (size_t q = 0; q < n; ++q) {
            if (q == p) continue;
            if (segs[i].xa >= lo[q] && segs[i].xa <= hi[q] && !sources[q].count(segs[i].from))
                above[p][q] = std::max(above[p][q], cost(segs[i].xa, lo[q], hi[q]));
            if (segs[i].xb >= lo[q] && segs[i].xb <= hi[q] && !targets[q].count(segs[i].to))
                above[q][p] = std::max(above[q][p], cost(segs[i].xb, lo[q], hi[q]));
        }
    }
    // Nets whose runs would overlap on one track need different tracks even when no stub forces an
    // order, unless they only meet where they share a port.
    for (size_t p = 0; p < n; ++p) {
        for (size_t q = p + 1; q < n; ++q) {
            if (above[p][q] || above[q][p]) continue;
            int l = std::max(lo[p], lo[q]), h = std::min(hi[p], hi[q]);
            if (l > h) continue;
            bool shared = false;
            for (size_t i : members[p])
                for (size_t j : members[q])
                    shared = shared ||
                        (l == h && segs[i].from == segs[j].from && segs[i].xa == l) ||
                        (l == h && segs[i].to == segs[j].to && segs[i].xb == l);
            if (!shared) above[p][q] = 1;
        }
    }

    std::vector<char> done(n, 0);
    std::vector<int> level(n, 0);
    for (size_t step = 0; step < n; ++step) {
        size_t pick = n;
        for (size_t x = 0; x < n && pick == n; ++x) {
            if (done[x]) continue;
            bool free = true;
            for (size_t y = 0; y < n; ++y) free = free && (done[y] || !above[y][x]);
            if (free) pick = x;
        }
        if (pick == n) {
            long best = LONG_MAX;
            for (size_t x = 0; x < n; ++x) {
                if (done[x]) continue;
                long sum = 0;
                for (size_t y = 0; y < n; ++y)
                    if (!done[y]) sum += above[y][x];
                if (sum < best) {
                    best = sum;
                    pick = x;
                }
            }
            for (size_t y = 0; y < n; ++y) {
                if (done[y] || !above[y][pick]) continue;
                above[pick][y] = std::max(above[pick][y], above[y][pick]);
                above[y][pick] = 0;
            }
        }
        for (size_t y = 0; y < n; ++y)
            if (done[y] && above[y][pick]) level[pick] = std::max(level[pick], level[y] + 1);
        done[pick] = 1;
    }
    for (size_t i = 0; i < count; ++i)
        if (netOf[i] >= 0) trackOf[i] = level[static_cast<size_t>(netOf[i])];
    return *std::max_element(level.begin(), level.end()) + 1;
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
    // dummies). Between two ranks lies a gap that holds one cell for each track, the lines along
    // which edges jog sideways, plus one cell where the edges leave and one where they arrive.
    std::vector<int> thick(ranks, 1);
    for (size_t i = 0; i < n; ++i) {
        size_t r = static_cast<size_t>(rank[i]);
        thick[r] = std::max(thick[r], td ? tune::kNodeRows : nodeW[i]);
    }
    std::vector<std::vector<GapSeg>> gapSegs(ranks);
    for (const std::vector<int>& c : chain)
        for (size_t i = 0; i + 1 < c.size(); ++i)
            gapSegs[static_cast<size_t>(items[static_cast<size_t>(c[i])].rank)].push_back(
                {c[i], c[i + 1], centre(c[i]), centre(c[i + 1])});
    std::vector<int> start(ranks, 0);
    std::map<std::pair<int, int>, int> trackOf;  // by the items at a segment's ends
    for (size_t r = 0; r + 1 < ranks; ++r) {
        std::vector<int> tracks;
        int used = assignTracks(gapSegs[r], tracks);
        for (size_t j = 0; j < tracks.size(); ++j)
            if (tracks[j] >= 0) trackOf[{gapSegs[r][j].from, gapSegs[r][j].to}] = tracks[j];
        int gap = td ? tune::kGapTd + used : std::max(tune::kGapLr, used + 2);
        start[r + 1] = start[r] + thick[r] + gap;
    }
    const int along = start[ranks - 1] + thick[ranks - 1];
    auto track = [&](size_t r, int t) { return 2 * (start[r] + thick[r] + 1 + t) + 1; };  // hu
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
            int t = track(static_cast<size_t>(items[static_cast<size_t>(c[i])].rank),
                          trackOf.at({c[i], c[i + 1]}));
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
    const size_t crossings = crossingPoints(lay).size();
    if (crossings > static_cast<size_t>(tune::kMaxCrossings)) {
        reason =
            "crossings: " + std::to_string(crossings) + " > " + std::to_string(tune::kMaxCrossings);
        return false;
    }
    return true;
}

std::vector<Pt> crossingPoints(const Layout& lay) {
    std::set<Pt> found;
    for (size_t j = 0; j < lay.edgePath.size(); ++j) {
        for (size_t k = j + 1; k < lay.edgePath.size(); ++k) {
            const std::vector<Pt>&pj = lay.edgePath[j], &pk = lay.edgePath[k];
            for (size_t a = 0; a + 1 < pj.size(); ++a) {
                for (size_t b = 0; b + 1 < pk.size(); ++b) {
                    Pt h0 = pj[a], h1 = pj[a + 1], v0 = pk[b], v1 = pk[b + 1];
                    if (h0.second != h1.second) {
                        std::swap(h0, v0);
                        std::swap(h1, v1);
                    }
                    if (h0.second != h1.second || v0.first != v1.first) continue;
                    int x = v0.first, y = h0.second;
                    if (x > std::min(h0.first, h1.first) && x < std::max(h0.first, h1.first) &&
                        y > std::min(v0.second, v1.second) && y < std::max(v0.second, v1.second))
                        found.insert({x, y});
                }
            }
        }
    }
    return std::vector<Pt>(found.begin(), found.end());
}

}  // namespace mermaid
