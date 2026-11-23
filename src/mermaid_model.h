// The data model shared by the stages of the built-in Mermaid flowchart renderer, and the stage
// entry points: parse (mermaid_parse.cpp) -> layout (mermaid_layout.cpp) -> draw
// (mermaid_draw.cpp). Later steps of docs/plan-mermaid-flowcharts.md extend these types by adding
// enum values and fields; they do not reshape them.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "mermaid.h"

namespace mermaid {

enum class Dir { TB, BT, LR, RL };
enum class Shape {
    Rect,
    Round,
    Stadium,
    Circle,
    DoubleCircle,
    Subroutine,
    Cylinder,
    Rhombus,
    Hexagon,
    LeanR,
    LeanL,
    TrapB,
    TrapT,
    Odd,
    DividedRect,    // a box with a line across it, a row below its top
    LinedRect,      // a box with a vertical line near its left side
    LinedCylinder,  // a cylinder with a second arc under its lid
    HCylinder       // a cylinder on its side: a full cap on the right, half of one on the left
};
enum class Line { Solid, Dotted, Thick, Invisible };
enum class End { None, Arrow, Circle, Cross };

struct Label {
    std::vector<std::string> lines;  // styled; width via Env::width
    // The source of a Markdown string, which render() turns into `lines` with Env::markdownLines.
    // Empty for a plain label, whose `lines` are literal text.
    std::string markdown;
};

// The colours a `style` or `classDef` gives a node, as 0xRRGGBB, or -1 where it gives none.
struct Style {
    int fill = -1, stroke = -1, color = -1;  // the box's fill, its outline, and its label text
};

struct Node {
    std::string id;
    Label label;
    Shape shape = Shape::Rect;
    Style style;
    int cluster = -1;  // the innermost cluster that holds the node (an index into Graph::clusters)
};

struct Edge {
    int from = 0, to = 0, minlen = 1;
    Line line = Line::Solid;
    End head = End::Arrow, tail = End::None;
    Label label;
};

// A `subgraph ... end`. It holds the nodes that were named inside it, unless a subgraph that closed
// earlier claimed them, and the subgraphs written inside it. It is never empty.
struct Cluster {
    std::string id;  // the id as Mermaid has it; one is generated for a title with spaces
    // The title, as for a node. No `lines` and no `markdown`: no title (`subgraph s1[" "]`). A
    // Markdown string has its `markdown` set and gets its `lines` where node labels do, in
    // render().
    Label title;
    int parent = -1;  // the enclosing cluster, or -1 at the top level
};

struct Graph {
    Dir dir = Dir::TB;
    std::string title;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    // The subgraphs in the order they close, so a cluster comes after the clusters inside it, and a
    // node that two subgraphs name belongs to the one that comes first.
    std::vector<Cluster> clusters;
    bool markdownAutoWrap = true;
};

struct CellBox {
    int col = 0, row = 0, cols = 0, rows = 0;
};

// Half-cell units ("hu"): a point (x, y) lies at x/2 cells across and y/2 cells down, so an odd
// coordinate is the middle of a cell.
using Pt = std::pair<int, int>;

struct Layout {  // cells for boxes, half-cell units for paths
    int cols = 0, rows = 0;
    int titleRows = 0;  // rows at the top that hold the title, and a blank one after it
    std::vector<CellBox> nodeFoot, nodeLabel;  // per node
    std::vector<CellBox> edgeLabel;            // per edge: its text's cells (cols == 0 if none)
    // Per cluster: the cells of its outline, the border included. The border is the outermost
    // cells, so its strokes run through the centres of those cells, as a node's do.
    std::vector<CellBox> clusterBox;
    // Per cluster: the cells of its title (the width of its longest line, one row per line), inside
    // the box and clear of the edges (cols == 0 if untitled).
    std::vector<CellBox> clusterLabel;
    std::vector<std::vector<Pt>> edgePath;  // per edge, orthogonal hu polyline, source to target
    // Per edge: its group, a split (edges from one source) or a merge (edges into one target) whose
    // edges share a port on each of their nodes, and have one style. Only edges of one group may
    // share or touch strokes. An invisible edge has no group (-1) and an empty path.
    std::vector<int> edgeGroup;
};

struct Stroke {  // what the rasterizer consumes, in hu
    enum Kind { Segment, Outline, Arrowhead, Dot, CircleEnd, CrossEnd, Corner } kind = Segment;
    Line line = Line::Solid;
    Shape shape = Shape::Rect;  // for Outline
    // Segment: its two ends. Outline: the box's top-left and bottom-right. Arrowhead: the tip, then
    // the point the path comes from. Dot: the center. Corner: the bend, then a point on its
    // vertical leg and one on its horizontal leg (only their sides of the bend matter).
    std::vector<Pt> pts;
    int node = -1;     // Outline: the node it outlines, or -1 for a cluster
    int cluster = -1;  // Outline: the cluster it outlines, or -1 for a node
    // Segment: bit 0 (bit 1) is set when the end at pts[0] (pts[1]) is a rounded Corner, so the
    // straight piece stops short of the bend.
    unsigned char round = 0;
    // Segment: bit 0 (bit 1) is set when the end at pts[0] (pts[1]) is the gap before an edge's
    // label. The point is then the center of the cell next to the label, and the stroke goes on
    // from it until it is a quarter of a cell's height short of the label.
    unsigned char reach = 0;
    // Segment, horizontal only: the x of each vertical stroke that crosses it, where the stroke
    // breaks to form a bridge.
    std::vector<int> breaks;
};

// What a character cell of the diagram holds, for the drawer and the validator.
enum class Occ : unsigned char { Empty, Node, Label, Edge, Cluster };

// Tunables: spacing and the gates that make a diagram fall back.
namespace tune {
constexpr int kMaxNodes = 60;  // bounds layout cost and clutter
constexpr int kMaxRanks = 60;  // bounds the dummy nodes that long links create
constexpr int kNodeFrame = 2;  // rows a node adds around its label's lines
constexpr int kNodePad = 4;    // columns a node adds around its label
constexpr int kGapTd =
    2;  // rows between two ranks, plus one for each track the edges between them use
constexpr int kGapLr = 4;  // columns between two ranks, at least; with many tracks, tracks + 2
constexpr int kSepTd = 2;  // columns between neighbouring footprints in a rank
constexpr int kSepLr = 1;  // rows between neighbouring footprints in a rank
constexpr int kColsPerCrossing = 10;  // a diagram may have one edge crossing (bridge) per this many
                                      // columns of the available width, and no more
constexpr int kColsPerBackEdge = 10;  // likewise for the edges that close a cycle
constexpr int kMinGateCols = 72;  // a narrower terminal counts as this wide for those two limits
constexpr int kMaxLabelWidth =
    24;                           // columns an edge label may take, and a Markdown string wraps at
constexpr int kNoWrap = 4096;     // a wrap width that no label reaches, for markdownAutoWrap: false
constexpr int kClusterPad = 2;    // columns a subgraph's box adds between its members and what lies
                                  // outside: its border's column and a blank one inside it
constexpr int kClusterPadLr = 2;  // rows a left-to-right subgraph's box adds between its members
                                  // and what lies outside: its border's row and a blank one
constexpr int kMinCylinderLabel =
    8;  // columns a cylinder is at least as wide as its label ("database")
}  // namespace tune

// The spacing the layout uses. Fitting a diagram to the width tries tighter ones in turn.
struct Fit {
    int sepTd = tune::kSepTd;      // columns between neighbouring footprints in a top-down rank
    int gapLr = tune::kGapLr;      // columns between two left-to-right ranks, at least
    int nodePad = tune::kNodePad;  // columns a node adds around its label
};

// Parse the body of a ```mermaid block. On failure `reason` is one of the stable fallback reasons.
bool parseFlowchart(const std::vector<std::string>& body, Graph& g, std::string& reason);

// Lay out a graph in ranks, ordering and placing the boxes of each rank so that few edges cross and
// long ones run straight, and routing the edges between two ranks along as many tracks as they
// need. The edges that close a cycle are reversed for the layout and drawn against the flow, each
// with ports of its own, as is each repeat of an edge between the same nodes in the same
// direction. A self-loop is no part of the ranks: it is drawn beside its node, in cells that the
// node's item holds after it. Does not check the width against the environment; the caller
// compares `lay.cols` with Env::availCols. Declines with Decline::TooComplex, instead of the usual
// Unsupported, a graph with more back edges or crossings than the width allows.
bool layoutGraph(
    const Graph& g, const Env& env, const Fit& fit, Layout& lay, std::string& reason, Decline& why);

// The points, in half-cell units, where one edge's path crosses another's without either path
// ending or bending there. Where one edge bends off a run that another passes straight along, the
// two share a port and the point is a junction, not a crossing.
std::vector<Pt> crossingPoints(const Layout& lay);

// Whether an edge's path runs level with a side of cluster `c`'s box, along it or within two cells
// of its end, so that the two seem to join: a segment on the row of a top or bottom border, or the
// column of a left or right one, that reaches to within two cells of the border's length. Edges
// cross a border at right angles, so a crossing is never level with it.
bool alignedWithBorder(const Layout& lay, size_t c);

// Turn a layout into strokes, a text layer and a PNG, validating it on the way.
bool drawDiagram(
    const Graph& g, const Layout& lay, const Env& env, Diagram& out, std::string& reason);

}  // namespace mermaid
