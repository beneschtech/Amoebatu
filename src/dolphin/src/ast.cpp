/**
 * @file ast.cpp
 * @brief Needs formatting and the S-expression dumper behind `dolphin --ast`.
 */
#include "ast.hpp"

#include <sstream>

namespace dolphin {

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

std::string dump(const Expr& e) {
    std::ostringstream os;
    switch (e.k) {
    case Expr::K::Int: os << static_cast<const IntLit&>(e).v; break;
    case Expr::K::Float: os << static_cast<const FloatLit&>(e).v; break;
    case Expr::K::Str: os << quote(static_cast<const StrLit&>(e).v); break;
    case Expr::K::Bytes: os << "b" << quote(static_cast<const BytesLit&>(e).v); break;
    case Expr::K::Bool: os << (static_cast<const BoolLit&>(e).v ? "True" : "False"); break;
    case Expr::K::None: os << "None"; break;
    case Expr::K::Name: os << static_cast<const Name&>(e).id; break;
    case Expr::K::FStr: {
        os << "(fstr";
        for (auto& p : static_cast<const FStr&>(e).parts) {
            if (p.expr) {
                os << " {" << dump(*p.expr);
                if (!p.spec.empty()) os << ":" << p.spec;
                os << "}";
            } else {
                os << " " << quote(p.text);
            }
        }
        os << ")";
        break;
    }
    case Expr::K::Unary: {
        auto& u = static_cast<const Unary&>(e);
        os << "(" << u.op << " " << dump(*u.operand) << ")";
        break;
    }
    case Expr::K::Binary: {
        auto& b = static_cast<const Binary&>(e);
        os << "(" << b.op << " " << dump(*b.lhs) << " " << dump(*b.rhs) << ")";
        break;
    }
    case Expr::K::BoolOp: {
        auto& b = static_cast<const BoolOp&>(e);
        os << "(" << b.op << " " << dump(*b.lhs) << " " << dump(*b.rhs) << ")";
        break;
    }
    case Expr::K::Compare: {
        auto& c = static_cast<const Compare&>(e);
        os << "(cmp " << dump(*c.first);
        for (auto& [op, r] : c.rest) os << " " << op << " " << dump(*r);
        os << ")";
        break;
    }
    case Expr::K::Call: {
        auto& c = static_cast<const Call&>(e);
        os << "(call " << dump(*c.fn);
        for (auto& a : c.args) os << " " << dump(*a);
        for (auto& [k, v] : c.kwargs) os << " " << k << "=" << dump(*v);
        os << ")";
        break;
    }
    case Expr::K::Slice: {
        auto& s = static_cast<const Slice&>(e);
        os << "(slice " << (s.lo ? dump(*s.lo) : "_") << " " << (s.hi ? dump(*s.hi) : "_") << ")";
        break;
    }
    case Expr::K::Index: {
        auto& i = static_cast<const Index&>(e);
        os << "(index " << dump(*i.obj) << " " << dump(*i.index) << ")";
        break;
    }
    case Expr::K::Attr: {
        auto& a = static_cast<const Attr&>(e);
        os << "(. " << dump(*a.obj) << " " << a.name << ")";
        break;
    }
    case Expr::K::List: {
        os << "(list";
        for (auto& x : static_cast<const ListLit&>(e).elems) os << " " << dump(*x);
        os << ")";
        break;
    }
    case Expr::K::ListComp: {
        auto& c = static_cast<const ListComp&>(e);
        os << "(listcomp " << dump(*c.elt) << " for " << c.var << " in " << dump(*c.iter);
        if (c.cond) os << " if " << dump(*c.cond);
        os << ")";
        break;
    }
    }
    return os.str();
}

static void dumpBlock(const Block& b, std::ostream& os, int ind);

static void dumpStmt(const Stmt& s, std::ostream& os, int ind) {
    std::string pad(ind * 2, ' ');
    switch (s.k) {
    case Stmt::K::Expr: os << pad << dump(*static_cast<const ExprStmt&>(s).e) << "\n"; break;
    case Stmt::K::Assign: {
        auto& a = static_cast<const Assign&>(s);
        os << pad << "(= " << dump(*a.target);
        if (a.annot) os << " : " << dump(*a.annot);
        os << " " << dump(*a.value) << ")\n";
        break;
    }
    case Stmt::K::AugAssign: {
        auto& a = static_cast<const AugAssign&>(s);
        os << pad << "(" << a.op << "= " << dump(*a.target) << " " << dump(*a.value) << ")\n";
        break;
    }
    case Stmt::K::If: {
        auto& f = static_cast<const If&>(s);
        bool first = true;
        for (auto& [c, b] : f.branches) {
            os << pad << (first ? "if " : "elif ") << dump(*c) << "\n";
            dumpBlock(b, os, ind + 1);
            first = false;
        }
        if (!f.orelse.empty()) {
            os << pad << "else\n";
            dumpBlock(f.orelse, os, ind + 1);
        }
        break;
    }
    case Stmt::K::While: {
        auto& w = static_cast<const While&>(s);
        os << pad << "while " << dump(*w.cond) << "\n";
        dumpBlock(w.body, os, ind + 1);
        break;
    }
    case Stmt::K::For: {
        auto& f = static_cast<const For&>(s);
        os << pad << "for " << f.var << " in " << dump(*f.iter) << "\n";
        dumpBlock(f.body, os, ind + 1);
        break;
    }
    case Stmt::K::Def: {
        auto& d = static_cast<const Def&>(s);
        os << pad << "def " << d.name << "(";
        for (size_t i = 0; i < d.params.size(); ++i) {
            if (i) os << ", ";
            os << d.params[i].name;
            if (d.params[i].annot) os << ": " << dump(*d.params[i].annot);
            if (d.params[i].dflt) os << " = " << dump(*d.params[i].dflt);
        }
        os << ")";
        if (d.ret) os << " -> " << dump(*d.ret);
        os << "  [needs: " << needsToString(d.needs) << "]\n";
        dumpBlock(d.body, os, ind + 1);
        break;
    }
    case Stmt::K::Return: {
        auto& r = static_cast<const Return&>(s);
        os << pad << "return" << (r.value ? " " + dump(*r.value) : "") << "\n";
        break;
    }
    case Stmt::K::Break: os << pad << "break\n"; break;
    case Stmt::K::Continue: os << pad << "continue\n"; break;
    case Stmt::K::Pass: os << pad << "pass\n"; break;
    case Stmt::K::Try: {
        auto& t = static_cast<const Try&>(s);
        os << pad << "try\n";
        dumpBlock(t.body, os, ind + 1);
        for (auto& h : t.handlers) {
            os << pad << "except" << (h.cls.empty() ? "" : " " + h.cls) << (h.as.empty() ? "" : " as " + h.as)
               << "\n";
            dumpBlock(h.body, os, ind + 1);
        }
        break;
    }
    case Stmt::K::Raise: {
        auto& r = static_cast<const Raise&>(s);
        os << pad << "raise" << (r.value ? " " + dump(*r.value) : "") << "\n";
        break;
    }
    }
}

static void dumpBlock(const Block& b, std::ostream& os, int ind) {
    for (auto& s : b) dumpStmt(*s, os, ind);
}

void dump(const Module& m, std::ostream& os) { dumpBlock(m.body, os, 0); }

} // namespace dolphin
