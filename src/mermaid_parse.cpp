#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

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
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
        static_cast<unsigned char>(c) >= 0x80;
}

// Where a node id or class name ends, from `p`: letters, digits, `_` and non-ASCII always belong
// to it, and a `-` or `.` only between two of those, so that `A-->B` and `A-.->B` still read as
// links.
size_t idEnd(const std::string& s, size_t p) {
    size_t b = p;
    while (p < s.size()) {
        bool inner =
            (s[p] == '-' || s[p] == '.') && p > b && p + 1 < s.size() && isIdChar(s[p + 1]);
        if (!isIdChar(s[p]) && !inner) break;
        ++p;
    }
    return p;
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

// The text of a plain label, from `raw`: its whitespace collapsed as HTML does, as Mermaid shows
// it. Text that is empty or holds syntax characters is declined.
bool plainLabel(const std::string& raw, std::string& label) {
    for (char c : raw)
        if (!plainLabelChar(c)) return false;
    label.clear();
    for (char c : trim(raw)) {
        if (c == ' ' || c == '\t') {
            if (label.empty() || label.back() != ' ') label += ' ';
        } else {
            label += c;
        }
    }
    return !label.empty();
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


// The title of a diagram, from its front matter: the text of a YAML plain or quoted scalar, with
// runs of whitespace collapsed. Anything more elaborate (block scalars, escapes, markup) is
// declined.
bool parseTitle(const std::string& value, std::string& title) {
    std::string v = trim(value);
    if (v.empty()) return false;
    if (v[0] == '"' || v[0] == '\'') {
        if (v.size() < 2 || v.back() != v[0]) return false;
        char q = v[0];
        v = v.substr(1, v.size() - 2);
        if (v.find(q) != std::string::npos || v.find('\\') != std::string::npos) return false;
    } else {
        if (std::strchr("|>[{&*!%@`", v[0])) return false;
        size_t hash = v.find(" #");
        if (hash != std::string::npos) v = trim(v.substr(0, hash));
        if (v.find(": ") != std::string::npos || v.back() == ':') return false;
    }
    title.clear();
    for (size_t i = 0; i < v.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(v[i]);
        if (c < 0x20 && c != '\t') return false;
        if (c == 0x7f || c == '<' || c == '>' || c == '`') return false;
        if (c == '#') {  // an entity such as #35;
            size_t e = i + 1;
            while (e < v.size() && std::isalnum(static_cast<unsigned char>(v[e]))) ++e;
            if (e > i + 1 && e < v.size() && v[e] == ';') return false;
        }
        if (c == ' ' || c == '\t') {
            if (!title.empty() && title.back() != ' ') title += ' ';
        } else {
            title += v[i];
        }
    }
    title = trim(title);
    return !title.empty();
}

// Read the YAML front matter: `title:` is kept, and so is `markdownAutoWrap` under `config:`. The
// rest of `config:` (layout, look, theme, ...) has no effect here.
bool parseFrontMatter(const std::vector<std::string>& lines, Graph& g) {
    bool inConfig = false;
    for (const std::string& raw : lines) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        size_t indent = 0;
        while (indent < raw.size() && raw[indent] == ' ') ++indent;
        if (indent < raw.size() && raw[indent] == '\t') return false;
        size_t colon = line.find(':');
        if (colon == std::string::npos) return false;
        std::string key = trim(line.substr(0, colon)), value = trim(line.substr(colon + 1));
        if (indent == 0) {
            inConfig = key == "config";
            if (key == "title" && !parseTitle(value, g.title)) return false;
        } else if (inConfig && key == "markdownAutoWrap") {
            if (value != "true" && value != "false") return false;
            g.markdownAutoWrap = value == "true";
        }
    }
    return true;
}

// Splits the text of a diagram into statements, which newlines and `;` separate. Comments
// (`%%`, also trailing ones) and directives (`%%{ ... }%%`, possibly over several lines) are
// skipped, and a `[...]` or `{...}` never ends a statement, even over a newline or `;`.
class StatementReader {
public:
    enum class Result { Statement, End, Error };

    explicit StatementReader(const std::string& text) : t(text) {}

    Result next(std::string& out) {
        for (;;) {
            while (i < t.size() && (std::isspace(static_cast<unsigned char>(t[i])) || t[i] == ';'))
                ++i;
            if (i >= t.size()) return Result::End;
            if (t.compare(i, 3, "%%{") == 0) {
                size_t end = t.find("}%%", i + 3);
                if (end == std::string::npos) return Result::Error;
                i = end + 3;
                continue;
            }
            if (t.compare(i, 2, "%%") == 0) {
                skipLine();
                continue;
            }
            size_t j = i;
            bool comment = false;
            while (j < t.size() && t[j] != '\n' && t[j] != ';') {
                if (t[j] == '[' || t[j] == '{') {
                    size_t close = t.find(t[j] == '[' ? ']' : '}', j + 1);
                    j = close == std::string::npos ? t.size() : close + 1;
                    continue;
                }
                if (t.compare(j, 2, "%%") == 0 &&
                    std::isspace(static_cast<unsigned char>(t[j - 1]))) {
                    comment = true;
                    break;
                }
                ++j;
            }
            out = trim(t.substr(i, j - i));
            i = j;
            if (comment) skipLine();
            if (!out.empty()) return Result::Statement;
        }
    }

private:
    void skipLine() {
        while (i < t.size() && t[i] != '\n') ++i;
    }

    const std::string& t;
    size_t i = 0;
};

const char* const kUnsupportedSyntax = "unsupported syntax";

struct Parser {
    Graph g;
    std::map<std::string, int> index;
    std::set<std::string> edgeIds;
    std::string reason;

    bool fail(const std::string& why) {
        reason = why;
        return false;
    }

    // The node `id` or `id[text]`, with an optional `:::class` that has no effect, at s[p], with
    // surrounding spaces skipped. A later text for a node replaces the earlier one.
    bool parseNode(const std::string& s, size_t& p, int& node) {
        while (p < s.size() && s[p] == ' ') ++p;
        size_t b = p;
        p = idEnd(s, p);
        if (p == b) return fail(kUnsupportedSyntax);
        std::string id = s.substr(b, p - b);
        if (reservedId(id)) return fail(kUnsupportedSyntax);
        if (s.compare(p, 2, "@{") == 0) return fail(kUnsupportedSyntax);
        bool labelled = false;
        std::string label;
        if (p < s.size() && s[p] == '[') {
            size_t close = s.find(']', p + 1);
            if (close == std::string::npos) return fail(kUnsupportedSyntax);
            if (!plainLabel(s.substr(p + 1, close - p - 1), label))
                return fail("unsupported label");
            labelled = true;
            p = close + 1;
        }
        if (s.compare(p, 3, ":::") == 0) {
            size_t e = idEnd(s, p + 3);
            if (e == p + 3) return fail(kUnsupportedSyntax);
            p = e;
        }
        while (p < s.size() && s[p] == ' ') ++p;

        auto it = index.find(id);
        if (it == index.end()) {
            if (static_cast<int>(g.nodes.size()) >= tune::kMaxNodes) return fail("too many nodes");
            it = index.emplace(id, static_cast<int>(g.nodes.size())).first;
            Node n;
            n.id = id;
            n.label.lines.push_back(id);
            g.nodes.push_back(n);
        }
        if (labelled) g.nodes[static_cast<size_t>(it->second)].label.lines[0] = label;
        node = it->second;
        return true;
    }

    // `node & node & ...`
    bool parseGroup(const std::string& s, size_t& p, std::vector<int>& nodes) {
        nodes.clear();
        for (;;) {
            int n = 0;
            if (!parseNode(s, p, n)) return false;
            nodes.push_back(n);
            if (p >= s.size() || s[p] != '&') return true;
            ++p;
        }
    }

    // The `graph|flowchart|flowchart-elk [DIR]` header.
    bool parseHeader(const std::string& line) {
        size_t p = 0;
        while (p < line.size() &&
               (std::isalpha(static_cast<unsigned char>(line[p])) || line[p] == '-'))
            ++p;
        std::string kw = line.substr(0, p);
        if (kw != "graph" && kw != "flowchart" && kw != "flowchart-elk")
            return fail("not a flowchart");
        if (p < line.size() && line[p] != ' ') return fail("not a flowchart");
        std::string dir = trim(line.substr(p));
        if (dir.empty() || dir == "TD" || dir == "TB" || dir == "v") {
            g.dir = Dir::TB;
        } else if (dir == "BT" || dir == "^") {
            g.dir = Dir::BT;
        } else if (dir == "LR" || dir == ">") {
            g.dir = Dir::LR;
        } else if (dir == "RL" || dir == "<") {
            g.dir = Dir::RL;
        } else {
            return fail("unsupported direction");
        }
        return true;
    }

    // Whether `s` is a statement that is understood but changes nothing that is drawn: styling,
    // links, and accessibility text.
    static bool isIgnored(const std::string& s) {
        size_t w = 0;
        while (w < s.size() && std::isalpha(static_cast<unsigned char>(s[w]))) ++w;
        std::string word = s.substr(0, w);
        size_t r = w;
        while (r < s.size() && std::isspace(static_cast<unsigned char>(s[r]))) ++r;
        if (r >= s.size()) return false;
        if (word == "accTitle") return s[r] == ':';
        if (word == "accDescr") return s[r] == ':' || (s[r] == '{' && s.back() == '}');
        static const char* const kStyling[] = {"style", "classDef", "class", "linkStyle", "click"};
        // `click --> B` is a reserved word misused, not a click statement.
        for (const char* k : kStyling)
            if (word == k) return r > w && isIdChar(s[r]);
        return false;
    }

    // `e1@{ animate: true }`: properties of an edge whose id was given before.
    bool parseEdgeProperties(const std::string& s, bool& handled) {
        handled = false;
        size_t e = idEnd(s, 0);
        if (e == 0 || s.compare(e, 2, "@{") != 0) return true;
        if (!edgeIds.count(s.substr(0, e))) return fail(kUnsupportedSyntax);
        if (s.back() != '}') return fail(kUnsupportedSyntax);
        std::string body = s.substr(e + 2, s.size() - e - 3);
        size_t at = 0;
        while (at < body.size()) {
            size_t comma = body.find(',', at);
            std::string item =
                trim(body.substr(at, comma == std::string::npos ? comma : comma - at));
            size_t colon = item.find(':');
            std::string key = trim(item.substr(0, colon));
            if (colon == std::string::npos ||
                (key != "animate" && key != "animation" && key != "step" && key != "curve"))
                return fail(kUnsupportedSyntax);
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        handled = true;
        return true;
    }

    // The link at s[p], as Mermaid reads it: an optional end marker (`<`, `o` or `x`), a stroke,
    // and an end (`>`, `o`, `x`, or none for an open link). The stroke is two or more `-`, `=` or
    // `~` (with `.` in `-.-` for a dotted one); a link is as many ranks long as it has extra
    // characters, so `-->` is 1, `--->` is 2, `---` is 1 and `----` is 2, and a dotted link counts
    // its dots. Markers at both ends must agree (`<-->`, `o--o`, `x--x`); Mermaid reads others as
    // something else, so they are declined.
    static End marker(char c) {
        return c == 'o'            ? End::Circle
            : c == 'x'             ? End::Cross
            : c == '>' || c == '<' ? End::Arrow
                                   : End::None;
    }

    bool parseLink(const std::string& s, size_t& p, Edge& edge) {
        auto at = [&](size_t i) { return i < s.size() ? s[i] : '\0'; };
        char left = '\0';
        if ((at(p) == 'o' || at(p) == 'x' || at(p) == '<') &&
            (at(p + 1) == '-' || at(p + 1) == '=')) {
            left = s[p];
            ++p;
        }
        size_t len = 0;
        char right = '\0';
        Line line = Line::Solid;
        if (at(p) == '~') {
            if (left) return fail(kUnsupportedSyntax);
            size_t tildes = 0;
            while (at(p + tildes) == '~') ++tildes;
            if (tildes < 3) return fail(kUnsupportedSyntax);
            p += tildes;
            len = tildes - 2;
            line = Line::Invisible;
        } else if (at(p) == '-' && at(p + 1) == '.') {
            size_t dots = 0;
            while (at(p + 1 + dots) == '.') ++dots;
            if (at(p + 1 + dots) != '-') return fail(kUnsupportedSyntax);
            p += dots + 2;
            if (at(p) == '>' || at(p) == 'o' || at(p) == 'x') right = s[p++];
            len = dots;
            line = Line::Dotted;
        } else if (at(p) == '-' || at(p) == '=') {
            const char stroke = s[p];
            size_t count = 0;
            while (at(p + count) == stroke) ++count;
            if (count < 2) return fail(kUnsupportedSyntax);
            p += count;
            char end = at(p);
            if (end == '>' || end == 'o' || end == 'x') {
                right = end;
                ++p;
                len = count - 1;
            } else {
                if (count < 3) return fail(kUnsupportedSyntax);
                len = count - 2;
            }
            if (stroke == '=') line = Line::Thick;
        } else {
            return fail(kUnsupportedSyntax);
        }
        if (left && !(left == right || (left == '<' && right == '>')))
            return fail(kUnsupportedSyntax);
        edge.line = line;
        edge.head = marker(right);
        edge.tail = marker(left);
        edge.minlen = static_cast<int>(std::min<size_t>(len, tune::kMaxRanks));
        return true;
    }

    // A link with its text in the middle, `-- text -->`, `-. text .->` or `== text ==>` at s[p], as
    // Mermaid reads it: a start (`--`, `-.` or `==`, after the same optional marker as parseLink
    // reads) and a space, then the text, then the end of a link of the same stroke. The end says
    // the length and the marker on the right, so `-- No ---->` is 3 ranks long. `handled` says
    // whether s[p] has that shape at all; if not, nothing is consumed.
    bool parseTextLink(const std::string& s, size_t& p, Edge& edge, bool& handled) {
        handled = false;
        auto at = [&](size_t i) { return i < s.size() ? s[i] : '\0'; };
        size_t q = p;
        char left = '\0';
        if ((at(q) == 'o' || at(q) == 'x' || at(q) == '<') &&
            (at(q + 1) == '-' || at(q + 1) == '=')) {
            left = s[q];
            ++q;
        }
        const bool dotted = at(q) == '-' && at(q + 1) == '.';
        const bool thick = at(q) == '=' && at(q + 1) == '=';
        if (!(dotted || thick || (at(q) == '-' && at(q + 1) == '-')) ||
            !std::isspace(static_cast<unsigned char>(at(q + 2))))
            return true;
        handled = true;
        const size_t textAt = q + 3;
        const char stroke = dotted ? '.' : s[q];
        char right = '\0';
        size_t len = 0, end = std::string::npos, after = 0;
        for (size_t j = textAt; j < s.size() && end == std::string::npos;) {
            size_t run = 0;
            while (at(j + run) == stroke) ++run;
            if (run == 0) {
                ++j;
                continue;
            }
            char next = at(j + run);
            if (dotted) {
                if (next == '-') {
                    after = j + run + 1;
                    len = run;
                    end = j;
                    if (at(after) == '>' || at(after) == 'o' || at(after) == 'x')
                        right = s[after++];
                }
            } else if (run >= 2) {
                if (next == '>' || next == 'o' || next == 'x') {
                    right = next;
                    after = j + run + 1;
                    len = run - 1;
                    end = j;
                } else if (run >= 3) {
                    after = j + run;
                    len = run - 2;
                    end = j;
                }
            }
            j += run;
        }
        if (end == std::string::npos) return fail(kUnsupportedSyntax);
        std::string text;
        if (!plainLabel(s.substr(textAt, end - textAt), text)) return fail("unsupported label");
        if (left && !(left == right || (left == '<' && right == '>')))
            return fail(kUnsupportedSyntax);
        edge.line = dotted ? Line::Dotted : thick ? Line::Thick : Line::Solid;
        edge.head = marker(right);
        edge.tail = marker(left);
        edge.minlen = static_cast<int>(std::min<size_t>(len, tune::kMaxRanks));
        edge.label.lines = {text};
        p = after;
        return true;
    }

    // A chain `group link group link ...`, where a link is as parseLink reads it, possibly
    // preceded by an edge id (`e1@-->`), and has text as parseTextLink or `|text|` after it reads.
    // A group is `node & node ...`, and every node of a group
    // is joined to every node of the next. A lone group just declares its nodes.
    bool parseChain(const std::string& s) {
        size_t p = 0;
        std::vector<int> from, to;
        if (!parseGroup(s, p, from)) return false;
        while (p < s.size()) {
            size_t e = idEnd(s, p);
            if (e > p && e < s.size() && s[e] == '@' && s.compare(e, 2, "@{") != 0) {
                edgeIds.insert(s.substr(p, e - p));
                p = e + 1;
            }
            Edge link;
            bool handled = false;
            if (!parseTextLink(s, p, link, handled)) return false;
            if (!handled) {
                if (!parseLink(s, p, link)) return false;
                size_t bar = p;
                while (bar < s.size() && s[bar] == ' ') ++bar;
                if (bar < s.size() && s[bar] == '|') {
                    size_t close = s.find('|', bar + 1);
                    std::string text;
                    if (close == std::string::npos ||
                        !plainLabel(s.substr(bar + 1, close - bar - 1), text) ||
                        link.line == Line::Invisible)
                        return fail("unsupported label");
                    link.label.lines = {text};
                    p = close + 1;
                }
            }
            if (!parseGroup(s, p, to)) return false;
            for (int a : from) {
                for (int b : to) {
                    if (g.edges.size() >= static_cast<size_t>(tune::kMaxNodes * tune::kMaxNodes))
                        return fail("too many edges");
                    Edge edge = link;
                    edge.from = a;
                    edge.to = b;
                    g.edges.push_back(edge);
                }
            }
            from = to;
        }
        return true;
    }

    bool parseStatement(const std::string& s) {
        if (isIgnored(s)) return true;
        bool handled = false;
        if (!parseEdgeProperties(s, handled)) return false;
        if (handled) return true;
        return parseChain(s);
    }
};

}  // namespace

bool parseFlowchart(const std::vector<std::string>& body, Graph& g, std::string& reason) {
    Parser ps;
    size_t first = 0;
    while (first < body.size() && trim(body[first]).empty()) ++first;
    std::vector<std::string> frontMatter;
    bool hasFrontMatter = false;
    if (first < body.size() && trim(body[first]) == "---") {
        size_t close = first + 1;
        while (close < body.size() && trim(body[close]) != "---") ++close;
        if (close == body.size()) {
            reason = "unsupported front matter";
            return false;
        }
        frontMatter.assign(body.begin() + static_cast<long>(first) + 1,
                           body.begin() + static_cast<long>(close));
        hasFrontMatter = true;
        first = close + 1;
    }
    std::string text;
    for (size_t i = first; i < body.size(); ++i) text += body[i] + '\n';

    StatementReader reader(text);
    bool sawHeader = false;
    std::string st;
    for (;;) {
        StatementReader::Result r = reader.next(st);
        if (r == StatementReader::Result::End) break;
        if (r == StatementReader::Result::Error) {
            reason = kUnsupportedSyntax;
            return false;
        }
        bool ok = sawHeader ? ps.parseStatement(st) : ps.parseHeader(st);
        if (ok && !sawHeader && hasFrontMatter && !parseFrontMatter(frontMatter, ps.g)) {
            ps.reason = "unsupported front matter";
            ok = false;
        }
        sawHeader = true;
        if (!ok) {
            // Name the offending statement in the reason, as in `unsupported syntax: subgraph X`.
            reason = ps.reason == kUnsupportedSyntax ? ps.reason + ": " + st : ps.reason;
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
