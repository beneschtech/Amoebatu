/**
 * @file ast.cpp
 * @brief Needs formatting, source rendering, and the clang-style tree dump
 *        behind `dolphin --ast`.
 *
 * The dumper works in two steps: build a small display tree (DNode: a label
 * and children) from the AST, then render it with clang's connectors. Keeping
 * those apart means the AST walk only decides *what* each node says, and the
 * renderer alone deals with `|-`, `` `- `` and `|` prefixes.
 */
#include "ast.hpp"

#include <stdexcept>
#include <unordered_map>

#include "value.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dolphin {

// ------------------------------------------------------------ operator codes

Op binaryOp(const std::string& s) {
    static const std::unordered_map<std::string, Op> k = {
    {"+", Op::Add},
    {"-", Op::Sub},
    {"*", Op::Mul},
    {"/", Op::Div},
    {"//", Op::FloorDiv},
    {"%", Op::Mod},
    {"**", Op::Pow},
    {"&", Op::BitAnd},
    {"|", Op::BitOr},
    {"^", Op::BitXor},
    {"<<", Op::Shl},
    {">>", Op::Shr},
    {"==", Op::Eq},
    {"!=", Op::Ne},
    {"<", Op::Lt},
    {"<=", Op::Le},
    {">", Op::Gt},
    {">=", Op::Ge},
    {"in", Op::In},
    {"not in", Op::NotIn},
    {"and", Op::And},
    {"or", Op::Or}
    };
    auto it = k.find(s);
    if (it == k.end()) throw std::logic_error("unknown operator " + s);   // parser bug
    return it->second;
}

Op unaryOp(const std::string& s) {
    if (s == "not") return Op::Not;
    if (s == "-") return Op::Neg;
    if (s == "+") return Op::Pos;
    if (s == "~") return Op::Invert;
    throw std::logic_error("unknown unary operator " + s);   // parser bug
}

const char* opSpelling(Op op) {
    static const char* const k[] = {"+", "-", "*", "/", "//", "%", "**", "&", "|", "^", "<<", ">>", "==", "!=", "<", "<=", ">", ">=", "in", "not in", "not", "-", "+", "~", "and", "or"};
    return k[static_cast<size_t>(op)];
}

// -------------------------------------------------------------- method codes

static const char* const kMethodNames[] = {"", "upper", "lower", "strip", "lstrip", "rstrip", "split", "join", "startswith", "endswith", "find", "replace", "count", "isdigit", "isalpha", "isspace", "encode", "decode", "hex", "append", "pop", "extend", "index", "get", "keys", "values",
    "open", "read", "readline", "write", "seek", "tell", "close", "set_timeout",
    "value", "toString", "prettyPrint",
    "bind", "threads", "per_connection", "limit", "drain", "start", "stop", "wait"};

Method methodByName(const std::string& name) {
    for (size_t i = 1; i < sizeof kMethodNames / sizeof *kMethodNames; ++i)
        if (name == kMethodNames[i]) return static_cast<Method>(i);
    return Method::None;
}

const char* methodName(Method m) { return kMethodNames[static_cast<size_t>(m)]; }

// ------------------------------------------------- nodes needing runtime types

StrLit::StrLit(int l, std::string s) : Expr(K::Str, l), v(std::move(s)), str(std::make_shared<const Str>(v)) {}

Index::Index(int l, ExprP o, ExprP i) : Expr(K::Index, l), obj(std::move(o)), index(std::move(i)) {
    Value key;
    if (index->k == K::Str) key = Value::str(static_cast<const StrLit&>(*index).v);
    else if (index->k == K::Int) key = Value(static_cast<const IntLit&>(*index).v);
    else return;
    hasConstKey = dictKey(key, constKey);
    if (hasConstKey) constHash = Dict::hashKey(constKey);
}

std::string needsToString(unsigned n) {
    if (n == NeedNone) return "pure";
    std::string s;
    auto add = [&](unsigned bit, const char* name) {
        if (n & bit) {
            if (!s.empty()) s += ",";
            s += name;
        }
    };
    add(NeedIO, "IO");
    add(NeedDatabase, "Database");
    add(NeedNetwork, "Network");
    add(NeedLocalOnly, "LocalOnly");
    add(NeedGlobals, "Globals");
    return s;
}

/// @brief Quote a string for dump output, escaping control and non-ASCII bytes.
static std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20 || c >= 0x7f) {
                char buf[5];
                std::snprintf(buf, sizeof buf, "\\x%02x", c);
                o += buf;
            } else {
                o += static_cast<char>(c);
            }
        }
    }
    return o + "\"";
}

// Only used for type annotations, which today are names and subscripts like
// `list[bytes]` or `array[f32]`; anything else falls back to a placeholder.
std::string exprToSource(const Expr& e) {
    switch (e.k) {
    case Expr::K::Name: return static_cast<const Name&>(e).id;
    case Expr::K::None: return "None";
    case Expr::K::Index: {
        auto& i = static_cast<const Index&>(e);
        return exprToSource(*i.obj) + "[" + exprToSource(*i.index) + "]";
    }
    case Expr::K::Attr: {
        auto& a = static_cast<const Attr&>(e);
        return exprToSource(*a.obj) + "." + a.name;
    }
    case Expr::K::Binary: {
        auto& b = static_cast<const Binary&>(e);
        return exprToSource(*b.lhs) + " " + b.op + " " + exprToSource(*b.rhs);
    }
    default: return "<expr>";
    }
}

namespace {

/// @brief One line of the dump and the lines nested under it.
struct DNode {
    std::string label;
    std::vector<DNode> kids;
};

std::string at(int line) { return " <line:" + std::to_string(line) + ">"; }

DNode expr(const Expr& e);
DNode block(const std::string& label, const Block& b);

/// What a resolved name refers to, e.g. `local slot=2`, `builtin needs=IO`.
std::string binding(const Name& n) {
    std::string s = bindName(n.bind);
    if (n.bind == Bind::Local) s += " slot=" + std::to_string(n.slot);
    if (n.bind == Bind::Builtin) s += " needs=" + needsToString(n.needs);
    if (n.def) s += " -> def@" + std::to_string(n.def->line) + " needs=" + needsToString(n.def->needs);
    return s;
}

std::string slotSuffix(int slot) { return slot >= 0 ? " local slot=" + std::to_string(slot) : " global"; }

DNode expr(const Expr& e) {
    switch (e.k) {
    case Expr::K::Int: return {"IntLit " + std::to_string(static_cast<const IntLit&>(e).v), {}};
    case Expr::K::Float: {   // shortest text that round-trips, always with a '.' or exponent
        double v = static_cast<const FloatLit&>(e).v;
        char buf[40];
        for (int p = 1; p <= 17; ++p) {
            std::snprintf(buf, sizeof buf, "%.*g", p, v);
            if (std::strtod(buf, nullptr) == v) break;
        }
        std::string s = buf;
        if (s.find_first_of(".en") == std::string::npos) s += ".0";
        return {"FloatLit " + s, {}};
    }
    case Expr::K::Str: return {"StrLit " + quote(static_cast<const StrLit&>(e).v), {}};
    case Expr::K::Bytes: return {"BytesLit b" + quote(static_cast<const BytesLit&>(e).v), {}};
    case Expr::K::Bool: return {std::string("BoolLit ") + (static_cast<const BoolLit&>(e).v ? "True" : "False"), {}};
    case Expr::K::None: return {"NoneLit", {}};
    case Expr::K::Name: {
        auto& n = static_cast<const Name&>(e);
        return {"Name `" + n.id + "` " + binding(n), {}};
    }
    case Expr::K::FStr: {
        DNode d{"FString" + at(e.line), {}};
        for (auto& p : static_cast<const FStr&>(e).parts) {
            if (!p.expr) d.kids.push_back({"Text " + quote(p.text), {}});
            else d.kids.push_back({"Field" + (p.spec.empty() ? std::string() : " spec=" + quote(p.spec)), {expr(*p.expr)}});
        }
        return d;
    }
    case Expr::K::Unary: {
        auto& u = static_cast<const Unary&>(e);
        return {"Unary '" + u.op + "'", {expr(*u.operand)}};
    }
    case Expr::K::Binary: {
        auto& b = static_cast<const Binary&>(e);
        return {"Binary '" + b.op + "'", {expr(*b.lhs), expr(*b.rhs)}};
    }
    case Expr::K::BoolOp: {
        auto& b = static_cast<const BoolOp&>(e);
        return {"BoolOp '" + b.op + "'", {expr(*b.lhs), expr(*b.rhs)}};
    }
    case Expr::K::Compare: {
        auto& c = static_cast<const Compare&>(e);
        DNode d{"Compare", {expr(*c.first)}};
        for (auto& [op, r] : c.rest) {
            d.label += " '" + op + "'";
            d.kids.push_back(expr(*r));
        }
        return d;
    }
    case Expr::K::Call: {
        auto& c = static_cast<const Call&>(e);
        DNode d{"Call" + at(e.line), {expr(*c.fn)}};
        for (auto& a : c.args) d.kids.push_back(expr(*a));
        for (auto& [k, v] : c.kwargs) d.kids.push_back({"Keyword `" + k + "`", {expr(*v)}});
        return d;
    }
    case Expr::K::Slice: {
        auto& s = static_cast<const Slice&>(e);
        return {"Slice", {s.lo ? DNode{"Lo", {expr(*s.lo)}} : DNode{"Lo <omitted>", {}},
                          s.hi ? DNode{"Hi", {expr(*s.hi)}} : DNode{"Hi <omitted>", {}}}};
    }
    case Expr::K::Index: {
        auto& i = static_cast<const Index&>(e);
        return {"Subscript", {expr(*i.obj), expr(*i.index)}};
    }
    case Expr::K::Attr: {
        auto& a = static_cast<const Attr&>(e);
        return {"Attribute `." + a.name + "`", {expr(*a.obj)}};
    }
    case Expr::K::List: {
        auto& l = static_cast<const ListLit&>(e);
        DNode d{"ListLit size=" + std::to_string(l.elems.size()), {}};
        for (auto& x : l.elems) d.kids.push_back(expr(*x));
        return d;
    }
    case Expr::K::Dict: {
        auto& l = static_cast<const DictLit&>(e);
        DNode d{"DictLit size=" + std::to_string(l.items.size()), {}};
        for (auto& [k, v] : l.items) d.kids.push_back({"Entry", {expr(*k), expr(*v)}});
        return d;
    }
    case Expr::K::ListComp: {
        auto& c = static_cast<const ListComp&>(e);
        DNode d{"ListComp `" + c.var + "`" + slotSuffix(c.slot) + at(e.line),
                {{"Iter", {expr(*c.iter)}}, {"Element", {expr(*c.elt)}}}};
        if (c.cond) d.kids.push_back({"If", {expr(*c.cond)}});
        return d;
    }
    }
    return {"?", {}};
}

DNode stmt(const Stmt& s) {
    switch (s.k) {
    case Stmt::K::Expr: return {"ExprStmt" + at(s.line), {expr(*static_cast<const ExprStmt&>(s).e)}};
    case Stmt::K::Assign: {
        auto& a = static_cast<const Assign&>(s);
        std::string label = "Assign" + at(s.line);
        if (a.annot) label += " : " + exprToSource(*a.annot);
        return {label, {expr(*a.target), expr(*a.value)}};
    }
    case Stmt::K::AugAssign: {
        auto& a = static_cast<const AugAssign&>(s);
        return {"AugAssign '" + a.op + "='" + at(s.line), {expr(*a.target), expr(*a.value)}};
    }
    case Stmt::K::If: {
        auto& i = static_cast<const If&>(s);
        DNode d{"If" + at(s.line), {}};
        bool first = true;
        for (auto& [c, b] : i.branches) {
            d.kids.push_back({first ? "Cond" : "ElifCond", {expr(*c)}});
            d.kids.push_back(block(first ? "Then" : "ElifThen", b));
            first = false;
        }
        if (!i.orelse.empty()) d.kids.push_back(block("Else", i.orelse));
        return d;
    }
    case Stmt::K::While: {
        auto& w = static_cast<const While&>(s);
        return {"While" + at(s.line), {{"Cond", {expr(*w.cond)}}, block("Body", w.body)}};
    }
    case Stmt::K::For: {
        auto& f = static_cast<const For&>(s);
        return {"For `" + f.var + "`" + slotSuffix(f.slot) + at(s.line), {{"Iter", {expr(*f.iter)}}, block("Body", f.body)}};
    }
    case Stmt::K::Def: {
        auto& d = static_cast<const Def&>(s);
        DNode n{"FunctionDef `" + d.name + "`" + at(s.line) + " needs=" + needsToString(d.needs) +
                    " locals=" + std::to_string(d.numLocals) + (d.mutatesArgs ? " mutates-args" : "") + (d.slot >= 0 ? " (nested, slot=" + std::to_string(d.slot) + ")" : ""),
                {}};
        for (auto& p : d.params) {
            std::string label = "Param `" + p.name + "` slot=" + std::to_string(p.slot);
            if (p.annot) label += " : " + exprToSource(*p.annot);
            DNode pn{label, {}};
            if (p.dflt) pn.kids.push_back({"Default", {expr(*p.dflt)}});
            n.kids.push_back(std::move(pn));
        }
        if (d.ret) n.kids.push_back({"Returns : " + exprToSource(*d.ret), {}});
        n.kids.push_back(block("Body", d.body));
        return n;
    }
    case Stmt::K::Return: {
        auto& r = static_cast<const Return&>(s);
        DNode d{"Return" + at(s.line), {}};
        if (r.value) d.kids.push_back(expr(*r.value));
        return d;
    }
    case Stmt::K::Break: return {"Break" + at(s.line), {}};
    case Stmt::K::Continue: return {"Continue" + at(s.line), {}};
    case Stmt::K::Pass: return {"Pass" + at(s.line), {}};
    case Stmt::K::Try: {
        auto& t = static_cast<const Try&>(s);
        DNode d{"Try" + at(s.line), {block("Body", t.body)}};
        for (auto& h : t.handlers) {
            std::string label = "Except";
            label += h.cls.empty() ? " <any>" : " `" + h.cls + "`";
            if (!h.as.empty()) label += " as `" + h.as + "`" + slotSuffix(h.asSlot);
            label += at(h.line);
            DNode hn = block(label, h.body);
            d.kids.push_back(std::move(hn));
        }
        return d;
    }
    case Stmt::K::Raise: {
        auto& r = static_cast<const Raise&>(s);
        DNode d{std::string("Raise") + (r.value ? "" : " <re-raise>") + at(s.line), {}};
        if (r.value) d.kids.push_back(expr(*r.value));
        return d;
    }
    }
    return {"?", {}};
}

DNode block(const std::string& label, const Block& b) {
    DNode d{label, {}};
    for (auto& s : b) d.kids.push_back(stmt(*s));
    return d;
}

/// Render like clang: children get `|-` (more siblings follow) or `` `- `` (last),
/// and their descendants inherit `| ` or two spaces to continue the vertical rule.
void render(const DNode& n, const std::string& prefix, bool isRoot, bool isLast, std::ostream& os) {
    os << prefix;
    if (!isRoot) os << (isLast ? "`-" : "|-");
    os << n.label << "\n";
    std::string childPrefix = isRoot ? prefix : prefix + (isLast ? "  " : "| ");
    for (size_t i = 0; i < n.kids.size(); ++i) render(n.kids[i], childPrefix, false, i + 1 == n.kids.size(), os);
}

} // namespace

void dump(const Module& m, std::ostream& os) {
    DNode root = block("Module `" + m.file + "`", m.body);
    render(root, "", true, true, os);
}

} // namespace dolphin
