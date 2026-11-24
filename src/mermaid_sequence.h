// The model of a Mermaid sequence diagram and its entry points: parse and draw
// (mermaid_sequence.cpp). The drawing builds a Scene and hands it to the shared painter
// (paintScene). See docs/plan-mermaid-sequence.md.
#pragma once

#include <string>
#include <vector>

#include "mermaid_model.h"

namespace mermaid {

struct Participant {
    std::string id;  // as written; the case matters, and it may hold spaces
    Label label;     // the display name: the id, or the text after `as`
};

// One thing that happens, in source order. Which fields mean what depends on `kind`.
struct SeqEvent {
    enum class Kind { Message, Note, Activate, Deactivate, BlockOpen, BlockDivider, BlockClose };
    enum class Side { LeftOf, RightOf, Over };
    enum class Block { Loop, Alt, Opt, Par, Critical, Break };

    Kind kind = Kind::Message;
    // Message: the sender and the receiver (equal for a message to itself). Note: the participants
    // it is over (equal for a note beside one participant). Activate, Deactivate: `from`.
    int from = 0, to = 0;
    // Message: the line (solid or dotted) and what ends it. An async arrow (`-)`) is drawn as an
    // ordinary one.
    Line line = Line::Solid;
    End head = End::None;
    bool async = false;
    // Message: `+` after the arrow activates the receiver, `-` deactivates the sender.
    bool activate = false, deactivate = false;
    Side side = Side::Over;     // Note
    Block block = Block::Loop;  // BlockOpen, BlockDivider: which block, by its opening keyword
    // Message and Note: the text. BlockOpen, BlockDivider: the text after the keyword (no lines
    // when there is none).
    Label label;
};

struct Sequence {
    std::string title;
    std::vector<Participant> participants;  // left to right, in order of first mention
    std::vector<SeqEvent> events;
};

// Parse the body of a ```mermaid block that starts with `sequenceDiagram`. On failure `reason`
// starts with `sequence:`; `why` (if not null) says whether the diagram is too large (TooComplex)
// or merely outside the supported syntax (Unsupported).
bool parseSequence(const std::vector<std::string>& body,
                   Sequence& seq,
                   std::string& reason,
                   Decline* why);

// Lay a sequence diagram out and draw it. Declines (false) what it cannot lay out without text
// and strokes sharing a cell, with a reason that starts with `sequence:`.
bool drawSequence(const Sequence& seq, const Env& env, Diagram& out, std::string& reason);

// parseSequence, then drawSequence.
bool renderSequence(const std::vector<std::string>& body,
                    const Env& env,
                    Diagram& out,
                    std::string& reason,
                    Decline* why);

}  // namespace mermaid
