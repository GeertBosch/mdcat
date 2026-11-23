#include <algorithm>
#include <climits>
#include <functional>
#include <map>
#include <set>

#include "mermaid_model.h"
#include "mermaid_shape.h"
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
// ranks passes through, which is also where an edge's label sits.
struct Item {
    int rank = 0;
    int ext = 1;     // extent across the flow, in cells
    int pad = 0;     // the cells at the far end of `ext` that hold a self-loop, not the node
    int skew = 0;    // how far the item's center lies from the middle of its extent, in hu
    int thick = 1;   // extent along the flow, in cells (of a label; nodes have their own)
    int left = 0;    // start across the flow, in cells
    int weight = 1;  // how strongly placement honours its desired position
    // The subgraph that holds the item, or -1: a member, the dummies and label of an edge between
    // two members, and the cluster's own border items. A border item (1: left, 2: right) stands
    // for one cell of the box's side on a rank the box spans, so that ordering and placement keep
    // the box's sides straight and everything else out of it.
    int cluster = -1;
    int border = 0;
    // A border item: the blank cells between it and what lies inside the box, after a left border
    // (the top of a left-to-right box, which has the title's lines too) and before a right one.
    int gap = 0;
};

// The separation between neighbouring items: `base` between footprints, and a cluster's border is
// as many blank cells from what lies inside the box as its `gap` says, whether that is a member,
// the border of a box inside it, or its other border.
int separation(const std::vector<Item>& items, int base, int a, int b) {
    const Item &x = items[static_cast<size_t>(a)], &y = items[static_cast<size_t>(b)];
    if (x.border == 1 && y.border == 2 && x.cluster == y.cluster) return std::max(x.gap, y.gap);
    if (x.border == 1) return x.gap;
    if (y.border == 2) return y.gap;
    return base;
}

// Place the items of one rank, in order, as close as possible to their desired starts (in cells)
// without overlapping or reordering: each item is put at its desired position, and neighbours that
// would overlap merge into a block placed at the weighted mean of their desires
// (pool-adjacent-violators).
void placeRank(std::vector<Item>& items,
               const std::vector<int>& order,
               const std::vector<int>& desire,
               int base) {
    struct Block {
        long sum;
        long weight;
        size_t first;
    };
    std::vector<int> offset(order.size());
    int off = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        offset[i] = off;
        off += items[static_cast<size_t>(order[i])].ext +
            (i + 1 < order.size() ? separation(items, base, order[i], order[i + 1]) : base);
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

// A run of a rank that moves as one when it is sorted: an item that is no cluster's border, or a
// cluster's block, from its left border to its right one, and what lies between.
struct Entry {
    size_t begin, end;
    bool block;
};

// The entries of `row` from `begin` to `end`, which is a whole rank or what lies inside a block.
std::vector<Entry> entriesOf(const std::vector<int>& row,
                             const std::vector<Item>& items,
                             size_t begin,
                             size_t end) {
    std::vector<Entry> list;
    for (size_t i = begin; i < end;) {
        const Item& it = items[static_cast<size_t>(row[i])];
        size_t j = i + 1;
        if (it.border == 1) {
            while (j < end &&
                   !(items[static_cast<size_t>(row[j - 1])].border == 2 &&
                     items[static_cast<size_t>(row[j - 1])].cluster == it.cluster))
                ++j;
        }
        list.push_back({i, j, it.border == 1});
        i = j;
    }
    return list;
}

// Make each cluster's items contiguous in a rank, and a cluster inside another within its block:
// its left border, its members and the blocks inside it in their current order, its right border.
// A block takes the place of its first member (or of the first member of the block inside it); one
// with no member (a rank of the box's span that holds none) goes last, in the order of the
// clusters.
void groupBlocks(std::vector<int>& row,
                 const std::vector<Item>& items,
                 const std::vector<Cluster>& clusters) {
    std::map<int, int> left, right;
    for (int id : row) {
        const Item& it = items[static_cast<size_t>(id)];
        if (it.border) (it.border == 1 ? left : right)[it.cluster] = id;
    }
    // The cluster directly inside `parent` that holds cluster `c`.
    auto childOf = [&](int parent, int c) {
        while (clusters[static_cast<size_t>(c)].parent != parent)
            c = clusters[static_cast<size_t>(c)].parent;
        return c;
    };
    std::function<void(int, const std::vector<int>&, std::vector<int>&)> build =
        [&](int parent, const std::vector<int>& ids, std::vector<int>& out) {
            std::vector<int> seq;  // an item, or -1 - c for the block of cluster c
            std::map<int, std::vector<int>> within;
            std::set<int> marked;
            for (int id : ids) {
                const Item& it = items[static_cast<size_t>(id)];
                if (it.border && it.cluster == parent) continue;  // the caller places these
                if (it.cluster == parent) {
                    seq.push_back(id);
                    continue;
                }
                const int child = childOf(parent, it.cluster);
                within[child].push_back(id);
                if (!it.border && marked.insert(child).second) seq.push_back(-1 - child);
            }
            for (const auto& [cl, id] : left)
                if (clusters[static_cast<size_t>(cl)].parent == parent && !marked.count(cl))
                    seq.push_back(-1 - cl);
            for (int id : seq) {
                if (id >= 0) {
                    out.push_back(id);
                    continue;
                }
                const int cl = -1 - id;
                out.push_back(left.at(cl));
                build(cl, within[cl], out);
                out.push_back(right.at(cl));
            }
        };
    std::vector<int> out;
    build(-1, row, out);
    row = out;
}

// The order that sorting units by key puts them in, as the unit that goes in each slot: a unit
// with no key keeps its slot, the others are sorted into the remaining slots, ties staying in
// their old order.
std::vector<size_t> sortedSlots(const std::vector<std::pair<bool, long>>& key) {
    std::vector<size_t> slots, movable;
    for (size_t i = 0; i < key.size(); ++i) {
        if (!key[i].first) continue;
        slots.push_back(i);
        movable.push_back(i);
    }
    std::stable_sort(movable.begin(), movable.end(), [&](size_t a, size_t b) {
        return key[a].second < key[b].second;
    });
    std::vector<size_t> out(key.size());
    for (size_t i = 0; i < out.size(); ++i) out[i] = i;
    for (size_t i = 0; i < slots.size(); ++i) out[slots[i]] = movable[i];
    return out;
}

// Reorder one rank by the barycenter of each item's neighbours in the adjacent rank, in positions
// times 1000. An item with no such neighbours keeps its slot; the others are sorted into the
// remaining slots, ties staying in their old order. Clusters stay contiguous, at every level: the
// entries inside each block are sorted among themselves, innermost first, and then the entries of
// the rank, a block keyed by the mean barycenter of its items, among theirs.
void sortByBarycenter(std::vector<std::vector<int>>& order,
                      std::vector<int>& pos,
                      const std::vector<std::vector<int>>& nbr,
                      const std::vector<Item>& items,
                      size_t r) {
    std::vector<int>& row = order[r];
    auto keyOf = [&](int id) -> std::pair<bool, long> {
        const std::vector<int>& from = nbr[static_cast<size_t>(id)];
        if (from.empty()) return {false, 0};
        long sum = 0;
        for (int p : from) sum += pos[static_cast<size_t>(p)];
        return {true, sum * 1000 / static_cast<long>(from.size())};
    };
    std::function<void(size_t, size_t)> sortRange = [&](size_t begin, size_t end) {
        const std::vector<Entry> entries = entriesOf(row, items, begin, end);
        for (const Entry& e : entries)
            if (e.block) sortRange(e.begin + 1, e.end - 1);
        std::vector<std::pair<bool, long>> key;
        for (const Entry& e : entries) {
            long sum = 0, count = 0;
            for (size_t i = e.begin; i < e.end; ++i) {
                const std::pair<bool, long> k = keyOf(row[i]);
                if (!k.first) continue;
                sum += k.second;
                ++count;
            }
            key.push_back(count ? std::pair<bool, long>{true, sum / count}
                                : std::pair<bool, long>{false, 0});
        }
        const std::vector<size_t> by = sortedSlots(key);
        std::vector<int> sorted;
        for (size_t e : by)
            sorted.insert(sorted.end(),
                          row.begin() + static_cast<long>(entries[e].begin),
                          row.begin() + static_cast<long>(entries[e].end));
        std::copy(sorted.begin(), sorted.end(), row.begin() + static_cast<long>(begin));
    };
    sortRange(0, row.size());
    setPositions(row, pos);
}

// Choose the order of the items in each rank so that few edges cross: sweeps of the barycenter
// heuristic, each followed by swaps of adjacent items that strictly reduce the crossings. The best
// order seen wins; the initial one on ties. A swap exchanges two items of one cluster (or of none),
// or two entries of a rank (or of a block) when one is a cluster's block, so the blocks stay
// contiguous.
void reduceCrossings(std::vector<std::vector<int>>& order,
                     const std::vector<Item>& items,
                     const std::vector<std::vector<int>>& up,
                     const std::vector<std::vector<int>>& down) {
    const size_t ranks = order.size();
    std::vector<int> pos(items.size(), 0);
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
    auto sameLevel = [&](int a, int b) {
        const Item &x = items[static_cast<size_t>(a)], &y = items[static_cast<size_t>(b)];
        return x.cluster == y.cluster && !x.border && !y.border;
    };
    bool improved = false;
    // Exchange neighbouring entries of a rank, from `begin` to `end`, when that reduces the
    // crossings and one of them is a block, then do the same inside each block.
    std::function<void(size_t, size_t, size_t)> rotateRange =
        [&](size_t r, size_t begin, size_t end) {
            std::vector<int>& row = order[r];
            std::vector<Entry> entries = entriesOf(row, items, begin, end);
            for (size_t e = 0; e + 1 < entries.size(); ++e) {
                if (!entries[e].block && !entries[e + 1].block) continue;
                auto first = row.begin() + static_cast<long>(entries[e].begin);
                auto mid = row.begin() + static_cast<long>(entries[e + 1].begin);
                auto last = row.begin() + static_cast<long>(entries[e + 1].end);
                int before = local(r);
                std::rotate(first, mid, last);
                setPositions(row, pos);
                if (local(r) < before) {
                    improved = true;
                    entries = entriesOf(row, items, begin, end);
                } else {
                    std::rotate(first, first + (last - mid), last);
                    setPositions(row, pos);
                }
            }
            for (const Entry& e : entriesOf(row, items, begin, end))
                if (e.block) rotateRange(r, e.begin + 1, e.end - 1);
        };
    std::vector<std::vector<int>> best = order;
    int bestCount = total();
    for (int sweep = 0; sweep < 8 && bestCount > 0; ++sweep) {
        if (sweep % 2 == 0) {
            for (size_t r = 1; r < ranks; ++r) sortByBarycenter(order, pos, up, items, r);
        } else {
            for (size_t r = ranks - 1; r-- > 0;) sortByBarycenter(order, pos, down, items, r);
        }
        for (improved = true; improved;) {
            improved = false;
            for (size_t r = 0; r < ranks; ++r) {
                std::vector<int>& row = order[r];
                for (size_t i = 0; i + 1 < row.size(); ++i) {
                    if (!sameLevel(row[i], row[i + 1])) continue;
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
                rotateRange(r, 0, row.size());
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

// Flip the layout along its rows (`rows`) or its columns, so that the flow runs against the axis it
// was laid out along. Boxes keep their size, and a hu coordinate x becomes 2 * extent - x.
void mirror(Layout& lay, bool rows) {
    const int extent = rows ? lay.rows : lay.cols;
    auto flip = [&](CellBox& b) {
        if (b.cols == 0) return;
        (rows ? b.row : b.col) = extent - (rows ? b.row + b.rows : b.col + b.cols);
    };
    for (CellBox& b : lay.nodeFoot) flip(b);
    for (CellBox& b : lay.nodeLabel) flip(b);
    for (CellBox& b : lay.edgeLabel) flip(b);
    for (CellBox& b : lay.clusterBox) flip(b);
    // clusterLabel is not flipped: it is only placed once the layout has been mirrored, as a title
    // is text and stays below its box's top border as drawn.
    for (std::vector<Pt>& path : lay.edgePath)
        for (Pt& p : path) (rows ? p.second : p.first) = 2 * extent - (rows ? p.second : p.first);
}

// Make room for a title `width` columns wide: a row for it and a blank one, at the top, and
// columns on either side if it is wider than the diagram, which is then centred under it.
void addTitle(Layout& lay, int width) {
    const int dx = std::max(0, (width - lay.cols) / 2), dy = 2;
    for (std::vector<CellBox>* boxes :
         {&lay.nodeFoot, &lay.nodeLabel, &lay.edgeLabel, &lay.clusterBox, &lay.clusterLabel}) {
        for (CellBox& b : *boxes) {
            if (b.cols == 0) continue;
            b.col += dx;
            b.row += dy;
        }
    }
    for (std::vector<Pt>& path : lay.edgePath) {
        for (Pt& p : path) {
            p.first += 2 * dx;
            p.second += 2 * dy;
        }
    }
    lay.cols = std::max(lay.cols, width);
    lay.rows += dy;
    lay.titleRows = dy;
}

// One pass of the layout. `grow` holds, per cluster, how many columns its box is asked to grow on
// its left and on its right beyond its natural width (both 0: as its title needs, on both sides).
// A title that finds no place clear of the edges that cross its rows makes the pass fail with
// `retry` set and `grow` changed to what makes room: the caller runs the pass once more, with
// `again` set, and a title that still has no place then declines the diagram. A box with an edge
// lined up with its border (see alignedWithBorder) is moved a cell across the flow, `nudge` holds
// by how much, and the pass is run again.
bool layoutOnce(const Graph& original,
                const Env& env,
                const Fit& fit,
                std::vector<std::pair<int, int>>& grow,
                std::vector<int>& nudge,
                bool& again,
                bool& retry,
                Layout& lay,
                std::string& reason,
                Decline& why) {
    const size_t n = original.nodes.size();
    // Subgraphs are laid out in every direction, nested up to kMaxClusterDepth deep.
    for (const Cluster& c : original.clusters) {
        int depth = 1;
        for (int p = c.parent; p >= 0; p = original.clusters[static_cast<size_t>(p)].parent)
            ++depth;
        if (depth > tune::kMaxClusterDepth) {
            reason = "subgraph layout: depth";
            return false;
        }
    }
    if (original.edges.empty()) {
        reason = "no edges";
        return false;
    }
    if (static_cast<int>(n) > tune::kMaxNodes) {
        reason = "too many nodes";
        return false;
    }
    // Like mmdc, draw only the last of several self-loops on a node, whatever their labels: the
    // others are dropped, by making them invisible links, which are not drawn.
    Graph source = original;
    {
        std::map<int, size_t> last;
        for (size_t k = 0; k < source.edges.size(); ++k) {
            const Edge& e = source.edges[k];
            if (e.from == e.to && e.line != Line::Invisible) last[e.from] = k;
        }
        for (size_t k = 0; k < source.edges.size(); ++k) {
            Edge& e = source.edges[k];
            if (e.from == e.to && e.line != Line::Invisible && last[e.from] != k)
                e.line = Line::Invisible;
        }
    }
    // A self-loop is no part of the ranked graph: it is drawn beside its node (see below).
    auto selfLoop = [&](size_t k) { return source.edges[k].from == source.edges[k].to; };

    // Cycles: a depth-first search from the nodes in order of first appearance, along the edges in
    // declaration order, finds the back edges, those that lead to a node still on the stack. Like
    // Dagre, the layout reverses them, so that it works on an acyclic graph, and each is drawn
    // from its real source to its real target at the end (see below).
    std::vector<char> reversed(source.edges.size(), 0);
    {
        std::vector<std::vector<int>> out(n);
        for (size_t k = 0; k < source.edges.size(); ++k)
            if (!selfLoop(k))
                out[static_cast<size_t>(source.edges[k].from)].push_back(static_cast<int>(k));
        std::vector<char> state(n, 0);  // 1: on the stack, 2: done
        std::function<void(size_t)> dfs = [&](size_t u) {
            state[u] = 1;
            for (int k : out[u]) {
                const size_t v = static_cast<size_t>(source.edges[static_cast<size_t>(k)].to);
                if (state[v] == 1)
                    reversed[static_cast<size_t>(k)] = 1;
                else if (state[v] == 0)
                    dfs(v);
            }
            state[u] = 2;
        };
        for (size_t u = 0; u < n; ++u)
            if (state[u] == 0) dfs(u);
    }
    int backEdges = 0;
    for (size_t k = 0; k < reversed.size(); ++k)
        if (reversed[k] && source.edges[k].line != Line::Invisible) ++backEdges;
    const int gateCols = std::max(env.availCols, tune::kMinGateCols);
    const int maxBack = gateCols / tune::kColsPerBackEdge;
    if (backEdges > maxBack) {
        reason = "back edges: " + std::to_string(backEdges) + " > " + std::to_string(maxBack);
        why = Decline::TooComplex;
        return false;
    }
    // From here on `g` is the graph that is laid out: a reversed edge runs from its real target
    // down to its real source, with its ends' markers swapped to match.
    Graph g = source;
    for (size_t k = 0; k < reversed.size(); ++k) {
        if (!reversed[k]) continue;
        Edge& e = g.edges[k];
        std::swap(e.from, e.to);
        std::swap(e.head, e.tail);
    }
    // BT and RL are laid out as TB and LR, and mirrored along the flow at the end.
    const bool td = g.dir == Dir::TB || g.dir == Dir::BT;
    // The clusters form a forest. `parentOf` steps up it, `within` says whether cluster `c` is
    // `of` or lies inside it (-1 is no cluster, which holds everything), and `lowestCommon` is the
    // deepest cluster that holds both of two clusters, or -1.
    auto parentOf = [&](int c) { return g.clusters[static_cast<size_t>(c)].parent; };
    auto within = [&](int c, int of) {
        for (; c >= 0; c = parentOf(c))
            if (c == of) return true;
        return of < 0;
    };
    auto lowestCommon = [&](int a, int b) {
        for (; a >= 0; a = parentOf(a))
            if (within(b, a)) return a;
        return -1;
    };

    // Edge labels, as in Dagre: every rank and minimum length is doubled, so that a labelled edge
    // has a rank halfway along it. Its label is an item there, which ordering and placement treat
    // like any other, so the labels get room without a search for it. Nodes stay on even ranks.
    auto labelWidth = [&](const Edge& e) {
        int w = 0;
        for (const std::string& line : e.label.lines) w = std::max(w, env.width(line));
        return w;
    };
    bool labelled = false;
    for (size_t k = 0; k < g.edges.size(); ++k) {
        const Edge& e = g.edges[k];
        if (e.label.lines.empty()) continue;
        // A self-loop's label sits beside the loop, not in a rank of its own.
        if (!selfLoop(k)) labelled = true;
        if (labelWidth(e) > tune::kMaxLabelWidth) {
            reason = "label too wide: " + std::to_string(labelWidth(e)) + " > " +
                std::to_string(tune::kMaxLabelWidth);
            return false;
        }
    }
    const int unit = labelled ? 2 : 1;

    std::vector<std::vector<int>> outE(n), inE(n);  // edge indices, in declaration order
    for (size_t k = 0; k < g.edges.size(); ++k) {
        if (selfLoop(k)) continue;
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
            rank[v] = std::max(rank[v], rank[u] + unit * e.minlen);
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
        if (outE[i].empty()) continue;  // a node with no edges stays in the first rank
        int r = INT_MAX;
        for (int k : outE[i]) {
            const Edge& e = g.edges[static_cast<size_t>(k)];
            r = std::min(r, rank[static_cast<size_t>(e.to)] - unit * e.minlen);
        }
        rank[i] = r;
    }
    // Tighten: move a node a rank (two, when labelled) toward the side with more edges while every
    // edge on the other side has slack to spare, which shortens the total edge length. Each move
    // shortens it by at least one, so this ends.
    for (bool moved = true; moved;) {
        moved = false;
        for (size_t i = 0; i < n; ++i) {
            int slackIn = INT_MAX, slackOut = INT_MAX;
            for (int k : inE[i]) {
                const Edge& e = g.edges[static_cast<size_t>(k)];
                slackIn = std::min(slackIn,
                                   rank[i] - rank[static_cast<size_t>(e.from)] - unit * e.minlen);
            }
            for (int k : outE[i]) {
                const Edge& e = g.edges[static_cast<size_t>(k)];
                slackOut =
                    std::min(slackOut, rank[static_cast<size_t>(e.to)] - rank[i] - unit * e.minlen);
            }
            if (outE[i].size() > inE[i].size() && slackOut >= unit) {
                rank[i] += unit;
                moved = true;
            } else if (inE[i].size() > outE[i].size() && slackIn >= unit) {
                rank[i] -= unit;
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

    // Footprints of the real nodes, in cells: (widest line + 4) x (lines + 2) for a box, and what
    // each shape needs for its outline to clear the label (see shape::footprint). For top-down the
    // width is made odd so that every footprint's center lies on a whole column and arrows can be
    // exactly vertical.
    std::vector<int> nodeW(n), nodeH(n), labelW(n), labelRow(n);
    std::vector<Item> items(n);
    for (size_t i = 0; i < n; ++i) {
        for (const std::string& line : g.nodes[i].label.lines)
            labelW[i] = std::max(labelW[i], env.width(line));
        const shape::Footprint f = shape::footprint(g.nodes[i].shape,
                                                    labelW[i],
                                                    static_cast<int>(g.nodes[i].label.lines.size()),
                                                    td,
                                                    std::max(1, env.cellW),
                                                    std::max(1, env.cellH),
                                                    fit.nodePad);
        nodeW[i] = f.cols;
        nodeH[i] = f.rows;
        labelRow[i] = f.labelRow;
        items[i].rank = rank[i];
        items[i].cluster = g.nodes[i].cluster;
        items[i].ext = td ? nodeW[i] : nodeH[i];
        // Across the flow of a left-to-right graph a node is centred on its label, which a
        // cylinder's lid pushes off the middle of its footprint, so that edges meet its sides
        // level with the labels of the nodes next to it. Top-down, the footprint of a node is
        // an odd number of columns wide, to put the edges on a whole column; but a footprint of
        // even width is centred on the line between two, and its edges go half a column right.
        items[i].skew = td
            ? nodeW[i] % 2 == 0 ? 1 : 0
            : 2 * f.labelRow + static_cast<int>(g.nodes[i].label.lines.size()) - f.rows;
    }
    // A self-loop is drawn beside its node, to the right of it top-down and below it left to right,
    // in cells the node's item holds after the node itself. The loop runs out from the outline and
    // back in, far enough for its arrowhead and a rounded corner; its label, if any, sits beyond
    // it. Left to right the label is centred under the node, and a wider one pushes the node along
    // its rank.
    std::vector<int> loopEdge(n, -1), loopLabelW(n, 0);
    for (size_t k = 0; k < g.edges.size(); ++k)
        if (selfLoop(k) && g.edges[k].line != Line::Invisible)
            loopEdge[static_cast<size_t>(g.edges[k].from)] = static_cast<int>(k);
    const int loopCells = [&] {
        const int cell = std::max(1, td ? env.cellW : env.cellH);
        return std::max(1, (2 * std::max(6, env.cellW) + cell - 1) / cell);
    }();
    std::vector<int> off(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (loopEdge[i] < 0) continue;
        const Edge& e = g.edges[static_cast<size_t>(loopEdge[i])];
        const int lines = static_cast<int>(e.label.lines.size());
        if (td && lines > nodeH[i]) {
            reason = "self-loop label too tall";
            return false;
        }
        loopLabelW[i] = labelWidth(e);
        items[i].pad = loopCells + (td ? loopLabelW[i] : lines);
        items[i].ext += items[i].pad;
        if (!td) off[i] = std::max(0, (loopLabelW[i] - nodeW[i]) / 2);
    }
    // Along the flow of a top-down graph, nodes of a rank are set down so that their labels start
    // on one row, as far down their band as the node whose label sits lowest. Mirroring turns a
    // node upside down in its band, so for bottom-up graphs it is the rows below the label that
    // line up before it. (Done once the ports are known, as they may widen a node.)

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
    std::vector<int> labelItem(g.edges.size(), -1);       // the item holding an edge's label
    std::vector<char> visited(n, 0);
    std::function<void(size_t)> visit = [&](size_t u) {
        visited[u] = 1;
        order[static_cast<size_t>(rank[u])].push_back(static_cast<int>(u));
        for (int k : outE[u]) {
            size_t v = static_cast<size_t>(g.edges[static_cast<size_t>(k)].to);
            std::vector<int>& c = chain[static_cast<size_t>(k)];
            c.push_back(static_cast<int>(u));
            const Edge& edge = g.edges[static_cast<size_t>(k)];
            const int mid = rank[u] + (rank[v] - rank[u]) / 2;
            // An edge inside a cluster stays inside it, and between two clusters it passes outside
            // both, inside the one that holds both (the lowest common cluster) if there is one.
            const int common = lowestCommon(g.nodes[u].cluster, g.nodes[v].cluster);
            for (int r = rank[u] + 1; r < rank[v]; ++r) {
                Item d;
                d.rank = r;
                d.weight = kDummyWeight;
                d.cluster = common;
                if (!edge.label.lines.empty() && r == mid) {
                    const int lines = static_cast<int>(edge.label.lines.size());
                    int w = labelWidth(edge);
                    if (td && w % 2 == 0) ++w;  // so that the edge can run through its middle
                    d.ext = td ? w : lines;
                    d.thick = td ? lines : labelWidth(edge);
                    labelItem[static_cast<size_t>(k)] = static_cast<int>(items.size());
                }
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

    // Border items. A cluster spans the ranks from its first member's to its last member's (a
    // member of a cluster inside it counts), and on each of them has a left and a right border
    // item, which ordering and placement tie to those of the ranks before and after as if by an
    // edge. A rank of the span that holds no member gets them too, so that nothing from outside
    // lands inside the box there.
    const size_t clusterCount = g.clusters.size();
    std::vector<int> titleW(clusterCount, 0), titleL(clusterCount, 0);  // columns, lines
    for (size_t c = 0; c < clusterCount; ++c) {
        for (const std::string& line : g.clusters[c].title.lines)
            titleW[c] = std::max(titleW[c], env.width(line));
        titleL[c] = static_cast<int>(g.clusters[c].title.lines.size());
    }
    std::vector<int> spanLo(clusterCount, INT_MAX), spanHi(clusterCount, -1);
    // A cluster spans the ranks of every node inside it, those of the clusters inside it too.
    for (size_t i = 0; i < n; ++i) {
        for (int c = g.nodes[i].cluster; c >= 0; c = parentOf(c)) {
            spanLo[static_cast<size_t>(c)] = std::min(spanLo[static_cast<size_t>(c)], rank[i]);
            spanHi[static_cast<size_t>(c)] = std::max(spanHi[static_cast<size_t>(c)], rank[i]);
        }
    }
    std::vector<std::vector<int>>
        borderPairs;  // consecutive border items: ties for order/placement
    std::vector<int> boxLeft(clusterCount, -1), boxRight(clusterCount, -1);  // first rank's items
    for (size_t c = 0; c < clusterCount; ++c) {
        int before[2] = {-1, -1};
        for (int r = spanLo[c]; r <= spanHi[c]; ++r) {
            for (int side = 1; side <= 2; ++side) {
                Item b;
                b.rank = r;
                b.weight = kDummyWeight;
                b.cluster = static_cast<int>(c);
                b.border = side;
                // Top-down a border is a column from what lies inside; left to right it is a row
                // from it, and the left border's (the box's top) also holds the title, which is
                // text and so sits across the flow there, with a blank row after it.
                b.gap = td ? tune::kClusterPad - 1
                           : tune::kClusterPadLr - 1 + (side == 1 ? titleL[c] : 0);
                items.push_back(b);
                const int id = static_cast<int>(items.size()) - 1;
                order[static_cast<size_t>(r)].push_back(id);
                if (before[side - 1] >= 0) borderPairs.push_back({before[side - 1], id});
                if (r == spanLo[c]) (side == 1 ? boxLeft : boxRight)[c] = id;
                before[side - 1] = id;
            }
        }
    }
    if (clusterCount) {
        for (std::vector<int>& row : order) groupBlocks(row, items, g.clusters);
    }

    // Ports. The edges leaving a node through one port share their first run, and those arriving
    // through one share their last, so strokes joined that way can be followed from any of their
    // ends to any other. That must not show a link the graph does not have. A split (one source,
    // several targets) or a merge (several sources, one target) is safe: the only ends to reach are
    // the real targets, or, against the arrows, other sources. Anything more is not: A->B, A->C and
    // B->C all sharing ports would show B->B, and even a complete A,B->C,D lets A's strokes climb
    // into B's bus and up to B. So the edges are split into stars, each a split or a merge, and a
    // star gets a port of its own on each of its nodes. A star's edges all have one style, so
    // edges of different styles never share a bus. Labels don't matter: each sits on its own edge.
    // Invisible links have no ports: they rank and order the nodes but are not routed. Greedily,
    // the next star is the largest left, ties going to the one holding the earliest-declared edge,
    // then to splits. A node's first port on each side is centred. An edge that is a group of its
    // own, a back edge (which the layout reversed) or a repeat of an earlier edge between the same
    // two nodes in the same direction, has a port of its own at each end, after the others.
    const size_t m = g.edges.size();
    std::vector<char> solo(m, 0);
    {
        std::set<std::pair<int, int>> had;
        for (size_t k = 0; k < m; ++k) {
            const Edge& e = g.edges[k];
            if (e.line == Line::Invisible || selfLoop(k)) continue;
            solo[k] = reversed[k] || !had.insert({e.from, e.to}).second;
        }
    }
    struct Port {
        std::vector<int> edges;  // in declaration order
    };
    std::vector<Port> ports;
    std::vector<std::vector<int>> outPorts(n), inPorts(n);
    std::vector<int> outPort(m), inPort(m);
    lay.edgeGroup.assign(m, -1);
    {
        std::map<std::pair<int, int>, int> edgeOf;
        size_t routed = 0;
        std::vector<std::set<int>> outLeft(n), inLeft(n);
        for (size_t k = 0; k < m; ++k) {
            const Edge& e = g.edges[k];
            if (e.line == Line::Invisible || selfLoop(k) || solo[k]) continue;
            edgeOf[{e.from, e.to}] = static_cast<int>(k);
            outLeft[static_cast<size_t>(e.from)].insert(e.to);
            inLeft[static_cast<size_t>(e.to)].insert(e.from);
            ++routed;
        }
        auto styleOf = [&](int a, int b) {
            const int k = edgeOf.at({a, b});
            const Edge& e = g.edges[static_cast<size_t>(k)];
            return (static_cast<int>(e.line) * 4 + static_cast<int>(e.head)) * 4 +
                static_cast<int>(e.tail);
        };
        size_t groups = 0;
        for (size_t placed = 0; placed < routed; ++groups) {
            std::set<int> bestS, bestT;
            size_t bestSize = 0;
            int bestFirst = INT_MAX;
            for (int side = 0; side < 2; ++side) {
                for (size_t v = 0; v < n; ++v) {
                    const std::set<int>& all = (side == 0 ? outLeft : inLeft)[v];
                    std::set<int> styles;
                    for (int w : all)
                        styles.insert(side == 0 ? styleOf(static_cast<int>(v), w)
                                                : styleOf(w, static_cast<int>(v)));
                    for (int style : styles) {
                        std::set<int> far;
                        for (int w : all)
                            if ((side == 0 ? styleOf(static_cast<int>(v), w)
                                           : styleOf(w, static_cast<int>(v))) == style)
                                far.insert(w);
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
            }
            // A decision's outgoing edges each leave through a port of their own, never a bus.
            const bool fan = bestS.size() == 1 && bestT.size() > 1 &&
                g.nodes[static_cast<size_t>(*bestS.begin())].shape == Shape::Rhombus;
            std::map<int, int> outOf, inOf;
            for (int a : bestS) {
                if (fan) break;
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
                    if (fan) {
                        outOf[a] = static_cast<int>(ports.size());
                        outPorts[static_cast<size_t>(a)].push_back(outOf[a]);
                        ports.push_back({});
                    }
                    outPort[k] = outOf[a];
                    inPort[k] = inOf[b];
                    lay.edgeGroup[k] = static_cast<int>(fan ? groups++ : groups);
                    ports[static_cast<size_t>(outOf[a])].edges.push_back(static_cast<int>(k));
                    ports[static_cast<size_t>(inOf[b])].edges.push_back(static_cast<int>(k));
                    ++placed;
                }
            }
        }
        // A solo edge is a group of its own, with a port of its own on each node, so that it never
        // shares strokes with the other edges between the same two nodes. A self-loop is one too,
        // though it has no ports.
        for (size_t k = 0; k < m; ++k) {
            if (selfLoop(k) && g.edges[k].line != Line::Invisible)
                lay.edgeGroup[k] = static_cast<int>(groups++);
            if (!solo[k]) continue;
            const Edge& e = g.edges[k];
            outPort[k] = static_cast<int>(ports.size());
            outPorts[static_cast<size_t>(e.from)].push_back(outPort[k]);
            ports.push_back({{static_cast<int>(k)}});
            inPort[k] = static_cast<int>(ports.size());
            inPorts[static_cast<size_t>(e.to)].push_back(inPort[k]);
            ports.push_back({{static_cast<int>(k)}});
            lay.edgeGroup[k] = static_cast<int>(groups++);
        }
        for (Port& p : ports) std::sort(p.edges.begin(), p.edges.end());
        // A node's first port is the one holding its earliest-declared edge, and a solo edge's
        // port comes after those of the others.
        auto byFirstEdge = [&](int a, int b) {
            auto key = [&](int p) {
                const int k = ports[static_cast<size_t>(p)].edges[0];
                return k + (solo[static_cast<size_t>(k)] ? static_cast<int>(m) : 0);
            };
            return key(a) < key(b);
        };
        for (size_t v = 0; v < n; ++v) {
            std::sort(outPorts[v].begin(), outPorts[v].end(), byFirstEdge);
            std::sort(inPorts[v].begin(), inPorts[v].end(), byFirstEdge);
        }
    }

    // A node with many ports on a side is made wider (taller, left to right), by two cells at a
    // time to keep it symmetric, until they fit a cell (half a cell, left to right) apart, clear of
    // the corners.
    // A node with an even number of ports on a side leaves its center slot empty, so that no edge
    // sits straight ahead and seems to take priority; with an odd number the center is used. The
    // empty slot counts as one slot more.
    auto slots = [&](size_t v, size_t cnt) {
        const bool skip = cnt >= 2 && cnt % 2 == 0;
        return static_cast<int>(cnt) + (skip ? 1 : 0);
    };
    for (size_t v = 0; v < n; ++v) {
        const int count = std::max(slots(v, outPorts[v].size()), slots(v, inPorts[v].size()));
        while (count >= 2) {
            const int room = (td ? nodeW[v] - 3 : nodeH[v] - 2) - std::abs(items[v].skew);
            if ((count - 1) * (td ? 2 : 1) <= 2 * room) break;
            if (td) {
                nodeW[v] += 2;
                items[v].ext = nodeW[v] + items[v].pad;
            } else {
                nodeH[v] += 2;
                ++labelRow[v];
                items[v].ext = nodeH[v] + items[v].pad;
            }
        }
    }
    if (td) {
        const bool flip = g.dir == Dir::BT;
        auto above = [&](size_t i) { return flip ? nodeH[i] - labelRow[i] : labelRow[i]; };
        std::vector<int> most(ranks, 0);
        for (size_t i = 0; i < n; ++i)
            most[static_cast<size_t>(rank[i])] =
                std::max(most[static_cast<size_t>(rank[i])], above(i));
        for (size_t i = 0; i < n; ++i) off[i] = most[static_cast<size_t>(rank[i])] - above(i);
    }

    std::vector<std::vector<int>> preds(items.size()), succs(items.size());
    for (const std::vector<int>& c : chain) {
        for (size_t i = 0; i + 1 < c.size(); ++i) {
            preds[static_cast<size_t>(c[i + 1])].push_back(c[i]);
            succs[static_cast<size_t>(c[i])].push_back(c[i + 1]);
        }
    }
    for (const std::vector<int>& pair : borderPairs) {
        preds[static_cast<size_t>(pair[1])].push_back(pair[0]);
        succs[static_cast<size_t>(pair[0])].push_back(pair[1]);
    }
    reduceCrossings(order, items, preds, succs);

    // Placement across the flow. Pack each rank, then in one pass from the first rank on, move each
    // item toward the mean center of the items its edges come from.
    const int sep = td ? fit.sepTd : tune::kSepLr;
    auto sepOf = [&](int a, int b) { return separation(items, sep, a, b); };
    for (const std::vector<int>& row : order) {
        int at = 0;
        for (size_t i = 0; i < row.size(); ++i) {
            items[static_cast<size_t>(row[i])].left = at;
            at += items[static_cast<size_t>(row[i])].ext +
                (i + 1 < row.size() ? sepOf(row[i], row[i + 1]) : sep);
        }
    }
    auto center = [&](int id) {  // in hu
        const Item& it = items[static_cast<size_t>(id)];
        return 2 * it.left + it.ext - it.pad + it.skew;
    };
    for (size_t r = 1; r < ranks; ++r) {
        const std::vector<int>& row = order[r];
        std::vector<int> desire(row.size(), 0);
        std::vector<char> known(row.size(), 0);
        for (size_t i = 0; i < row.size(); ++i) {
            const std::vector<int>& from = preds[static_cast<size_t>(row[i])];
            if (from.empty()) continue;
            int sum = 0;
            for (int p : from) sum += center(p);
            int count = static_cast<int>(from.size());
            int mean = floorDiv(2 * sum + count, 2 * count);
            const Item& it = items[static_cast<size_t>(row[i])];
            desire[i] = floorDiv(mean - (it.ext - it.pad) - it.skew + 1, 2);
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
                desire[i] = desire[i + 1] - items[static_cast<size_t>(row[i])].ext -
                    sepOf(row[i], row[i + 1]);
            for (size_t i = fk + 1; i < row.size(); ++i)
                if (!known[i])
                    desire[i] = desire[i - 1] + items[static_cast<size_t>(row[i - 1])].ext +
                        sepOf(row[i - 1], row[i]);
        }
        placeRank(items, row, desire, sep);
    }
    // Refine: alternate passes down and up, moving each item toward the median center of its
    // neighbours in the rank before (or after) it, until a round moves nothing, or after four. The
    // result is the best placement seen, by total edge slant, the initial one on ties.
    auto slant = [&] {
        long sum = 0;
        for (size_t v = 0; v < items.size(); ++v)
            for (int u : preds[v])
                sum += static_cast<long>(
                           std::max(items[v].weight, items[static_cast<size_t>(u)].weight)) *
                    std::abs(center(u) - center(static_cast<int>(v)));
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
                    for (int p : nbr[static_cast<size_t>(row[i])]) cs.push_back(center(p));
                    if (cs.empty()) {
                        desire[i] = it.left;
                        continue;
                    }
                    std::sort(cs.begin(), cs.end());
                    size_t m = cs.size() / 2;
                    int median = cs.size() % 2 ? cs[m] : floorDiv(cs[m - 1] + cs[m] + 1, 2);
                    desire[i] = floorDiv(median - (it.ext - it.pad) - it.skew + 1, 2);
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
    // Straight sides. Each box spans from the leftmost of its left border items to the rightmost of
    // its right ones, and wide enough to hold the boxes inside it; every border item goes to those
    // sides, innermost box first, and the rest of the rank is made to keep clear of them. A box
    // moves as a whole (its members, the boxes inside it and its borders, on every rank), so it
    // stays straight; an item outside any box moves alone, as does a box's right border (the
    // sides are then straightened again, which widens the box). Only ever rightward, so this ends
    // unless two boxes are in the wrong order on some pair of ranks, which there is no room for.
    // Each box's width in columns before a title widens it, and the columns it was widened by at
    // the start and at the end of the flow's axis it grows along: across the flow top-down, along
    // it left to right (see below).
    std::vector<int> natural(clusterCount, 0), grewFirst(clusterCount, 0),
        grewLast(clusterCount, 0);
    const bool hflip = g.dir == Dir::RL;  // the columns run against the layout's
    if (clusterCount) {
        // Every item of a box, those of the boxes inside it and its own borders too, and the
        // same without its own borders.
        std::vector<std::vector<int>> held(clusterCount), inner(clusterCount);
        for (size_t i = 0; i < items.size(); ++i) {
            for (int c = items[i].cluster; c >= 0; c = parentOf(c)) {
                held[static_cast<size_t>(c)].push_back(static_cast<int>(i));
                if (c != items[i].cluster || !items[i].border)
                    inner[static_cast<size_t>(c)].push_back(static_cast<int>(i));
            }
        }
        // The blank cells between a box's border and what lies inside it, on its left and right.
        auto gapOf = [&](size_t c, int side) {
            return items[static_cast<size_t>(side == 1 ? boxLeft[c] : boxRight[c])].gap;
        };
        std::vector<int> sideLo(clusterCount, 0), sideHi(clusterCount, 0);
        // A box's sides, as its borders have them: the leftmost left border and the rightmost
        // right one, beyond those of the boxes inside it (`enclose`, for the first time).
        auto sides = [&](size_t c, bool enclose, int& lo, int& hi) {
            lo = INT_MAX;
            hi = INT_MIN;
            for (int id : held[c]) {
                const Item& it = items[static_cast<size_t>(id)];
                if (it.cluster != static_cast<int>(c)) continue;
                if (it.border == 1) lo = std::min(lo, it.left);
                if (it.border == 2) hi = std::max(hi, it.left);
            }
            for (size_t d = 0; d < c; ++d) {
                if (parentOf(static_cast<int>(d)) != static_cast<int>(c)) continue;
                if (enclose) lo = std::min(lo, sideLo[d] - 1 - gapOf(c, 1));
                hi = std::max(hi, sideHi[d] + 1 + gapOf(c, 2));
            }
        };
        for (size_t c = 0; c < clusterCount; ++c) {
            int lo, hi;
            sides(c, true, lo, hi);
            // Top-down the box is at least as wide as its title and a blank column and the border
            // on each side, or as `grow` asks. Unless it asks, both sides move out, so the members
            // stay in the middle. The pass below makes room for it: where a neighbour is in the
            // way it moves the whole box (members, borders and all) or the neighbour right, never
            // one side alone. Left to right the title's width lies along the flow, which the
            // spacing between the ranks takes care of.
            int left = 0, right = 0;
            if (td) {
                natural[c] = hi - lo + 1;
                left = grow[c].first;
                right = grow[c].second;
                if (!left && !right && titleL[c]) {
                    const int extra = std::max(0, titleW[c] + 2 * tune::kClusterPad - natural[c]);
                    left = extra / 2;
                    right = extra - left;
                }
                grewFirst[c] = left;
                grewLast[c] = right;
            }
            lo += nudge[c] - left;
            hi += nudge[c] + right;
            for (int id : inner[c]) items[static_cast<size_t>(id)].left += nudge[c];
            for (int id : held[c]) {
                Item& it = items[static_cast<size_t>(id)];
                if (it.cluster == static_cast<int>(c) && it.border)
                    it.left = it.border == 1 ? lo : hi;
            }
            sideLo[c] = lo;
            sideHi[c] = hi;
        }
        bool settled = false;
        for (int sweep = 0; sweep < 100 && !settled; ++sweep) {
            settled = true;
            for (const std::vector<int>& row : order) {
                for (size_t i = 1; i < row.size(); ++i) {
                    const int prev = row[i - 1], next = row[i];
                    const Item &a = items[static_cast<size_t>(prev)],
                               &b = items[static_cast<size_t>(next)];
                    const int delta = a.left + a.ext + sepOf(prev, next) - b.left;
                    if (delta <= 0) continue;
                    settled = false;
                    if (b.border == 1) {
                        for (int id : held[static_cast<size_t>(b.cluster)])
                            items[static_cast<size_t>(id)].left += delta;
                    } else {
                        items[static_cast<size_t>(next)].left += delta;
                    }
                }
            }
            // A right border that was pushed makes its box wider on every rank.
            for (size_t c = 0; c < clusterCount; ++c) {
                int lo, hi;
                sides(c, false, lo, hi);
                sideLo[c] = lo;
                sideHi[c] = hi;
                for (int id : held[c]) {
                    Item& it = items[static_cast<size_t>(id)];
                    if (it.cluster != static_cast<int>(c) || !it.border) continue;
                    const int at = it.border == 1 ? lo : hi;
                    if (it.left == at) continue;
                    it.left = at;
                    settled = false;
                }
            }
        }
        if (!settled) {
            reason = "subgraph layout: sides";
            return false;
        }
    }
    int minLeft = INT_MAX, across = 0;
    for (const Item& it : items) minLeft = std::min(minLeft, it.left);
    for (Item& it : items) {
        it.left -= minLeft;
        across = std::max(across, it.left + it.ext);
    }

    // Where each port sits on its side, as an offset from the center in hu. The ports of one side
    // are ordered by where their edges' far ends lie, so that their runs do not cross, and spaced a
    // cell apart in TD and half a cell in LR. An odd count uses the center; an even one leaves it
    // empty.
    std::vector<int> portOff(ports.size(), 0);
    for (size_t side = 0; side < 2; ++side) {
        for (size_t v = 0; v < n; ++v) {
            const std::vector<int>& list = (side == 0 ? outPorts : inPorts)[v];
            if (list.size() < 2) continue;
            std::vector<std::pair<long, int>> far;  // mean far-end center (x size), port
            for (int p : list) {
                const std::vector<int>& es = ports[static_cast<size_t>(p)].edges;
                long sum = 0;
                for (int k : es) {
                    const std::vector<int>& c = chain[static_cast<size_t>(k)];
                    sum += center(side == 0 ? c[1] : c[c.size() - 2]);
                }
                far.push_back({sum * 1000 / static_cast<long>(es.size()), p});
            }
            std::stable_sort(far.begin(), far.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            const int step = td ? 2 : 1;
            const int lim = (td ? nodeW[v] - 3 : nodeH[v] - 2) -
                std::abs(items[v].skew);  // clear of the corners
            const bool skip = slots(v, far.size()) > static_cast<int>(far.size());
            const int total = static_cast<int>(far.size()) + (skip ? 1 : 0);
            int lo = -(total / 2) * step;
            int hi = lo + (total - 1) * step;
            int shift = hi > lim ? lim - hi : lo < -lim ? -lim - lo : 0;
            if (lo + shift < -lim || hi + shift > lim) {
                reason = "too many ports";
                return false;
            }
            for (size_t i = 0; i < far.size(); ++i) {
                // Past the middle of an even count, step over the empty center slot.
                const int slot = static_cast<int>(i) + (skip && 2 * i >= far.size() ? 1 : 0);
                portOff[static_cast<size_t>(far[i].second)] = lo + shift + slot * step;
            }
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
        int x = center(c[i]);
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
        thick[r] =
            std::max({thick[r], off[i] + (td ? nodeH[i] : nodeW[i]), td ? 0 : loopLabelW[i]});
    }
    for (size_t i = n; i < items.size(); ++i) {
        size_t r = static_cast<size_t>(items[i].rank);
        thick[r] = std::max(thick[r], items[i].thick);
    }
    std::vector<char> plain(ranks, 1), labelRank(ranks, 0);  // only dummies; holds a label
    for (size_t i = 0; i < n; ++i) plain[static_cast<size_t>(rank[i])] = 0;
    for (int item : labelItem) {
        if (item < 0) continue;
        plain[static_cast<size_t>(items[static_cast<size_t>(item)].rank)] = 0;
        labelRank[static_cast<size_t>(items[static_cast<size_t>(item)].rank)] = 1;
    }
    std::vector<std::vector<GapSeg>> gapSegs(ranks);
    for (size_t k = 0; k < m; ++k) {
        if (g.edges[k].line == Line::Invisible) continue;
        const std::vector<int>& c = chain[k];
        for (size_t i = 0; i + 1 < c.size(); ++i)
            gapSegs[static_cast<size_t>(items[static_cast<size_t>(c[i])].rank)].push_back(
                {endId(k, i), endId(k, i + 1), endX(k, i), endX(k, i + 1)});
    }
    // A box's top border lies in the gap above its first rank, one row above the row where edges
    // arrive, which is the blank row inside the box; its bottom border lies in the gap below its
    // last rank, one row below the row where edges leave. Boxes that neither hold each other nor
    // touch never share a column, so those of one gap share a row, and a gap that holds borders is
    // one row taller for each kind, or as tall as the borders of boxes inside one another need
    // (see `reaches`). Above the first rank and below the last, the border and its blank row get
    // rows of their own.
    // Left to right the same holds for columns. A title is text, so it lies in the rows under the
    // box's top border as drawn: top-down they come between the border and the row where edges
    // arrive, bottom-up (where the flow's end is the top) between the row where edges leave and
    // the border, and left to right they are rows across the flow, which the spacing of the border
    // items made room for. There the title's width is along the flow, and a box narrower than it
    // is widened by columns inside its first and last gap, half each: `growFirst` and `growLast`.
    std::vector<int> usedAt(ranks, 0);
    std::map<std::pair<int, int>, int> trackOf;  // by the ends of a segment
    for (size_t r = 0; r + 1 < ranks; ++r) {
        std::vector<int> tracks;
        usedAt[r] = assignTracks(gapSegs[r], tracks);
        for (size_t j = 0; j < tracks.size(); ++j)
            if (tracks[j] >= 0) trackOf[{gapSegs[r][j].from, gapSegs[r][j].to}] = tracks[j];
    }
    std::vector<int> endsAt(ranks, 0), startsAt(ranks, 0), start(ranks, 0);
    std::vector<int> growFirst(clusterCount, 0), growLast(clusterCount, 0);
    std::vector<char> asked(clusterCount, 0);  // `grow` says how wide the box is to be
    for (size_t c = 0; c < clusterCount; ++c) {
        if (td) continue;
        // `grow` is in the columns of the picture; the layout's run the other way for RL.
        growFirst[c] = hflip ? grow[c].second : grow[c].first;
        growLast[c] = hflip ? grow[c].first : grow[c].second;
        asked[c] = growFirst[c] || growLast[c];
    }
    // How far a box's borders lie from the nodes of its first and last rank, in rows or columns
    // along the flow: the distance from the row where edges arrive back to the top border
    // (`reachFirst`), and from the row where they leave on to the bottom one (`reachLast`). Those
    // are one for the border itself, as many as the title or the growth for it takes, and, when a
    // box inside it starts (ends) on the same rank, that box's reach and a blank row (column)
    // between the two borders, so that nested borders never touch, which is also what `kClusterPad`
    // makes of them across the flow. `reachNatural` is the same for a box's own length without
    // what it takes for its title.
    const bool bt = g.dir == Dir::BT;
    std::vector<int> reachFirst(clusterCount, 0), reachLast(clusterCount, 0);
    std::vector<int> reachFirstBare(clusterCount, 0), reachLastBare(clusterCount, 0);
    auto reaches = [&] {
        for (size_t c = 0; c < clusterCount; ++c) {  // inner boxes come first
            const int own1 = td ? (bt ? 0 : titleL[c]) : growFirst[c];
            const int own2 = td ? (bt ? titleL[c] : 0) : growLast[c];
            reachFirstBare[c] = reachLastBare[c] = 1;
            for (size_t d = 0; d < c; ++d) {
                if (parentOf(static_cast<int>(d)) != static_cast<int>(c)) continue;
                if (spanLo[d] == spanLo[c])
                    reachFirstBare[c] = std::max(reachFirstBare[c], reachFirst[d] + 2);
                if (spanHi[d] == spanHi[c])
                    reachLastBare[c] = std::max(reachLastBare[c], reachLast[d] + 2);
            }
            reachFirst[c] = reachFirstBare[c] + own1;
            reachLast[c] = reachLastBare[c] + own2;
        }
    };
    auto position = [&] {
        std::fill(endsAt.begin(), endsAt.end(), 0);
        std::fill(startsAt.begin(), startsAt.end(), 0);
        reaches();
        for (size_t c = 0; c < clusterCount; ++c) {
            const size_t lo = static_cast<size_t>(spanLo[c]), hi = static_cast<size_t>(spanHi[c]);
            startsAt[lo] = std::max(startsAt[lo], reachFirst[c]);
            endsAt[hi] = std::max(endsAt[hi], reachLast[c]);
        }
        start[0] = startsAt[0] ? startsAt[0] + 1 : 0;
        for (size_t r = 0; r + 1 < ranks; ++r) {
            // In a graph with edge labels, a rank that holds only the dummies of edges that go
            // straight on is an artefact of the doubled ranks: the edges need no room to leave and
            // arrive either side of it, so one cell does for each.
            const bool lean = labelled && usedAt[r] == 0 && (plain[r] || plain[r + 1]) &&
                !labelRank[r] && !labelRank[r + 1] && !endsAt[r] && !startsAt[r + 1];
            const int borders = endsAt[r] + startsAt[r + 1];
            // Left to right a border column and the nearest track are a blank column apart, as
            // two such lines a column (half the height of a row) apart would seem to merge.
            const int blanks = usedAt[r] > 0 ? (endsAt[r] > 0) + (startsAt[r + 1] > 0) : 0;
            int gap = td
                ? (lean ? tune::kGapTd - 1 : tune::kGapTd + usedAt[r] + borders)
                : (lean ? fit.gapLr / 2 : std::max(fit.gapLr, usedAt[r] + 2 + borders + blanks));
            start[r + 1] = start[r] + thick[r] + gap;
        }
    };
    position();
    if (!td) {
        // Widen the boxes whose title is wider than they are, until none is. Each pass only ever
        // adds columns, so this ends. A box with a box inside it that grew in the same pass waits
        // for the next, as what it holds is longer than it was.
        for (size_t pass = 0; pass <= clusterCount; ++pass) {
            bool grew = false;
            std::vector<char> changed(clusterCount, 0);
            for (size_t c = 0; c < clusterCount; ++c) {
                if (changed[c] && parentOf(static_cast<int>(c)) >= 0)
                    changed[static_cast<size_t>(parentOf(static_cast<int>(c)))] = 1;
                if (!titleL[c] || asked[c] || changed[c]) continue;
                const size_t lo = static_cast<size_t>(spanLo[c]),
                             hi = static_cast<size_t>(spanHi[c]);
                const int need = titleW[c] + 2 * tune::kClusterPad -
                    (start[hi] + thick[hi] - start[lo] + 2 + reachFirst[c] + reachLast[c]);
                if (need <= 0) continue;
                growFirst[c] += need / 2;
                growLast[c] += need - need / 2;
                grew = changed[c] = 1;
                if (parentOf(static_cast<int>(c)) >= 0)
                    changed[static_cast<size_t>(parentOf(static_cast<int>(c)))] = 1;
            }
            if (!grew) break;
            position();
        }
        for (size_t c = 0; c < clusterCount; ++c) {
            const size_t lo = static_cast<size_t>(spanLo[c]), hi = static_cast<size_t>(spanHi[c]);
            natural[c] =
                start[hi] + thick[hi] - start[lo] + 2 + reachFirstBare[c] + reachLastBare[c];
            grewFirst[c] = growFirst[c];
            grewLast[c] = growLast[c];
        }
    }
    const int along =
        start[ranks - 1] + thick[ranks - 1] + (endsAt[ranks - 1] ? endsAt[ranks - 1] + 1 : 0);
    auto track = [&](size_t r, int t) {          // hu
        const int blank = !td && endsAt[r] > 0;  // see `blanks` above (tracks are in use here)
        return 2 * (start[r] + thick[r] + 1 + endsAt[r] + blank + t) + 1;
    };
    auto point = [&](int cross, int flow) { return td ? Pt{cross, flow} : Pt{flow, cross}; };

    lay.cols = td ? across : along;
    lay.rows = td ? along : across;
    lay.nodeFoot.assign(n, CellBox());
    lay.nodeLabel.assign(n, CellBox());
    for (size_t i = 0; i < n; ++i) {
        const Item& it = items[i];
        int at = start[static_cast<size_t>(it.rank)] + off[i];
        CellBox& f = lay.nodeFoot[i];
        f.cols = nodeW[i];
        f.rows = nodeH[i];
        f.col = td ? it.left : at;
        f.row = td ? at : it.left;
        CellBox& l = lay.nodeLabel[i];
        l.cols = labelW[i];
        l.rows = static_cast<int>(g.nodes[i].label.lines.size());
        l.col = f.col + (f.cols - l.cols) / 2;
        l.row = f.row + labelRow[i];
    }

    lay.clusterBox.assign(clusterCount, CellBox());
    for (size_t c = 0; c < clusterCount; ++c) {
        const size_t first = static_cast<size_t>(spanLo[c]), last = static_cast<size_t>(spanHi[c]);
        const int l = items[static_cast<size_t>(boxLeft[c])].left;
        const int r = items[static_cast<size_t>(boxRight[c])].left;
        // Along the flow the box runs from the border of its first gap, which every box of the gap
        // shares, to that of its last. A box that starts (ends) on the rank of the box around it
        // lies inside that one, at its own reach from the nodes.
        const int par = parentOf(static_cast<int>(c));
        const bool nestedFirst = par >= 0 && spanLo[static_cast<size_t>(par)] == spanLo[c];
        const bool nestedLast = par >= 0 && spanHi[static_cast<size_t>(par)] == spanHi[c];
        const int from = start[first] - 1 - (nestedFirst ? reachFirst[c] : startsAt[first]);
        const int to = start[last] + thick[last] + (nestedLast ? reachLast[c] : endsAt[last]) + 1;
        CellBox& b = lay.clusterBox[c];
        b.col = td ? l : from;
        b.cols = td ? r - l + 1 : to - from;
        b.row = td ? from : l;
        b.rows = td ? to - from : r - l + 1;
    }

    // Each edge leaves its source's outline at the middle of the side facing the flow, jogs along
    // the track of every gap where its ends differ, and arrives at the middle of its target's
    // facing side, each through its port.
    lay.edgeLabel.assign(g.edges.size(), CellBox());
    for (size_t k = 0; k < g.edges.size(); ++k) {
        if (labelItem[k] < 0) continue;
        const Item& it = items[static_cast<size_t>(labelItem[k])];
        const size_t r = static_cast<size_t>(it.rank);
        const int w = labelWidth(g.edges[k]);
        const int lines = static_cast<int>(g.edges[k].label.lines.size());
        CellBox& b = lay.edgeLabel[k];
        b.cols = w;
        b.rows = lines;
        // Centred on the item across the flow, and on its band along it.
        b.col = td ? it.left + (it.ext - w) / 2 : start[r] + (thick[r] - w) / 2;
        b.row = td ? start[r] + (thick[r] - lines) / 2 : it.left + (it.ext - lines) / 2;
    }
    lay.edgePath.assign(g.edges.size(), std::vector<Pt>());
    for (size_t k = 0; k < g.edges.size(); ++k) {
        if (g.edges[k].line == Line::Invisible) continue;
        if (selfLoop(k)) {
            // Out from the outline, along the side away from the flow, and back in, a cell (top
            // down) or two (left to right) apart, centred on the node; the arrowhead is on the way
            // in. The label sits beyond the loop, centred on the node's other axis.
            const size_t v = static_cast<size_t>(g.edges[k].from);
            const CellBox& f = lay.nodeFoot[v];
            const int reach = 2 * loopCells;
            const int lines = static_cast<int>(g.edges[k].label.lines.size());
            CellBox& b = lay.edgeLabel[k];
            if (lines > 0) {
                b.cols = loopLabelW[v];
                b.rows = lines;
            }
            if (td) {
                const int edge = 2 * (f.col + f.cols) - 1, mid = 2 * f.row + f.rows;
                lay.edgePath[k] = {{edge, mid - 1},
                                   {edge + reach, mid - 1},
                                   {edge + reach, mid + 1},
                                   {edge, mid + 1}};
                b.col = f.col + f.cols + loopCells;
                b.row = f.row + (f.rows - lines) / 2;
            } else {
                const int edge = 2 * (f.row + f.rows) - 1, mid = 2 * f.col + f.cols;
                lay.edgePath[k] = {{mid - 2, edge},
                                   {mid - 2, edge + reach},
                                   {mid + 2, edge + reach},
                                   {mid + 2, edge}};
                b.col = f.col + (f.cols - loopLabelW[v]) / 2;
                b.row = f.row + f.rows + loopCells;
            }
            continue;
        }
        const std::vector<int>& c = chain[k];
        size_t from = static_cast<size_t>(g.edges[k].from), to = static_cast<size_t>(g.edges[k].to);
        std::vector<Pt> path;
        int leave = 2 *
                (start[static_cast<size_t>(rank[from])] + off[from] +
                 (td ? nodeH[from] : nodeW[from])) -
            1;
        path.push_back(point(endX(k, 0), leave));
        for (size_t i = 0; i + 1 < c.size(); ++i) {
            int a = endX(k, i), b = endX(k, i + 1);
            if (a == b) continue;
            int t = track(static_cast<size_t>(items[static_cast<size_t>(c[i])].rank),
                          trackOf.at({endId(k, i), endId(k, i + 1)}));
            path.push_back(point(a, t));
            path.push_back(point(b, t));
        }
        path.push_back(
            point(endX(k, c.size() - 1), 2 * (start[static_cast<size_t>(rank[to])] + off[to]) + 1));
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
        // The path of a reversed edge was laid out from its real target; it is drawn from its real
        // source, against the flow, so that its markers are where they belong.
        if (reversed[k]) std::reverse(out.begin(), out.end());
    }
    const size_t crossings = crossingPoints(lay).size();
    const int maxCross = gateCols / tune::kColsPerCrossing;
    if (crossings > static_cast<size_t>(maxCross)) {
        reason = "crossings: " + std::to_string(crossings) + " > " + std::to_string(maxCross);
        why = Decline::TooComplex;
        return false;
    }
    if (g.dir == Dir::BT || g.dir == Dir::RL) {
        mirror(lay, /*rows=*/td);
        // A shape keeps its own way up: mirroring moved a label that sits off the middle of its
        // node (a cylinder's, below the lid) to the wrong side.
        for (size_t i = 0; i < n && td; ++i)
            lay.nodeLabel[i].row = lay.nodeFoot[i].row + labelRow[i];
    }
    // An edge lined up with a box's border, and so seeming to run into it, is cleared by moving
    // the box a cell across the flow, which shifts the border but not the outsiders' ports.
    {
        bool nudged = false;
        for (size_t c = 0; c < clusterCount; ++c) {
            if (nudge[c] >= 3 || !alignedWithBorder(lay, c)) continue;
            ++nudge[c];
            nudged = true;
        }
        if (nudged) {
            retry = true;
            reason = "subgraph layout: edge";
            return false;
        }
    }
    // Titles, in the picture's own rows and columns, so that mirroring need not know them. Each is
    // centred in the rows below its box's top border, unless an edge crosses them, as every edge
    // into a top-down box's first rank does (and, bottom-up, every edge out of its last; left to
    // right, one that jogs between the ranks): it then moves along the row to the
    // nearest place that is clear of the edges, with a blank column on either side. If there is
    // none, the box is asked to be wider for another pass, by as much as makes a place, or the
    // diagram is declined.
    lay.clusterLabel.assign(clusterCount, CellBox());
    {
        bool retryNow = false;
        const bool grown = again;  // the boxes have been grown for their titles already
        for (size_t c = 0; c < clusterCount; ++c) {
            if (!titleL[c]) continue;
            const CellBox& box = lay.clusterBox[c];
            const int row0 = box.row + 1, row1 = row0 + titleL[c] - 1;
            std::set<int> blocked;  // columns of the box that an edge crosses in the title's rows
            for (const std::vector<Pt>& path : lay.edgePath) {
                for (size_t i = 0; i + 1 < path.size(); ++i) {
                    const Pt &a = path[i], &b = path[i + 1];
                    if (std::min(a.second, b.second) / 2 > row1 ||
                        std::max(a.second, b.second) / 2 < row0)
                        continue;
                    for (int x = std::min(a.first, b.first) / 2;
                         x <= std::max(a.first, b.first) / 2;
                         ++x)
                        blocked.insert(x - box.col);
                }
            }
            // The title's left column, in the box, nearest the middle of `cols` columns among
            // those, when the box's left side lies `shift` columns further left than it does (a
            // wider box), or -1.
            auto place = [&](int cols, int shift) {
                const int middle = (cols - titleW[c]) / 2;
                int best = -1;
                for (int x = 2; x + titleW[c] <= cols - 2; ++x) {
                    bool clear = true;
                    for (int k = x - 1; k <= x + titleW[c] && clear; ++k)
                        clear = !blocked.count(k - shift);
                    if (clear && (best < 0 || std::abs(x - middle) < std::abs(best - middle)))
                        best = x;
                }
                return best;
            };
            // What the box grew by on the left of the picture; `grow` is asked for in the same
            // terms, and the layout's own first side is the right one in a mirrored one (RL).
            const int grewLeft = hflip ? grewLast[c] : grewFirst[c];
            const int x = place(box.cols, 0);
            if (x < 0) {
                // The least growth beyond the natural width that makes a place, as columns added on
                // the left and on the right; of those, the most even.
                std::pair<int, int> found{-1, -1};
                const int most = 2 * (titleW[c] + 4);
                for (int extra = 0; extra <= most && found.first < 0; ++extra) {
                    for (int d = 0; d <= 2 * extra && found.first < 0; ++d) {
                        const int left = extra / 2 + (d % 2 ? (d + 1) / 2 : -d / 2);
                        if (left < 0 || left > extra) continue;
                        if (place(natural[c] + extra, left - grewLeft) >= 0)
                            found = {left, extra - left};
                    }
                }
                if (found.first < 0 || grown) {
                    reason = "subgraph layout: title";
                    return false;
                }
                grow[c] = found;  // as the picture has them; the next pass turns them round
                retryNow = true;
                continue;
            }
            CellBox& t = lay.clusterLabel[c];
            t.cols = titleW[c];
            t.rows = titleL[c];
            t.col = box.col + x;
            t.row = row0;
        }
        if (retryNow) {
            again = true;
            retry = true;
            reason = "subgraph layout: title";
            return false;
        }
    }
    if (!g.title.empty()) addTitle(lay, env.width(g.title));
    return true;
}

}  // namespace

bool layoutGraph(const Graph& original,
                 const Env& env,
                 const Fit& fit,
                 Layout& lay,
                 std::string& reason,
                 Decline& why) {
    std::vector<std::pair<int, int>> grow(original.clusters.size(), {0, 0});
    std::vector<int> nudge(original.clusters.size(), 0);
    bool again = false;
    // Each pass either succeeds, declines for good, or changes `grow` or `nudge` for the next:
    // a title takes one change of `grow`, each box at most three of `nudge`.
    for (size_t pass = 0; pass <= 4 * original.clusters.size() + 2; ++pass) {
        bool retry = false;
        if (layoutOnce(original, env, fit, grow, nudge, again, retry, lay, reason, why))
            return true;
        if (!retry) return false;
    }
    return false;
}

bool alignedWithBorder(const Layout& lay, size_t c) {
    const CellBox& box = lay.clusterBox[c];
    const int l = 2 * box.col + 1, t = 2 * box.row + 1, r = 2 * (box.col + box.cols) - 1,
              b = 2 * (box.row + box.rows) - 1;
    for (const std::vector<Pt>& path : lay.edgePath) {
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            const Pt &p = path[i], &q = path[i + 1];
            const int x0 = std::min(p.first, q.first), x1 = std::max(p.first, q.first);
            const int y0 = std::min(p.second, q.second), y1 = std::max(p.second, q.second);
            // Level with a top or bottom border and within two columns of it along its length, or
            // level with a side and within two rows. (A crossing is at right angles, so is level
            // with neither.)
            constexpr int kNearCols = 4, kNearRows = 4;  // in hu
            if (y0 == y1 && (y0 == t || y0 == b) && x1 >= l - kNearCols && x0 <= r + kNearCols)
                return true;
            if (x0 == x1 && (x0 == l || x0 == r) && y1 >= t - kNearRows && y0 <= b + kNearRows)
                return true;
        }
    }
    return false;
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
