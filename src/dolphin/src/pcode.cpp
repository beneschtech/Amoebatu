/**
 * @file pcode.cpp
 * @brief AST → p-code compiler and disassembler. See pcode.hpp.
 *
 * The compiler mirrors the tree-walker's evaluation order exactly (same
 * operands evaluated in the same order, same double evaluation of an
 * augmented-assignment target), so the two engines produce identical output
 * and identical errors. Anything it can't express makes compileFunction()
 * return null and the function runs on the tree-walker instead.
 */
#include "pcode.hpp"

#include <algorithm>
#include <bitset>
#include <cstdio>
#include <sstream>
#include <unordered_map>

namespace dolphin {

const char* opName(OpCode op) {
    static const char* const k[] = {
        "MOVE", "LOADK", "LOADNIL", "LOADTRUE", "LOADFALSE", "CHKB", "GETG",
        "ADD", "SUB", "MUL", "DIV", "FDIV", "MOD", "POW", "BAND", "BOR", "BXOR", "SHL", "SHR",
        "EQ", "NE", "LT", "LE", "GT", "GE", "IN", "NOTIN",
        "NOT", "NEG", "POS", "INV", "JMP", "JMPIF", "JMPIFNOT", "ITER", "FORNEXT",
        "NEWLIST", "APPEND", "NEWDICT", "DICTSET", "INDEX", "INDEXK", "SLICE", "SETINDEX", "ATTR",
        "FORMAT", "CONCAT", "CALL", "RETURN", "RETURNNIL", "RAISE", "RERAISE", "EXCMATCH", "EXCBIND",
        "HPUSH", "HPOP", "RETHROW", "SETTLE", "SPAWN", "FORCE", "JMPNOPOOL", "PMAP",
        "JEQ", "JNE", "JLT", "JLE", "JGT", "JGE", "JIN", "JNOTIN"};
    return k[static_cast<size_t>(op)];
}

namespace {

/// Thrown inside the compiler when a function can't be compiled; caught by compileFunction().
struct CannotCompile {};

class Compiler {
public:
    Compiler(const Def& d, const CompileHooks& hooks, PCode& p) : d_(d), hooks_(hooks), builtins_(hooks.builtin), p_(p) {}

    void run() {
        p_.def = &d_;
        p_.nparams = static_cast<unsigned>(d_.params.size());
        p_.regNames.assign(256, "");
        for (auto& par : d_.params) p_.regNames[static_cast<size_t>(par.slot)] = par.name;
        top_ = static_cast<unsigned>(d_.numLocals);
        for (unsigned i = 0; i < p_.nparams; ++i) known_.set.set(i);   // parameters are always bound
        if (top_ > 256) throw CannotCompile{};
        p_.nregs = top_;
        line_ = d_.line;
        nameLocals(d_.body);
        findSpawns(d_.body);
        block(d_.body);
        emit(ins::abc(OpCode::RETURNNIL, 0));   // falling off the end returns None
        p_.regNames.resize(p_.nregs);
    }

private:
    const Def& d_;
    const CompileHooks& hooks_;
    const std::function<Value(const std::string&)>& builtins_;
    std::vector<bool> mayBeFuture_ = std::vector<bool>(256, false);   ///< Locals a SPAWN writes.

    /**
     * Definite assignment (--opt 1): which locals are certainly bound at the
     * current point of the code being emitted. A local in the set needs no
     * CHKB. Merges (after if/else, after a try) intersect; code after a
     * return/raise/break/continue is dead and doesn't count in a merge. A
     * loop body may run zero times, so nothing it assigns is known after it;
     * a handler may start anywhere in its try, so it starts from what was
     * known at the try's entry. A CHKB that passes proves its local for the
     * code after it.
     */
    struct Known {
        std::bitset<256> set;
        bool live = true;
    };
    Known known_;
    static Known merge(const Known& a, const Known& b) {
        if (!a.live) return b;
        if (!b.live) return a;
        return Known{a.set & b.set, true};
    }
    void mark(unsigned slot) { known_.set.set(slot); }
    PCode& p_;
    unsigned top_ = 0;     ///< Next free temporary register.
    unsigned iterDepth_ = 0;
    int line_ = 0;
    std::unordered_map<std::string, unsigned> nameIndex_;

    /// Enclosing constructs a break/continue/return must leave cleanly.
    struct Region {
        enum class K { Loop, TryBody, Handler } k;
        size_t continueTarget = 0;
        std::vector<size_t> breaks, continues;
    };
    std::vector<Region> regions_;

    // ------------------------------------------------------------- emitting

    size_t emit(uint32_t w) {
        p_.code.push_back(w);
        p_.lines.push_back(line_);
        return p_.code.size() - 1;
    }
    size_t here() const { return p_.code.size(); }
    /// Point the jump at @p at to @p target.
    void patch(size_t at, size_t target) {
        long off = static_cast<long>(target) - static_cast<long>(at + 1);
        if (off < -32768 || off > 32767) throw CannotCompile{};
        uint32_t w = p_.code[at];
        OpCode op = ins::op(w);
        p_.code[at] = ins::asbx(op, ins::a(w), static_cast<int>(off));
    }
    size_t jump(OpCode op, unsigned a = 0) { return emit(ins::asbx(op, a, 0)); }

    unsigned temp() {
        if (top_ >= 256) throw CannotCompile{};   // more than 256 registers: tree-walker
        unsigned r = top_++;
        p_.nregs = std::max(p_.nregs, top_);
        return r;
    }
    unsigned konst(Value v, std::string key = "") {
        if (p_.consts.size() >= 65536) throw CannotCompile{};
        p_.consts.push_back(std::move(v));
        p_.constKeys.push_back(std::move(key));
        return static_cast<unsigned>(p_.consts.size() - 1);
    }
    unsigned name(const std::string& s) {
        auto it = nameIndex_.find(s);
        if (it != nameIndex_.end()) return it->second;
        if (p_.names.size() >= 65536) throw CannotCompile{};
        p_.names.push_back(s);
        return nameIndex_[s] = static_cast<unsigned>(p_.names.size() - 1);
    }
    unsigned callSite(CallSite cs) {
        if (p_.calls.size() >= 65536) throw CannotCompile{};
        p_.calls.push_back(std::move(cs));
        return static_cast<unsigned>(p_.calls.size() - 1);
    }

    /// Record local names per slot, for UnboundLocalError messages and --disasm.
    void nameLocals(const Block& b) {
        for (auto& s : b) {
            switch (s->k) {
            case Stmt::K::Assign: {
                auto& a = static_cast<const Assign&>(*s);
                if (a.target->k == Expr::K::Name) setName(static_cast<const Name&>(*a.target));
                break;
            }
            case Stmt::K::AugAssign: {
                auto& a = static_cast<const AugAssign&>(*s);
                if (a.target->k == Expr::K::Name) setName(static_cast<const Name&>(*a.target));
                break;
            }
            case Stmt::K::For: {
                auto& f = static_cast<const For&>(*s);
                if (f.slot >= 0) p_.regNames[static_cast<size_t>(f.slot)] = f.var;
                nameLocals(f.body);
                break;
            }
            case Stmt::K::While: nameLocals(static_cast<const While&>(*s).body); break;
            case Stmt::K::If:
                for (auto& [c, body] : static_cast<const If&>(*s).branches) nameLocals(body);
                nameLocals(static_cast<const If&>(*s).orelse);
                break;
            case Stmt::K::Try:
                nameLocals(static_cast<const Try&>(*s).body);
                for (auto& h : static_cast<const Try&>(*s).handlers) {
                    if (h.asSlot >= 0) p_.regNames[static_cast<size_t>(h.asSlot)] = h.as;
                    nameLocals(h.body);
                }
                break;
            case Stmt::K::Def: {
                auto& nd = static_cast<const Def&>(*s);
                if (nd.slot >= 0) p_.regNames[static_cast<size_t>(nd.slot)] = nd.name;
                break;
            }
            default: break;
            }
        }
    }
    void setName(const Name& n) {
        if (n.bind == Bind::Local && n.slot >= 0) p_.regNames[static_cast<size_t>(n.slot)] = n.id;
    }

    // ---------------------------------------------------------- expressions

    /// Read local @p slot: check it was assigned (parameters always are).
    unsigned local(int slot) {
        if (slot < 0) throw CannotCompile{};
        unsigned u = static_cast<unsigned>(slot);
        if (hooks_.optimize ? !known_.set.test(u) : u >= p_.nparams) emit(ins::abc(OpCode::CHKB, u));
        mark(u);   // past this point (if the check passed) it's bound
        if (mayBeFuture_[static_cast<size_t>(slot)]) emit(ins::abc(OpCode::FORCE, static_cast<unsigned>(slot)));
        return static_cast<unsigned>(slot);
    }

    /// The parallel-safe function a call names statically, if any (as Interp::dispatchTarget()).
    static const Def* spawnTarget(const Expr& e) {
        if (e.k != Expr::K::Call) return nullptr;
        auto& c = static_cast<const Call&>(e);
        if (c.fn->k != Expr::K::Name) return nullptr;
        const Def* d = static_cast<const Name&>(*c.fn).def;
        return d && d->parallelSafe() ? d : nullptr;
    }
    /// Is @p s a statement the tree-walker would try to dispatch? Returns the callee.
    const Def* spawnable(const Stmt& s) const {
        if (!hooks_.overlap || !hooks_.overlap(s)) return nullptr;
        if (s.k == Stmt::K::Assign) {
            auto& a = static_cast<const Assign&>(s);
            if (a.target->k != Expr::K::Name || static_cast<const Name&>(*a.target).bind != Bind::Local) return nullptr;
            return spawnTarget(*a.value);
        }
        if (s.k == Stmt::K::Expr) return spawnTarget(*static_cast<const ExprStmt&>(s).e);
        return nullptr;
    }
    /// Mark every local a SPAWN may write: reads of those need FORCE.
    void findSpawns(const Block& b) {
        for (auto& s : b) {
            if (spawnable(*s) && s->k == Stmt::K::Assign) {
                int slot = static_cast<const Name&>(*static_cast<const Assign&>(*s).target).slot;
                if (slot >= 0) mayBeFuture_[static_cast<size_t>(slot)] = true;
            }
            switch (s->k) {
            case Stmt::K::If:
                for (auto& [c, body] : static_cast<const If&>(*s).branches) findSpawns(body);
                findSpawns(static_cast<const If&>(*s).orelse);
                break;
            case Stmt::K::While: findSpawns(static_cast<const While&>(*s).body); break;
            case Stmt::K::For: findSpawns(static_cast<const For&>(*s).body); break;
            case Stmt::K::Try:
                findSpawns(static_cast<const Try&>(*s).body);
                for (auto& h : static_cast<const Try&>(*s).handlers) findSpawns(h.body);
                break;
            default: break;
            }
        }
    }

    /// Evaluate @p e and return the register holding it. A local is used in
    /// place (no copy); anything else goes into a new temporary.
    unsigned operand(const Expr& e) {
        if (e.k == Expr::K::Name) {
            auto& n = static_cast<const Name&>(e);
            if (n.bind == Bind::Local) {
                line_ = e.line;
                return local(n.slot);
            }
        }
        unsigned t = temp();
        into(e, t);
        return t;
    }

    /// Can @p e be evaluated straight into a local it might also read? True
    /// when its code reads every operand before writing the destination.
    static bool safeInto(const Expr& e) {
        switch (e.k) {
        case Expr::K::Int: case Expr::K::Float: case Expr::K::Str: case Expr::K::Bytes: case Expr::K::Bool:
        case Expr::K::None: case Expr::K::Name: case Expr::K::Binary: case Expr::K::Unary: case Expr::K::Call:
        case Expr::K::Index: case Expr::K::Attr: case Expr::K::FStr: case Expr::K::List:
            return true;
        case Expr::K::Compare: return static_cast<const Compare&>(e).rest.size() == 1;
        default: return false;   // BoolOp, comparison chains, dicts, comprehensions write dst early
        }
    }

    /// Evaluate @p e into register @p dst.
    void into(const Expr& e, unsigned dst) {
        line_ = e.line;
        switch (e.k) {
        case Expr::K::Int: emit(ins::abx(OpCode::LOADK, dst, konst(Value(static_cast<const IntLit&>(e).v)))); return;
        case Expr::K::Float: emit(ins::abx(OpCode::LOADK, dst, konst(Value(static_cast<const FloatLit&>(e).v)))); return;
        case Expr::K::Str:
            emit(ins::abx(OpCode::LOADK, dst, konst(Value::borrowedStr(static_cast<const StrLit&>(e).str.get()))));
            return;
        case Expr::K::Bytes: emit(ins::abx(OpCode::LOADK, dst, konst(Value::bytes(static_cast<const BytesLit&>(e).v)))); return;
        case Expr::K::Bool:
            emit(ins::abc(static_cast<const BoolLit&>(e).v ? OpCode::LOADTRUE : OpCode::LOADFALSE, dst));
            return;
        case Expr::K::None: emit(ins::abc(OpCode::LOADNIL, dst)); return;

        case Expr::K::Name: {
            auto& n = static_cast<const Name&>(e);
            switch (n.bind) {
            case Bind::Local: emit(ins::abc(OpCode::MOVE, dst, local(n.slot))); return;
            case Bind::Global:
            case Bind::Function: emit(ins::abx(OpCode::GETG, dst, name(n.id))); return;
            case Bind::Builtin:
            case Bind::Exception: emit(ins::abx(OpCode::LOADK, dst, konst(builtins_(n.id)))); return;
            case Bind::Unresolved: break;
            }
            throw CannotCompile{};
        }

        case Expr::K::FStr: {
            auto& fs = static_cast<const FStr&>(e);
            if (fs.parts.empty()) {
                emit(ins::abx(OpCode::LOADK, dst, konst(Value::str(""))));
                return;
            }
            if (fs.parts.size() > 255) throw CannotCompile{};
            // Reserve every part's register first: compiling a part may take
            // temporaries of its own, and CONCAT needs the parts consecutive.
            unsigned base = top_;
            for (size_t i = 0; i < fs.parts.size(); ++i) temp();
            for (size_t i = 0; i < fs.parts.size(); ++i) {
                auto& part = fs.parts[i];
                unsigned r = base + static_cast<unsigned>(i);
                if (!part.expr) {
                    line_ = e.line;
                    emit(ins::abx(OpCode::LOADK, r, konst(Value::str(part.text))));
                } else {
                    into(*part.expr, r);
                    line_ = e.line;
                    emit(ins::abx(OpCode::FORMAT, r, name(part.spec)));
                }
            }
            line_ = e.line;
            emit(ins::abc(OpCode::CONCAT, dst, base, static_cast<unsigned>(fs.parts.size())));
            return;
        }

        case Expr::K::Unary: {
            auto& u = static_cast<const Unary&>(e);
            unsigned s = operand(*u.operand);
            line_ = e.line;
            OpCode op = u.code == Op::Not ? OpCode::NOT : u.code == Op::Neg ? OpCode::NEG
                      : u.code == Op::Pos ? OpCode::POS : OpCode::INV;
            emit(ins::abc(op, dst, s));
            return;
        }

        case Expr::K::Binary: {
            auto& b = static_cast<const Binary&>(e);
            unsigned l = operand(*b.lhs);
            unsigned r = operand(*b.rhs);
            line_ = e.line;
            emit(ins::abc(binOp(b.code), dst, l, r));
            return;
        }

        case Expr::K::BoolOp: {
            auto& b = static_cast<const BoolOp&>(e);
            into(*b.lhs, dst);
            line_ = e.line;
            size_t j = jump(b.code == Op::And ? OpCode::JMPIFNOT : OpCode::JMPIF, dst);
            Known before = known_;   // the right side may not run
            into(*b.rhs, dst);
            known_ = before;
            patch(j, here());
            return;
        }

        case Expr::K::Compare: {
            auto& c = static_cast<const Compare&>(e);
            if (c.rest.size() == 1) {
                unsigned l = operand(*c.first);
                unsigned r = operand(*c.rest[0].second);
                line_ = e.line;
                emit(ins::abc(cmpOp(c.ops[0]), dst, l, r));
                return;
            }
            // a < b < c: each operand once; stop at the first false comparison.
            unsigned tl = temp();
            into(*c.first, tl);
            Known always;   // after the first comparison, later operands may not run
            std::vector<size_t> exits;
            for (size_t i = 0; i < c.rest.size(); ++i) {
                unsigned tr = temp();
                into(*c.rest[i].second, tr);
                if (i == 0) always = known_;
                line_ = e.line;
                emit(ins::abc(cmpOp(c.ops[i]), dst, tl, tr));
                if (i + 1 < c.rest.size()) {
                    exits.push_back(jump(OpCode::JMPIFNOT, dst));
                    emit(ins::abc(OpCode::MOVE, tl, tr));
                }
            }
            for (size_t j : exits) patch(j, here());
            known_ = always;
            return;
        }

        case Expr::K::Call: call(static_cast<const Call&>(e), dst); return;

        case Expr::K::Index: {
            auto& ix = static_cast<const Index&>(e);
            if (ix.hasConstKey) {   // r["station"]: key precomputed
                into(*ix.obj, dst);
                Value key = ix.index->k == Expr::K::Str
                                ? Value::borrowedStr(static_cast<const StrLit&>(*ix.index).str.get())
                                : Value(static_cast<const IntLit&>(*ix.index).v);
                line_ = e.line;
                emit(ins::abx(OpCode::INDEXK, dst, konst(key, ix.constKey)));
                return;
            }
            if (ix.index->k == Expr::K::Slice) {
                auto& s = static_cast<const Slice&>(*ix.index);
                unsigned o = operand(*ix.obj);
                unsigned lo = temp(), hi = temp();   // consecutive: SLICE reads rC and r(C+1)
                if (s.lo) into(*s.lo, lo); else emit(ins::abc(OpCode::LOADNIL, lo));
                if (s.hi) into(*s.hi, hi); else emit(ins::abc(OpCode::LOADNIL, hi));
                line_ = e.line;
                emit(ins::abc(OpCode::SLICE, dst, o, lo));
                return;
            }
            unsigned o = operand(*ix.obj);
            unsigned k = operand(*ix.index);
            line_ = e.line;
            emit(ins::abc(OpCode::INDEX, dst, o, k));
            return;
        }

        case Expr::K::Attr: {
            auto& a = static_cast<const Attr&>(e);
            into(*a.obj, dst);
            line_ = e.line;
            emit(ins::abx(OpCode::ATTR, dst, name(a.name)));
            return;
        }

        case Expr::K::List: {
            auto& l = static_cast<const ListLit&>(e);
            if (l.elems.size() > 255) throw CannotCompile{};
            unsigned base = top_;   // consecutive, reserved before compiling (see FStr)
            for (size_t i = 0; i < l.elems.size(); ++i) temp();
            for (size_t i = 0; i < l.elems.size(); ++i) into(*l.elems[i], base + static_cast<unsigned>(i));
            line_ = e.line;
            emit(ins::abc(OpCode::NEWLIST, dst, base, static_cast<unsigned>(l.elems.size())));
            return;
        }

        case Expr::K::Dict: {
            auto& d = static_cast<const DictLit&>(e);
            emit(ins::abc(OpCode::NEWDICT, dst));
            for (auto& [kx, vx] : d.items) {
                unsigned k = operand(*kx);
                unsigned v = operand(*vx);
                line_ = e.line;
                emit(ins::abc(OpCode::DICTSET, dst, k, v));
            }
            return;
        }

        case Expr::K::ListComp: {
            auto& c = static_cast<const ListComp&>(e);
            Known before = known_;   // each form runs from here; the loop variable isn't known after
            if (const Def* pd = mapTarget(c)) {
                // Parallel form when there's a pool, the plain loop otherwise.
                size_t toSeq = jump(OpCode::JMPNOPOOL);
                parallelMap(c, *pd, dst);
                known_ = before;
                size_t done = jump(OpCode::JMP);
                patch(toSeq, here());
                sequentialComp(c, dst);
                patch(done, here());
                known_ = before;
                return;
            }
            sequentialComp(c, dst);
            known_ = before;
            return;
        }

        case Expr::K::Slice: throw CannotCompile{};   // only valid inside a subscript
        }
        throw CannotCompile{};
    }

    /// Could the tree-walker run this comprehension as a parallel map? (Interp::parallelComprehension's static tests.)
    const Def* mapTarget(const ListComp& c) const {
        const Def* d = spawnTarget(*c.elt);
        if (!d || !hooks_.reorderable) return nullptr;
        auto& call = static_cast<const Call&>(*c.elt);
        for (auto& a : call.args)
            if (!hooks_.reorderable(*a)) return nullptr;
        for (auto& [k, v] : call.kwargs)
            if (!hooks_.reorderable(*v)) return nullptr;
        if (c.cond && !hooks_.reorderable(*c.cond)) return nullptr;
        return d;
    }

    /// Collect every element's argument list (conditions and arguments in
    /// order, on this thread), then PMAP runs the calls.
    void parallelMap(const ListComp& c, const Def& d, unsigned dst) {
        auto& call = static_cast<const Call&>(*c.elt);
        size_t nargs = call.args.size() + call.kwargs.size();
        if (nargs > 255 || c.slot < 0) throw CannotCompile{};
        unsigned acc = temp();
        unsigned it = operand(*c.iter);
        unsigned slot = iterDepth_++;
        p_.niters = std::max(p_.niters, iterDepth_);
        line_ = c.line;
        emit(ins::abc(OpCode::ITER, slot, it));
        emit(ins::abc(OpCode::NEWLIST, acc, 0, 0));
        size_t loop = here();
        emit(ins::abc(OpCode::FORNEXT, static_cast<unsigned>(c.slot), slot));
        size_t exit = jump(OpCode::JMP);
        mark(static_cast<unsigned>(c.slot));
        if (c.cond) {
            std::vector<size_t> skip;
            condJump(*c.cond, false, skip);
            for (size_t j : skip) patch(j, loop);
        }
        unsigned base = top_;
        for (size_t i = 0; i < nargs; ++i) temp();
        size_t i = 0;
        for (auto& a : call.args) into(*a, base + static_cast<unsigned>(i++));
        for (auto& [k, v] : call.kwargs) into(*v, base + static_cast<unsigned>(i++));
        unsigned row = temp();
        line_ = c.line;
        emit(ins::abc(OpCode::NEWLIST, row, base, static_cast<unsigned>(nargs)));
        emit(ins::abc(OpCode::APPEND, acc, row));
        patch(jump(OpCode::JMP), loop);
        patch(exit, here());
        --iterDepth_;
        CallSite cs;
        cs.kind = CallSite::Kind::Function;
        cs.def = &d;
        cs.self = static_cast<uint8_t>(acc);
        for (auto& [k, v] : call.kwargs) cs.kwNames.push_back(k);
        cs.args.assign(nargs, 0);   // count only; the values are in each row
        emit(ins::abx(OpCode::PMAP, dst, callSite(std::move(cs))));
    }

    void sequentialComp(const ListComp& c, unsigned dst) {
        {
            unsigned it = operand(*c.iter);
            unsigned slot = iterDepth_++;
            p_.niters = std::max(p_.niters, iterDepth_);
            line_ = c.line;
            emit(ins::abc(OpCode::ITER, slot, it));
            emit(ins::abc(OpCode::NEWLIST, dst, 0, 0));
            if (c.slot < 0) throw CannotCompile{};
            size_t loop = here();
            emit(ins::abc(OpCode::FORNEXT, static_cast<unsigned>(c.slot), slot));
            size_t exit = jump(OpCode::JMP);
            mark(static_cast<unsigned>(c.slot));
            if (c.cond) {
                std::vector<size_t> skip;
                condJump(*c.cond, false, skip);
                for (size_t j : skip) patch(j, loop);
            }
            unsigned v = operand(*c.elt);
            line_ = c.line;
            emit(ins::abc(OpCode::APPEND, dst, v));
            patch(jump(OpCode::JMP), loop);
            patch(exit, here());
            --iterDepth_;
        }
    }

    static OpCode binOp(Op op) {
        return static_cast<OpCode>(static_cast<unsigned>(OpCode::ADD) + static_cast<unsigned>(op) - static_cast<unsigned>(Op::Add));
    }
    static OpCode cmpOp(Op op) {
        return static_cast<OpCode>(static_cast<unsigned>(OpCode::EQ) + static_cast<unsigned>(op) - static_cast<unsigned>(Op::Eq));
    }

    /**
     * Branch on a condition: emit code that jumps (to targets the caller
     * patches, listed in @p jumps) when @p e's truth equals @p when, and falls
     * through otherwise. With --opt 1, a comparison becomes one fused
     * compare-and-branch, and `and`/`or`/`not` become jump chains with no bool
     * values. Same evaluation order and short-circuiting as the tree-walker.
     */
    void condJump(const Expr& e, bool when, std::vector<size_t>& jumps) {
        if (hooks_.optimize) {
            if (e.k == Expr::K::Compare && static_cast<const Compare&>(e).rest.size() == 1) {
                auto& c = static_cast<const Compare&>(e);
                unsigned l = operand(*c.first);
                unsigned r = operand(*c.rest[0].second);
                line_ = e.line;
                OpCode op = static_cast<OpCode>(static_cast<unsigned>(OpCode::JEQ) + static_cast<unsigned>(c.ops[0]) -
                                                static_cast<unsigned>(Op::Eq));
                emit(ins::abc(op, l, r, when ? 1 : 0));
                jumps.push_back(jump(OpCode::JMP));
                return;
            }
            if (e.k == Expr::K::Unary && static_cast<const Unary&>(e).code == Op::Not) {
                condJump(*static_cast<const Unary&>(e).operand, !when, jumps);
                return;
            }
            if (e.k == Expr::K::BoolOp) {
                auto& b = static_cast<const BoolOp&>(e);
                bool isAnd = b.code == Op::And;
                // `a and b` is false if either is; `a or b` is true if either is.
                if (when != isAnd) {   // and/false, or/true: either side decides the jump
                    condJump(*b.lhs, when, jumps);
                    Known before = known_;
                    condJump(*b.rhs, when, jumps);
                    known_ = before;
                } else {               // and/true, or/false: the left side can rule it out
                    std::vector<size_t> skip;
                    condJump(*b.lhs, !when, skip);
                    Known before = known_;
                    condJump(*b.rhs, when, jumps);
                    known_ = before;
                    for (size_t j : skip) patch(j, here());
                }
                return;
            }
        }
        unsigned c = operand(e);
        jumps.push_back(jump(when ? OpCode::JMPIF : OpCode::JMPIFNOT, c));
    }

    /// A call: everything the tree-walker decides per call, decided here once.
    void call(const Call& c, unsigned dst, OpCode op = OpCode::CALL, bool discard = false) {
        CallSite cs;
        cs.discard = discard;
        if (c.fn->k == Expr::K::Name && static_cast<const Name&>(*c.fn).bind == Bind::Builtin) {
            auto& n = static_cast<const Name&>(*c.fn);
            cs.kind = CallSite::Kind::Builtin;
            cs.builtinValue = builtins_(n.id);
            cs.builtin = cs.builtinValue.as<std::shared_ptr<const Builtin>>().get();
        } else if (c.fn->k == Expr::K::Name && static_cast<const Name&>(*c.fn).bind == Bind::Function &&
                   static_cast<const Name&>(*c.fn).def) {
            auto& n = static_cast<const Name&>(*c.fn);
            cs.kind = CallSite::Kind::Function;
            cs.def = n.def;
            cs.name = n.id;
        } else if (c.fn->k == Expr::K::Attr && static_cast<const Attr&>(*c.fn).method != Method::None) {
            auto& at = static_cast<const Attr&>(*c.fn);
            cs.kind = CallSite::Kind::Method;
            cs.method = at.method;
            cs.name = at.name;
            cs.self = static_cast<uint8_t>(operand(*at.obj));   // receiver first, then arguments
        } else {
            cs.kind = CallSite::Kind::Value;
            cs.self = static_cast<uint8_t>(operand(*c.fn));
        }
        for (auto& a : c.args) cs.args.push_back(static_cast<uint8_t>(operand(*a)));
        for (auto& [k, v] : c.kwargs) {
            cs.args.push_back(static_cast<uint8_t>(operand(*v)));
            cs.kwNames.push_back(k);
        }
        line_ = c.line;
        emit(ins::abx(op, dst, callSite(std::move(cs))));
    }

    /// A SPAWN's call site names the parallel-safe callee directly (Function kind),
    /// whether it's a module function or a nested one in a local.
    void spawn(const Call& c, const Def& d, unsigned dst, bool discard) {
        CallSite cs;
        cs.kind = CallSite::Kind::Function;
        cs.def = &d;
        cs.name = static_cast<const Name&>(*c.fn).bind == Bind::Function ? static_cast<const Name&>(*c.fn).id : "";
        cs.discard = discard;
        for (auto& a : c.args) cs.args.push_back(static_cast<uint8_t>(operand(*a)));
        for (auto& [k, v] : c.kwargs) {
            cs.args.push_back(static_cast<uint8_t>(operand(*v)));
            cs.kwNames.push_back(k);
        }
        line_ = c.line;
        emit(ins::abx(OpCode::SPAWN, dst, callSite(std::move(cs))));
    }

    // ----------------------------------------------------------- statements

    void block(const Block& b) {
        for (auto& s : b) stmt(*s);
    }

    /// Leave regions down to (not including) the innermost loop, or all of them
    /// for a return: end exception handling, pass join points.
    void unwind(bool toLoop) {
        for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
            if (toLoop && it->k == Region::K::Loop) return;
            if (it->k == Region::K::TryBody) emit(ins::abc(OpCode::SETTLE, 0));
            if (it->k == Region::K::Handler) emit(ins::abc(OpCode::HPOP, 0));
        }
        if (toLoop) throw CannotCompile{};   // break/continue outside a loop: the parser rejects it
    }
    Region& innermostLoop() {
        for (auto it = regions_.rbegin(); it != regions_.rend(); ++it)
            if (it->k == Region::K::Loop) return *it;
        throw CannotCompile{};
    }

    void stmt(const Stmt& s) {
        unsigned savedTop = top_;
        line_ = s.line;
        switch (s.k) {
        case Stmt::K::Expr: {
            auto& es = static_cast<const ExprStmt&>(s);
            if (const Def* sd = spawnable(s)) spawn(static_cast<const Call&>(*es.e), *sd, temp(), true);
            else into(*es.e, temp());
            break;
        }

        case Stmt::K::Assign: {
            auto& a = static_cast<const Assign&>(s);
            if (const Def* sd = spawnable(s)) {   // x = f(...): x may become a Future
                unsigned slot = static_cast<unsigned>(static_cast<const Name&>(*a.target).slot);
                spawn(static_cast<const Call&>(*a.value), *sd, slot, false);
                mark(slot);
                break;
            }
            assign(*a.target, *a.value);
            break;
        }

        case Stmt::K::AugAssign: {
            auto& a = static_cast<const AugAssign&>(s);
            if (a.target->k == Expr::K::Name) {
                auto& n = static_cast<const Name&>(*a.target);
                if (n.bind != Bind::Local) throw CannotCompile{};
                unsigned slot = local(n.slot);   // unassigned: UnboundLocalError before the value is evaluated
                unsigned r = operand(*a.value);
                line_ = s.line;
                emit(ins::abc(binOp(a.code), slot, slot, r));
            } else if (a.target->k == Expr::K::Index) {
                // As the tree-walker: read the target, compute, then assign to the target
                // (its object and index are evaluated again).
                auto& ix = static_cast<const Index&>(*a.target);
                if (ix.index->k == Expr::K::Slice) throw CannotCompile{};
                unsigned cur = temp();
                into(*a.target, cur);
                unsigned r = operand(*a.value);
                line_ = s.line;
                emit(ins::abc(binOp(a.code), cur, cur, r));
                storeIndex(ix, cur);
            } else {
                throw CannotCompile{};
            }
            break;
        }

        case Stmt::K::If: {
            auto& i = static_cast<const If&>(s);
            std::vector<size_t> ends;
            Known out{{}, false};   // merge of every branch's end state
            for (auto& [cond, body] : i.branches) {
                unsigned t = top_;
                std::vector<size_t> next;
                condJump(*cond, false, next);
                Known afterCond = known_;   // what the false path (next branch) also knows
                top_ = t;
                block(body);
                out = merge(out, known_);
                if (known_.live) ends.push_back(jump(OpCode::JMP));
                known_ = afterCond;
                for (size_t j : next) patch(j, here());
            }
            block(i.orelse);
            known_ = merge(out, known_);
            for (size_t j : ends) patch(j, here());
            break;
        }

        case Stmt::K::While: {
            auto& w = static_cast<const While&>(s);
            size_t loop = here();
            std::vector<size_t> exits;
            condJump(*w.cond, false, exits);
            Known afterCond = known_;   // the exit state: the body may never run
            top_ = savedTop;
            regions_.push_back(Region{Region::K::Loop, loop, {}, {}});
            block(w.body);
            known_ = afterCond;
            Region r = std::move(regions_.back());
            regions_.pop_back();
            patch(jump(OpCode::JMP), loop);
            for (size_t j : exits) patch(j, here());
            for (size_t j : r.breaks) patch(j, here());
            for (size_t j : r.continues) patch(j, loop);
            break;
        }

        case Stmt::K::For: {
            auto& f = static_cast<const For&>(s);
            if (f.slot < 0) throw CannotCompile{};
            unsigned it = operand(*f.iter);
            unsigned slot = iterDepth_++;
            p_.niters = std::max(p_.niters, iterDepth_);
            line_ = s.line;
            emit(ins::abc(OpCode::ITER, slot, it));
            top_ = savedTop;
            size_t loop = here();
            emit(ins::abc(OpCode::FORNEXT, static_cast<unsigned>(f.slot), slot));
            size_t exit = jump(OpCode::JMP);
            Known afterIter = known_;   // the exit state: the body may never run
            mark(static_cast<unsigned>(f.slot));
            regions_.push_back(Region{Region::K::Loop, loop, {}, {}});
            block(f.body);
            known_ = afterIter;
            Region r = std::move(regions_.back());
            regions_.pop_back();
            patch(jump(OpCode::JMP), loop);
            patch(exit, here());
            for (size_t j : r.breaks) patch(j, here());
            for (size_t j : r.continues) patch(j, loop);
            --iterDepth_;
            break;
        }

        case Stmt::K::Def: {
            auto& nd = static_cast<const Def&>(s);
            if (nd.slot < 0) throw CannotCompile{};
            emit(ins::abx(OpCode::LOADK, static_cast<unsigned>(nd.slot),
                          konst(Value(std::make_shared<const Function>(Function{&nd})))));
            mark(static_cast<unsigned>(nd.slot));
            break;
        }

        case Stmt::K::Return: {
            auto& r = static_cast<const Return&>(s);
            if (r.value) {
                unsigned v = operand(*r.value);   // the value first, then leave try/handler regions
                line_ = s.line;
                unwind(false);
                emit(ins::abc(OpCode::RETURN, v));
            } else {
                unwind(false);
                emit(ins::abc(OpCode::RETURNNIL, 0));
            }
            known_.live = false;   // what follows in this block never runs
            break;
        }

        case Stmt::K::Break: {
            unwind(true);
            innermostLoop().breaks.push_back(jump(OpCode::JMP));
            known_.live = false;
            break;
        }
        case Stmt::K::Continue: {
            unwind(true);
            innermostLoop().continues.push_back(jump(OpCode::JMP));
            known_.live = false;
            break;
        }
        case Stmt::K::Pass: break;

        case Stmt::K::Try: tryStmt(static_cast<const Try&>(s)); break;

        case Stmt::K::Raise: {
            auto& r = static_cast<const Raise&>(s);
            if (r.value) {
                unsigned v = operand(*r.value);
                line_ = s.line;
                emit(ins::abc(OpCode::RAISE, v));
            } else {
                emit(ins::abc(OpCode::RERAISE, 0));
            }
            known_.live = false;
            break;
        }
        }
        top_ = savedTop;
    }

    void assign(const Expr& target, const Expr& value) {
        if (target.k == Expr::K::Name) {
            auto& n = static_cast<const Name&>(target);
            if (n.bind != Bind::Local || n.slot < 0) throw CannotCompile{};
            unsigned slot = static_cast<unsigned>(n.slot);
            if (safeInto(value)) {
                into(value, slot);
            } else {
                unsigned t = operand(value);
                emit(ins::abc(OpCode::MOVE, slot, t));
            }
            mark(slot);
            return;
        }
        if (target.k == Expr::K::Index) {
            auto& ix = static_cast<const Index&>(target);
            if (ix.index->k == Expr::K::Slice) throw CannotCompile{};   // tree-walker raises the TypeError
            unsigned v = operand(value);   // the value first, then the target's object and index
            storeIndex(ix, v);
            return;
        }
        throw CannotCompile{};   // attribute assignment: tree-walker raises the AttributeError
    }

    void storeIndex(const Index& ix, unsigned v) {
        unsigned o = operand(*ix.obj);
        unsigned k = operand(*ix.index);
        line_ = ix.line;
        emit(ins::abc(OpCode::SETINDEX, o, k, v));
    }

    void tryStmt(const Try& t) {
        int tryLine = line_;
        emit(ins::abc(OpCode::SETTLE, 0));   // join point: earlier failures aren't this try's to catch
        size_t start = here();
        Known atEntry = known_;   // a handler may start from any point in the body
        regions_.push_back(Region{Region::K::TryBody, 0, {}, {}});
        block(t.body);
        Known out = known_;
        regions_.pop_back();
        line_ = tryLine;
        emit(ins::abc(OpCode::SETTLE, 0));   // join point inside the range: failures from the body are caught
        size_t end = here();
        std::vector<size_t> done{jump(OpCode::JMP)};
        size_t handler = here();
        p_.excTable.push_back(ExcEntry{static_cast<uint32_t>(start), static_cast<uint32_t>(end),
                                       static_cast<uint32_t>(handler), false});
        for (auto& h : t.handlers) {
            line_ = h.line;
            size_t next = SIZE_MAX;
            if (!h.cls.empty()) {
                emit(ins::abx(OpCode::EXCMATCH, 0, name(h.cls)));
                next = jump(OpCode::JMP);
            }
            known_ = atEntry;
            if (!h.as.empty()) {
                if (h.asSlot < 0) throw CannotCompile{};
                emit(ins::abc(OpCode::EXCBIND, static_cast<unsigned>(h.asSlot)));
                mark(static_cast<unsigned>(h.asSlot));
            }
            emit(ins::abc(OpCode::HPUSH, 0));
            size_t bodyStart = here();
            regions_.push_back(Region{Region::K::Handler, 0, {}, {}});
            block(h.body);
            regions_.pop_back();
            size_t bodyEnd = here();
            p_.excTable.push_back(ExcEntry{static_cast<uint32_t>(bodyStart), static_cast<uint32_t>(bodyEnd), 0, true});
            out = merge(out, known_);
            emit(ins::abc(OpCode::HPOP, 0));
            done.push_back(jump(OpCode::JMP));
            if (next != SIZE_MAX) patch(next, here());
        }
        line_ = tryLine;
        emit(ins::abc(OpCode::RETHROW, 0));
        for (size_t j : done) patch(j, here());
        known_ = out;
    }
};

} // namespace

std::unique_ptr<PCode> compileFunction(const Def& d, const CompileHooks& hooks) {
    auto p = std::make_unique<PCode>();
    try {
        Compiler(d, hooks, *p).run();
    } catch (const CannotCompile&) {
        return nullptr;
    }
    p->globalCache = std::make_unique<std::atomic<const Value*>[]>(p->names.size());
    for (size_t i = 0; i < p->names.size(); ++i) p->globalCache[i].store(nullptr);
    p->calleeBound = std::make_unique<std::atomic<bool>[]>(p->calls.size());
    for (size_t i = 0; i < p->calls.size(); ++i) p->calleeBound[i].store(false);
    return p;
}

std::string disassemble(const PCode& p) {
    std::ostringstream o;
    o << p.def->name << "  params " << p.nparams << "  regs " << p.nregs << "  iters " << p.niters << "  consts "
      << p.consts.size() << "  calls " << p.calls.size() << "\n";
    auto reg = [&](unsigned r) {
        std::string s = "r" + std::to_string(r);
        if (r < p.regNames.size() && !p.regNames[r].empty()) s += "(" + p.regNames[r] + ")";
        return s;
    };
    for (size_t pc = 0; pc < p.code.size(); ++pc) {
        uint32_t w = p.code[pc];
        OpCode op = ins::op(w);
        char head[48];
        std::snprintf(head, sizeof head, "%5zu  %4d  %-9s ", pc, p.lines[pc], opName(op));
        o << head;
        unsigned a = ins::a(w), b = ins::b(w), c = ins::c(w), bx = ins::bx(w);
        int target = static_cast<int>(pc) + 1 + ins::sbx(w);
        switch (op) {
        case OpCode::LOADK: o << reg(a) << " K" << bx << " " << toRepr(p.consts[bx]); break;
        case OpCode::GETG: case OpCode::ATTR: o << reg(a) << " " << p.names[bx]; break;
        case OpCode::FORMAT: o << reg(a) << " '" << p.names[bx] << "'"; break;
        case OpCode::INDEXK: o << reg(a) << " [" << toRepr(p.consts[bx]) << "]"; break;
        case OpCode::EXCMATCH: o << p.names[bx]; break;
        case OpCode::JMP: o << "-> " << target; break;
        case OpCode::JMPIF: case OpCode::JMPIFNOT: o << reg(a) << " -> " << target; break;
        case OpCode::JMPNOPOOL: o << "-> " << target; break;
        case OpCode::JEQ: case OpCode::JNE: case OpCode::JLT: case OpCode::JLE: case OpCode::JGT: case OpCode::JGE:
        case OpCode::JIN: case OpCode::JNOTIN: {
            int t2 = static_cast<int>(pc) + 2 + ins::sbx(p.code[pc + 1]);   // the next word holds the offset
            o << reg(a) << " " << reg(b) << (c ? "  if true" : "  if false") << " -> " << t2;
            break;
        }
        case OpCode::FORCE: o << reg(a); break;
        case OpCode::PMAP: o << reg(a) << " = map " << p.calls[bx].def->name << " over " << reg(p.calls[bx].self); break;
        case OpCode::SPAWN:
        case OpCode::CALL: {
            const CallSite& cs = p.calls[bx];
            o << reg(a) << " = ";
            switch (cs.kind) {
            case CallSite::Kind::Builtin: o << cs.builtin->name; break;
            case CallSite::Kind::Function: o << (cs.name.empty() ? cs.def->name : cs.name); break;
            case CallSite::Kind::Method: o << reg(cs.self) << "." << cs.name; break;
            case CallSite::Kind::Value: o << reg(cs.self); break;
            }
            o << "(";
            size_t npos = cs.args.size() - cs.kwNames.size();
            for (size_t i = 0; i < cs.args.size(); ++i) {
                if (i) o << ", ";
                if (i >= npos) o << cs.kwNames[i - npos] << "=";
                o << reg(cs.args[i]);
            }
            o << ")";
            break;
        }
        case OpCode::LOADNIL: case OpCode::LOADTRUE: case OpCode::LOADFALSE: case OpCode::CHKB:
        case OpCode::RETURN: case OpCode::RAISE: case OpCode::EXCBIND: case OpCode::NEWDICT:
            o << reg(a);
            break;
        case OpCode::RETURNNIL: case OpCode::RERAISE: case OpCode::HPUSH: case OpCode::HPOP:
        case OpCode::RETHROW: case OpCode::SETTLE:
            break;
        case OpCode::ITER: o << "it" << a << " " << reg(b); break;
        case OpCode::FORNEXT: o << reg(a) << " it" << b; break;
        case OpCode::NEWLIST: case OpCode::CONCAT: o << reg(a) << " " << reg(b) << ".." << c; break;
        case OpCode::MOVE: case OpCode::NOT: case OpCode::NEG: case OpCode::POS: case OpCode::INV:
        case OpCode::APPEND:
            o << reg(a) << " " << reg(b);
            break;
        default: o << reg(a) << " " << reg(b) << " " << reg(c); break;
        }
        o << "\n";
    }
    for (auto& e : p.excTable)
        o << "  " << (e.handlerBody ? "handler body " : "try ") << e.start << ".." << e.end
          << (e.handlerBody ? "" : " -> " + std::to_string(e.handler)) << "\n";
    return o.str();
}

} // namespace dolphin
