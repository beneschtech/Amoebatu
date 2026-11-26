/**
 * @file fill.cpp
 * @brief The parallel-fill rewrite. See fill.hpp for what qualifies and why.
 */
#include "fill.hpp"

#include <string>
#include <vector>

#include "analyzer.hpp"

namespace dolphin {

namespace {

// ------------------------------------------------------------- cloning

ExprP clone(const Expr& e);
ExprP cloneOpt(const ExprP& e) { return e ? clone(*e) : nullptr; }

ExprP clone(const Expr& e) {
    switch (e.k) {
    case Expr::K::Int: return std::make_unique<IntLit>(e.line, static_cast<const IntLit&>(e).v);
    case Expr::K::Float: return std::make_unique<FloatLit>(e.line, static_cast<const FloatLit&>(e).v);
    case Expr::K::Str: return std::make_unique<StrLit>(e.line, static_cast<const StrLit&>(e).v);
    case Expr::K::Bytes: return std::make_unique<BytesLit>(e.line, static_cast<const BytesLit&>(e).v);
    case Expr::K::Bool: return std::make_unique<BoolLit>(e.line, static_cast<const BoolLit&>(e).v);
    case Expr::K::None: return std::make_unique<NoneLit>(e.line);
    case Expr::K::Name: return std::make_unique<Name>(e.line, static_cast<const Name&>(e).id);   // the resolver rebinds it
    case Expr::K::FStr: {
        auto out = std::make_unique<FStr>(e.line);
        for (auto& p : static_cast<const FStr&>(e).parts) {
            FStrPart q;
            q.text = p.text;
            q.expr = cloneOpt(p.expr);
            q.spec = p.spec;
            out->parts.push_back(std::move(q));
        }
        return out;
    }
    case Expr::K::Unary: {
        auto& u = static_cast<const Unary&>(e);
        return std::make_unique<Unary>(e.line, u.op, clone(*u.operand));
    }
    case Expr::K::Binary: {
        auto& b = static_cast<const Binary&>(e);
        return std::make_unique<Binary>(e.line, b.op, clone(*b.lhs), clone(*b.rhs));
    }
    case Expr::K::BoolOp: {
        auto& b = static_cast<const BoolOp&>(e);
        return std::make_unique<BoolOp>(e.line, b.op, clone(*b.lhs), clone(*b.rhs));
    }
    case Expr::K::Compare: {
        auto& c = static_cast<const Compare&>(e);
        auto out = std::make_unique<Compare>(e.line, clone(*c.first));
        for (auto& [op, r] : c.rest) out->add(op, clone(*r));
        return out;
    }
    case Expr::K::Call: {
        auto& c = static_cast<const Call&>(e);
        auto out = std::make_unique<Call>(e.line, clone(*c.fn));
        for (auto& a : c.args) out->args.push_back(clone(*a));
        for (auto& [k, v] : c.kwargs) out->kwargs.emplace_back(k, clone(*v));
        return out;
    }
    case Expr::K::Index: {
        auto& i = static_cast<const Index&>(e);
        return std::make_unique<Index>(e.line, clone(*i.obj), clone(*i.index));
    }
    case Expr::K::Slice: {
        auto& s = static_cast<const Slice&>(e);
        auto out = std::make_unique<Slice>(e.line);
        out->lo = cloneOpt(s.lo);
        out->hi = cloneOpt(s.hi);
        return out;
    }
    case Expr::K::Attr: {
        auto& a = static_cast<const Attr&>(e);
        return std::make_unique<Attr>(e.line, clone(*a.obj), a.name);
    }
    case Expr::K::List: {
        auto out = std::make_unique<ListLit>(e.line);
        for (auto& x : static_cast<const ListLit&>(e).elems) out->elems.push_back(clone(*x));
        return out;
    }
    case Expr::K::Dict: {
        auto out = std::make_unique<DictLit>(e.line);
        for (auto& [k, v] : static_cast<const DictLit&>(e).items) out->items.emplace_back(clone(*k), clone(*v));
        return out;
    }
    case Expr::K::ListComp: {
        auto& c = static_cast<const ListComp&>(e);
        auto out = std::make_unique<ListComp>(e.line);
        out->elt = clone(*c.elt);
        out->var = c.var;
        out->iter = clone(*c.iter);
        out->cond = cloneOpt(c.cond);
        return out;
    }
    }
    return nullptr;   // unreachable: every kind is handled
}

// ---------------------------------------------------- local containment
//
// Per local slot: is it "contained"? That is, every assignment to it creates
// a new list or dict, and its value is never put anywhere another piece of
// code could read it later (another variable, a container, a user function's
// argument). Parameters, loop variables and `except ... as` names never are.

/// Does evaluating @p v always give a new list or dict nobody else references?
bool freshContainer(const Expr& v) {
    switch (v.k) {
    case Expr::K::List:
    case Expr::K::Dict:
    case Expr::K::ListComp:
    case Expr::K::Binary:   // list + list, [x] * n: a new list
        return true;
    case Expr::K::Index:    // a slice copies
        return static_cast<const Index&>(v).index->k == Expr::K::Slice;
    case Expr::K::Call: {
        auto& c = static_cast<const Call&>(v);
        if (c.fn->k != Expr::K::Name) return false;
        auto& n = static_cast<const Name&>(*c.fn);
        return n.bind == Bind::Builtin && (n.id == "array" || n.id == "range" || n.id == "sorted");
    }
    default:
        return false;
    }
}

struct Containment {
    std::vector<bool> bad;   ///< Per slot: not contained.

    explicit Containment(const Def& d) : bad(static_cast<size_t>(d.numLocals), false) {
        for (auto& p : d.params) mark(p.slot);
        block(d.body);
    }
    void mark(int slot) {
        if (slot >= 0 && static_cast<size_t>(slot) < bad.size()) bad[static_cast<size_t>(slot)] = true;
    }
    bool ok(int slot) const { return slot >= 0 && static_cast<size_t>(slot) < bad.size() && !bad[static_cast<size_t>(slot)]; }

    /// Visit @p e. @p harmless: a bare local name in this position doesn't let its value escape.
    void use(const Expr& e, bool harmless) {
        switch (e.k) {
        case Expr::K::Name: {
            auto& n = static_cast<const Name&>(e);
            if (n.bind == Bind::Local && !harmless) mark(n.slot);
            return;
        }
        case Expr::K::Index: {
            auto& i = static_cast<const Index&>(e);
            use(*i.obj, true);   // reads an element, or slices a copy
            use(*i.index, false);
            return;
        }
        case Expr::K::Slice: {
            auto& s = static_cast<const Slice&>(e);
            if (s.lo) use(*s.lo, false);
            if (s.hi) use(*s.hi, false);
            return;
        }
        case Expr::K::Attr: use(*static_cast<const Attr&>(e).obj, false); return;
        case Expr::K::Call: {
            auto& c = static_cast<const Call&>(e);
            bool builtin = false;
            if (c.fn->k == Expr::K::Attr) {
                use(*static_cast<const Attr&>(*c.fn).obj, true);   // a method's receiver: no method returns it
            } else if (c.fn->k == Expr::K::Name) {
                auto& n = static_cast<const Name&>(*c.fn);
                builtin = n.bind == Bind::Builtin;   // builtins don't keep their arguments
                use(n, false);
            } else {
                use(*c.fn, false);
            }
            for (auto& a : c.args) use(*a, builtin);
            for (auto& [k, v] : c.kwargs) use(*v, builtin);
            return;
        }
        case Expr::K::Compare: {
            auto& c = static_cast<const Compare&>(e);
            use(*c.first, true);
            for (auto& [op, r] : c.rest) use(*r, true);
            return;
        }
        case Expr::K::Binary: {   // always a new value
            auto& b = static_cast<const Binary&>(e);
            use(*b.lhs, true);
            use(*b.rhs, true);
            return;
        }
        case Expr::K::Unary: use(*static_cast<const Unary&>(e).operand, true); return;
        case Expr::K::BoolOp: {   // returns an operand
            auto& b = static_cast<const BoolOp&>(e);
            use(*b.lhs, false);
            use(*b.rhs, false);
            return;
        }
        case Expr::K::FStr:
            for (auto& p : static_cast<const FStr&>(e).parts)
                if (p.expr) use(*p.expr, true);
            return;
        case Expr::K::List:
            for (auto& x : static_cast<const ListLit&>(e).elems) use(*x, false);
            return;
        case Expr::K::Dict:
            for (auto& [k, v] : static_cast<const DictLit&>(e).items) {
                use(*k, false);
                use(*v, false);
            }
            return;
        case Expr::K::ListComp: {
            auto& c = static_cast<const ListComp&>(e);
            mark(c.slot);   // aliases elements
            use(*c.iter, true);
            use(*c.elt, false);
            if (c.cond) use(*c.cond, true);
            return;
        }
        default: return;   // literals
        }
    }

    void target(const Expr& t) {   // the left side of = or op=
        if (t.k == Expr::K::Index) {
            auto& i = static_cast<const Index&>(t);
            use(*i.obj, true);
            use(*i.index, false);
        } else if (t.k == Expr::K::Attr) {
            use(*static_cast<const Attr&>(t).obj, false);
        }
    }

    void block(const Block& b) {
        for (auto& s : b) stmt(*s);
    }

    void stmt(const Stmt& s) {
        switch (s.k) {
        case Stmt::K::Expr: use(*static_cast<const ExprStmt&>(s).e, true); break;
        case Stmt::K::Assign: {
            auto& a = static_cast<const Assign&>(s);
            if (a.target->k == Expr::K::Name) {
                auto& n = static_cast<const Name&>(*a.target);
                if (n.bind == Bind::Local && !freshContainer(*a.value)) mark(n.slot);
            } else {
                target(*a.target);
            }
            use(*a.value, false);
            break;
        }
        case Stmt::K::AugAssign: {
            auto& a = static_cast<const AugAssign&>(s);
            if (a.target->k == Expr::K::Name) mark(static_cast<const Name&>(*a.target).slot);
            else target(*a.target);
            use(*a.value, false);
            break;
        }
        case Stmt::K::If: {
            auto& i = static_cast<const If&>(s);
            for (auto& [c, b] : i.branches) {
                use(*c, true);
                block(b);
            }
            block(i.orelse);
            break;
        }
        case Stmt::K::While: {
            auto& w = static_cast<const While&>(s);
            use(*w.cond, true);
            block(w.body);
            break;
        }
        case Stmt::K::For: {
            auto& f = static_cast<const For&>(s);
            mark(f.slot);
            use(*f.iter, true);
            block(f.body);
            break;
        }
        case Stmt::K::Return:   // handing it back ends this call; the next call makes a new one
            if (auto& v = static_cast<const Return&>(s).value) use(*v, true);
            break;
        case Stmt::K::Raise:
            if (auto& v = static_cast<const Raise&>(s).value) use(*v, false);
            break;
        case Stmt::K::Try: {
            auto& t = static_cast<const Try&>(s);
            block(t.body);
            for (auto& h : t.handlers) {
                mark(h.asSlot);
                block(h.body);
            }
            break;
        }
        case Stmt::K::Def: mark(static_cast<const Def&>(s).slot); break;   // its body is another scope
        default: break;
        }
    }
};

/// Does @p e mention local slot @p slot?
bool mentions(const Expr& e, int slot) {
    switch (e.k) {
    case Expr::K::Name: {
        auto& n = static_cast<const Name&>(e);
        return n.bind == Bind::Local && n.slot == slot;
    }
    case Expr::K::FStr:
        for (auto& p : static_cast<const FStr&>(e).parts)
            if (p.expr && mentions(*p.expr, slot)) return true;
        return false;
    case Expr::K::Unary: return mentions(*static_cast<const Unary&>(e).operand, slot);
    case Expr::K::Binary: return mentions(*static_cast<const Binary&>(e).lhs, slot) || mentions(*static_cast<const Binary&>(e).rhs, slot);
    case Expr::K::BoolOp: return mentions(*static_cast<const BoolOp&>(e).lhs, slot) || mentions(*static_cast<const BoolOp&>(e).rhs, slot);
    case Expr::K::Compare: {
        auto& c = static_cast<const Compare&>(e);
        if (mentions(*c.first, slot)) return true;
        for (auto& [op, r] : c.rest)
            if (mentions(*r, slot)) return true;
        return false;
    }
    case Expr::K::Call: {
        auto& c = static_cast<const Call&>(e);
        if (mentions(*c.fn, slot)) return true;
        for (auto& a : c.args)
            if (mentions(*a, slot)) return true;
        for (auto& [k, v] : c.kwargs)
            if (mentions(*v, slot)) return true;
        return false;
    }
    case Expr::K::Index: return mentions(*static_cast<const Index&>(e).obj, slot) || mentions(*static_cast<const Index&>(e).index, slot);
    case Expr::K::Slice: {
        auto& s = static_cast<const Slice&>(e);
        return (s.lo && mentions(*s.lo, slot)) || (s.hi && mentions(*s.hi, slot));
    }
    case Expr::K::Attr: return mentions(*static_cast<const Attr&>(e).obj, slot);
    case Expr::K::List:
        for (auto& x : static_cast<const ListLit&>(e).elems)
            if (mentions(*x, slot)) return true;
        return false;
    case Expr::K::Dict:
        for (auto& [k, v] : static_cast<const DictLit&>(e).items)
            if (mentions(*k, slot) || mentions(*v, slot)) return true;
        return false;
    case Expr::K::ListComp: {
        auto& c = static_cast<const ListComp&>(e);
        return c.slot == slot || mentions(*c.elt, slot) || mentions(*c.iter, slot) || (c.cond && mentions(*c.cond, slot));
    }
    default: return false;
    }
}

// ------------------------------------------------------------ rewriting

class Rewriter {
public:
    int count = 0;

    void def(Def& d) {
        Containment c(d);
        block(d.body, c);
    }

private:
    void block(Block& b, const Containment& c) {
        for (auto& s : b) {
            switch (s->k) {
            case Stmt::K::If: {
                auto& i = static_cast<If&>(*s);
                for (auto& [cond, body] : i.branches) block(body, c);
                block(i.orelse, c);
                break;
            }
            case Stmt::K::While: block(static_cast<While&>(*s).body, c); break;
            case Stmt::K::Try: {
                auto& t = static_cast<Try&>(*s);
                block(t.body, c);
                for (auto& h : t.handlers) block(h.body, c);
                break;
            }
            case Stmt::K::For:
                if (qualifies(static_cast<const For&>(*s), c)) s = rewrite(std::move(s));
                else block(static_cast<For&>(*s).body, c);
                break;
            default: break;   // nested defs are visited as functions of their own
            }
        }
    }

    static bool qualifies(const For& f, const Containment& c) {
        if (f.slot < 0 || f.body.size() != 1 || f.body[0]->k != Stmt::K::Assign) return false;
        auto& a = static_cast<const Assign&>(*f.body[0]);
        if (a.target->k != Expr::K::Index || a.value->k != Expr::K::Call) return false;
        auto& tgt = static_cast<const Index&>(*a.target);
        if (tgt.obj->k != Expr::K::Name || tgt.index->k == Expr::K::Slice) return false;
        auto& out = static_cast<const Name&>(*tgt.obj);
        if (out.bind != Bind::Local || !c.ok(out.slot) || out.slot == f.slot) return false;
        auto& call = static_cast<const Call&>(*a.value);
        if (call.fn->k != Expr::K::Name) return false;
        const Def* callee = static_cast<const Name&>(*call.fn).def;
        if (!callee || !callee->parallelSafe()) return false;
        if (!isReorderableExpr(*f.iter) || !isReorderableExpr(*tgt.index) || mentions(*tgt.index, out.slot)) return false;
        for (auto& x : call.args)
            if (!isReorderableExpr(*x) || mentions(*x, out.slot)) return false;
        for (auto& [k, v] : call.kwargs)
            if (!isReorderableExpr(*v) || mentions(*v, out.slot)) return false;
        return true;
    }

    StmtP rewrite(StmtP loop) {
        auto& f = static_cast<For&>(*loop);
        auto& a = static_cast<Assign&>(*f.body[0]);
        auto& tgt = static_cast<Index&>(*a.target);
        const std::string out = static_cast<Name&>(*tgt.obj).id;
        const int line = f.line;
        const std::string n = std::to_string(++count);
        const std::string ok = "$fill" + n, items = "$items" + n, keys = "$keys" + n, vals = "$vals" + n;

        auto name = [&](const std::string& id) { return std::make_unique<Name>(line, id); };
        auto assign = [&](const std::string& id, ExprP v) {
            auto s = std::make_unique<Assign>(line);
            s->target = name(id);
            s->value = std::move(v);
            return s;
        };
        auto comp = [&](ExprP elt, ExprP iter) {
            auto c = std::make_unique<ListComp>(line);
            c->elt = std::move(elt);
            c->var = f.var;
            c->iter = std::move(iter);
            return c;
        };
        auto call = [&](const char* fn, std::vector<ExprP> args) {
            auto c = std::make_unique<Call>(line, name(fn));
            c->args = std::move(args);
            return c;
        };
        auto args = [](auto... xs) {
            std::vector<ExprP> v;
            (v.push_back(std::move(xs)), ...);
            return v;
        };

        auto body = std::make_unique<If>(line);   // `if True:` holds the statements in place of the loop
        Block stmts;
        stmts.push_back(assign(ok, std::make_unique<BoolLit>(line, false)));

        auto t = std::make_unique<Try>(line);
        t->body.push_back(assign(items, comp(name(f.var), clone(*f.iter))));
        t->body.push_back(assign(keys, comp(clone(*tgt.index), name(items))));
        t->body.push_back(assign(vals, comp(clone(*a.value), name(items))));
        t->body.push_back(assign(ok, call("__fill_ok", args(name(out), name(keys)))));
        Handler h;
        h.line = line;
        h.body.push_back(std::make_unique<Pass>(line));
        t->handlers.push_back(std::move(h));
        stmts.push_back(std::move(t));

        auto choose = std::make_unique<If>(line);
        Block fast;
        fast.push_back(std::make_unique<ExprStmt>(line, call("__fill", args(name(out), name(keys), name(vals)))));
        choose->branches.emplace_back(name(ok), std::move(fast));
        choose->orelse.push_back(std::move(loop));   // the original, untouched
        stmts.push_back(std::move(choose));

        body->branches.emplace_back(std::make_unique<BoolLit>(line, true), std::move(stmts));
        return body;
    }
};

void collectDefs(Block& b, std::vector<Def*>& out) {
    for (auto& s : b) {
        switch (s->k) {
        case Stmt::K::Def: {
            auto* d = static_cast<Def*>(s.get());
            out.push_back(d);
            collectDefs(d->body, out);
            break;
        }
        case Stmt::K::If:
            for (auto& [c, body] : static_cast<If&>(*s).branches) collectDefs(body, out);
            collectDefs(static_cast<If&>(*s).orelse, out);
            break;
        case Stmt::K::While: collectDefs(static_cast<While&>(*s).body, out); break;
        case Stmt::K::For: collectDefs(static_cast<For&>(*s).body, out); break;
        case Stmt::K::Try:
            collectDefs(static_cast<Try&>(*s).body, out);
            for (auto& h : static_cast<Try&>(*s).handlers) collectDefs(h.body, out);
            break;
        default: break;
        }
    }
}

} // namespace

int rewriteParallelFills(Module& m) {
    std::vector<Def*> defs;
    collectDefs(m.body, defs);
    Rewriter r;
    for (Def* d : defs) r.def(*d);
    return r.count;
}

} // namespace dolphin
