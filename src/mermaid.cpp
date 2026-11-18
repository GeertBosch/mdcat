#include "mermaid.h"

#include "mermaid_model.h"

namespace mermaid {

namespace {

// Give the labels that are Markdown strings their lines, wrapped at kMaxLabelWidth columns unless
// the diagram's front matter turns that off.
bool renderMarkdown(Graph& g, const Env& env, std::string& reason) {
    const int wrap = g.markdownAutoWrap ? tune::kMaxLabelWidth : tune::kNoWrap;
    auto render = [&](Label& label) {
        if (label.markdown.empty()) return true;
        if (env.markdownLines) label.lines = env.markdownLines(label.markdown, wrap);
        int width = 0;
        for (const std::string& line : label.lines) width += env.width(line);
        if (width == 0) {
            reason = "unsupported label";
            return false;
        }
        return true;
    };
    for (Node& n : g.nodes)
        if (!render(n.label)) return false;
    for (Edge& e : g.edges)
        if (!render(e.label)) return false;
    return true;
}

}  // namespace

bool render(const std::vector<std::string>& body,
            const Env& env,
            Diagram& out,
            std::string& reason) {
    Graph g;
    if (!parseFlowchart(body, g, reason)) return false;
    if (!renderMarkdown(g, env, reason)) return false;
    Layout lay;
    if (!layoutGraph(g, env, lay, reason)) return false;
    if (lay.cols > env.availCols) {
        reason = "too wide: " + std::to_string(lay.cols) + " > " + std::to_string(env.availCols);
        return false;
    }
    return drawDiagram(g, lay, env, out, reason);
}

}  // namespace mermaid
