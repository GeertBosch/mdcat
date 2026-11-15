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
    std::vector<CellBox> nodeFoot, nodeLabel;  // per node
    std::vector<CellBox> edgeLabel;            // per edge (cols == 0 if none)
    std::vector<std::vector<Pt>> edgePath;     // per edge, orthogonal hu polyline
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
};

// What a character cell of the diagram holds, for the drawer and the validator.
enum class Occ : unsigned char { Empty, Node, Label, Edge };

// Tunables: spacing and the gates that make a diagram fall back.
namespace tune {
constexpr int kMaxNodes = 60;   // bounds layout cost and clutter
constexpr int kMaxRanks = 60;   // bounds the dummy nodes that long links create
constexpr int kNodeRows = 3;    // a node is a box with its label on the middle row
constexpr int kNodePad = 4;     // columns a node adds around its label
constexpr int kGapTd = 2;       // rows between two ranks when every edge between them is straight
constexpr int kGapTdTrack = 3;  // rows between two ranks when an edge jogs sideways
constexpr int kGapLr = 4;       // columns between two ranks
constexpr int kSepTd = 2;       // columns between neighbouring footprints in a rank
constexpr int kSepLr = 1;       // rows between neighbouring footprints in a rank
}  // namespace tune

// Parse the body of a ```mermaid block. On failure `reason` is one of the stable fallback reasons.
bool parseFlowchart(const std::vector<std::string>& body, Graph& g, std::string& reason);

// Lay out an acyclic graph in ranks, routing every edge with one track per gap between ranks. Does
// not check the width against the environment; the caller compares `lay.cols` with
// Env::availCols.
bool layoutGraph(const Graph& g, const Env& env, Layout& lay, std::string& reason);

// Turn a layout into strokes, a text layer and a PNG, validating it on the way.
bool drawDiagram(
    const Graph& g, const Layout& lay, const Env& env, Diagram& out, std::string& reason);

}  // namespace mermaid
