/**
 * @file parser.cpp
 * @brief Parser implementation. See parser.hpp for the overall approach and
 *        the precedence table.
 */
#include "parser.hpp"

#include <cerrno>
#include <cstdlib>

namespace dolphin {

Parser::Parser(std::vector<Token> toks, std::string file) : t_(std::move(toks)), file_(std::move(file)) {}

Module parseSource(const std::string& src, const std::string& file) {
    Lexer lx(src, file);
    Parser p(lx.run(), file);
    return p.parseModule();
}

// ------------------------------------------------------------------ helpers

const Token& Parser::peek(size_t k) const {
    size_t i = p_ + k;
    return i < t_.size() ? t_[i] : t_.back();
}
Token Parser::next() {
    Token t = peek();
    if (p_ < t_.size() - 1) ++p_;
    return t;
}
bool Parser::isOp(const char* op, size_t k) const { return peek(k).kind == Tok::Op && peek(k).text == op; }
bool Parser::isKw(const char* kw, size_t k) const { return peek(k).kind == Tok::Name && peek(k).text == kw; }
bool Parser::acceptOp(const char* op) {
    if (!isOp(op)) return false;
    next();
    return true;
}
bool Parser::acceptKw(const char* kw) {
    if (!isKw(kw)) return false;
    next();
    return true;
}
void Parser::expectOp(const char* op) {
    if (!acceptOp(op)) error(std::string("expected '") + op + "'");
}
void Parser::expectKw(const char* kw) {
    if (!acceptKw(kw)) error(std::string("expected '") + kw + "'");
}
std::string Parser::expectName() {
    const Token& t = peek();
    if (t.kind != Tok::Name || isKeyword(t.text)) error("expected identifier");
    return next().text;
}
void Parser::expectNewline() {
    if (peek().kind == Tok::End) return;
    if (peek().kind != Tok::Newline) error("expected end of line");
    next();
}
void Parser::error(const std::string& msg) const {
    const Token& t = peek();
    std::string near;
    switch (t.kind) {
    case Tok::Newline: near = "end of line"; break;
    case Tok::Indent: near = "indent"; break;
    case Tok::Dedent: near = "dedent"; break;
    case Tok::End: near = "end of file"; break;
    default: near = "'" + t.text + "'";
    }
    throw SyntaxError(file_, t.line, msg + " near " + near);
}

// --------------------------------------------------------------- statements

Module Parser::parseModule() {
    Module m;
    m.file = file_;
    while (peek().kind != Tok::End) {
        if (peek().kind == Tok::Newline) { next(); continue; }
        if (peek().kind == Tok::Indent) throw SyntaxError(file_, peek().line, "unexpected indent");
        m.body.push_back(parseStmt());
    }
    return m;
}

// A block is either an indented suite
//     if x:
//         a()
//         b()
// or a single simple statement on the same line:  `if x: return 1`.
Block Parser::parseBlock() {
    expectOp(":");
    Block b;
    if (peek().kind != Tok::Newline) {   // one-line body: `if x: return 1`
        b.push_back(parseSimpleStmt());
        expectNewline();
        return b;
    }
    next();
    if (peek().kind != Tok::Indent) error("expected an indented block");
    next();
    while (peek().kind != Tok::Dedent && peek().kind != Tok::End) {
        if (peek().kind == Tok::Newline) { next(); continue; }
        b.push_back(parseStmt());
    }
    if (peek().kind == Tok::Dedent) next();
    return b;
}

StmtP Parser::parseStmt() {
    if (peek().kind == Tok::Indent) throw SyntaxError(file_, peek().line, "unexpected indent");
    if (isKw("if")) return parseIf();
    if (isKw("while")) return parseWhile();
    if (isKw("for")) return parseFor();
    if (isKw("def")) return parseDef();
    if (isKw("try")) return parseTry();
    if (isKw("struct") || isKw("layout") || isKw("extern") || isKw("import") || isKw("from") ||
        isKw("lambda"))
        error("'" + peek().text + "' is reserved but not implemented yet");
    StmtP s = parseSimpleStmt();
    expectNewline();
    return s;
}

/// @brief Can @p e appear on the left of `=`? (name, subscript, attribute)
static bool isAssignable(const Expr& e) {
    return e.k == Expr::K::Name || e.k == Expr::K::Index || e.k == Expr::K::Attr;
}

// Simple statements start with a keyword (pass, break, continue, return,
// raise) or an expression. An expression may then turn out to be the target
// of `=`, `name: type = ...`, or an augmented assignment, so parse the
// expression first and decide by what follows.
StmtP Parser::parseSimpleStmt() {
    int line = peek().line;
    if (acceptKw("pass")) return std::make_unique<Pass>(line);
    if (acceptKw("break")) return std::make_unique<Break>(line);
    if (acceptKw("continue")) return std::make_unique<Continue>(line);
    if (acceptKw("return")) {
        auto r = std::make_unique<Return>(line);
        if (peek().kind != Tok::Newline && peek().kind != Tok::End) r->value = parseExpr();
        return r;
    }
    if (acceptKw("raise")) {
        auto r = std::make_unique<Raise>(line);
        if (peek().kind != Tok::Newline && peek().kind != Tok::End) r->value = parseExpr();
        return r;
    }

    ExprP lhs = parseExpr();

    // annotated assignment:  name: type = value
    if (isOp(":")) {
        if (lhs->k != Expr::K::Name) error("only a plain name can carry a type annotation");
        next();
        auto a = std::make_unique<Assign>(line);
        a->annot = parseExpr();
        expectOp("=");
        a->target = std::move(lhs);
        a->value = parseExpr();
        return a;
    }
    if (isOp("=")) {
        next();
        if (!isAssignable(*lhs)) error("cannot assign to this expression");
        auto a = std::make_unique<Assign>(line);
        a->target = std::move(lhs);
        a->value = parseExpr();
        if (isOp("=")) error("chained assignment is not supported");
        return a;
    }
    static const char* aug[] = {"+=", "-=", "*=", "/=", "//=", "%=", "**=", "&=", "|=", "^=", "<<=", ">>=", nullptr};
    for (const char** op = aug; *op; ++op) {
        if (isOp(*op)) {
            next();
            if (!isAssignable(*lhs)) error("cannot assign to this expression");
            auto a = std::make_unique<AugAssign>(line);
            a->target = std::move(lhs);
            std::string o = *op;
            a->op = o.substr(0, o.size() - 1);
            a->code = binaryOp(a->op);
            a->value = parseExpr();
            return a;
        }
    }
    return std::make_unique<ExprStmt>(line, std::move(lhs));
}

StmtP Parser::parseIf() {
    auto s = std::make_unique<If>(peek().line);
    expectKw("if");
    ExprP c = parseExpr();
    s->branches.emplace_back(std::move(c), parseBlock());
    while (isKw("elif")) {
        next();
        ExprP ec = parseExpr();
        s->branches.emplace_back(std::move(ec), parseBlock());
    }
    if (acceptKw("else")) s->orelse = parseBlock();
    return s;
}

StmtP Parser::parseWhile() {
    auto s = std::make_unique<While>(peek().line);
    expectKw("while");
    s->cond = parseExpr();
    s->body = parseBlock();
    return s;
}

StmtP Parser::parseFor() {
    auto s = std::make_unique<For>(peek().line);
    expectKw("for");
    s->var = expectName();
    expectKw("in");
    s->iter = parseExpr();
    s->body = parseBlock();
    return s;
}

StmtP Parser::parseDef() {
    auto d = std::make_unique<Def>(peek().line);
    expectKw("def");
    d->name = expectName();
    expectOp("(");
    bool sawDefault = false;
    while (!isOp(")")) {
        Param p;
        p.name = expectName();
        if (acceptOp(":")) p.annot = parseExpr();
        if (acceptOp("=")) {
            p.dflt = parseExpr();
            sawDefault = true;
        } else if (sawDefault) {
            error("parameter without default follows parameter with default");
        }
        for (auto& q : d->params)
            if (q.name == p.name) error("duplicate parameter '" + p.name + "'");
        d->params.push_back(std::move(p));
        if (!acceptOp(",")) break;
    }
    expectOp(")");
    if (acceptOp("->")) d->ret = parseExpr();
    d->body = parseBlock();
    return d;
}

StmtP Parser::parseTry() {
    auto s = std::make_unique<Try>(peek().line);
    expectKw("try");
    s->body = parseBlock();
    while (isKw("except")) {
        Handler h;
        h.line = peek().line;
        next();
        if (!isOp(":")) {
            h.cls = expectName();
            if (acceptKw("as")) h.as = expectName();
        }
        h.body = parseBlock();
        s->handlers.push_back(std::move(h));
    }
    if (s->handlers.empty()) error("'try' needs at least one 'except'");
    return s;
}

// -------------------------------------------------------------- expressions
//
// Precedence (low -> high):
//   1 or   2 and   3 not (prefix)   4 comparisons / in   5 |   6 ^   7 &
//   8 << >>   9 + -   10 * / // %   11 unary - + ~   12 ** (right assoc)
//   13 call / index / attribute

/**
 * @brief Binding power of the binary operator at @p t, or -1 if @p t isn't one.
 * @param t   The current token.
 * @param t2  The following token, needed to recognize the two-word `not in`.
 *
 * This table is the whole operator precedence of the language; see
 * parser.hpp for it laid out as a table.
 */
static int binPrec(const Token& t, const Token& t2) {
    if (t.kind == Tok::Name) {
        if (t.text == "or") return 1;
        if (t.text == "and") return 2;
        if (t.text == "in") return 4;
        if (t.text == "not" && t2.kind == Tok::Name && t2.text == "in") return 4;
        return -1;
    }
    if (t.kind != Tok::Op) return -1;
    const std::string& o = t.text;
    if (o == "==" || o == "!=" || o == "<" || o == "<=" || o == ">" || o == ">=") return 4;
    if (o == "|") return 5;
    if (o == "^") return 6;
    if (o == "&") return 7;
    if (o == "<<" || o == ">>") return 8;
    if (o == "+" || o == "-") return 9;
    if (o == "*" || o == "/" || o == "//" || o == "%") return 10;
    if (o == "**") return 12;
    return -1;
}

// Pratt / precedence-climbing loop. Parse a prefix expression, then keep
// absorbing binary operators that bind at least as tightly as minPrec. The
// right operand is parsed with minPrec = prec + 1 for left-associative
// operators (so `a - b - c` groups as `(a - b) - c`) and with minPrec = prec
// for `**` (so `a ** b ** c` groups as `a ** (b ** c)`).
//
// Comparisons are special: Python chains them (`a < b < c`), so all
// consecutive comparison operators are collected into one Compare node
// instead of nesting.
ExprP Parser::parseExpr(int minPrec) {
    ExprP lhs = parsePrefix();
    for (;;) {
        int prec = binPrec(peek(), peek(1));
        if (prec < 0 || prec < minPrec) break;
        int line = peek().line;

        if (prec == 4) {   // comparison chain
            auto c = std::make_unique<Compare>(line, std::move(lhs));
            while (binPrec(peek(), peek(1)) == 4) {
                std::string op = next().text;
                if (op == "not") {
                    next();   // 'in'
                    op = "not in";
                }
                c->add(op, parseExpr(5));
            }
            lhs = std::move(c);
            continue;
        }

        std::string op = next().text;
        if (op == "or" || op == "and") {
            ExprP rhs = parseExpr(prec + 1);
            lhs = std::make_unique<BoolOp>(line, op, std::move(lhs), std::move(rhs));
        } else {
            ExprP rhs = parseExpr(op == "**" ? prec : prec + 1);   // ** is right-associative
            lhs = std::make_unique<Binary>(line, op, std::move(lhs), std::move(rhs));
        }
    }
    return lhs;
}

ExprP Parser::parsePrefix() {
    int line = peek().line;
    if (acceptKw("not")) return std::make_unique<Unary>(line, "not", parseExpr(3));
    if (isOp("-") || isOp("+") || isOp("~")) {
        std::string op = next().text;
        // unary binds looser than **:  -2 ** 2 == -(2 ** 2)
        return std::make_unique<Unary>(line, op, parseExpr(11));
    }
    return parsePostfix(parseAtom());
}

// Postfix operations bind tightest and apply left to right:
// `f(x)[0].upper()` is ((f(x))[0]).upper then a call.
// A subscript is a Slice node if it contains a top-level ':'.
ExprP Parser::parsePostfix(ExprP e) {
    for (;;) {
        int line = peek().line;
        if (acceptOp("(")) {
            auto c = std::make_unique<Call>(line, std::move(e));
            while (!isOp(")")) {
                if (peek().kind == Tok::Name && !isKeyword(peek().text) && isOp("=", 1)) {
                    std::string k = next().text;
                    next();
                    for (auto& kv : c->kwargs)
                        if (kv.first == k) error("duplicate keyword argument '" + k + "'");
                    c->kwargs.emplace_back(k, parseExpr());
                } else {
                    if (!c->kwargs.empty()) error("positional argument after keyword argument");
                    c->args.push_back(parseExpr());
                }
                if (!acceptOp(",")) break;
            }
            expectOp(")");
            e = std::move(c);
        } else if (acceptOp("[")) {
            ExprP idx;
            if (isOp(":")) {
                auto s = std::make_unique<Slice>(line);
                next();
                if (!isOp("]")) s->hi = parseExpr();
                idx = std::move(s);
            } else {
                ExprP first = parseExpr();
                if (acceptOp(":")) {
                    auto s = std::make_unique<Slice>(line);
                    s->lo = std::move(first);
                    if (!isOp("]")) s->hi = parseExpr();
                    idx = std::move(s);
                } else {
                    idx = std::move(first);
                }
            }
            expectOp("]");
            e = std::make_unique<Index>(line, std::move(e), std::move(idx));
        } else if (acceptOp(".")) {
            const Token& t = peek();
            if (t.kind != Tok::Name) error("expected attribute name");
            e = std::make_unique<Attr>(line, std::move(e), next().text);
        } else {
            return e;
        }
    }
}

/**
 * @brief Convert integer literal text from the lexer to int64.
 * @param s        Literal text, possibly with a `0x`/`0b`/`0o` prefix.
 * @param[out] ok  False if malformed or larger than INT64_MAX. Negative
 *                 literals don't exist; `-5` is unary minus applied to 5.
 */
static int64_t parseIntLiteral(const std::string& s, bool& ok) {
    int base = 10;
    std::string digits = s;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'b' || s[1] == 'o')) {
        base = s[1] == 'x' ? 16 : (s[1] == 'b' ? 2 : 8);
        digits = s.substr(2);
    }
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(digits.c_str(), &end, base);
    ok = errno == 0 && end && *end == '\0' && v <= static_cast<unsigned long long>(INT64_MAX);
    return static_cast<int64_t>(v);
}

ExprP Parser::parseAtom() {
    const Token& t = peek();
    int line = t.line;
    switch (t.kind) {
    case Tok::Int: {
        bool ok;
        int64_t v = parseIntLiteral(t.text, ok);
        if (!ok) error("integer literal out of range or malformed");
        next();
        return std::make_unique<IntLit>(line, v);
    }
    case Tok::Float: {
        char* end = nullptr;
        double v = std::strtod(t.text.c_str(), &end);
        if (!end || *end) error("malformed float literal");
        next();
        return std::make_unique<FloatLit>(line, v);
    }
    case Tok::Str:
    case Tok::FStr: {
        // Adjacent string literals concatenate, as in Python: "a" "b" == "ab".
        // If any piece is an f-string, the result is one f-string whose parts
        // are the plain pieces as text plus each f-string's parts.
        std::vector<Token> pieces;
        bool anyF = false;
        while (peek().kind == Tok::Str || peek().kind == Tok::FStr) {
            anyF = anyF || peek().kind == Tok::FStr;
            pieces.push_back(next());
        }
        if (!anyF) {
            std::string s;
            for (auto& p : pieces) s += p.text;
            return std::make_unique<StrLit>(line, std::move(s));
        }
        auto f = std::make_unique<FStr>(line);
        for (auto& p : pieces) {
            if (p.kind == Tok::Str) {
                FStrPart part;
                part.text = p.text;
                f->parts.push_back(std::move(part));
            } else {
                auto sub = parseFString(p);
                for (auto& part : static_cast<FStr&>(*sub).parts) f->parts.push_back(std::move(part));
            }
        }
        return f;
    }
    case Tok::Bytes: {
        std::string s = next().text;
        while (peek().kind == Tok::Bytes) s += next().text;
        return std::make_unique<BytesLit>(line, std::move(s));
    }
    case Tok::Name: {
        if (t.text == "True") { next(); return std::make_unique<BoolLit>(line, true); }
        if (t.text == "False") { next(); return std::make_unique<BoolLit>(line, false); }
        if (t.text == "None") { next(); return std::make_unique<NoneLit>(line); }
        if (isKeyword(t.text)) error("unexpected keyword");
        return std::make_unique<Name>(line, next().text);
    }
    case Tok::Op: {
        if (t.text == "(") {
            next();
            ExprP e = parseExpr();
            if (isOp(",")) error("tuples are not supported yet");
            expectOp(")");
            return e;
        }
        if (t.text == "{") {   // dict display: {} or {k: v, ...}
            next();
            auto d = std::make_unique<DictLit>(line);
            while (!isOp("}")) {
                ExprP k = parseExpr();
                if (!isOp(":")) error("expected ':' in dict display (sets are not supported)");
                next();
                ExprP v = parseExpr();
                d->items.emplace_back(std::move(k), std::move(v));
                if (!acceptOp(",")) break;
            }
            expectOp("}");
            return d;
        }
        if (t.text == "[") {
            next();
            if (acceptOp("]")) return std::make_unique<ListLit>(line);
            ExprP first = parseExpr();
            if (acceptKw("for")) {
                auto c = std::make_unique<ListComp>(line);
                c->elt = std::move(first);
                c->var = expectName();
                expectKw("in");
                c->iter = parseExpr();   // stops at "if": it is not a binary operator
                if (acceptKw("if")) c->cond = parseExpr();
                expectOp("]");
                return c;
            }
            auto l = std::make_unique<ListLit>(line);
            l->elems.push_back(std::move(first));
            while (acceptOp(",")) {
                if (isOp("]")) break;
                l->elems.push_back(parseExpr());
            }
            expectOp("]");
            return l;
        }
        break;
    }
    default: break;
    }
    error("expected an expression");
}

ExprP Parser::parseStandaloneExpr() {
    ExprP e = parseExpr();
    while (peek().kind == Tok::Newline) next();
    if (peek().kind != Tok::End) error("unexpected text in f-string field");
    return e;
}

// f-string fields are found by scanning for '{' and its matching '}' while
// respecting nested brackets and quotes, so `{d['k']}` and `{f(a, {1})}` work.
// The first ':' at bracket depth 0 starts the format spec. `{{` and `}}` are
// literal braces. Each field's text is then re-lexed and parsed as an
// ordinary expression, with the line offset set so errors point at the right line.
ExprP Parser::parseFString(const Token& t) {
    auto f = std::make_unique<FStr>(t.line);
    const std::string& s = t.text;
    std::string lit;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '{' && i + 1 < s.size() && s[i + 1] == '{') { lit += '{'; i += 2; continue; }
        if (c == '}' && i + 1 < s.size() && s[i + 1] == '}') { lit += '}'; i += 2; continue; }
        if (c == '}') throw SyntaxError(file_, t.line, "single '}' in f-string");
        if (c != '{') { lit += c; ++i; continue; }

        if (!lit.empty()) {
            FStrPart p;
            p.text = std::move(lit);
            lit.clear();
            f->parts.push_back(std::move(p));
        }
        // find matching '}' and top-level ':' (format spec), respecting brackets and quotes
        size_t j = i + 1;
        int depth = 0;
        size_t colon = std::string::npos;
        char quote = 0;
        for (; j < s.size(); ++j) {
            char d = s[j];
            if (quote) { if (d == quote) quote = 0; continue; }
            if (d == '\'' || d == '"') { quote = d; continue; }
            if (d == '(' || d == '[' || d == '{') ++depth;
            else if (d == ')' || d == ']') --depth;
            else if (d == '}') { if (depth == 0) break; --depth; }
            else if (d == ':' && depth == 0 && colon == std::string::npos) colon = j;
        }
        if (j >= s.size()) throw SyntaxError(file_, t.line, "unterminated '{' in f-string");
        size_t exprEnd = colon == std::string::npos ? j : colon;
        std::string exprSrc = s.substr(i + 1, exprEnd - i - 1);
        if (exprSrc.find_first_not_of(" ") == std::string::npos)
            throw SyntaxError(file_, t.line, "empty expression in f-string");
        Lexer lx(exprSrc, file_, t.line - 1);
        Parser sub(lx.run(), file_);
        FStrPart p;
        p.expr = sub.parseStandaloneExpr();
        if (colon != std::string::npos) p.spec = s.substr(colon + 1, j - colon - 1);
        f->parts.push_back(std::move(p));
        i = j + 1;
    }
    if (!lit.empty()) {
        FStrPart p;
        p.text = std::move(lit);
        f->parts.push_back(std::move(p));
    }
    return f;
}

} // namespace dolphin
