/**
 * @file resolver.cpp
 * @brief Name resolution. See resolver.hpp for the rules.
 *
 * Two steps per scope:
 *  1. **Collect.** Find every name the scope binds (assignment targets,
 *     `for` and comprehension variables, `except … as`, nested `def`), without
 *     descending into nested functions, and give each a slot in order of first
 *     appearance, after the parameters.
 *  2. **Resolve.** Walk the scope's code and bind each Name against, in
 *     order, the scope's locals, the module's names, the builtins and the
 *     builtin exceptions.
 *
 * The module scope is collected first, so a function can call another
 * function defined further down the file.
 */
#include "resolver.hpp"

#include <unordered_map>
#include <unordered_set>

#include "interp.hpp"
#include "lexer.hpp"

namespace dolphin {

const char* bindName(Bind b) {
    switch (b) {
    case Bind::Unresolved: return "unresolved";
    case Bind::Local: return "local";
    case Bind::Global: return "global";
    case Bind::Function: return "function";
    case Bind::Builtin: return "builtin";
    case Bind::Exception: return "exception";
    }
    return "?";
}

namespace {

/// @brief How a scope binds one name: how many `def`s and how many other bindings.
struct BindingInfo {
    int slot = -1;            ///< Local slot (function scopes only).
    int defs = 0;             ///< Number of `def` statements binding the name.
    int others = 0;           ///< Number of other bindings (assignment, for, …).
    const Def* def = nullptr; ///< The def, when defs == 1.
};

/// @brief One function's (or the module's) bindings.
struct Scope {
    const Def* fn = nullptr;                              ///< Null for the module scope.
    Scope* parent = nullptr;                              ///< Enclosing scope.
    std::unordered_map<std::string, BindingInfo> names;   ///< Everything this scope binds.
    int nextSlot = 0;                                     ///< Next free local slot.

    /// Record a binding of @p id and return its info (allocating a slot on first sight).
    BindingInfo& bind(const std::string& id) {
        auto [it, fresh] = names.try_emplace(id);
        if (fresh && fn) it->second.slot = nextSlot++;
        return it->second;
    }
    /// The function this name is bound to, if a single `def` is its only binding.
    const Def* onlyDef(const std::string& id) const {
        auto it = names.find(id);
        if (it == names.end() || it->second.defs != 1 || it->second.others != 0) return nullptr;
        return it->second.def;
    }
};

class Resolver {
public:
    explicit Resolver(std::string file) : file_(std::move(file)) {
        for (auto& [name, parent] : builtinExceptions()) exceptions_.insert(name);
    }

    void run(Module& m) {
        module_.fn = nullptr;
        collectBlock(m.body, module_);
        resolveBlock(m.body, module_);
    }

private:
    // ------------------------------------------------------------ collect

    /// Bindings made by an assignment target. Only a plain name binds;
    /// `a[i] = x` and `a.f = x` mutate an existing object.
    void collectTarget(const Expr& t, Scope& sc) {
        if (t.k == Expr::K::Name) sc.bind(static_cast<const Name&>(t).id).others++;
    }

    /// Comprehension variables hide inside expressions.
    void collectExpr(const Expr& e, Scope& sc) {
        switch (e.k) {
        case Expr::K::ListComp: {
            auto& c = static_cast<const ListComp&>(e);
            sc.bind(c.var).others++;
            collectExpr(*c.iter, sc);
            collectExpr(*c.elt, sc);
            if (c.cond) collectExpr(*c.cond, sc);
            break;
        }
        case Expr::K::FStr:
            for (auto& p : static_cast<const FStr&>(e).parts)
                if (p.expr) collectExpr(*p.expr, sc);
            break;
        case Expr::K::Unary: collectExpr(*static_cast<const Unary&>(e).operand, sc); break;
        case Expr::K::Binary: {
            auto& b = static_cast<const Binary&>(e);
            collectExpr(*b.lhs, sc);
            collectExpr(*b.rhs, sc);
            break;
        }
        case Expr::K::BoolOp: {
            auto& b = static_cast<const BoolOp&>(e);
            collectExpr(*b.lhs, sc);
            collectExpr(*b.rhs, sc);
            break;
        }
        case Expr::K::Compare: {
            auto& c = static_cast<const Compare&>(e);
            collectExpr(*c.first, sc);
            for (auto& [op, r] : c.rest) collectExpr(*r, sc);
            break;
        }
        case Expr::K::Call: {
            auto& c = static_cast<const Call&>(e);
            collectExpr(*c.fn, sc);
            for (auto& a : c.args) collectExpr(*a, sc);
            for (auto& [k, v] : c.kwargs) collectExpr(*v, sc);
            break;
        }
        case Expr::K::Index: {
            auto& i = static_cast<const Index&>(e);
            collectExpr(*i.obj, sc);
            collectExpr(*i.index, sc);
            break;
        }
        case Expr::K::Slice: {
            auto& s = static_cast<const Slice&>(e);
            if (s.lo) collectExpr(*s.lo, sc);
            if (s.hi) collectExpr(*s.hi, sc);
            break;
        }
        case Expr::K::Attr: collectExpr(*static_cast<const Attr&>(e).obj, sc); break;
        case Expr::K::List:
            for (auto& x : static_cast<const ListLit&>(e).elems) collectExpr(*x, sc);
            break;
        case Expr::K::Dict:
            for (auto& [k, v] : static_cast<const DictLit&>(e).items) {
                collectExpr(*k, sc);
                collectExpr(*v, sc);
            }
            break;
        default: break;
        }
    }

    void collectBlock(const Block& b, Scope& sc) {
        for (auto& s : b) collectStmt(*s, sc);
    }

    void collectStmt(const Stmt& s, Scope& sc) {
        switch (s.k) {
        case Stmt::K::Expr: collectExpr(*static_cast<const ExprStmt&>(s).e, sc); break;
        case Stmt::K::Assign: {
            auto& a = static_cast<const Assign&>(s);
            collectExpr(*a.value, sc);
            collectTarget(*a.target, sc);
            break;
        }
        case Stmt::K::AugAssign: {
            auto& a = static_cast<const AugAssign&>(s);
            collectExpr(*a.value, sc);
            collectTarget(*a.target, sc);
            break;
        }
        case Stmt::K::If:
            for (auto& [c, body] : static_cast<const If&>(s).branches) {
                collectExpr(*c, sc);
                collectBlock(body, sc);
            }
            collectBlock(static_cast<const If&>(s).orelse, sc);
            break;
        case Stmt::K::While:
            collectExpr(*static_cast<const While&>(s).cond, sc);
            collectBlock(static_cast<const While&>(s).body, sc);
            break;
        case Stmt::K::For: {
            auto& f = static_cast<const For&>(s);
            sc.bind(f.var).others++;
            collectExpr(*f.iter, sc);
            collectBlock(f.body, sc);
            break;
        }
        case Stmt::K::Def: {   // binds its name here; its body is a separate scope
            auto& d = static_cast<const Def&>(s);
            BindingInfo& bi = sc.bind(d.name);
            bi.defs++;
            bi.def = &d;
            break;
        }
        case Stmt::K::Return:
            if (auto& v = static_cast<const Return&>(s).value) collectExpr(*v, sc);
            break;
        case Stmt::K::Raise:
            if (auto& v = static_cast<const Raise&>(s).value) collectExpr(*v, sc);
            break;
        case Stmt::K::Try: {
            auto& t = static_cast<const Try&>(s);
            collectBlock(t.body, sc);
            for (auto& h : t.handlers) {
                if (!h.as.empty()) sc.bind(h.as).others++;
                collectBlock(h.body, sc);
            }
            break;
        }
        default: break;
        }
    }

    // ------------------------------------------------------------ resolve

    [[noreturn]] void fail(int line, const std::string& msg, const char* kind = "NameError") const {
        throw SyntaxError(file_, line, msg, kind);
    }

    int slotOf(const Scope& sc, const std::string& id) const {
        if (!sc.fn) return -1;
        return sc.names.at(id).slot;
    }

    void resolveName(Name& n, const Scope& sc) {
        if (sc.fn) {
            auto it = sc.names.find(n.id);
            if (it != sc.names.end()) {
                n.bind = Bind::Local;
                n.slot = it->second.slot;
                n.def = sc.onlyDef(n.id);
                return;
            }
            for (const Scope* p = sc.parent; p && p->fn; p = p->parent)
                if (p->names.count(n.id))
                    fail(n.line, "'" + n.id + "' is a local of enclosing function '" + p->fn->name +
                                     "'; closures are not supported yet (pass it as an argument)");
        }
        if (module_.names.count(n.id)) {
            if (const Def* d = module_.onlyDef(n.id)) {
                n.bind = Bind::Function;
                n.def = d;
            } else {
                n.bind = Bind::Global;
            }
            return;
        }
        if (auto it = builtinNeeds().find(n.id); it != builtinNeeds().end()) {
            n.bind = Bind::Builtin;
            n.needs = it->second;
            return;
        }
        if (exceptions_.count(n.id)) { n.bind = Bind::Exception; return; }
        fail(n.line, "name '" + n.id + "' is not defined");
    }

    void resolveExpr(Expr& e, Scope& sc) {
        switch (e.k) {
        case Expr::K::Name: resolveName(static_cast<Name&>(e), sc); break;
        case Expr::K::FStr:
            for (auto& p : static_cast<FStr&>(e).parts)
                if (p.expr) resolveExpr(*p.expr, sc);
            break;
        case Expr::K::Unary: resolveExpr(*static_cast<Unary&>(e).operand, sc); break;
        case Expr::K::Binary: {
            auto& b = static_cast<Binary&>(e);
            resolveExpr(*b.lhs, sc);
            resolveExpr(*b.rhs, sc);
            break;
        }
        case Expr::K::BoolOp: {
            auto& b = static_cast<BoolOp&>(e);
            resolveExpr(*b.lhs, sc);
            resolveExpr(*b.rhs, sc);
            break;
        }
        case Expr::K::Compare: {
            auto& c = static_cast<Compare&>(e);
            resolveExpr(*c.first, sc);
            for (auto& [op, r] : c.rest) resolveExpr(*r, sc);
            break;
        }
        case Expr::K::Call: {
            auto& c = static_cast<Call&>(e);
            resolveExpr(*c.fn, sc);
            for (auto& a : c.args) resolveExpr(*a, sc);
            for (auto& [k, v] : c.kwargs) resolveExpr(*v, sc);
            break;
        }
        case Expr::K::Index: {
            auto& i = static_cast<Index&>(e);
            resolveExpr(*i.obj, sc);
            resolveExpr(*i.index, sc);
            break;
        }
        case Expr::K::Slice: {
            auto& s = static_cast<Slice&>(e);
            if (s.lo) resolveExpr(*s.lo, sc);
            if (s.hi) resolveExpr(*s.hi, sc);
            break;
        }
        case Expr::K::Attr: resolveExpr(*static_cast<Attr&>(e).obj, sc); break;   // attribute names aren't variables
        case Expr::K::List:
            for (auto& x : static_cast<ListLit&>(e).elems) resolveExpr(*x, sc);
            break;
        case Expr::K::Dict:
            for (auto& [k, v] : static_cast<DictLit&>(e).items) {
                resolveExpr(*k, sc);
                resolveExpr(*v, sc);
            }
            break;
        case Expr::K::ListComp: {
            auto& c = static_cast<ListComp&>(e);
            c.slot = slotOf(sc, c.var);
            resolveExpr(*c.iter, sc);
            resolveExpr(*c.elt, sc);
            if (c.cond) resolveExpr(*c.cond, sc);
            break;
        }
        default: break;   // literals
        }
    }

    void resolveBlock(Block& b, Scope& sc) {
        for (auto& s : b) resolveStmt(*s, sc);
    }

    void resolveStmt(Stmt& s, Scope& sc) {
        switch (s.k) {
        case Stmt::K::Expr: resolveExpr(*static_cast<ExprStmt&>(s).e, sc); break;
        case Stmt::K::Assign: {
            auto& a = static_cast<Assign&>(s);   // annotations are types: left for the type checker
            resolveExpr(*a.value, sc);
            resolveExpr(*a.target, sc);
            break;
        }
        case Stmt::K::AugAssign: {
            auto& a = static_cast<AugAssign&>(s);
            resolveExpr(*a.value, sc);
            resolveExpr(*a.target, sc);
            break;
        }
        case Stmt::K::If: {
            auto& i = static_cast<If&>(s);
            for (auto& [c, body] : i.branches) {
                resolveExpr(*c, sc);
                resolveBlock(body, sc);
            }
            resolveBlock(i.orelse, sc);
            break;
        }
        case Stmt::K::While: {
            auto& w = static_cast<While&>(s);
            resolveExpr(*w.cond, sc);
            resolveBlock(w.body, sc);
            break;
        }
        case Stmt::K::For: {
            auto& f = static_cast<For&>(s);
            f.slot = slotOf(sc, f.var);
            resolveExpr(*f.iter, sc);
            resolveBlock(f.body, sc);
            break;
        }
        case Stmt::K::Def: resolveDef(static_cast<Def&>(s), sc); break;
        case Stmt::K::Return:
            if (auto& v = static_cast<Return&>(s).value) resolveExpr(*v, sc);
            break;
        case Stmt::K::Raise:
            if (auto& v = static_cast<Raise&>(s).value) resolveExpr(*v, sc);
            break;
        case Stmt::K::Try: {
            auto& t = static_cast<Try&>(s);
            resolveBlock(t.body, sc);
            for (auto& h : t.handlers) {
                if (!h.cls.empty()) {
                    Name probe(h.line, h.cls);
                    resolveName(probe, sc);
                    if (probe.bind != Bind::Exception)
                        fail(h.line, "'" + h.cls + "' is not an exception class", "TypeError");
                }
                if (!h.as.empty()) h.asSlot = slotOf(sc, h.as);
                resolveBlock(h.body, sc);
            }
            break;
        }
        default: break;
        }
    }

    void resolveDef(Def& d, Scope& enclosing) {
        d.slot = slotOf(enclosing, d.name);
        // Defaults are evaluated at call time in the module scope (see
        // Interp::callFunction), so resolve them there.
        for (auto& p : d.params)
            if (p.dflt) resolveExpr(*p.dflt, module_);

        Scope sc;
        sc.fn = &d;
        sc.parent = &enclosing;
        for (auto& p : d.params) {
            BindingInfo& bi = sc.bind(p.name);
            bi.others++;
            p.slot = bi.slot;
        }
        collectBlock(d.body, sc);
        resolveBlock(d.body, sc);
        d.numLocals = sc.nextSlot;
    }

    std::string file_;
    Scope module_;
    std::unordered_set<std::string> exceptions_;
};

} // namespace

void resolveNames(Module& m) {
    Resolver r(m.file);
    r.run(m);
}

} // namespace dolphin
