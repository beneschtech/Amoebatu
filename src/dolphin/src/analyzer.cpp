/**
 * @file analyzer.cpp
 * @brief Needs inference. See analyzer.hpp for what it computes and why.
 *
 * Runs after the resolver, so every Name already says what it refers to.
 * Two phases:
 *  1. Walk each function body once and record its direct needs (builtins,
 *     unresolvable calls) and the set of functions it calls.
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
    unsigned direct = NeedNone;     ///< From builtins and unresolvable calls.
    std::set<const Def*> calls;     ///< Functions called with a statically known target.
};

/// @brief Visits one function body (or the module's top level) and fills in Facts.
struct Walker {
    NeedsReport& report;
    std::string where;   ///< "def name" or "module", for warnings.
    Facts facts;

    void call(const Call& c) {
        if (c.fn->k == Expr::K::Name) {
            auto& n = static_cast<const Name&>(*c.fn);
            if (n.def) {                       // a known function (global or nested def)
                facts.calls.insert(n.def);
            } else if (n.bind == Bind::Builtin) {
                facts.direct |= builtinNeeds().at(n.id);
            } else if (n.bind != Bind::Exception) {   // constructing an exception is pure
                unresolved(c.line, std::string("call through ") + bindName(n.bind) + " variable '" + n.id + "'");
            }
        } else if (c.fn->k == Expr::K::Attr) {
            // Methods of builtin types (str/bytes/list/dict) are all pure.
            expr(*static_cast<const Attr&>(*c.fn).obj);
        } else {
            expr(*c.fn);
            unresolved(c.line, "call through an expression");
        }
        for (auto& a : c.args) expr(*a);
        for (auto& [k, v] : c.kwargs) expr(*v);
    }

    void expr(const Expr& e) {
        switch (e.k) {
        case Expr::K::Call: call(static_cast<const Call&>(e)); break;
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
        case Expr::K::Dict:
            for (auto& [k, v] : static_cast<const DictLit&>(e).items) {
                expr(*k);
                expr(*v);
            }
            break;
        case Expr::K::ListComp: {
            auto& c = static_cast<const ListComp&>(e);
            expr(*c.iter);
            expr(*c.elt);
            if (c.cond) expr(*c.cond);
            break;
        }
        default: break;
        }
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
            expr(*a.target);
            break;
        }
        case Stmt::K::AugAssign: {
            auto& a = static_cast<const AugAssign&>(s);
            expr(*a.value);
            expr(*a.target);
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
            block(f.body);
            break;
        }
        case Stmt::K::Def: break;   // analyzed separately
        case Stmt::K::Return:
            if (auto& v = static_cast<const Return&>(s).value) expr(*v);
            break;
        case Stmt::K::Try: {
            auto& t = static_cast<const Try&>(s);
            block(t.body);
            for (auto& h : t.handlers) block(h.body);
            break;
        }
        case Stmt::K::Raise:
            if (auto& v = static_cast<const Raise&>(s).value) expr(*v);
            break;
        default: break;
        }
    }

    void unresolved(int line, const std::string& what) {
        facts.direct |= NeedAll;
        report.warnings.push_back(where + " (line " + std::to_string(line) + "): " + what +
                                  " cannot be analyzed; treated as worst case");
    }
};

// ===================================================== mutation analysis
//
// Decides Def::mutatesArgs. A mutation site is
//   - a call to a mutating method:  x.append(...), x.extend(...), x.pop(...)
//   - an item assignment:           x[i] = v,  x[i] += v
//   - passing x to a function that itself mutates its arguments (or to a
//     call the analyzer can't see through).
// Each site's receiver is traced to its root: `r["rvr"].append(g)` has root
// `r`. The site is harmless if the root is a *fresh* local: one whose every
// assignment creates a new object (see isFresh()). Otherwise the function may
// mutate something its caller can see.

/// Method names that mutate their receiver (list and dict).
bool isMutatingMethod(const std::string& m) { return m == "append" || m == "extend" || m == "pop"; }

/// Does evaluating @p e always produce a new object that nothing else references?
bool isFresh(const Expr& e) {
    switch (e.k) {
    case Expr::K::List:
    case Expr::K::Dict:
    case Expr::K::ListComp:
    case Expr::K::Binary:   // list + list, str * n, arithmetic: always a new value
    case Expr::K::Int: case Expr::K::Float: case Expr::K::Str: case Expr::K::FStr:
    case Expr::K::Bytes: case Expr::K::Bool: case Expr::K::None:
        return true;
    case Expr::K::Index:    // a slice copies; a plain subscript returns an existing element
        return static_cast<const Index&>(e).index->k == Expr::K::Slice;
    case Expr::K::Call: {
        auto& c = static_cast<const Call&>(e);
        if (c.fn->k == Expr::K::Name) return static_cast<const Name&>(*c.fn).bind == Bind::Builtin;
        if (c.fn->k == Expr::K::Attr) {   // methods return new values, except these return existing elements
            const std::string& m = static_cast<const Attr&>(*c.fn).name;
            return m != "pop" && m != "get";
        }
        return false;   // a user function may return one of its arguments
    }
    default:
        return false;   // names (aliases), and/or (returns an operand), etc.
    }
}

/// Walks one function body and decides which non-fresh objects it may mutate.
struct MutationWalker {
    const Def& def;
    std::vector<bool> fresh;       ///< Per local slot: does every assignment create a new object?
    std::vector<bool> paramMut;    ///< Per parameter position: may be mutated.
    bool other = false;            ///< May mutate a global or an untraceable object.

    explicit MutationWalker(const Def& d)
        : def(d), fresh(static_cast<size_t>(d.numLocals), true), paramMut(d.params.size(), false) {
        for (auto& p : d.params) fresh[static_cast<size_t>(p.slot)] = false;   // the caller's objects
    }

    // ---- pass 1: which locals are fresh
    void markNotFresh(int slot) {
        if (slot >= 0) fresh[static_cast<size_t>(slot)] = false;
    }
    void freshnessExpr(const Expr& e) {   // comprehension variables alias list elements
        if (e.k == Expr::K::ListComp) markNotFresh(static_cast<const ListComp&>(e).slot);
        forEachChild(e, [&](const Expr& c) { freshnessExpr(c); });
    }
    void freshnessBlock(const Block& b) {
        for (auto& s : b) {
            switch (s->k) {
            case Stmt::K::Assign: {
                auto& a = static_cast<const Assign&>(*s);
                if (a.target->k == Expr::K::Name && !isFresh(*a.value))
                    markNotFresh(static_cast<const Name&>(*a.target).slot);
                freshnessExpr(*a.value);
                break;
            }
            case Stmt::K::For:
                markNotFresh(static_cast<const For&>(*s).slot);   // aliases an element
                freshnessExpr(*static_cast<const For&>(*s).iter);
                freshnessBlock(static_cast<const For&>(*s).body);
                break;
            default:
                forEachChildBlock(*s, [&](const Block& c) { freshnessBlock(c); },
                                  [&](const Expr& c) { freshnessExpr(c); });
            }
        }
    }

    // ---- pass 2: mutation sites
    /// Record that the object @p e refers to may be mutated: trace it to its
    /// root name and classify it as fresh (harmless), a parameter, or other.
    void touch(const Expr& e) {
        switch (e.k) {
        case Expr::K::Name: {
            auto& n = static_cast<const Name&>(e);
            if (n.bind != Bind::Local) { other = true; return; }   // a global
            if (fresh[static_cast<size_t>(n.slot)]) return;
            for (size_t i = 0; i < def.params.size(); ++i)
                if (def.params[i].slot == n.slot) { paramMut[i] = true; return; }
            other = true;   // a non-fresh local: may alias anything
            return;
        }
        case Expr::K::Index: touch(*static_cast<const Index&>(e).obj); return;
        case Expr::K::Attr: touch(*static_cast<const Attr&>(e).obj); return;
        default:
            if (!isFresh(e)) other = true;   // e.g. the result of a user function call
        }
    }

    void siteExpr(const Expr& e) {
        if (e.k == Expr::K::Call) {
            auto& c = static_cast<const Call&>(e);
            if (c.fn->k == Expr::K::Attr) {
                auto& a = static_cast<const Attr&>(*c.fn);
                if (isMutatingMethod(a.name)) touch(*a.obj);
            } else if (c.fn->k == Expr::K::Name) {
                auto& n = static_cast<const Name&>(*c.fn);
                if (const Def* callee = n.def) {
                    // Only the argument positions the callee mutates matter.
                    for (size_t i = 0; i < c.args.size(); ++i)
                        if (callee->mutatesOther || (i < callee->mutatedParams.size() && callee->mutatedParams[i]))
                            touch(*c.args[i]);
                    for (auto& [k, v] : c.kwargs)
                        for (size_t i = 0; i < callee->params.size(); ++i)
                            if (callee->params[i].name == k &&
                                (callee->mutatesOther || callee->mutatedParams[i]))
                                touch(*v);
                } else if (n.bind != Bind::Builtin && n.bind != Bind::Exception) {
                    for (auto& a : c.args) touch(*a);   // unknown callee: assume the worst
                    for (auto& [k, v] : c.kwargs) touch(*v);
                }
            } else {
                other = true;   // call through an expression: can't tell
            }
        }
        forEachChild(e, [&](const Expr& c) { siteExpr(c); });
    }
    void siteBlock(const Block& b) {
        for (auto& s : b) {
            const Expr* target = nullptr;
            if (s->k == Stmt::K::Assign) target = static_cast<const Assign&>(*s).target.get();
            if (s->k == Stmt::K::AugAssign) target = static_cast<const AugAssign&>(*s).target.get();
            if (target && target->k == Expr::K::Index) touch(*static_cast<const Index&>(*target).obj);
            forEachChildBlock(*s, [&](const Block& c) { siteBlock(c); }, [&](const Expr& c) { siteExpr(c); });
        }
    }

    // ---- generic child iteration (nested defs are separate functions: skipped)
    template <class F> static void forEachChild(const Expr& e, F f) {
        switch (e.k) {
        case Expr::K::FStr:
            for (auto& p : static_cast<const FStr&>(e).parts)
                if (p.expr) f(*p.expr);
            break;
        case Expr::K::Unary: f(*static_cast<const Unary&>(e).operand); break;
        case Expr::K::Binary: f(*static_cast<const Binary&>(e).lhs); f(*static_cast<const Binary&>(e).rhs); break;
        case Expr::K::BoolOp: f(*static_cast<const BoolOp&>(e).lhs); f(*static_cast<const BoolOp&>(e).rhs); break;
        case Expr::K::Compare:
            f(*static_cast<const Compare&>(e).first);
            for (auto& [op, r] : static_cast<const Compare&>(e).rest) f(*r);
            break;
        case Expr::K::Call: {
            auto& c = static_cast<const Call&>(e);
            f(*c.fn);
            for (auto& a : c.args) f(*a);
            for (auto& [k, v] : c.kwargs) f(*v);
            break;
        }
        case Expr::K::Index: f(*static_cast<const Index&>(e).obj); f(*static_cast<const Index&>(e).index); break;
        case Expr::K::Slice: {
            auto& s = static_cast<const Slice&>(e);
            if (s.lo) f(*s.lo);
            if (s.hi) f(*s.hi);
            break;
        }
        case Expr::K::Attr: f(*static_cast<const Attr&>(e).obj); break;
        case Expr::K::List:
            for (auto& x : static_cast<const ListLit&>(e).elems) f(*x);
            break;
        case Expr::K::Dict:
            for (auto& [k, v] : static_cast<const DictLit&>(e).items) { f(*k); f(*v); }
            break;
        case Expr::K::ListComp: {
            auto& c = static_cast<const ListComp&>(e);
            f(*c.iter);
            f(*c.elt);
            if (c.cond) f(*c.cond);
            break;
        }
        default: break;
        }
    }
    template <class FB, class FE> static void forEachChildBlock(const Stmt& s, FB fb, FE fe) {
        switch (s.k) {
        case Stmt::K::Expr: fe(*static_cast<const ExprStmt&>(s).e); break;
        case Stmt::K::Assign: fe(*static_cast<const Assign&>(s).target); fe(*static_cast<const Assign&>(s).value); break;
        case Stmt::K::AugAssign: fe(*static_cast<const AugAssign&>(s).target); fe(*static_cast<const AugAssign&>(s).value); break;
        case Stmt::K::If:
            for (auto& [c, b] : static_cast<const If&>(s).branches) { fe(*c); fb(b); }
            fb(static_cast<const If&>(s).orelse);
            break;
        case Stmt::K::While: fe(*static_cast<const While&>(s).cond); fb(static_cast<const While&>(s).body); break;
        case Stmt::K::For: fe(*static_cast<const For&>(s).iter); fb(static_cast<const For&>(s).body); break;
        case Stmt::K::Return:
            if (auto& v = static_cast<const Return&>(s).value) fe(*v);
            break;
        case Stmt::K::Raise:
            if (auto& v = static_cast<const Raise&>(s).value) fe(*v);
            break;
        case Stmt::K::Try:
            fb(static_cast<const Try&>(s).body);
            for (auto& h : static_cast<const Try&>(s).handlers) fb(h.body);
            break;
        default: break;   // Def: a separate function
        }
    }
};

/// @brief Find every `def` in the program, including nested ones.
void collectDefs(Block& b, std::vector<Def*>& defs) {
    for (auto& s : b) {
        switch (s->k) {
        case Stmt::K::Def: {
            auto* d = static_cast<Def*>(s.get());
            defs.push_back(d);
            collectDefs(d->body, defs);
            break;
        }
        case Stmt::K::If:
            for (auto& [c, body] : static_cast<If*>(s.get())->branches) collectDefs(body, defs);
            collectDefs(static_cast<If*>(s.get())->orelse, defs);
            break;
        case Stmt::K::While: collectDefs(static_cast<While*>(s.get())->body, defs); break;
        case Stmt::K::For: collectDefs(static_cast<For*>(s.get())->body, defs); break;
        case Stmt::K::Try: {
            auto* t = static_cast<Try*>(s.get());
            collectDefs(t->body, defs);
            for (auto& h : t->handlers) collectDefs(h.body, defs);
            break;
        }
        default: break;
        }
    }
}

} // namespace

NeedsReport analyzeNeeds(Module& m) {
    NeedsReport report;
    std::vector<Def*> defs;
    collectDefs(m.body, defs);

    std::map<const Def*, Facts> facts;
    for (Def* d : defs) {
        Walker w{report, "def " + d->name, {}};
        for (auto& p : d->params)
            if (p.dflt) w.expr(*p.dflt);
        w.block(d->body);
        facts[d] = std::move(w.facts);
    }
    Walker top{report, "module", {}};
    top.block(m.body);

    // Fixpoint over the call graph (handles recursion and mutual recursion).
    for (auto& [d, f] : facts) const_cast<Def*>(d)->needs = f.direct;
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& [d, f] : facts) {
            unsigned n = d->needs;
            for (const Def* callee : f.calls) n |= callee->needs;
            if (n != d->needs) {
                const_cast<Def*>(d)->needs = n;
                changed = true;
            }
        }
    }

    // Mutation analysis. Freshness is per function and doesn't change; mutation
    // depends on callees, so iterate to a fixpoint (mutatesArgs only goes false → true).
    std::map<const Def*, MutationWalker> walkers;
    for (Def* d : defs) {
        auto& w = walkers.emplace(d, MutationWalker(*d)).first->second;
        w.freshnessBlock(d->body);
    }
    for (Def* d : defs) d->mutatedParams.assign(d->params.size(), false);
    for (bool changed = true; changed;) {
        changed = false;
        for (Def* d : defs) {
            auto& w = walkers.at(d);
            w.siteBlock(d->body);   // paramMut/other only ever gain bits
            bool other = w.other;
            if (other != d->mutatesOther || w.paramMut != d->mutatedParams) {
                d->mutatesOther = other;
                d->mutatedParams = w.paramMut;
                changed = true;
            }
        }
    }
    for (Def* d : defs) {
        d->mutatesArgs = d->mutatesOther;
        for (bool m : d->mutatedParams) d->mutatesArgs = d->mutatesArgs || m;
    }

    for (Def* d : defs) {
        report.mutatesArgs[d->name] = report.mutatesArgs[d->name] || d->mutatesArgs;
        if (report.functions.count(d->name))
            report.warnings.push_back("function name '" + d->name + "' is defined more than once (line " +
                                      std::to_string(d->line) + "); --needs shows their union");
        report.functions[d->name] |= d->needs;
    }
    report.topLevel = top.facts.direct;
    for (const Def* callee : top.facts.calls) report.topLevel |= callee->needs;
    for (Def* d : defs)   // main() is called implicitly when defined at module level
        if (d->name == "main" && d->slot < 0) report.topLevel |= d->needs;
    return report;
}

} // namespace dolphin
