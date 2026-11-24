/**
 * @file lexer.cpp
 * @brief Lexer implementation. See lexer.hpp for the rules it enforces.
 */
#include "lexer.hpp"

#include <cctype>
#include <cstring>
#include <set>

namespace dolphin {

// Reserved words. Some aren't implemented yet (struct, layout, extern, needs,
// mut, lambda, import, from); they're reserved now so programs written today
// don't use them as identifiers and break later.
static const std::set<std::string> kKeywords = {
    "def", "return", "if", "elif", "else", "while", "for", "in", "break", "continue",
    "pass", "try", "except", "as", "raise", "and", "or", "not", "True", "False", "None",
    "lambda", "import", "from", "struct", "layout", "extern", "needs", "mut",
};

bool isKeyword(const std::string& s) { return kKeywords.count(s) != 0; }

const char* tokName(Tok t) {
    switch (t) {
    case Tok::Name: return "NAME";
    case Tok::Int: return "INT";
    case Tok::Float: return "FLOAT";
    case Tok::Str: return "STR";
    case Tok::FStr: return "FSTR";
    case Tok::Bytes: return "BYTES";
    case Tok::Op: return "OP";
    case Tok::Newline: return "NEWLINE";
    case Tok::Indent: return "INDENT";
    case Tok::Dedent: return "DEDENT";
    case Tok::End: return "END";
    }
    return "?";
}

Lexer::Lexer(std::string src, std::string file, int lineOffset)
    : src_(std::move(src)), file_(std::move(file)), line_(1 + lineOffset) {}

void Lexer::error(const std::string& msg) const { throw SyntaxError(file_, line_, msg); }

/// @brief Value of a hex digit, or -1 if @p c isn't one.
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Handles '...', "...", and triple-quoted '''...''' / """...""" (which may
// span lines; line_ and lineStart_ are kept current so later tokens still get
// the right line and column). Escapes: \n \t \r \0 \\ \' \" \xHH, and a
// backslash-newline inside a string is a line continuation that produces nothing.
std::string Lexer::readString(size_t& i, bool raw) {
    char q = src_[i];
    bool triple = i + 2 < src_.size() && src_[i + 1] == q && src_[i + 2] == q;
    i += triple ? 3 : 1;
    std::string out;
    for (;;) {
        if (i >= src_.size()) error("unterminated string literal");
        char c = src_[i];
        if (triple) {
            if (c == q && i + 2 < src_.size() && src_[i + 1] == q && src_[i + 2] == q) {
                i += 3;
                return out;
            }
        } else if (c == q) {
            ++i;
            return out;
        }
        if (c == '\n') {
            if (!triple) error("newline in string literal");
            ++line_;
            lineStart_ = i + 1;
        }
        if (c == '\\' && !raw) {
            if (i + 1 >= src_.size()) error("unterminated string literal");
            char e = src_[i + 1];
            i += 2;
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case '\\': out += '\\'; break;
            case '\'': out += '\''; break;
            case '"': out += '"'; break;
            case '\n': ++line_; lineStart_ = i; break;   // line continuation inside string
            case 'x': {
                if (i + 1 >= src_.size()) error("bad \\x escape");
                int h = hexval(src_[i]), l = hexval(src_[i + 1]);
                if (h < 0 || l < 0) error("bad \\x escape");
                out += static_cast<char>(h * 16 + l);
                i += 2;
                break;
            }
            default: error(std::string("unknown escape \\") + e);
            }
            continue;
        }
        out += c;
        ++i;
    }
}

// Main loop. One pass over the buffer, one token (or one layout decision) per
// iteration. Two pieces of state drive layout:
//
//   indents      stack of indentation widths currently open. Starts at {0}.
//   depth        bracket nesting. While > 0, newlines and indentation are
//                ignored, so `f(a,\n  b)` is one logical line.
//
// At the start of each physical line (atLineStart && depth == 0) the leading
// spaces are measured and compared with the top of the stack: deeper pushes
// and emits Indent; shallower pops and emits one Dedent per level closed. A
// width that doesn't match any open level is an error, as in Python.
std::vector<Token> Lexer::run() {
    std::vector<Token> out;
    std::vector<int> indents{0};
    size_t i = 0;
    int depth = 0;            // bracket nesting: no NEWLINE/INDENT inside brackets
    bool atLineStart = true;

    auto col = [&](size_t pos) { return static_cast<int>(pos - lineStart_) + 1; };
    auto push = [&](Tok k, std::string text, size_t pos) {
        out.push_back(Token{k, std::move(text), line_, col(pos)});
    };

    while (i < src_.size()) {
        if (atLineStart && depth == 0) {
            int width = 0;
            size_t j = i;
            while (j < src_.size() && (src_[j] == ' ' || src_[j] == '\t' || src_[j] == '\r')) {
                if (src_[j] == '\t') error("tabs are not allowed in indentation");
                if (src_[j] == ' ') ++width;
                ++j;
            }
            if (j >= src_.size()) { i = j; break; }
            if (src_[j] == '\n') { i = j + 1; ++line_; lineStart_ = i; continue; }   // blank line
            if (src_[j] == '#') {                                                    // comment line
                while (j < src_.size() && src_[j] != '\n') ++j;
                i = j;
                continue;
            }
            if (width > indents.back()) {
                indents.push_back(width);
                push(Tok::Indent, "", j);
            } else {
                while (width < indents.back()) {
                    indents.pop_back();
                    push(Tok::Dedent, "", j);
                }
                if (width != indents.back()) error("unindent does not match any outer level");
            }
            i = j;
            atLineStart = false;
            continue;
        }

        char c = src_[i];
        if (c == '\n') {
            if (depth == 0) {
                push(Tok::Newline, "", i);
                atLineStart = true;
            }
            ++i;
            ++line_;
            lineStart_ = i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (c == '#') {
            while (i < src_.size() && src_[i] != '\n') ++i;
            continue;
        }
        if (c == '\\' && i + 1 < src_.size() && src_[i + 1] == '\n') {   // explicit line join
            i += 2;
            ++line_;
            lineStart_ = i;
            continue;
        }

        size_t start = i;

        // String prefixes. Checked before identifiers, because `b`, `f` and `r`
        // are also valid identifier starts: `b"x"` is bytes, `b` alone is a name.: b"", f"", r"", rb"", br""
        if (std::isalpha(static_cast<unsigned char>(c))) {
            size_t j = i;
            std::string prefix;
            while (j < src_.size() && j - i < 2 && std::strchr("bBfFrR", src_[j]) && src_[j] != '\0') {
                prefix += static_cast<char>(std::tolower(static_cast<unsigned char>(src_[j])));
                ++j;
            }
            if (!prefix.empty() && j < src_.size() && (src_[j] == '"' || src_[j] == '\'')) {
                bool raw = prefix.find('r') != std::string::npos;
                bool isBytes = prefix.find('b') != std::string::npos;
                bool isF = prefix.find('f') != std::string::npos;
                if (isBytes && isF) error("bytes literals cannot be f-strings");
                int startLine = line_;
                i = j;
                std::string s = readString(i, raw);
                Tok k = isBytes ? Tok::Bytes : (isF ? Tok::FStr : Tok::Str);
                if (isBytes)
                    for (unsigned char ch : s)
                        if (ch >= 0x80) error("bytes literal may only contain ASCII; use \\x escapes");
                out.push_back(Token{k, std::move(s), startLine, col(start)});
                continue;
            }
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            while (i < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[i])) || src_[i] == '_')) ++i;
            push(Tok::Name, src_.substr(start, i - start), start);
            continue;
        }

        // Numbers: 0x / 0b / 0o prefixed ints, decimal ints, and floats (with a
        // '.', an exponent, or both). Underscores are digit separators and are
        // dropped. Range checking happens in the parser, which knows the target type.
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[i + 1])))) {
            std::string digits;
            bool isFloat = false;
            if (c == '0' && i + 1 < src_.size() && std::strchr("xXbBoO", src_[i + 1]) && src_[i + 1] != '\0') {
                char base = static_cast<char>(std::tolower(static_cast<unsigned char>(src_[i + 1])));
                i += 2;
                while (i < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[i])) || src_[i] == '_')) {
                    if (src_[i] != '_') digits += src_[i];
                    ++i;
                }
                if (digits.empty()) error("malformed number literal");
                push(Tok::Int, std::string("0") + base + digits, start);
                continue;
            }
            while (i < src_.size()) {
                char d = src_[i];
                if (std::isdigit(static_cast<unsigned char>(d)) || d == '_') {
                    if (d != '_') digits += d;
                    ++i;
                } else if (d == '.' && !isFloat) {
                    isFloat = true;
                    digits += d;
                    ++i;
                } else if (d == 'e' || d == 'E') {
                    isFloat = true;
                    digits += d;
                    ++i;
                    if (i < src_.size() && (src_[i] == '+' || src_[i] == '-')) digits += src_[i++];
                } else {
                    break;
                }
            }
            push(isFloat ? Tok::Float : Tok::Int, digits, start);
            continue;
        }

        if (c == '"' || c == '\'') {
            int startLine = line_;
            std::string s = readString(i, false);
            out.push_back(Token{Tok::Str, std::move(s), startLine, col(start)});
            continue;
        }

        // Operators by maximal munch: try 3-character, then 2, then 1, so that
        // `**=` is never read as `**` followed by `=`.
        static const char* three[] = {"**=", "//=", ">>=", "<<=", nullptr};
        static const char* two[] = {"**", "//", "==", "!=", "<=", ">=", "<<", ">>", "->", "+=", "-=",
                                    "*=", "/=", "%=", "&=", "|=", "^=", nullptr};
        std::string op;
        for (const char** p = three; *p && op.empty(); ++p)
            if (src_.compare(i, 3, *p) == 0) op = *p;
        for (const char** p = two; *p && op.empty(); ++p)
            if (src_.compare(i, 2, *p) == 0) op = *p;
        if (op.empty()) {
            if (!std::strchr("()[]{}:,.;+-*/%<>=|&^~@", c) || c == '\0')
                error(std::string("unexpected character '") + c + "'");
            op = std::string(1, c);
            if (c == '(' || c == '[' || c == '{') ++depth;
            if (c == ')' || c == ']' || c == '}') {
                if (depth == 0) error(std::string("unmatched '") + c + "'");
                --depth;
            }
        }
        i += op.size();
        push(Tok::Op, op, start);
    }

    // End of input: close the last logical line, then every open block, so
    // the parser always sees balanced Indent/Dedent and a final End.
    if (depth != 0) error("unclosed bracket at end of file");
    if (!out.empty() && out.back().kind != Tok::Newline && out.back().kind != Tok::Dedent)
        push(Tok::Newline, "", i);
    while (indents.size() > 1) {
        indents.pop_back();
        push(Tok::Dedent, "", i);
    }
    push(Tok::End, "", i);
    return out;
}

} // namespace dolphin
