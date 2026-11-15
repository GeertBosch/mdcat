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
    enum Kind { Segment, Outline, Arrowhead, Dot, CircleEnd, CrossEnd } kind = Segment;
    Line line = Line::Solid;
    Shape shape = Shape::Rect;  // for Outline
    // Segment: its two ends. Outline: the box's top-left and bottom-right. Arrowhead: the tip, then
    // the point the path comes from.
    std::vector<Pt> pts;
};

// What a character cell of the diagram holds, for the drawer and the validator.
enum class Occ : unsigned char { Empty, Node, Label, Edge };

// Tunables: spacing and the gates that make a diagram fall back.
namespace tune {
constexpr int kMaxNodes = 60;  // bounds layout cost and clutter
constexpr int kNodeRows = 3;   // a node is a box with its label on the middle row
constexpr int kNodePad = 4;    // columns a node adds around its label
constexpr int kGapTd = 2;      // rows between nodes in a top-down chain
constexpr int kGapLr = 4;      // columns between nodes in a left-to-right chain
}  // namespace tune

// Parse the body of a ```mermaid block. On failure `reason` is one of the stable fallback reasons.
bool parseFlowchart(const std::vector<std::string>& body, Graph& g, std::string& reason);

// Lay out a graph that is a single chain of nodes. Does not check the width against the
// environment; the caller compares `lay.cols` with Env::availCols.
bool layoutChain(const Graph& g, const Env& env, Layout& lay, std::string& reason);

// Turn a layout into strokes, a text layer and a PNG, validating it on the way.
bool drawDiagram(
    const Graph& g, const Layout& lay, const Env& env, Diagram& out, std::string& reason);

}  // namespace mermaid
