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
    Odd
};
enum class Line { Solid, Dotted, Thick, Invisible };
enum class End { None, Arrow, Circle, Cross };

struct Label {
    std::vector<std::string> lines;  // styled; width via Env::width
    // The source of a Markdown string, which render() turns into `lines` with Env::markdownLines.
    // Empty for a plain label, whose `lines` are literal text.
    std::string markdown;
};

struct Node {
    std::string id;
    Label label;
    Shape shape = Shape::Rect;
};

struct Edge {
    int from = 0, to = 0, minlen = 1;
    Line line = Line::Solid;
    End head = End::Arrow, tail = End::None;
    Label label;
};

struct Graph {
    Dir dir = Dir::TB;
    std::string title;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
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
    std::vector<std::vector<Pt>> edgePath;     // per edge, orthogonal hu polyline, source to target
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
    // the point the path comes from. Dot: the centre. Corner: the bend, then a point on its
    // vertical leg and one on its horizontal leg (only their sides of the bend matter).
    std::vector<Pt> pts;
    // Segment: bit 0 (bit 1) is set when the end at pts[0] (pts[1]) is a rounded Corner, so the
    // straight piece stops short of the bend.
    unsigned char round = 0;
    // Segment: bit 0 (bit 1) is set when the end at pts[0] (pts[1]) is the gap before an edge's
    // label. The point is then the centre of the cell next to the label, and the stroke goes on
    // from it until it is a quarter of a cell's height short of the label.
    unsigned char reach = 0;
    // Segment, horizontal only: the x of each vertical stroke that crosses it, where the stroke
    // breaks to form a bridge.
    std::vector<int> breaks;
};

// What a character cell of the diagram holds, for the drawer and the validator.
enum class Occ : unsigned char { Empty, Node, Label, Edge };

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
    24;                        // columns an edge label may take, and a Markdown string wraps at
constexpr int kNoWrap = 4096;  // a wrap width that no label reaches, for markdownAutoWrap: false
constexpr int kMinCylinderLabel =
    8;  // columns a cylinder is at least as wide as its label ("database")
}  // namespace tune

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
bool layoutGraph(const Graph& g, const Env& env, Layout& lay, std::string& reason, Decline& why);

// The points, in half-cell units, where one edge's path crosses another's without either path
// ending or bending there. Where one edge bends off a run that another passes straight along, the
// two share a port and the point is a junction, not a crossing.
std::vector<Pt> crossingPoints(const Layout& lay);

// Turn a layout into strokes, a text layer and a PNG, validating it on the way.
bool drawDiagram(
    const Graph& g, const Layout& lay, const Env& env, Diagram& out, std::string& reason);

}  // namespace mermaid
