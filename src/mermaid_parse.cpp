#include <algorithm>
#include <cctype>
#include <map>

#include "mermaid_model.h"

namespace mermaid {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool isIdChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Characters that are syntax in Mermaid labels (shapes, quoting, entities, HTML, markdown strings);
// a label containing one is not plain text.
bool plainLabelChar(char c) {
    switch (c) {
    case '[':
    case ']':
    case '(':
    case ')':
    case '{':
    case '}':
    case '"':
    case '|':
    case '<':
    case '>':
    case '`':
    case '#': return false;
    default: return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f;
    }
}

bool reservedId(const std::string& id) {
    static const char* const kReserved[] = {"end",
                                            "subgraph",
                                            "style",
                                            "class",
                                            "classDef",
                                            "click",
                                            "linkStyle",
                                            "direction",
                                            "graph",
                                            "flowchart"};
    for (const char* r : kReserved)
        if (id == r) return true;
    return false;
}

const char* const kUnsupportedSyntax = "unsupported syntax";

struct Parser {
    Graph g;
    std::vector<bool> hasLabel;  // per node: whether a `[text]` has been seen
    std::map<std::string, int> index;
    std::string reason;

    bool fail(const std::string& why) {
        reason = why;
        return false;
    }

    // Parse `id` or `id[text]` at s[p], skipping surrounding spaces; yields the node index.
    bool parseNode(const std::string& s, size_t& p, int& node) {
        while (p < s.size() && s[p] == ' ') ++p;
        size_t b = p;
        while (p < s.size() && isIdChar(s[p])) ++p;
        if (p == b) return fail(kUnsupportedSyntax);
        std::string id = s.substr(b, p - b);
        if (reservedId(id)) return fail(kUnsupportedSyntax);
        bool labelled = false;
        std::string label;
        if (p < s.size() && s[p] == '[') {
            size_t close = s.find(']', p + 1);
            if (close == std::string::npos) return fail(kUnsupportedSyntax);
            std::string raw = s.substr(p + 1, close - p - 1);
            for (char c : raw)
                if (!plainLabelChar(c)) return fail("unsupported label");
            // HTML-style collapsing of whitespace runs, as Mermaid shows them.
            raw = trim(raw);
            for (char c : raw) {
                if (c == ' ' || c == '\t') {
                    if (label.empty() || label.back() != ' ') label += ' ';
                } else {
                    label += c;
                }
            }
            if (label.empty()) return fail("unsupported label");
            labelled = true;
            p = close + 1;
        }
        while (p < s.size() && s[p] == ' ') ++p;

        auto it = index.find(id);
        if (it == index.end()) {
            it = index.emplace(id, static_cast<int>(g.nodes.size())).first;
            Node n;
            n.id = id;
            n.label.lines.push_back(id);
            g.nodes.push_back(n);
            hasLabel.push_back(false);
        }
        size_t at = static_cast<size_t>(it->second);
        if (labelled) {
            Node& n = g.nodes[at];
            if (hasLabel[at] && n.label.lines[0] != label) return fail("conflicting labels");
            n.label.lines[0] = label;
            hasLabel[at] = true;
        }
        node = it->second;
        return true;
    }

    // Parse the `graph|flowchart DIR` header line.
    bool parseHeader(const std::string& line) {
        size_t p = 0;
        while (p < line.size() && std::isalpha(static_cast<unsigned char>(line[p]))) ++p;
        std::string kw = line.substr(0, p);
        if (kw != "graph" && kw != "flowchart") return fail("not a flowchart");
        std::string dir = trim(line.substr(p));
        if (p == line.size() || line[p] != ' ') return fail("unsupported direction");
        if (dir == "TD" || dir == "TB") {
            g.dir = Dir::TB;
        } else if (dir == "LR") {
            g.dir = Dir::LR;
        } else {
            return fail("unsupported direction");
        }
        return true;
    }

    // Parse `node --> node`, the only statement understood so far. Each dash beyond the two before
    // the head makes the link span one more rank: `-->` is 1, `--->` is 2.
    bool parseStatement(const std::string& line) {
        size_t p = 0;
        int from = 0, to = 0;
        if (!parseNode(line, p, from)) return false;
        size_t dashes = 0;
        while (p + dashes < line.size() && line[p + dashes] == '-') ++dashes;
        if (dashes < 2 || p + dashes >= line.size() || line[p + dashes] != '>')
            return fail(kUnsupportedSyntax);
        p += dashes + 1;
        if (!parseNode(line, p, to)) return false;
        if (p != line.size()) return fail(kUnsupportedSyntax);
        Edge e;
        e.from = from;
        e.to = to;
        e.minlen = static_cast<int>(std::min<size_t>(dashes - 1, tune::kMaxRanks));
        g.edges.push_back(e);
        return true;
    }
};

}  // namespace

bool parseFlowchart(const std::vector<std::string>& body, Graph& g, std::string& reason) {
    Parser ps;
    bool sawHeader = false;
    for (const std::string& raw : body) {
        std::string line = trim(raw);
        if (line.empty()) continue;
        bool ok = sawHeader ? ps.parseStatement(line) : ps.parseHeader(line);
        sawHeader = true;
        if (!ok) {
            // Name the offending line in the reason, as in `unsupported syntax: subgraph X`.
            reason = ps.reason == kUnsupportedSyntax ? ps.reason + ": " + line : ps.reason;
            return false;
        }
    }
    if (!sawHeader) {
        reason = "empty diagram";
        return false;
    }
    g = std::move(ps.g);
    return true;
}

}  // namespace mermaid
