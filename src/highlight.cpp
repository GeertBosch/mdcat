// highlight.cpp — syntax highlighting for fenced code blocks.
//
// Design: a character-by-character state machine that processes all lines joined by '\n'
// as a single stream, so block-spanning constructs (/* */ comments, Python docstrings) work
// naturally.  The result is split back into lines and returned.
//
// Each language is described by a small LangConfig struct; the tokenizer is shared.
//
// Known limitations (intentionally out of scope):
//   - C++  raw strings  R"delimiter(...)delimiter"
//   - Shell / Ruby heredocs
//   - JS regex literals
//   - Nested block comments (Rust /* /* */ */)
//   - Template literal ${...} re-highlighting

#include "highlight.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Color constants — all are fg-only; the caller keeps the bg active.
// Changing a color means changing exactly one string here.
// ---------------------------------------------------------------------------
// kColReset must cancel every attribute that any token color activates (bold=22, italic=23)
// in addition to restoring the fg.  Using SGR 0 would also reset the background set by the
// caller's kCodeOn, so we cancel only what we set.
//
// Two fixed palettes, one per terminal theme. Both are immutable; setHighlightTheme (called once,
// before any highlightCode) just points `pal` at one of them. Light keeps the historical saturated
// / dark hues (readable on the light-gray code background); dark uses the SAME hues lightened, with
// a light-gray code fg in `reset`. The `reset` fg MUST match mdcat's code-block fg (kCodeOn): 236
// light, 252 dark, so an un-coloured token returns to the panel's text colour. (An identifier
// carries no extra colour — it is emitted on the code-block fg the surrounding kCodeOn already set
// — so there is no identifier entry; the reset fg is the identifier colour.)
struct Palette {
    std::string reset;  // cancel bold+italic, restore code fg
    std::string keyword;
    std::string str;  // string literals
    std::string comment;
    std::string number;
    std::string preproc;
};

const Palette kLightPalette = {
    /*reset*/ "\033[22;23;38;5;236m",  // dark-gray code fg
    /*keyword*/ "\033[38;5;26;1m",     // blue bold
    /*str*/ "\033[38;5;88m",           // dark red
    /*comment*/ "\033[38;5;22;3m",     // dark green italic
    /*number*/ "\033[38;5;125m",       // magenta
    /*preproc*/ "\033[38;5;130m",      // orange/brown
};

const Palette kDarkPalette = {
    /*reset*/ "\033[22;23;38;5;252m",  // light-gray code fg
    /*keyword*/ "\033[38;5;75;1m",     // light blue bold
    /*str*/ "\033[38;5;210m",          // salmon / light red
    /*comment*/ "\033[38;5;108;3m",    // light green italic
    /*number*/ "\033[38;5;176m",       // light pink / magenta
    /*preproc*/ "\033[38;5;180m",      // tan / light orange
};

// The active palette: a single mutable selector, defaulting to light (mdcat's historical look).
const Palette* pal = &kLightPalette;

// Accessors so the tokenizer reads the active palette through one indirection.
const std::string& kColReset() {
    return pal->reset;
}
const std::string& kColKeyword() {
    return pal->keyword;
}
const std::string& kColString() {
    return pal->str;
}
const std::string& kColComment() {
    return pal->comment;
}
const std::string& kColNumber() {
    return pal->number;
}
const std::string& kColPreproc() {
    return pal->preproc;
}

// ---------------------------------------------------------------------------
// Language configuration
// ---------------------------------------------------------------------------
struct LangConfig {
    std::vector<std::string> keywords;
    std::string lineComment;       // e.g. "//" or "#"; empty = none
    std::string blockOpen;         // e.g. "/*"; empty = none
    std::string blockClose;        // e.g. "*/"
    bool tripleString = false;     // Python """ / '''
    bool charLiterals = false;     // C/C++/Java/Ada: 'x'
    bool hasPreprocessor = false;  // C/C++: lines beginning with #
    char digitSep = '\0';  // digit separator in numeric literals (' for C++/Rust, _ for others)
};

// ---------------------------------------------------------------------------
// Language keyword tables
// ---------------------------------------------------------------------------
LangConfig makeCpp() {
    return {
        {"alignas",
         "alignof",
         "and",
         "and_eq",
         "asm",
         "auto",
         "bitand",
         "bitor",
         "bool",
         "break",
         "case",
         "catch",
         "char",
         "char8_t",
         "char16_t",
         "char32_t",
         "class",
         "compl",
         "concept",
         "const",
         "consteval",
         "constexpr",
         "constinit",
         "const_cast",
         "continue",
         "co_await",
         "co_return",
         "co_yield",
         "decltype",
         "default",
         "delete",
         "do",
         "double",
         "dynamic_cast",
         "else",
         "enum",
         "explicit",
         "export",
         "extern",
         "false",
         "float",
         "for",
         "friend",
         "goto",
         "if",
         "inline",
         "int",
         "long",
         "mutable",
         "namespace",
         "new",
         "noexcept",
         "not",
         "not_eq",
         "nullptr",
         "operator",
         "or",
         "or_eq",
         "private",
         "protected",
         "public",
         "register",
         "reinterpret_cast",
         "requires",
         "return",
         "short",
         "signed",
         "sizeof",
         "static",
         "static_assert",
         "static_cast",
         "struct",
         "switch",
         "template",
         "this",
         "thread_local",
         "throw",
         "true",
         "try",
         "typedef",
         "typeid",
         "typename",
         "union",
         "unsigned",
         "using",
         "virtual",
         "void",
         "volatile",
         "wchar_t",
         "while",
         "xor",
         "xor_eq",
         // common attributes / specifiers often treated as keywords
         "override",
         "final",
         "[[nodiscard]]",
         "[[maybe_unused]]",
         "[[likely]]",
         "[[unlikely]]"},
        "//",
        "/*",
        "*/",
        /*tripleString=*/false,
        /*charLiterals=*/true,
        /*hasPreprocessor=*/true,
        /*digitSep=*/'\'',
    };
}

LangConfig makeC() {
    auto cfg = makeCpp();
    // Remove C++-only keywords
    static const std::vector<std::string> cppOnly = {"alignof",
                                                     "and",
                                                     "and_eq",
                                                     "bitand",
                                                     "bitor",
                                                     "bool",
                                                     "catch",
                                                     "char8_t",
                                                     "char16_t",
                                                     "char32_t",
                                                     "class",
                                                     "compl",
                                                     "concept",
                                                     "const_cast",
                                                     "co_await",
                                                     "co_return",
                                                     "co_yield",
                                                     "decltype",
                                                     "delete",
                                                     "dynamic_cast",
                                                     "explicit",
                                                     "export",
                                                     "friend",
                                                     "inline",
                                                     "mutable",
                                                     "namespace",
                                                     "new",
                                                     "noexcept",
                                                     "not",
                                                     "not_eq",
                                                     "nullptr",
                                                     "operator",
                                                     "or",
                                                     "or_eq",
                                                     "private",
                                                     "protected",
                                                     "public",
                                                     "reinterpret_cast",
                                                     "requires",
                                                     "static_assert",
                                                     "static_cast",
                                                     "template",
                                                     "this",
                                                     "thread_local",
                                                     "throw",
                                                     "try",
                                                     "typeid",
                                                     "typename",
                                                     "using",
                                                     "virtual",
                                                     "wchar_t",
                                                     "xor",
                                                     "xor_eq",
                                                     "override",
                                                     "final",
                                                     "[[nodiscard]]",
                                                     "[[maybe_unused]]",
                                                     "[[likely]]",
                                                     "[[unlikely]]"};
    auto& kw = cfg.keywords;
    kw.erase(std::remove_if(kw.begin(),
                            kw.end(),
                            [&](const std::string& k) {
                                return std::find(cppOnly.begin(), cppOnly.end(), k) !=
                                    cppOnly.end();
                            }),
             kw.end());
    // Add C23 keywords
    for (auto& k : std::vector<std::string>{"_Alignas",
                                            "_Alignof",
                                            "_Atomic",
                                            "_Bool",
                                            "_Complex",
                                            "_Generic",
                                            "_Imaginary",
                                            "_Noreturn",
                                            "_Static_assert",
                                            "_Thread_local",
                                            "nullptr",
                                            "true",
                                            "false",
                                            "constexpr"})
        kw.push_back(k);
    cfg.digitSep = '\'';
    return cfg;
}

LangConfig makePython() {
    return {
        {"False",
         "None",
         "True",
         "and",
         "as",
         "assert",
         "async",
         "await",
         "break",
         "class",
         "continue",
         "def",
         "del",
         "elif",
         "else",
         "except",
         "finally",
         "for",
         "from",
         "global",
         "if",
         "import",
         "in",
         "is",
         "lambda",
         "nonlocal",
         "not",
         "or",
         "pass",
         "raise",
         "return",
         "try",
         "while",
         "with",
         "yield",
         // common builtins often visually treated as keywords
         "print",
         "len",
         "range",
         "enumerate",
         "zip",
         "map",
         "filter",
         "type",
         "isinstance",
         "issubclass",
         "super",
         "object",
         "int",
         "float",
         "str",
         "list",
         "dict",
         "set",
         "tuple",
         "bool",
         "bytes",
         "bytearray",
         "memoryview",
         "open",
         "input",
         "abs",
         "round",
         "min",
         "max",
         "sum",
         "sorted",
         "reversed",
         "any",
         "all",
         "vars",
         "dir",
         "getattr",
         "setattr",
         "hasattr",
         "delattr",
         "staticmethod",
         "classmethod",
         "property"},
        "#",
        "",
        "",
        /*tripleString=*/true,
        /*charLiterals=*/false,
        /*hasPreprocessor=*/false,
        /*digitSep=*/'_',
    };
}

LangConfig makeJavaScript() {
    return {
        {"abstract",  "arguments",  "async",  "await",     "boolean", "break",      "byte",
         "case",      "catch",      "char",   "class",     "const",   "continue",   "debugger",
         "default",   "delete",     "do",     "double",    "else",    "enum",       "eval",
         "export",    "extends",    "false",  "final",     "finally", "float",      "for",
         "from",      "function",   "get",    "goto",      "if",      "implements", "import",
         "in",        "instanceof", "int",    "interface", "let",     "long",       "native",
         "new",       "null",       "of",     "package",   "private", "protected",  "public",
         "return",    "set",        "short",  "static",    "super",   "switch",     "synchronized",
         "this",      "throw",      "throws", "transient", "true",    "try",        "typeof",
         "undefined", "var",        "void",   "volatile",  "while",   "with",       "yield"},
        "//",
        "/*",
        "*/",
        /*tripleString=*/false,
        /*charLiterals=*/false,
        /*hasPreprocessor=*/false,
        /*digitSep=*/'_',
    };
}

LangConfig makeTypeScript() {
    auto cfg = makeJavaScript();
    for (auto& k : std::vector<std::string>{
             "any",       "as",     "asserts",   "bigint", "declare",  "infer",    "is",
             "keyof",     "never",  "namespace", "object", "override", "readonly", "require",
             "satisfies", "symbol", "type",      "unique", "unknown",  "using"})
        cfg.keywords.push_back(k);
    return cfg;
}

LangConfig makeGo() {
    return {
        {"break",      "case",        "chan",       "const",   "continue", "default", "defer",
         "else",       "fallthrough", "for",        "func",    "go",       "goto",    "if",
         "import",     "interface",   "map",        "package", "range",    "return",  "select",
         "struct",     "switch",      "type",       "var",     "any",      "bool",    "byte",
         "comparable", "complex64",   "complex128", "error",   "float32",  "float64", "int",
         "int8",       "int16",       "int32",      "int64",   "rune",     "string",  "uint",
         "uint8",      "uint16",      "uint32",     "uint64",  "uintptr",  "true",    "false",
         "nil",        "iota",        "append",     "cap",     "close",    "copy",    "delete",
         "len",        "make",        "new",        "panic",   "print",    "println", "recover"},
        "//",
        "/*",
        "*/",
        /*tripleString=*/false,
        /*charLiterals=*/true,
        /*hasPreprocessor=*/false,
        /*digitSep=*/'_',
    };
}

LangConfig makeRust() {
    return {
        {"as",
         "async",
         "await",
         "break",
         "const",
         "continue",
         "crate",
         "dyn",
         "else",
         "enum",
         "extern",
         "false",
         "fn",
         "for",
         "if",
         "impl",
         "in",
         "let",
         "loop",
         "match",
         "mod",
         "move",
         "mut",
         "pub",
         "ref",
         "return",
         "self",
         "Self",
         "static",
         "struct",
         "super",
         "trait",
         "true",
         "type",
         "union",
         "unsafe",
         "use",
         "where",
         "while",
         "abstract",
         "become",
         "box",
         "do",
         "final",
         "macro",
         "override",
         "priv",
         "try",
         "typeof",
         "unsized",
         "virtual",
         "yield",
         // primitive types
         "bool",
         "char",
         "f32",
         "f64",
         "i8",
         "i16",
         "i32",
         "i64",
         "i128",
         "isize",
         "str",
         "u8",
         "u16",
         "u32",
         "u64",
         "u128",
         "usize",
         // common stdlib items often treated visually as keywords
         "Option",
         "Some",
         "None",
         "Result",
         "Ok",
         "Err",
         "String",
         "Vec",
         "Box",
         "println",
         "eprintln",
         "panic",
         "assert",
         "assert_eq",
         "assert_ne",
         "todo",
         "unimplemented",
         "unreachable"},
        "//",
        "/*",
        "*/",
        /*tripleString=*/false,
        /*charLiterals=*/true,
        /*hasPreprocessor=*/false,
        /*digitSep=*/'_',
    };
}

LangConfig makeJava() {
    return {
        {"abstract",  "assert", "boolean",    "break",        "byte",       "case",    "catch",
         "char",      "class",  "const",      "continue",     "default",    "do",      "double",
         "else",      "enum",   "extends",    "final",        "finally",    "float",   "for",
         "goto",      "if",     "implements", "import",       "instanceof", "int",     "interface",
         "long",      "native", "new",        "non-sealed",   "package",    "permits", "private",
         "protected", "public", "record",     "return",       "sealed",     "short",   "static",
         "strictfp",  "super",  "switch",     "synchronized", "this",       "throw",   "throws",
         "transient", "try",    "var",        "void",         "volatile",   "while",   "true",
         "false",     "null",   "yield",      "when",         "pattern"},
        "//",
        "/*",
        "*/",
        /*tripleString=*/false,
        /*charLiterals=*/true,
        /*hasPreprocessor=*/false,
        /*digitSep=*/'_',
    };
}

LangConfig makeAda() {
    return {
        {"abort",
         "abs",
         "abstract",
         "accept",
         "access",
         "aliased",
         "all",
         "and",
         "array",
         "at",
         "begin",
         "body",
         "case",
         "constant",
         "declare",
         "delay",
         "delta",
         "digits",
         "do",
         "else",
         "elsif",
         "end",
         "entry",
         "exception",
         "exit",
         "for",
         "function",
         "generic",
         "goto",
         "if",
         "in",
         "interface",
         "is",
         "limited",
         "loop",
         "mod",
         "new",
         "not",
         "null",
         "of",
         "or",
         "others",
         "out",
         "overriding",
         "package",
         "parallel",
         "pragma",
         "private",
         "procedure",
         "protected",
         "raise",
         "range",
         "record",
         "rem",
         "renames",
         "requeue",
         "return",
         "reverse",
         "select",
         "separate",
         "some",
         "subtype",
         "synchronized",
         "tagged",
         "task",
         "terminate",
         "then",
         "type",
         "until",
         "use",
         "when",
         "while",
         "with",
         "xor",
         // predefined types / attributes commonly highlighted
         "Boolean",
         "Integer",
         "Float",
         "Character",
         "String",
         "Natural",
         "Positive",
         "Duration",
         "Wide_Character",
         "Wide_String",
         "True",
         "False"},
        "--",
        "",
        "",
        /*tripleString=*/false,
        /*charLiterals=*/true,
        /*hasPreprocessor=*/false,
        /*digitSep=*/'_',
    };
}

LangConfig makeShell() {
    return {
        {"case",
         "do",
         "done",
         "elif",
         "else",
         "esac",
         "fi",
         "for",
         "function",
         "if",
         "in",
         "select",
         "then",
         "time",
         "until",
         "while",
         // builtins commonly highlighted
         "alias",
         "bg",
         "bind",
         "break",
         "builtin",
         "caller",
         "cd",
         "command",
         "compgen",
         "complete",
         "compopt",
         "continue",
         "declare",
         "dirs",
         "disown",
         "echo",
         "enable",
         "eval",
         "exec",
         "exit",
         "export",
         "false",
         "fc",
         "fg",
         "getopts",
         "hash",
         "help",
         "history",
         "jobs",
         "kill",
         "let",
         "local",
         "logout",
         "mapfile",
         "popd",
         "printf",
         "pushd",
         "pwd",
         "read",
         "readarray",
         "readonly",
         "return",
         "set",
         "shift",
         "shopt",
         "source",
         "suspend",
         "test",
         "times",
         "trap",
         "true",
         "type",
         "typeset",
         "ulimit",
         "umask",
         "unalias",
         "unset",
         "wait"},
        "#",
        "",
        "",
        /*tripleString=*/false,
        /*charLiterals=*/false,
        /*hasPreprocessor=*/false,
        /*digitSep=*/'_',
    };
}

// JSON and YAML: minimal — just strings and a few literal keywords.
LangConfig makeJson() {
    return {{"true", "false", "null"}, "", "", "", false, false, false, '\0'};
}

LangConfig makeYaml() {
    return {{"true", "false", "null", "yes", "no", "on", "off"},
            "#",
            "",
            "",
            false,
            false,
            false,
            '\0'};
}

// ---------------------------------------------------------------------------
// Language registry
// ---------------------------------------------------------------------------
const std::map<std::string, LangConfig>& langRegistry() {
    static const std::map<std::string, LangConfig> reg = {
        {"c", makeC()},           {"cpp", makeCpp()},        {"c++", makeCpp()},
        {"cxx", makeCpp()},       {"cc", makeCpp()},         {"h", makeCpp()},
        {"python", makePython()}, {"py", makePython()},      {"javascript", makeJavaScript()},
        {"js", makeJavaScript()}, {"jsx", makeJavaScript()}, {"typescript", makeTypeScript()},
        {"ts", makeTypeScript()}, {"tsx", makeTypeScript()}, {"go", makeGo()},
        {"rust", makeRust()},     {"rs", makeRust()},        {"java", makeJava()},
        {"ada", makeAda()},       {"adb", makeAda()},        {"ads", makeAda()},
        {"sh", makeShell()},      {"bash", makeShell()},     {"zsh", makeShell()},
        {"shell", makeShell()},   {"json", makeJson()},      {"yaml", makeYaml()},
        {"yml", makeYaml()},
    };
    return reg;
}

// ---------------------------------------------------------------------------
// Tokenizer state machine
// ---------------------------------------------------------------------------
enum class State {
    Normal,
    Identifier,  // accumulating [_a-zA-Z0-9] — resolved as keyword or plain on exit
    Number,      // numeric literal
    LineComment,
    BlockComment,
    StringDQ,      // double-quoted string
    StringSQ,      // single-quoted string (languages without char literals)
    CharLit,       // single-quoted char literal (C/C++/Java/Ada)
    TripleDQ,      // Python """ docstring
    TripleSQ,      // Python ''' docstring
    Preprocessor,  // C/C++ #directive line
};

struct Tokenizer {
    const LangConfig& cfg;
    // Build a set for O(1) keyword lookup.
    std::map<std::string, bool> kwSet;

    explicit Tokenizer(const LangConfig& c) : cfg(c) {
        for (const auto& k : c.keywords) kwSet[k] = true;
    }

    bool isKw(const std::string& word) const {
        if (cfg.keywords.empty()) return false;
        // Ada is case-insensitive for keywords.
        // We detect Ada by checking whether "--" is the line comment marker.
        if (cfg.lineComment == "--") {
            std::string lo = word;
            for (char& ch : lo)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return kwSet.count(lo) || kwSet.count(word);
        }
        return kwSet.count(word) != 0;
    }

    // Returns the full highlighted flat string (lines joined and re-split by the caller).
    std::string run(const std::string& src) {
        std::string out;
        out.reserve(src.size() * 2);

        State state = State::Normal;
        bool escaped = false;  // one-shot: next char is escaped (inside string/char)
        std::string identBuf;  // accumulated identifier chars
        size_t n = src.size();

        // Helper: emit current color reset (back to code-fg) inline.
        auto reset = [&] { out += kColReset(); };

        // Helper: flush an accumulated identifier, coloring if it's a keyword.
        auto flushIdent = [&] {
            if (identBuf.empty()) return;
            if (isKw(identBuf)) {
                out += kColKeyword();
                out += identBuf;
                reset();
            } else {
                out += identBuf;  // kColIdentifier == kColReset, no extra sequence needed
            }
            identBuf.clear();
        };

        for (size_t i = 0; i < n;) {
            char c = src[i];

            // ----------------------------------------------------------------
            // Escaped character inside string/char literal
            // ----------------------------------------------------------------
            if (escaped) {
                escaped = false;
                out += c;
                ++i;
                continue;
            }

            switch (state) {

            // ----------------------------------------------------------------
            case State::Normal: {
                // Check for preprocessor line start (C/C++): '#' at column 0 after spaces.
                if (cfg.hasPreprocessor && c == '#') {
                    // Only treat as preprocessor if we're at the start of a line.
                    // We track this by checking that everything before i on this line is spaces.
                    bool atLineStart = true;
                    for (size_t k = i; k > 0; --k) {
                        char pk = src[k - 1];
                        if (pk == '\n') break;
                        if (pk != ' ' && pk != '\t') {
                            atLineStart = false;
                            break;
                        }
                    }
                    if (atLineStart) {
                        out += kColPreproc();
                        out += c;
                        state = State::Preprocessor;
                        ++i;
                        continue;
                    }
                }

                // Line comment
                if (!cfg.lineComment.empty() &&
                    src.compare(i, cfg.lineComment.size(), cfg.lineComment) == 0) {
                    out += kColComment();
                    out += cfg.lineComment;
                    i += cfg.lineComment.size();
                    state = State::LineComment;
                    continue;
                }

                // Block comment open
                if (!cfg.blockOpen.empty() &&
                    src.compare(i, cfg.blockOpen.size(), cfg.blockOpen) == 0) {
                    out += kColComment();
                    out += cfg.blockOpen;
                    i += cfg.blockOpen.size();
                    state = State::BlockComment;
                    continue;
                }

                // Triple-quoted string (Python)
                if (cfg.tripleString) {
                    if (src.compare(i, 3, "\"\"\"") == 0) {
                        out += kColString() + "\"\"\"";
                        i += 3;
                        state = State::TripleDQ;
                        continue;
                    }
                    if (src.compare(i, 3, "'''") == 0) {
                        out += kColString() + "'''";
                        i += 3;
                        state = State::TripleSQ;
                        continue;
                    }
                }

                // Double-quoted string
                if (c == '"') {
                    out += kColString();
                    out += c;
                    state = State::StringDQ;
                    ++i;
                    continue;
                }

                // Single-quoted: char literal vs. string depending on language
                if (c == '\'') {
                    out += kColString();
                    out += c;
                    state = cfg.charLiterals ? State::CharLit : State::StringSQ;
                    ++i;
                    continue;
                }

                // Start of identifier or keyword
                if (c == '_' || std::isalpha(static_cast<unsigned char>(c))) {
                    identBuf += c;
                    state = State::Identifier;
                    ++i;
                    continue;
                }

                // Start of number: digit, or '.' followed by a digit
                if (std::isdigit(static_cast<unsigned char>(c)) ||
                    (c == '.' && i + 1 < n &&
                     std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
                    out += kColNumber();
                    out += c;
                    state = State::Number;
                    ++i;
                    continue;
                }

                // Everything else: punctuation, operators, whitespace — pass through.
                out += c;
                ++i;
                break;
            }

            // ----------------------------------------------------------------
            case State::Identifier: {
                if (c == '_' || std::isalnum(static_cast<unsigned char>(c))) {
                    identBuf += c;
                    ++i;
                } else {
                    // Word boundary — resolve keyword vs identifier.
                    flushIdent();
                    state = State::Normal;
                    // Do NOT consume c; let Normal re-process it.
                }
                break;
            }

            // ----------------------------------------------------------------
            case State::Number: {
                // Accept digits, hex digits (a-fA-F), digit separator, base prefixes (x/o/b/X/O/B),
                // decimal point, and exponent markers (e/E/p/P with optional sign).
                bool cont = std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F') ||
                    (c == 'x' || c == 'X' || c == 'o' || c == 'O' || c == 'b' || c == 'B') ||
                    c == '.' || (cfg.digitSep != '\0' && c == cfg.digitSep) || c == 'e' ||
                    c == 'E' || c == 'p' || c == 'P' ||
                    ((c == '+' || c == '-') && i > 0 &&
                     (src[i - 1] == 'e' || src[i - 1] == 'E' || src[i - 1] == 'p' ||
                      src[i - 1] == 'P'))
                    // integer suffixes: u/U/l/L/z/Z/f/F
                    || c == 'u' || c == 'U' || c == 'l' || c == 'L' || c == 'z' || c == 'Z' ||
                    c == 'f' || c == 'F';
                if (cont) {
                    out += c;
                    ++i;
                } else {
                    reset();
                    state = State::Normal;
                    // re-process c in Normal
                }
                break;
            }

            // ----------------------------------------------------------------
            case State::LineComment: {
                out += c;
                ++i;
                if (c == '\n') {
                    reset();
                    state = State::Normal;
                }
                break;
            }

            // ----------------------------------------------------------------
            case State::BlockComment: {
                if (c == '\n') {
                    // Re-emit the comment color at the start of each line so that
                    // when the caller splits on '\n' every line carries its own color prefix.
                    reset();
                    out += c;
                    out += kColComment();
                    ++i;
                    break;
                }
                out += c;
                ++i;
                if (!cfg.blockClose.empty() &&
                    src.compare(i - cfg.blockClose.size(), cfg.blockClose.size(), cfg.blockClose) ==
                        0) {
                    reset();
                    state = State::Normal;
                }
                break;
            }

            // ----------------------------------------------------------------
            case State::StringDQ: {
                out += c;
                ++i;
                if (c == '\\') {
                    escaped = true;
                    break;
                }
                if (c == '"') {
                    reset();
                    state = State::Normal;
                }
                break;
            }

            case State::StringSQ: {
                out += c;
                ++i;
                if (c == '\\') {
                    escaped = true;
                    break;
                }
                if (c == '\'') {
                    reset();
                    state = State::Normal;
                }
                break;
            }

            case State::CharLit: {
                out += c;
                ++i;
                if (c == '\\') {
                    escaped = true;
                    break;
                }
                if (c == '\'') {
                    reset();
                    state = State::Normal;
                }
                break;
            }

            // ----------------------------------------------------------------
            case State::TripleDQ: {
                if (c == '\n' && !escaped) {
                    reset();
                    out += c;
                    out += kColString();
                    ++i;
                    break;
                }
                out += c;
                ++i;
                if (c == '\\') {
                    escaped = true;
                    break;
                }
                if (src.compare(i - 3, 3, "\"\"\"") == 0) {
                    reset();
                    state = State::Normal;
                }
                break;
            }

            case State::TripleSQ: {
                if (c == '\n' && !escaped) {
                    reset();
                    out += c;
                    out += kColString();
                    ++i;
                    break;
                }
                out += c;
                ++i;
                if (c == '\\') {
                    escaped = true;
                    break;
                }
                if (src.compare(i - 3, 3, "'''") == 0) {
                    reset();
                    state = State::Normal;
                }
                break;
            }

            // ----------------------------------------------------------------
            case State::Preprocessor: {
                out += c;
                ++i;
                if (c == '\n') {
                    reset();
                    state = State::Normal;
                }
                break;
            }

            }  // switch
        }  // for

        // Flush any open state at end of input.
        flushIdent();
        // If we ended mid-color, reset so the block background is clean.
        if (state != State::Normal && state != State::Identifier) reset();

        return out;
    }
};

// ---------------------------------------------------------------------------
// Normalize the language tag to lowercase.
// ---------------------------------------------------------------------------
std::string normalizeTag(const std::string& lang) {
    std::string lo = lang;
    for (char& c : lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lo;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Select the colour palette for syntax highlighting. Call once, before any highlightCode,
 * after the terminal theme is known. Just repoints pal; both palettes are fixed (see
 * kLightPalette / kDarkPalette). The reset fg in each is kept in sync with mdcat's
 * code-block fg (236 light, 252 dark).
 */
void setHighlightTheme(bool dark) {
    pal = dark ? &kDarkPalette : &kLightPalette;
}

std::vector<std::string> highlightCode(const std::vector<std::string>& lines,
                                       const std::string& lang) {
    if (lang.empty() || lines.empty()) return lines;

    const auto& reg = langRegistry();
    auto it = reg.find(normalizeTag(lang));
    if (it == reg.end()) return lines;

    // Join lines into a single string for cross-line state tracking.
    std::string src;
    for (size_t i = 0; i < lines.size(); ++i) {
        src += lines[i];
        if (i + 1 < lines.size()) src += '\n';
    }

    Tokenizer tok(it->second);
    std::string highlighted = tok.run(src);

    // Split back on '\n'.
    std::vector<std::string> result;
    result.reserve(lines.size());
    std::string cur;
    for (char c : highlighted) {
        if (c == '\n') {
            result.push_back(std::move(cur));
            cur.clear();
        } else
            cur += c;
    }
    result.push_back(std::move(cur));

    // Pad to original line count (should always match, but be safe).
    while (result.size() < lines.size()) result.push_back({});

    return result;
}
