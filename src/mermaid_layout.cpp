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

// Dummies pull harder than real nodes during placement, so that long edges stay straight.
constexpr int kDummyWeight = 8;

// One thing that occupies a slot in a rank: a real node, or a dummy that an edge spanning several
// ranks passes through.
struct Item {
    int rank = 0;
    int ext = 1;     // extent across the flow, in cells
    int left = 0;    // start across the flow, in cells
    int weight = 1;  // how strongly placement honours its desired position
};

// Place the items of one rank, in order, as close as possible to their desired starts (in cells)
// without overlapping or reordering: each item is put at its desired position, and neighbours that
// would overlap merge into a block placed at the weighted mean of their desires
// (pool-adjacent-violators).
void placeRank(std::vector<Item>& items,
               const std::vector<int>& order,
               const std::vector<int>& desire,
               int sep) {
    struct Block {
        long sum;
        long weight;
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
        long w = items[static_cast<size_t>(order[i])].weight;
        blocks.push_back({w * (desire[i] - offset[i]), w, i});
        while (blocks.size() >= 2) {
            const Block& a = blocks[blocks.size() - 2];
            const Block& b = blocks.back();
            if (a.sum * b.weight <= b.sum * a.weight) break;
            Block merged{a.sum + b.sum, a.weight + b.weight, a.first};
            blocks.pop_back();
            blocks.back() = merged;
        }
    }
    for (size_t k = 0; k < blocks.size(); ++k) {
        size_t end = k + 1 < blocks.size() ? blocks[k + 1].first : order.size();
        int at = floorDiv(static_cast<int>(2 * blocks[k].sum + blocks[k].weight),
                          static_cast<int>(2 * blocks[k].weight));
        for (size_t i = blocks[k].first; i < end; ++i)
            items[static_cast<size_t>(order[i])].left = at + offset[i];
    }
}

// Pairs of segments between rank r and rank r + 1 that cross: one starts to the left of the other
// but ends to its right. Segments that share an end never cross.
int gapCrossings(const std::vector<std::vector<int>>& order,
                 const std::vector<int>& pos,
                 const std::vector<std::vector<int>>& down,
                 size_t r) {
    if (r + 1 >= order.size()) return 0;
    std::vector<std::pair<int, int>> segs;
    for (int u : order[r])
        for (int v : down[static_cast<size_t>(u)])
            segs.push_back({pos[static_cast<size_t>(u)], pos[static_cast<size_t>(v)]});
    int crossings = 0;
    for (size_t i = 0; i < segs.size(); ++i)
        for (size_t j = 0; j < segs.size(); ++j)
            if (segs[i].first < segs[j].first && segs[i].second > segs[j].second) ++crossings;
    return crossings;
}

void setPositions(const std::vector<int>& row, std::vector<int>& pos) {
    for (size_t i = 0; i < row.size(); ++i) pos[static_cast<size_t>(row[i])] = static_cast<int>(i);
}

// Reorder one rank by the barycenter of each item's neighbours in the adjacent rank, in positions
// times 1000. An item with no such neighbours keeps its slot; the others are sorted into the
// remaining slots, ties staying in their old order.
void sortByBarycenter(std::vector<std::vector<int>>& order,
                      std::vector<int>& pos,
                      const std::vector<std::vector<int>>& nbr,
                      size_t r) {
    std::vector<int>& row = order[r];
    std::vector<size_t> slots;
    std::vector<std::pair<long, int>> keyed;  // barycenter, then old position
    std::vector<int> movable;
    for (size_t i = 0; i < row.size(); ++i) {
        const std::vector<int>& from = nbr[static_cast<size_t>(row[i])];
        if (from.empty()) continue;
        long sum = 0;
        for (int p : from) sum += pos[static_cast<size_t>(p)];
        slots.push_back(i);
        keyed.push_back({sum * 1000 / static_cast<long>(from.size()), static_cast<int>(i)});
        movable.push_back(row[i]);
    }
    std::vector<size_t> by(keyed.size());
    for (size_t i = 0; i < by.size(); ++i) by[i] = i;
    std::stable_sort(by.begin(), by.end(), [&](size_t a, size_t b) { return keyed[a] < keyed[b]; });
    for (size_t i = 0; i < by.size(); ++i) row[slots[i]] = movable[by[i]];
    setPositions(row, pos);
}

// Choose the order of the items in each rank so that few edges cross: sweeps of the barycenter
// heuristic, each followed by swaps of adjacent items that strictly reduce the crossings. The best
// order seen wins; the initial one on ties.
void reduceCrossings(std::vector<std::vector<int>>& order,
                     size_t itemCount,
                     const std::vector<std::vector<int>>& up,
                     const std::vector<std::vector<int>>& down) {
    const size_t ranks = order.size();
    std::vector<int> pos(itemCount, 0);
    for (const std::vector<int>& row : order) setPositions(row, pos);
    auto total = [&] {
        int sum = 0;
        for (size_t r = 0; r + 1 < ranks; ++r) sum += gapCrossings(order, pos, down, r);
        return sum;
    };
    auto local = [&](size_t r) {  // the crossings that moving items of rank r can change
        return (r > 0 ? gapCrossings(order, pos, down, r - 1) : 0) +
            gapCrossings(order, pos, down, r);
    };
    std::vector<std::vector<int>> best = order;
    int bestCount = total();
    for (int sweep = 0; sweep < 8 && bestCount > 0; ++sweep) {
        if (sweep % 2 == 0) {
            for (size_t r = 1; r < ranks; ++r) sortByBarycenter(order, pos, up, r);
        } else {
            for (size_t r = ranks - 1; r-- > 0;) sortByBarycenter(order, pos, down, r);
        }
        for (bool improved = true; improved;) {
            improved = false;
            for (size_t r = 0; r < ranks; ++r) {
                std::vector<int>& row = order[r];
                for (size_t i = 0; i + 1 < row.size(); ++i) {
                    int before = local(r);
                    std::swap(row[i], row[i + 1]);
                    setPositions(row, pos);
                    if (local(r) < before) {
                        improved = true;
                    } else {
                        std::swap(row[i], row[i + 1]);
                        setPositions(row, pos);
                    }
                }
            }
        }
        int count = total();
        if (count < bestCount) {
            bestCount = count;
            best = order;
        }
    }
    order = best;
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
    // Tighten: move a node a rank toward the side with more edges while every edge on the other
    // side has slack to spare, which shortens the total edge length. Each move shortens it by at
    // least one, so this ends.
    for (bool moved = true; moved;) {
        moved = false;
        for (size_t i = 0; i < n; ++i) {
            int slackIn = INT_MAX, slackOut = INT_MAX;
            for (int k : inE[i]) {
                const Edge& e = g.edges[static_cast<size_t>(k)];
                slackIn = std::min(slackIn, rank[i] - rank[static_cast<size_t>(e.from)] - e.minlen);
            }
            for (int k : outE[i]) {
                const Edge& e = g.edges[static_cast<size_t>(k)];
                slackOut = std::min(slackOut, rank[static_cast<size_t>(e.to)] - rank[i] - e.minlen);
            }
            if (outE[i].size() > inE[i].size() && slackOut >= 1) {
                ++rank[i];
                moved = true;
            } else if (inE[i].size() > outE[i].size() && slackIn >= 1) {
                --rank[i];
                moved = true;
            }
        }
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
                d.weight = kDummyWeight;
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

    // Ports. The edges leaving a node through one port share their first run, and those arriving
    // through one share their last, so strokes joined that way can be followed from any of their
    // ends to any other. That must not show a link the graph does not have. A split (one source,
    // several targets) or a merge (several sources, one target) is safe: the only ends to reach are
    // the real targets, or, against the arrows, other sources. Anything more is not: A->B, A->C and
    // B->C all sharing ports would show B->B, and even a complete A,B->C,D lets A's strokes climb
    // into B's bus and up to B. So the edges are split into stars, each a split or a merge, and a
    // star gets a port of its own on each of its nodes. Greedily, the next star is the largest
    // left, ties going to the one holding the earliest-declared edge, then to splits. A node's
    // first port on each side is centred.
    const size_t m = g.edges.size();
    struct Port {
        std::vector<int> edges;  // in declaration order
    };
    std::vector<Port> ports;
    std::vector<std::vector<int>> outPorts(n), inPorts(n);
    std::vector<int> outPort(m), inPort(m);
    lay.edgeGroup.assign(m, 0);
    {
        std::map<std::pair<int, int>, int> edgeOf;
        for (size_t k = 0; k < m; ++k)
            edgeOf[{g.edges[k].from, g.edges[k].to}] = static_cast<int>(k);
        std::vector<std::set<int>> outLeft(n), inLeft(n);
        for (const Edge& e : g.edges) {
            outLeft[static_cast<size_t>(e.from)].insert(e.to);
            inLeft[static_cast<size_t>(e.to)].insert(e.from);
        }
        for (size_t placed = 0, groups = 0; placed < m; ++groups) {
            std::set<int> bestS, bestT;
            size_t bestSize = 0;
            int bestFirst = INT_MAX;
            for (int side = 0; side < 2; ++side) {
                for (size_t v = 0; v < n; ++v) {
                    const std::set<int>& far = (side == 0 ? outLeft : inLeft)[v];
                    if (far.empty()) continue;
                    const std::set<int> near = {static_cast<int>(v)};
                    const std::set<int>& s = side == 0 ? near : far;
                    const std::set<int>& t = side == 0 ? far : near;
                    int first = INT_MAX;
                    for (int a : s)
                        for (int b : t) first = std::min(first, edgeOf.at({a, b}));
                    size_t size = s.size() * t.size();
                    if (size > bestSize || (size == bestSize && first < bestFirst)) {
                        bestS = s;
                        bestT = t;
                        bestSize = size;
                        bestFirst = first;
                    }
                }
            }
            std::map<int, int> outOf, inOf;
            for (int a : bestS) {
                outOf[a] = static_cast<int>(ports.size());
                outPorts[static_cast<size_t>(a)].push_back(static_cast<int>(ports.size()));
                ports.push_back({});
            }
            for (int b : bestT) {
                inOf[b] = static_cast<int>(ports.size());
                inPorts[static_cast<size_t>(b)].push_back(static_cast<int>(ports.size()));
                ports.push_back({});
            }
            for (int a : bestS) {
                for (int b : bestT) {
                    size_t k = static_cast<size_t>(edgeOf.at({a, b}));
                    outLeft[static_cast<size_t>(a)].erase(b);
                    inLeft[static_cast<size_t>(b)].erase(a);
                    outPort[k] = outOf[a];
                    inPort[k] = inOf[b];
                    lay.edgeGroup[k] = static_cast<int>(groups);
                    ports[static_cast<size_t>(outOf[a])].edges.push_back(static_cast<int>(k));
                    ports[static_cast<size_t>(inOf[b])].edges.push_back(static_cast<int>(k));
                    ++placed;
                }
            }
        }
        for (Port& p : ports) std::sort(p.edges.begin(), p.edges.end());
        // A node's first port is the one holding its earliest-declared edge.
        auto byFirstEdge = [&](int a, int b) {
            return ports[static_cast<size_t>(a)].edges[0] < ports[static_cast<size_t>(b)].edges[0];
        };
        for (size_t v = 0; v < n; ++v) {
            std::sort(outPorts[v].begin(), outPorts[v].end(), byFirstEdge);
            std::sort(inPorts[v].begin(), inPorts[v].end(), byFirstEdge);
        }
    }

    std::vector<std::vector<int>> preds(items.size()), succs(items.size());
    for (const std::vector<int>& c : chain) {
        for (size_t i = 0; i + 1 < c.size(); ++i) {
            preds[static_cast<size_t>(c[i + 1])].push_back(c[i]);
            succs[static_cast<size_t>(c[i])].push_back(c[i + 1]);
        }
    }
    reduceCrossings(order, items.size(), preds, succs);

    // Placement across the flow. Pack each rank, then in one pass from the first rank on, move each
    // item toward the mean centre of the items its edges come from.
    const int sep = td ? tune::kSepTd : tune::kSepLr;
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
    // Refine: alternate passes down and up, moving each item toward the median centre of its
    // neighbours in the rank before (or after) it, until a round moves nothing, or after four. The
    // result is the best placement seen, by total edge slant, the initial one on ties.
    auto slant = [&] {
        long sum = 0;
        for (size_t v = 0; v < items.size(); ++v)
            for (int u : preds[v])
                sum += static_cast<long>(
                           std::max(items[v].weight, items[static_cast<size_t>(u)].weight)) *
                    std::abs(centre(u) - centre(static_cast<int>(v)));
        return sum;
    };
    auto lefts = [&] {
        std::vector<int> at;
        for (const Item& it : items) at.push_back(it.left);
        return at;
    };
    std::vector<int> best = lefts();
    long bestSlant = slant();
    for (int round = 0; round < 4; ++round) {
        const std::vector<int> before = lefts();
        for (int pass = 0; pass < 2; ++pass) {
            const std::vector<std::vector<int>>& nbr = pass == 0 ? preds : succs;
            for (size_t step = 0; step < ranks; ++step) {
                size_t r = pass == 0 ? step : ranks - 1 - step;
                const std::vector<int>& row = order[r];
                std::vector<int> desire(row.size());
                for (size_t i = 0; i < row.size(); ++i) {
                    const Item& it = items[static_cast<size_t>(row[i])];
                    std::vector<int> cs;
                    for (int p : nbr[static_cast<size_t>(row[i])]) cs.push_back(centre(p));
                    if (cs.empty()) {
                        desire[i] = it.left;
                        continue;
                    }
                    std::sort(cs.begin(), cs.end());
                    size_t m = cs.size() / 2;
                    int median = cs.size() % 2 ? cs[m] : floorDiv(cs[m - 1] + cs[m] + 1, 2);
                    desire[i] = floorDiv(median - it.ext + 1, 2);
                }
                placeRank(items, row, desire, sep);
            }
        }
        const std::vector<int> after = lefts();
        if (slant() < bestSlant) {
            bestSlant = slant();
            best = after;
        }
        if (after == before) break;
    }
    for (size_t i = 0; i < items.size(); ++i) items[i].left = best[i];
    int minLeft = INT_MAX, across = 0;
    for (const Item& it : items) minLeft = std::min(minLeft, it.left);
    for (Item& it : items) {
        it.left -= minLeft;
        across = std::max(across, it.left + it.ext);
    }

    // Where each port sits on its side, as an offset from the centre in hu. The ports of one side
    // are ordered by where their edges' far ends lie, so that their runs do not cross, and spaced a
    // cell apart in TD and half a cell in LR; the first port stays centred where there is room.
    std::vector<int> portOff(ports.size(), 0);
    for (size_t side = 0; side < 2; ++side) {
        for (size_t v = 0; v < n; ++v) {
            const std::vector<int>& list = (side == 0 ? outPorts : inPorts)[v];
            if (list.size() < 2) continue;
            std::vector<std::pair<long, int>> far;  // mean far-end centre (x size), port
            for (int p : list) {
                const std::vector<int>& es = ports[static_cast<size_t>(p)].edges;
                long sum = 0;
                for (int k : es) {
                    const std::vector<int>& c = chain[static_cast<size_t>(k)];
                    sum += centre(side == 0 ? c[1] : c[c.size() - 2]);
                }
                far.push_back({sum * 1000 / static_cast<long>(es.size()), p});
            }
            std::stable_sort(far.begin(), far.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            const int step = td ? 2 : 1;
            const int lim = td ? nodeW[v] - 3 : tune::kNodeRows - 2;  // keeps clear of the corners
            int first = 0;
            for (size_t i = 0; i < far.size(); ++i)
                if (far[i].second == list[0]) first = static_cast<int>(i);
            int lo = -first * step, hi = (static_cast<int>(far.size()) - 1 - first) * step;
            int shift = hi > lim ? lim - hi : lo < -lim ? -lim - lo : 0;
            if (lo + shift < -lim || hi + shift > lim) {
                reason = "too many ports";
                return false;
            }
            for (size_t i = 0; i < far.size(); ++i)
                portOff[static_cast<size_t>(far[i].second)] =
                    lo + shift + static_cast<int>(i) * step;
        }
    }
    // An edge end, as the GapSeg sees it: a port of a real node (numbered after the items), or a
    // dummy, and where it lies across the flow in hu.
    auto endId = [&](size_t k, size_t i) {
        const std::vector<int>& c = chain[k];
        if (i == 0) return static_cast<int>(items.size()) + outPort[k];
        if (i + 1 == c.size()) return static_cast<int>(items.size()) + inPort[k];
        return c[i];
    };
    auto endX = [&](size_t k, size_t i) {
        const std::vector<int>& c = chain[k];
        int x = centre(c[i]);
        if (i == 0) x += portOff[static_cast<size_t>(outPort[k])];
        if (i + 1 == c.size()) x += portOff[static_cast<size_t>(inPort[k])];
        return x;
    };

    // Position along the flow. A rank is as thick as its thickest node (1 when it holds only
    // dummies). Between two ranks lies a gap that holds one cell for each track, the lines along
    // which edges jog sideways, plus one cell where the edges leave and one where they arrive.
    std::vector<int> thick(ranks, 1);
    for (size_t i = 0; i < n; ++i) {
        size_t r = static_cast<size_t>(rank[i]);
        thick[r] = std::max(thick[r], td ? tune::kNodeRows : nodeW[i]);
    }
    std::vector<std::vector<GapSeg>> gapSegs(ranks);
    for (size_t k = 0; k < m; ++k) {
        const std::vector<int>& c = chain[k];
        for (size_t i = 0; i + 1 < c.size(); ++i)
            gapSegs[static_cast<size_t>(items[static_cast<size_t>(c[i])].rank)].push_back(
                {endId(k, i), endId(k, i + 1), endX(k, i), endX(k, i + 1)});
    }
    std::vector<int> start(ranks, 0);
    std::map<std::pair<int, int>, int> trackOf;  // by the ends of a segment
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
    // facing side, each through its port.
    lay.edgeLabel.assign(g.edges.size(), CellBox());
    lay.edgePath.assign(g.edges.size(), std::vector<Pt>());
    for (size_t k = 0; k < g.edges.size(); ++k) {
        const std::vector<int>& c = chain[k];
        size_t from = static_cast<size_t>(g.edges[k].from), to = static_cast<size_t>(g.edges[k].to);
        std::vector<Pt> path;
        int leave =
            2 * (start[static_cast<size_t>(rank[from])] + (td ? tune::kNodeRows : nodeW[from])) - 1;
        path.push_back(point(endX(k, 0), leave));
        for (size_t i = 0; i + 1 < c.size(); ++i) {
            int a = endX(k, i), b = endX(k, i + 1);
            if (a == b) continue;
            int t = track(static_cast<size_t>(items[static_cast<size_t>(c[i])].rank),
                          trackOf.at({endId(k, i), endId(k, i + 1)}));
            path.push_back(point(a, t));
            path.push_back(point(b, t));
        }
        path.push_back(point(endX(k, c.size() - 1), 2 * start[static_cast<size_t>(rank[to])] + 1));
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
    // Where a path bends, edges sharing a port split or merge: a junction, not a crossing, even
    // where another edge of the bus runs straight through it.
    std::set<Pt> bends;
    for (const std::vector<Pt>& path : lay.edgePath)
        for (size_t i = 1; i + 1 < path.size(); ++i) bends.insert(path[i]);
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
                        y > std::min(v0.second, v1.second) && y < std::max(v0.second, v1.second) &&
                        !bends.count({x, y}))
                        found.insert({x, y});
                }
            }
        }
    }
    return std::vector<Pt>(found.begin(), found.end());
}

}  // namespace mermaid
