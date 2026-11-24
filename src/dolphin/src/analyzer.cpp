/**
 * @file analyzer.cpp
 * @brief Needs inference. See analyzer.hpp for what it computes and why.
 *
 * Two phases:
 *  1. Walk each function body once and record its direct needs (builtins,
 *     unresolved calls) and the set of user functions it calls.
 *  2. Propagate needs along the call graph until nothing changes. Needs only
 *     ever gain bits and there are finitely many, so this terminates, and it
 *     handles recursion without special cases.
 */
#include "analyzer.hpp"

#include <set>

#include "interp.hpp"

namespace dolphin {

namespace {

/// @brief What one walk of a function body found.
struct Facts {
    unsigned direct = NeedNone;            // from builtins and unresolved calls
    std::set<std::string> calls;           // user functions called by name
};

/// @brief Visits one function body (or the module's top level) and fills in Facts.
///
/// Tracks local names, so a call through a parameter or local variable
/// (`f(x)` where f is a parameter) is recognized as unresolvable instead of
/// being mistaken for a call to a global function named `f`.
struct Walker {
    const std::map<std::string, Def*>& defs;
    const std::set<std::string>& excNames;
    NeedsReport& report;
    std::string where;
    std::set<std::string> locals;          // parameters and assigned names in this scope
    Facts facts;

    void expr(const Expr& e) {
        switch (e.k) {
        case Expr::K::Call: {
            auto& c = static_cast<const Call&>(e);
            if (c.fn->k == Expr::K::Name) {
                const std::string& id = static_cast<const Name&>(*c.fn).id;
                if (locals.count(id)) {
                    unresolved(e.line, "call through local variable '" + id + "'");
                } else if (defs.count(id)) {
                    facts.calls.insert(id);
                } else if (auto it = builtinNeeds().find(id); it != builtinNeeds().end()) {
                    facts.direct |= it->second;
                } else if (!excNames.count(id)) {
                    unresolved(e.line, "call to unknown name '" + id + "'");
                }
            } else if (c.fn->k == Expr::K::Attr) {
                // Methods on builtin types (str/bytes/list) are all pure.
                expr(*static_cast<const Attr&>(*c.fn).obj);
            } else {
                expr(*c.fn);
                unresolved(e.line, "call through an expression");
            }
            for (auto& a : c.args) expr(*a);
            for (auto& [k, v] : c.kwargs) expr(*v);
            break;
        }
        case Expr::K::FStr:
            for (auto& p : static_cast<const FStr&>(e).parts)
                if (p.expr) expr(*p.expr);
            break;
        case Expr::K::Unary: expr(*static_cast<const Unary&>(e).operand); break;
        case Expr::K::Binary: {
            auto& b = static_cast<const Binary&>(e);
            expr(*b.lhs);
            expr(*b.rhs);
            break;
        }
        case Expr::K::BoolOp: {
            auto& b = static_cast<const BoolOp&>(e);
            expr(*b.lhs);
            expr(*b.rhs);
            break;
        }
        case Expr::K::Compare: {
            auto& c = static_cast<const Compare&>(e);
            expr(*c.first);
            for (auto& [op, r] : c.rest) expr(*r);
            break;
        }
        case Expr::K::Index: {
            auto& i = static_cast<const Index&>(e);
            expr(*i.obj);
            expr(*i.index);
            break;
        }
        case Expr::K::Slice: {
            auto& s = static_cast<const Slice&>(e);
            if (s.lo) expr(*s.lo);
            if (s.hi) expr(*s.hi);
            break;
        }
        case Expr::K::Attr: expr(*static_cast<const Attr&>(e).obj); break;
        case Expr::K::List:
            for (auto& x : static_cast<const ListLit&>(e).elems) expr(*x);
            break;
        case Expr::K::ListComp: {
            auto& c = static_cast<const ListComp&>(e);
            expr(*c.iter);
            locals.insert(c.var);
            expr(*c.elt);
            if (c.cond) expr(*c.cond);
            break;
        }
        default: break;
        }
    }

    void bindTarget(const Expr& t) {
        if (t.k == Expr::K::Name) locals.insert(static_cast<const Name&>(t).id);
        else expr(t);
    }

    void block(const Block& b) {
        for (auto& s : b) stmt(*s);
    }

    void stmt(const Stmt& s) {
        switch (s.k) {
        case Stmt::K::Expr: expr(*static_cast<const ExprStmt&>(s).e); break;
        case Stmt::K::Assign: {
            auto& a = static_cast<const Assign&>(s);
            expr(*a.value);
            bindTarget(*a.target);
            break;
        }
        case Stmt::K::AugAssign: {
            auto& a = static_cast<const AugAssign&>(s);
            expr(*a.value);
            bindTarget(*a.target);
            break;
        }
        case Stmt::K::If: {
            auto& i = static_cast<const If&>(s);
            for (auto& [c, b] : i.branches) {
                expr(*c);
                block(b);
            }
            block(i.orelse);
            break;
        }
        case Stmt::K::While: {
            auto& w = static_cast<const While&>(s);
            expr(*w.cond);
            block(w.body);
            break;
        }
        case Stmt::K::For: {
            auto& f = static_cast<const For&>(s);
            expr(*f.iter);
            locals.insert(f.var);
            block(f.body);
            break;
        }
        case Stmt::K::Def: break;   // analyzed separately
        case Stmt::K::Return: {
            auto& r = static_cast<const Return&>(s);
            if (r.value) expr(*r.value);
            break;
        }
        case Stmt::K::Try: {
            auto& t = static_cast<const Try&>(s);
            block(t.body);
            for (auto& h : t.handlers) {
                if (!h.as.empty()) locals.insert(h.as);
                block(h.body);
            }
            break;
        }
        case Stmt::K::Raise: {
            auto& r = static_cast<const Raise&>(s);
            if (r.value) expr(*r.value);
            break;
        }
        default: break;
        }
    }

    void unresolved(int line, const std::string& what) {
        facts.direct |= NeedAll;
        report.warnings.push_back(where + " (line " + std::to_string(line) + "): " + what +
                                  " cannot be analyzed; treated as worst case");
    }
};

/// @brief Find every `def` in the program, including nested ones.
void collectDefs(Block& b, std::map<std::string, Def*>& defs, NeedsReport& report) {
    for (auto& s : b) {
        if (s->k == Stmt::K::Def) {
            auto* d = static_cast<Def*>(s.get());
            if (defs.count(d->name))
                report.warnings.push_back("function '" + d->name + "' defined more than once (line " +
                                          std::to_string(d->line) + "); analysis uses the last one");
            defs[d->name] = d;
            collectDefs(d->body, defs, report);
        }
    }
}

} // namespace

NeedsReport analyzeNeeds(Module& m) {
    NeedsReport report;
    std::map<std::string, Def*> defs;
    collectDefs(m.body, defs, report);

    static const std::set<std::string> excNames = {
        "Error", "ValueError", "TypeError", "IndexError", "KeyError", "NameError", "AttributeError",
        "ZeroDivisionError", "OverflowError", "IOError", "DecodeError", "RecursionError", "RuntimeError",
        "SyntaxError"};

    std::map<std::string, Facts> facts;
    for (auto& [name, d] : defs) {
        Walker w{defs, excNames, report, "def " + name, {}, {}};
        for (auto& p : d->params) {
            w.locals.insert(p.name);
            if (p.dflt) w.expr(*p.dflt);
        }
        w.block(d->body);
        facts[name] = std::move(w.facts);
    }
    Walker top{defs, excNames, report, "module", {}, {}};
    top.block(m.body);

    // Fixpoint over the call graph (handles recursion and mutual recursion).
    std::map<std::string, unsigned> needs;
    for (auto& [name, f] : facts) needs[name] = f.direct;
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& [name, f] : facts) {
            unsigned n = needs[name];
            for (auto& callee : f.calls) n |= needs[callee];
            if (n != needs[name]) {
                needs[name] = n;
                changed = true;
            }
        }
    }

    for (auto& [name, d] : defs) d->needs = needs[name];
    report.functions = needs;
    report.topLevel = top.facts.direct;
    for (auto& callee : top.facts.calls) report.topLevel |= needs[callee];
    if (defs.count("main")) report.topLevel |= needs["main"];   // main() is called implicitly
    return report;
}

} // namespace dolphin
