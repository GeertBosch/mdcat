// Mermaid sequence diagrams: the parser, and the layout that turns a Sequence into a Scene for the
// shared painter. Participants are boxes in a row at the top and, mirrored, at the bottom, each
// with a dotted lifeline between; a message is a horizontal stroke between two lifelines, with its
// text on the rows above it.
#include "mermaid_sequence.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace mermaid {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Whether the `#` at s[p] starts an entity code (`#35;`, `#quot;`), which Mermaid decodes. Any
// other `#` starts a comment that runs to the end of the line.
bool entityAtHash(const std::string& s, size_t p) {
    size_t e = p + 1;
    while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_')) ++e;
    return e > p + 1 && e < s.size() && s[e] == ';';
}

// `text` up to a `#` that is not an entity code.
std::string cutComment(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '#' && !entityAtHash(text, i)) return text.substr(0, i);
    return text;
}

// Split a line into statements at its `;`, not counting the one that ends an entity code.
std::vector<std::string> splitStatements(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '#' && entityAtHash(line, i)) {
            const size_t semi = line.find(';', i);
            cur += line.substr(i, semi + 1 - i);
            i = semi;
        } else if (line[i] == ';') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += line[i];
        }
    }
    out.push_back(cur);
    return out;
}

// Whether the statement `st` starts with the keyword `kw` (lower case; the match ignores case),
// followed by a space, the end, or (when `colon`) a colon. `rest` is what follows, trimmed, and
// without that colon.
bool keyword(const std::string& st, const char* kw, std::string& rest, bool colon = false) {
    const size_t n = std::strlen(kw);
    if (st.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(st[i])) != kw[i]) return false;
    if (st.size() > n && !std::isspace(static_cast<unsigned char>(st[n])) &&
        !(colon && st[n] == ':'))
        return false;
    rest = trim(st.substr(n));
    if (colon && !rest.empty() && rest[0] == ':') rest = trim(rest.substr(1));
    return true;
}

struct ArrowDef {
    const char* token;
    Line line;
    End head;
    bool async;
};

// The message arrows, the longer of two that start alike first.
const ArrowDef kArrows[] = {{"-->>", Line::Dotted, End::Arrow, false},
                            {"->>", Line::Solid, End::Arrow, false},
                            {"--x", Line::Dotted, End::Cross, false},
                            {"-x", Line::Solid, End::Cross, false},
                            {"--)", Line::Dotted, End::Arrow, true},
                            {"-)", Line::Solid, End::Arrow, true},
                            {"-->", Line::Dotted, End::None, false},
                            {"->", Line::Solid, End::None, false}};

// The arrow that starts at s[p], or null. `bidirectional` is set when it is one of the two-headed
// arrows `<<->>` and `<<-->>`, which are not drawn.
const ArrowDef* arrowAt(const std::string& s, size_t p, bool& bidirectional) {
    bidirectional = s.compare(p, 5, "<<->>") == 0 || s.compare(p, 6, "<<-->>") == 0;
    if (bidirectional) return nullptr;
    for (const ArrowDef& a : kArrows)
        if (s.compare(p, std::strlen(a.token), a.token) == 0) return &a;
    return nullptr;
}

class SeqParser {
public:
    Sequence seq;
    std::string reason;
    Decline why = Decline::Unsupported;

    bool run(const std::string& text) {
        bool sawHeader = false;
        bool inDirective = false;
        size_t at = 0;
        while (at <= text.size()) {
            size_t nl = text.find('\n', at);
            if (nl == std::string::npos) nl = text.size();
            const std::string line = trim(text.substr(at, nl - at));
            at = nl + 1;
            if (inDirective) {
                inDirective = line.find("}%%") == std::string::npos;
                continue;
            }
            if (line.empty() || line[0] == '#') continue;
            if (line.compare(0, 2, "%%") == 0) {
                inDirective =
                    line.compare(0, 3, "%%{") == 0 && line.find("}%%") == std::string::npos;
                continue;
            }
            for (const std::string& piece : splitStatements(line)) {
                const std::string st = trim(piece);
                if (st.empty()) continue;
                if (!sawHeader) {
                    if (st != "sequenceDiagram") return unsupported(st);
                    sawHeader = true;
                } else if (!statement(st)) {
                    return false;
                }
                if (static_cast<int>(seq.events.size()) > tune::kMaxEvents) return tooLarge();
            }
        }
        if (!open.empty()) return unsupported(openStatement.back());
        if (seq.participants.empty()) return fail("empty diagram");
        return true;
    }

private:
    std::map<std::string, int> index;
    std::vector<SeqEvent::Block> open;
    std::vector<std::string> openStatement;

    bool fail(const std::string& r) {
        reason = "sequence: " + r;
        return false;
    }
    bool unsupported(const std::string& what) { return fail("unsupported syntax: " + what); }
    bool tooLarge() {
        why = Decline::TooComplex;
        return fail("too large");
    }

    // The index of the participant called `name`, which a first mention adds on the right.
    bool participantIndex(const std::string& name, int& idx) {
        auto it = index.find(name);
        if (it != index.end()) {
            idx = it->second;
            return true;
        }
        if (name.empty() || name.find("@{") != std::string::npos) return unsupported(name);
        Participant p;
        p.id = name;
        std::string r;
        if (!plainLabelText(name, p.label, r)) return fail(r);
        if (static_cast<int>(seq.participants.size()) >= tune::kMaxParticipants) return tooLarge();
        idx = static_cast<int>(seq.participants.size());
        index[name] = idx;
        seq.participants.push_back(p);
        return true;
    }

    // The text of a message, note or block line: entities decoded, `<br>` a line break, and a
    // comment after a `#` dropped. Blank text gives a label with no lines when `mayBeEmpty`.
    bool text(const std::string& raw, bool mayBeEmpty, Label& label, const std::string& st) {
        const std::string t = trim(cutComment(raw));
        label = Label();
        if (t.empty()) return mayBeEmpty ? true : unsupported(st);
        std::string r;
        if (!plainLabelText(t, label, r)) return fail(r);
        return true;
    }

    bool statement(const std::string& st) {
        std::string rest;
        if (keyword(st, "title", rest, true)) {
            Label l;
            if (!text(rest, false, l, st)) return false;
            if (l.lines.size() != 1) return unsupported(st);
            seq.title = l.lines[0];
            return true;
        }
        if (keyword(st, "acctitle", rest, true) || keyword(st, "accdescr", rest, true))
            return st.find(':') != std::string::npos ? true : unsupported(st);
        if (keyword(st, "participant", rest)) return participant(st, rest);
        for (const char* kw : {"actor",
                               "box",
                               "create",
                               "destroy",
                               "autonumber",
                               "rect",
                               "link",
                               "links",
                               "properties",
                               "details"})
            if (keyword(st, kw, rest)) return unsupported(st.substr(0, std::strlen(kw)));
        if (keyword(st, "note", rest)) return note(st, rest);
        struct Opener {
            const char* word;
            SeqEvent::Block block;
        };
        for (const Opener& o : {Opener{"loop", SeqEvent::Block::Loop},
                                Opener{"alt", SeqEvent::Block::Alt},
                                Opener{"opt", SeqEvent::Block::Opt},
                                Opener{"par", SeqEvent::Block::Par},
                                Opener{"critical", SeqEvent::Block::Critical},
                                Opener{"break", SeqEvent::Block::Break}}) {
            if (keyword(st, o.word, rest))
                return block(SeqEvent::Kind::BlockOpen, o.block, rest, st);
        }
        struct Divider {
            const char* word;
            SeqEvent::Block block;
        };
        for (const Divider& d : {Divider{"else", SeqEvent::Block::Alt},
                                 Divider{"and", SeqEvent::Block::Par},
                                 Divider{"option", SeqEvent::Block::Critical}}) {
            if (keyword(st, d.word, rest)) {
                if (open.empty() || open.back() != d.block) return unsupported(st);
                return block(SeqEvent::Kind::BlockDivider, d.block, rest, st);
            }
        }
        if (keyword(st, "end", rest)) {
            if (open.empty() || !rest.empty()) return unsupported(st);
            SeqEvent e;
            e.kind = SeqEvent::Kind::BlockClose;
            e.block = open.back();
            open.pop_back();
            openStatement.pop_back();
            seq.events.push_back(e);
            return true;
        }
        const bool act = keyword(st, "activate", rest);
        if (act || keyword(st, "deactivate", rest)) {
            auto it = index.find(rest);
            if (it == index.end()) return unsupported(st);  // Mermaid cannot draw it either
            SeqEvent e;
            e.kind = act ? SeqEvent::Kind::Activate : SeqEvent::Kind::Deactivate;
            e.from = e.to = it->second;
            seq.events.push_back(e);
            return true;
        }
        return message(st);
    }

    // `participant A` or `participant A as Display name`.
    bool participant(const std::string& st, const std::string& rest) {
        if (rest.empty()) return unsupported(st);
        std::string id = rest, alias;
        bool aliased = false;
        for (size_t p = 0; p + 3 < rest.size(); ++p) {
            if (std::isspace(static_cast<unsigned char>(rest[p])) &&
                std::tolower(static_cast<unsigned char>(rest[p + 1])) == 'a' &&
                std::tolower(static_cast<unsigned char>(rest[p + 2])) == 's' &&
                std::isspace(static_cast<unsigned char>(rest[p + 3]))) {
                id = trim(rest.substr(0, p));
                alias = trim(rest.substr(p + 4));
                aliased = true;
                break;
            }
        }
        int idx = 0;
        if (!participantIndex(id, idx)) return false;
        if (!aliased) return true;
        Label l;
        if (!text(alias, false, l, st)) return false;
        seq.participants[static_cast<size_t>(idx)].label = l;
        return true;
    }

    // `note right of A: text`, `note left of A: text`, `note over A: text`, `note over A,B: text`.
    bool note(const std::string& st, const std::string& rest) {
        const size_t colon = rest.find(':');
        if (colon == std::string::npos) return unsupported(st);
        std::string head = trim(rest.substr(0, colon)), names;
        SeqEvent e;
        e.kind = SeqEvent::Kind::Note;
        std::string w;
        if (keyword(head, "right", w) && keyword(w, "of", names)) {
            e.side = SeqEvent::Side::RightOf;
        } else if (keyword(head, "left", w) && keyword(w, "of", names)) {
            e.side = SeqEvent::Side::LeftOf;
        } else if (keyword(head, "over", names)) {
            e.side = SeqEvent::Side::Over;
        } else {
            return unsupported(st);
        }
        const size_t comma = names.find(',');
        if (comma != std::string::npos && e.side != SeqEvent::Side::Over) return unsupported(st);
        if (!participantIndex(trim(names.substr(0, comma)), e.from)) return false;
        e.to = e.from;
        if (comma != std::string::npos && !participantIndex(trim(names.substr(comma + 1)), e.to))
            return false;
        if (!text(rest.substr(colon + 1), false, e.label, st)) return false;
        seq.events.push_back(e);
        return true;
    }

    bool block(SeqEvent::Kind kind,
               SeqEvent::Block which,
               const std::string& rest,
               const std::string& st) {
        SeqEvent e;
        e.kind = kind;
        e.block = which;
        if (!text(rest, true, e.label, st)) return false;
        if (kind == SeqEvent::Kind::BlockOpen) {
            open.push_back(which);
            openStatement.push_back(st);
        }
        seq.events.push_back(e);
        return true;
    }

    // `A->>B: text`, with `+` or `-` after the arrow to activate or deactivate.
    bool message(const std::string& st) {
        const size_t colon = st.find(':');
        if (colon == std::string::npos) return unsupported(st);
        const std::string head = st.substr(0, colon);
        const ArrowDef* arrow = nullptr;
        size_t at = 0;
        for (size_t p = 0; p < head.size() && !arrow; ++p) {
            bool both = false;
            arrow = arrowAt(head, p, both);
            if (both) return unsupported(head.compare(p, 5, "<<-->") == 0 ? "<<-->>" : "<<->>");
            at = p;
        }
        if (!arrow) return unsupported(st);
        const std::string from = trim(head.substr(0, at));
        std::string to = trim(head.substr(at + std::strlen(arrow->token)));
        SeqEvent e;
        e.kind = SeqEvent::Kind::Message;
        e.line = arrow->line;
        e.head = arrow->head;
        e.async = arrow->async;
        if (!to.empty() && (to[0] == '+' || to[0] == '-')) {
            (to[0] == '+' ? e.activate : e.deactivate) = true;
            to = trim(to.substr(1));
        }
        // A name that holds another arrow, or starts like central-connection syntax, is not read.
        for (const std::string& name : {from, to}) {
            if (name.empty() || name[0] == '(' || name[0] == ')') return unsupported(st);
            for (size_t p = 0; p < name.size(); ++p) {
                bool both = false;
                if (arrowAt(name, p, both) || both) return unsupported(st);
            }
        }
        if (!participantIndex(from, e.from) || !participantIndex(to, e.to)) return false;
        if (!text(st.substr(colon + 1), true, e.label, st)) return false;
        seq.events.push_back(e);
        return true;
    }
};

}  // namespace

bool parseSequence(const std::vector<std::string>& body,
                   Sequence& seq,
                   std::string& reason,
                   Decline* why) {
    if (why) *why = Decline::Unsupported;
    std::vector<std::string> frontMatter;
    bool hasFrontMatter = false;
    std::string text;
    if (!splitDiagram(body, frontMatter, hasFrontMatter, text, reason)) {
        reason = "sequence: " + reason;
        return false;
    }
    SeqParser ps;
    if (hasFrontMatter && !parseFrontMatterTitle(frontMatter, ps.seq.title)) {
        reason = "sequence: unsupported front matter";
        return false;
    }
    if (!ps.run(text)) {
        reason = ps.reason;
        if (why) *why = ps.why;
        return false;
    }
    seq = std::move(ps.seq);
    return true;
}

namespace {

// The width of the widest line of a label, in columns.
int labelWidth(const Label& l, const Env& env) {
    int w = 0;
    for (const std::string& line : l.lines) w = std::max(w, env.width(line));
    return w;
}

}  // namespace

bool drawSequence(const Sequence& seq, const Env& env, Diagram& out, std::string& reason) {
    // What is not drawn yet, the first of it in source order.
    for (const SeqEvent& e : seq.events) {
        switch (e.kind) {
        case SeqEvent::Kind::Note: reason = "sequence: not yet: note"; return false;
        case SeqEvent::Kind::BlockOpen:
        case SeqEvent::Kind::BlockDivider:
        case SeqEvent::Kind::BlockClose: reason = "sequence: not yet: block"; return false;
        case SeqEvent::Kind::Message:
            if (e.from == e.to) {
                reason = "sequence: not yet: self-message";
                return false;
            }
            break;
        default: break;  // activations are read and not drawn yet
        }
    }

    const int n = static_cast<int>(seq.participants.size());

    // Columns. A box is its widest line plus the padding, made odd so that its lifeline, in the
    // middle, runs down the centre of a cell. Neighbouring boxes are kSepTd columns apart, and a
    // message needs room between its lifelines for its text and a column on either side. Each
    // constraint asks for a distance between two lifelines; the gaps between neighbouring
    // lifelines are widened to meet them, the constraints that span fewest gaps first, and a
    // deficit over several gaps is shared among them.
    std::vector<int> boxW(static_cast<size_t>(n)), half(static_cast<size_t>(n));
    int boxLines = 1;
    for (int i = 0; i < n; ++i) {
        const Label& l = seq.participants[static_cast<size_t>(i)].label;
        int w = labelWidth(l, env) + tune::kNodePad;
        if (w % 2 == 0) ++w;
        boxW[static_cast<size_t>(i)] = w;
        half[static_cast<size_t>(i)] = (w - 1) / 2;
        boxLines = std::max(boxLines, static_cast<int>(l.lines.size()));
    }
    std::vector<int> gap(static_cast<size_t>(std::max(0, n - 1)));
    for (int i = 0; i + 1 < n; ++i)
        gap[static_cast<size_t>(i)] =
            half[static_cast<size_t>(i)] + half[static_cast<size_t>(i + 1)] + 1 + tune::kSepTd;
    struct Need {
        int lo, hi, dist;
    };
    std::vector<Need> needs;
    for (const SeqEvent& e : seq.events) {
        if (e.kind != SeqEvent::Kind::Message || e.label.lines.empty()) continue;
        needs.push_back(
            {std::min(e.from, e.to), std::max(e.from, e.to), labelWidth(e.label, env) + 2});
    }
    std::stable_sort(needs.begin(), needs.end(), [](const Need& a, const Need& b) {
        return a.hi - a.lo < b.hi - b.lo;
    });
    for (const Need& nd : needs) {
        const int span = nd.hi - nd.lo;
        int have = 0;
        for (int k = nd.lo; k < nd.hi; ++k) have += gap[static_cast<size_t>(k)];
        if (have >= nd.dist) continue;
        const int deficit = nd.dist - have;
        for (int k = 0; k < span; ++k)
            gap[static_cast<size_t>(nd.lo + k)] += deficit / span + (k < deficit % span ? 1 : 0);
    }
    std::vector<int> centre(static_cast<size_t>(n));
    int cols = 0;
    for (int i = 0; i < n; ++i) {
        const size_t k = static_cast<size_t>(i);
        centre[k] = i == 0 ? half[0] : centre[k - 1] + gap[k - 1];
        cols = std::max(cols, centre[k] + half[k] + 1);
    }
    // A title wider than the diagram widens it, and the diagram is centred under it.
    const int titleRows = seq.title.empty() ? 0 : 2;
    if (titleRows) {
        const int dx = std::max(0, (env.width(seq.title) - cols) / 2);
        for (int& c : centre) c += dx;
        cols = std::max(cols, env.width(seq.title));
    }

    // Rows: the title, the boxes, a blank row, each message (its text, its arrow, a blank row),
    // and the mirrored boxes.
    const int boxH = boxLines + tune::kNodeFrame;
    const int topRow = titleRows;
    int row = topRow + boxH + 1;
    struct Placed {
        const SeqEvent* event;
        int labelRow, arrowRow;
    };
    std::vector<Placed> messages;
    for (const SeqEvent& e : seq.events) {
        if (e.kind != SeqEvent::Kind::Message) continue;
        messages.push_back({&e, row, row + static_cast<int>(e.label.lines.size())});
        row = messages.back().arrowRow + 2;
    }
    const int bottomRow = row;
    const int rows = bottomRow + boxH;

    Scene scene;
    scene.cols = cols;
    scene.rows = rows;
    scene.title = seq.title;
    auto outline = [&](int col, int r, int w, int h) {
        Stroke st;
        st.kind = Stroke::Outline;
        st.shape = Shape::Rect;
        st.pts = {{2 * col + 1, 2 * r + 1}, {2 * (col + w) - 1, 2 * (r + h) - 1}};
        scene.strokes.push_back(st);
    };
    auto addText = [&](const CellBox& box, const std::vector<std::string>& lines) {
        Scene::Text t;
        t.box = box;
        t.lines = lines;
        scene.texts.push_back(std::move(t));
    };
    // The boxes: an outline, and the name in the middle, with the same name on both bands.
    for (int i = 0; i < n; ++i) {
        const size_t k = static_cast<size_t>(i);
        const std::vector<std::string>& lines = seq.participants[k].label.lines;
        const int col = centre[k] - half[k];
        const int off = (boxH - tune::kNodeFrame - static_cast<int>(lines.size())) / 2;
        for (int band : {topRow, bottomRow}) {
            outline(col, band, boxW[k], boxH);
            addText(
                {col + 2, band + 1 + off, boxW[k] - tune::kNodePad, static_cast<int>(lines.size())},
                lines);
        }
    }
    // The lifelines, each broken where the text of a message that passes it is.
    for (int i = 0; i < n; ++i) {
        const int x = 2 * centre[static_cast<size_t>(i)] + 1;
        std::vector<std::pair<int, int>> gaps;  // rows, from and to
        for (const Placed& m : messages) {
            const int lines = static_cast<int>(m.event->label.lines.size());
            if (lines == 0) continue;
            const int w = labelWidth(m.event->label, env);
            const int lo = std::min(centre[static_cast<size_t>(m.event->from)],
                                    centre[static_cast<size_t>(m.event->to)]);
            const int hi = std::max(centre[static_cast<size_t>(m.event->from)],
                                    centre[static_cast<size_t>(m.event->to)]);
            const int start = lo + 1 + (hi - lo - 1 - w) / 2;
            const int c = centre[static_cast<size_t>(i)];
            if (c >= start && c < start + w) gaps.push_back({m.labelRow, m.arrowRow});
        }
        std::sort(gaps.begin(), gaps.end());
        int from = 2 * (topRow + boxH - 1) + 1;
        unsigned char reachFrom = 0;
        auto piece = [&](int to, unsigned char reachTo) {
            Stroke st;
            st.kind = Stroke::Segment;
            st.line = Line::Dotted;
            st.pts = {{x, from}, {x, to}};
            st.reach = static_cast<unsigned char>((reachFrom ? 1 : 0) | (reachTo ? 2 : 0));
            scene.strokes.push_back(st);
        };
        for (const auto& g : gaps) {
            piece(2 * (g.first - 1) + 1, 1);
            from = 2 * g.second + 1;
            reachFrom = 1;
        }
        piece(2 * bottomRow + 1, 0);
    }
    // The messages: a stroke from one lifeline to the other, ended by a marker, and the text
    // centred between the lifelines.
    for (const Placed& m : messages) {
        const SeqEvent& e = *m.event;
        const int xa = 2 * centre[static_cast<size_t>(e.from)] + 1;
        const int xb = 2 * centre[static_cast<size_t>(e.to)] + 1;
        const int y = 2 * m.arrowRow + 1;
        Stroke line;
        line.kind = Stroke::Segment;
        line.line = e.line;
        line.pts = {{xa, y}, {xb, y}};
        scene.strokes.push_back(line);
        if (e.head != End::None) {
            Stroke head;
            head.kind = e.head == End::Cross ? Stroke::CrossEnd : Stroke::Arrowhead;
            head.line = e.line;
            head.pts = {{xb, y}, {xa, y}};
            scene.strokes.push_back(head);
        }
        if (e.label.lines.empty()) continue;
        const int w = labelWidth(e.label, env);
        const int lo =
            std::min(centre[static_cast<size_t>(e.from)], centre[static_cast<size_t>(e.to)]);
        const int hi =
            std::max(centre[static_cast<size_t>(e.from)], centre[static_cast<size_t>(e.to)]);
        addText(
            {lo + 1 + (hi - lo - 1 - w) / 2, m.labelRow, w, static_cast<int>(e.label.lines.size())},
            e.label.lines);
    }

    // Validate: no two texts share a cell, and no stroke touches a cell of text. (A lifeline stops
    // short of the text it passes behind, and an arrow runs on a row of its own.)
    std::vector<char> text(static_cast<size_t>(cols) * static_cast<size_t>(rows), 0);
    auto cell = [&](int c, int r) -> char& {
        return text[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)];
    };
    if (titleRows)
        for (int c = 0; c < cols; ++c) cell(c, 0) = 1;
    for (const Scene::Text& t : scene.texts) {
        for (int r = t.box.row; r < t.box.row + t.box.rows; ++r) {
            for (int c = t.box.col; c < t.box.col + t.box.cols; ++c) {
                if (c < 0 || c >= cols || r < 0 || r >= rows || cell(c, r)) {
                    reason = "overlap";
                    return false;
                }
                cell(c, r) = 1;
            }
        }
    }
    auto touches = [&](int c0, int r0, int c1, int r1) {
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
                if (cell(c, r)) return true;
        return false;
    };
    for (const Stroke& st : scene.strokes) {
        if (st.kind == Stroke::Segment) {
            if (touches(std::min(st.pts[0].first, st.pts[1].first) / 2,
                        std::min(st.pts[0].second, st.pts[1].second) / 2,
                        std::max(st.pts[0].first, st.pts[1].first) / 2,
                        std::max(st.pts[0].second, st.pts[1].second) / 2)) {
                reason = "overlap";
                return false;
            }
        } else if (st.kind == Stroke::Outline) {
            const int c0 = st.pts[0].first / 2, r0 = st.pts[0].second / 2;
            const int c1 = st.pts[1].first / 2, r1 = st.pts[1].second / 2;
            if (touches(c0, r0, c1, r0) || touches(c0, r1, c1, r1) || touches(c0, r0, c0, r1) ||
                touches(c1, r0, c1, r1)) {
                reason = "overlap";
                return false;
            }
        }
    }
    return paintScene(scene, env, out, reason);
}

bool renderSequence(const std::vector<std::string>& body,
                    const Env& env,
                    Diagram& out,
                    std::string& reason,
                    Decline* why) {
    Sequence seq;
    if (!parseSequence(body, seq, reason, why)) return false;
    return drawSequence(seq, env, out, reason);
}

}  // namespace mermaid
