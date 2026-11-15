#include "mermaid.h"

#include "mermaid_model.h"

namespace mermaid {

bool render(const std::vector<std::string>& body,
            const Env& env,
            Diagram& out,
            std::string& reason) {
    Graph g;
    if (!parseFlowchart(body, g, reason)) return false;
    Layout lay;
    if (!layoutGraph(g, env, lay, reason)) return false;
    if (lay.cols > env.availCols) {
        reason = "too wide: " + std::to_string(lay.cols) + " > " + std::to_string(env.availCols);
        return false;
    }
    return drawDiagram(g, lay, env, out, reason);
}

}  // namespace mermaid
