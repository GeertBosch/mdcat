#include "mermaid.h"

#include <utility>

#include "mermaid_model.h"

namespace mermaid {

namespace {

// Give the labels that are Markdown strings their lines, wrapped at `wrap` columns unless the
// diagram's front matter turns that off.
bool renderMarkdown(Graph& g, const Env& env, int wrap, std::string& reason) {
    if (!g.markdownAutoWrap) wrap = tune::kNoWrap;
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

// Break the lines of a plain label that are wider than `wrap` columns after spaces, so that no
// line is wider unless a single word is.
void wrapPlain(Label& label, const Env& env, int wrap) {
    if (!label.markdown.empty()) return;
    std::vector<std::string> out;
    for (const std::string& line : label.lines) {
        if (env.width(line) <= wrap) {
            out.push_back(line);
            continue;
        }
        std::string cur;
        size_t i = 0;
        while (i < line.size()) {
            size_t j = line.find(' ', i);
            if (j == std::string::npos) j = line.size();
            const std::string word = line.substr(i, j - i);
            i = j + 1;
            if (word.empty()) continue;
            if (!cur.empty() && env.width(cur) + 1 + env.width(word) > wrap) {
                out.push_back(cur);
                cur.clear();
            }
            cur += cur.empty() ? word : " " + word;
        }
        if (!cur.empty()) out.push_back(cur);
    }
    label.lines = out;
}

// What one attempt to lay a diagram out is allowed to change: the spacing, and the width at which
// labels are wrapped (0: only Markdown strings, at kMaxLabelWidth).
struct Attempt {
    Fit fit;
    int wrap;
};

// The attempts in order: the usual spacing, then tight spacing, then tight spacing with the
// labels that have spaces wrapped ever narrower. The direction never changes.
std::vector<Attempt> attempts() {
    Fit tight;
    tight.sepTd = tune::kSepTd - 1;
    tight.gapLr = tune::kGapLr - 1;
    tight.nodePad = tune::kNodePad - 2;
    std::vector<Attempt> list{{Fit(), 0}, {tight, 0}};
    for (int wrap : {24, 16, 12}) list.push_back({tight, wrap});
    return list;
}

}  // namespace

bool render(const std::vector<std::string>& body,
            const Env& env,
            Diagram& out,
            std::string& reason,
            Decline* why) {
    if (why) *why = Decline::Unsupported;
    Graph parsed;
    if (!parseFlowchart(body, parsed, reason)) return false;
    // Try each attempt until a diagram fits the width. If none does, the narrowest is drawn
    // anyway, wider than env.availCols: the reader can widen the terminal or scroll sideways,
    // which beats showing the source. An attempt that fails is skipped, but if the first fails
    // that is the answer, with its reason and kind of decline.
    bool have = false;
    for (const Attempt& a : attempts()) {
        Graph g = parsed;
        Layout lay;
        Decline declined = Decline::Unsupported;
        std::string why_;
        bool ok = renderMarkdown(g, env, a.wrap ? a.wrap : tune::kMaxLabelWidth, why_);
        if (ok) {
            if (a.wrap) {
                for (Node& n : g.nodes) wrapPlain(n.label, env, a.wrap);
                for (Edge& e : g.edges) wrapPlain(e.label, env, a.wrap);
            }
            ok = layoutGraph(g, env, a.fit, lay, why_, declined);
        }
        Diagram d;
        ok = ok && drawDiagram(g, lay, env, d, why_);
        if (!ok) {
            if (have) continue;
            reason = why_;
            if (why) *why = declined;
            return false;
        }
        if (!have || d.cols < out.cols) out = std::move(d);
        have = true;
        if (out.cols <= env.availCols) break;
    }
    return true;
}

}  // namespace mermaid
