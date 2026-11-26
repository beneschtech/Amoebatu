/**
 * @file verify.cpp
 * @brief Verifier for loaded `.dcl` programs (doc/PCODE.md §7).
 *
 * A loaded file may be malformed by accident or by design. Three passes per
 * function, then one over the whole program:
 *
 * 1. **Structure.** Every opcode is known; every register, constant, name,
 *    call site and iterator index is in range; every jump lands inside the
 *    code; instructions that read a following word (fused compare-and-branch,
 *    FORNEXT, EXCMATCH) have a JMP there; no path falls off the end; exception
 *    table ranges are inside the code; SETG appears only in module code; SPAWN
 *    and PMAP name a function. After this the interpreter can't index out of
 *    any array.
 * 2. **Fresh containers.** APPEND and DICTSET skip the barrier and type check
 *    that ordinary mutation has, because the compiler only emits them on a list
 *    or dict the function has just created (comprehensions, dict displays).
 *    A forward dataflow pass proves that: their target must hold a container
 *    made by NEWLIST/NEWDICT on every path, not read since by anything that
 *    could let it escape. PMAP's argument rows are checked the same way.
 * 3. **Analysis, recomputed.** Whether a function may run on another thread
 *    depends on its needs (effects) and on whether it mutates its arguments.
 *    A file states both, but a file could claim an IO function is pure. The
 *    verifier recomputes them from the code, mirroring the source analyzer
 *    (analyzer.cpp) on registers, and keeps the more conservative of claim and
 *    result. So a file's claims can make a function less parallel, never more.
 *
 * Set DOLPHIN_DCL_TRACE=1 to log functions whose recomputed analysis is more
 * conservative than their claim (a precision gap that costs parallelism).
 */
#include <bitset>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>

#include "dcl.hpp"
#include "stream.hpp"
#include "value.hpp"

namespace dolphin {

namespace {

[[noreturn]] void fail(const Def& d, size_t pc, const std::string& why) {
    throw DclError("verify: " + d.name + " at " + std::to_string(pc) + ": " + why);
}
[[noreturn]] void fail(const Def& d, const std::string& why) { throw DclError("verify: " + d.name + ": " + why); }

/// Registers an instruction reads and writes, for the dataflow passes.
struct Effects {
    std::vector<unsigned> reads;
    std::vector<unsigned> writes;
};

/// Instructions that read the word after them (which must be a JMP).
bool readsNextWord(OpCode op) {
    return op == OpCode::FORNEXT || op == OpCode::EXCMATCH || (op >= OpCode::JEQ && op <= OpCode::JNOTIN);
}
/// Instructions after which execution never falls through.
bool isTerminator(OpCode op) {
    return op == OpCode::RETURN || op == OpCode::RETURNNIL || op == OpCode::RAISE || op == OpCode::RERAISE ||
           op == OpCode::RETHROW || op == OpCode::JMP;
}

class FunctionVerifier {
public:
    FunctionVerifier(const Def& d, PCode& p, bool module) : d_(d), p_(p), module_(module), n_(p.code.size()) {}

    void run() {
        structure();
        freshContainers();
    }

    /// Successors of instruction @p pc (valid after structure()).
    std::vector<size_t> successors(size_t pc) const {
        uint32_t w = p_.code[pc];
        OpCode op = ins::op(w);
        std::vector<size_t> s;
        if (op == OpCode::JMP) {
            s.push_back(target(pc));
        } else if (op == OpCode::JMPIF || op == OpCode::JMPIFNOT || op == OpCode::JMPNOPOOL) {
            s.push_back(pc + 1);
            s.push_back(target(pc));
        } else if (readsNextWord(op)) {
            s.push_back(pc + 1);   // the JMP word (taken or exit)
            s.push_back(pc + 2);   // skipped
        } else if (!isTerminator(op)) {
            s.push_back(pc + 1);
        }
        return s;
    }

    /// Registers read and written by instruction @p pc (valid after structure()).
    Effects effects(size_t pc) const {
        uint32_t w = p_.code[pc];
        unsigned a = ins::a(w), b = ins::b(w), c = ins::c(w);
        Effects e;
        auto range = [&](unsigned base, unsigned cnt) {
            for (unsigned i = 0; i < cnt; ++i) e.reads.push_back(base + i);
        };
        switch (ins::op(w)) {
        case OpCode::MOVE: e.reads = {b}; e.writes = {a}; break;
        case OpCode::LOADK: case OpCode::LOADNIL: case OpCode::LOADTRUE: case OpCode::LOADFALSE:
        case OpCode::GETG: case OpCode::NEWDICT: case OpCode::EXCBIND:
            e.writes = {a};
            break;
        case OpCode::CHKB: case OpCode::JMPIF: case OpCode::JMPIFNOT: case OpCode::RETURN: case OpCode::RAISE:
        case OpCode::SETG:
            e.reads = {a};
            break;
        case OpCode::FORCE: e.reads = {a}; e.writes = {a}; break;
        case OpCode::NOT: case OpCode::NEG: case OpCode::POS: case OpCode::INV: e.reads = {b}; e.writes = {a}; break;
        case OpCode::ITER: e.reads = {b}; break;
        case OpCode::FORNEXT: e.writes = {a}; break;
        case OpCode::NEWLIST: range(b, c); e.writes = {a}; break;
        case OpCode::APPEND: e.reads = {a, b}; break;
        case OpCode::DICTSET: e.reads = {a, b, c}; break;
        case OpCode::INDEX: e.reads = {b, c}; e.writes = {a}; break;
        case OpCode::INDEXK: case OpCode::ATTR: case OpCode::FORMAT: e.reads = {a}; e.writes = {a}; break;
        case OpCode::SLICE: e.reads = {b, c, c + 1}; e.writes = {a}; break;
        case OpCode::SETINDEX: e.reads = {a, b, c}; break;
        case OpCode::CONCAT: range(b, c); e.writes = {a}; break;
        case OpCode::CALL: case OpCode::SPAWN: {
            const CallSite& cs = p_.calls[ins::bx(w)];
            if (cs.kind == CallSite::Kind::Method || cs.kind == CallSite::Kind::Value) e.reads.push_back(cs.self);
            for (uint8_t r : cs.args) e.reads.push_back(r);
            e.writes = {a};
            break;
        }
        case OpCode::PMAP: e.reads = {p_.calls[ins::bx(w)].self}; e.writes = {a}; break;
        case OpCode::JEQ: case OpCode::JNE: case OpCode::JLT: case OpCode::JLE: case OpCode::JGT: case OpCode::JGE:
        case OpCode::JIN: case OpCode::JNOTIN:
            e.reads = {a, b};
            break;
        default:
            if (ins::op(w) >= OpCode::ADD && ins::op(w) <= OpCode::NOTIN) {
                e.reads = {b, c};
                e.writes = {a};
            }
            break;   // JMP, RETURNNIL, RERAISE, EXCMATCH, HPUSH, HPOP, RETHROW, SETTLE, JMPNOPOOL
        }
        return e;
    }

    /// Handler entry points (exception table), reached from anywhere in their range.
    std::vector<size_t> handlers() const {
        std::vector<size_t> h;
        for (const ExcEntry& x : p_.excTable)
            if (!x.handlerBody) h.push_back(x.handler);
        return h;
    }

private:
    size_t target(size_t pc) const { return static_cast<size_t>(static_cast<long>(pc) + 1 + ins::sbx(p_.code[pc])); }

    void reg(size_t pc, unsigned r) const {
        if (r >= p_.nregs) fail(d_, pc, "register r" + std::to_string(r) + " out of range");
    }
    void jumpTo(size_t pc, long t) const {
        if (t < 0 || static_cast<size_t>(t) >= n_) fail(d_, pc, "jump outside the code");
    }

    // ------------------------------------------------------------- structure
    void structure() {
        if (p_.lines.size() != n_) fail(d_, "line table doesn't match the code");
        // Call sites, once each.
        for (size_t i = 0; i < p_.calls.size(); ++i) {
            const CallSite& cs = p_.calls[i];
            for (uint8_t r : cs.args)
                if (r >= p_.nregs) fail(d_, "call site " + std::to_string(i) + ": register out of range");
            if ((cs.kind == CallSite::Kind::Method || cs.kind == CallSite::Kind::Value) && cs.self >= p_.nregs)
                fail(d_, "call site " + std::to_string(i) + ": register out of range");
            if (cs.kind == CallSite::Kind::Function && !cs.def) fail(d_, "call site " + std::to_string(i) + ": no function");
            if (cs.kwNames.size() > cs.args.size()) fail(d_, "call site " + std::to_string(i) + ": bad keyword count");
        }
        for (size_t pc = 0; pc < n_; ++pc) {
            uint32_t w = p_.code[pc];
            if (static_cast<unsigned>(ins::op(w)) > static_cast<unsigned>(OpCode::SETG)) fail(d_, pc, "unknown opcode");
            OpCode op = ins::op(w);
            unsigned a = ins::a(w), b = ins::b(w), c = ins::c(w), bx = ins::bx(w);
            auto konst = [&] {
                if (bx >= p_.consts.size()) fail(d_, pc, "constant index out of range");
            };
            auto name = [&] {
                if (bx >= p_.names.size()) fail(d_, pc, "name index out of range");
            };
            auto site = [&]() -> const CallSite& {
                if (bx >= p_.calls.size()) fail(d_, pc, "call site index out of range");
                return p_.calls[bx];
            };
            switch (op) {
            case OpCode::MOVE: reg(pc, a); reg(pc, b); break;
            case OpCode::LOADK: reg(pc, a); konst(); break;
            case OpCode::LOADNIL: case OpCode::LOADTRUE: case OpCode::LOADFALSE: case OpCode::CHKB:
            case OpCode::NEWDICT: case OpCode::RETURN: case OpCode::RAISE: case OpCode::EXCBIND: case OpCode::FORCE:
                reg(pc, a);
                break;
            case OpCode::GETG: case OpCode::ATTR: case OpCode::FORMAT: reg(pc, a); name(); break;
            case OpCode::SETG:
                if (!module_) fail(d_, pc, "SETG outside module code");
                reg(pc, a);
                name();
                break;
            case OpCode::NOT: case OpCode::NEG: case OpCode::POS: case OpCode::INV: reg(pc, a); reg(pc, b); break;
            case OpCode::JMP: case OpCode::JMPNOPOOL: jumpTo(pc, static_cast<long>(target(pc))); break;
            case OpCode::JMPIF: case OpCode::JMPIFNOT: reg(pc, a); jumpTo(pc, static_cast<long>(target(pc))); break;
            case OpCode::ITER:
                if (a >= p_.niters) fail(d_, pc, "iterator slot out of range");
                reg(pc, b);
                break;
            case OpCode::FORNEXT:
                reg(pc, a);
                if (b >= p_.niters) fail(d_, pc, "iterator slot out of range");
                break;
            case OpCode::NEWLIST: case OpCode::CONCAT:
                reg(pc, a);
                if (b + c > p_.nregs) fail(d_, pc, "register range out of range");
                break;
            case OpCode::APPEND: reg(pc, a); reg(pc, b); break;
            case OpCode::DICTSET: case OpCode::INDEX: case OpCode::SETINDEX: reg(pc, a); reg(pc, b); reg(pc, c); break;
            case OpCode::INDEXK:
                reg(pc, a);
                konst();
                if (p_.constKeys[bx].empty()) fail(d_, pc, "INDEXK constant has no key");
                break;
            case OpCode::SLICE:
                reg(pc, a);
                reg(pc, b);
                if (c + 1 >= p_.nregs) fail(d_, pc, "register out of range");
                break;
            case OpCode::CALL: reg(pc, a); site(); break;
            case OpCode::SPAWN:
                reg(pc, a);
                if (site().kind != CallSite::Kind::Function) fail(d_, pc, "SPAWN of something other than a function");
                break;
            case OpCode::PMAP: {
                reg(pc, a);
                const CallSite& cs = site();
                if (cs.kind != CallSite::Kind::Function) fail(d_, pc, "PMAP of something other than a function");
                if (cs.self >= p_.nregs) fail(d_, pc, "register out of range");
                break;
            }
            case OpCode::EXCMATCH: name(); break;
            case OpCode::RETURNNIL: case OpCode::RERAISE: case OpCode::HPUSH: case OpCode::HPOP: case OpCode::RETHROW:
            case OpCode::SETTLE:
                break;
            default:
                if (op >= OpCode::ADD && op <= OpCode::NOTIN) {
                    reg(pc, a);
                    reg(pc, b);
                    reg(pc, c);
                } else if (op >= OpCode::JEQ && op <= OpCode::JNOTIN) {
                    reg(pc, a);
                    reg(pc, b);
                    if (c > 1) fail(d_, pc, "bad branch sense");
                } else {
                    fail(d_, pc, "unknown opcode");
                }
            }
            if (readsNextWord(op) && (pc + 2 >= n_ + 1 || pc + 1 >= n_ || ins::op(p_.code[pc + 1]) != OpCode::JMP))
                fail(d_, pc, "must be followed by a JMP");
            if (readsNextWord(op) && pc + 2 >= n_) fail(d_, pc, "falls off the end of the code");
            if (!isTerminator(op) && !readsNextWord(op) && pc + 1 >= n_) fail(d_, pc, "falls off the end of the code");
        }
        for (const ExcEntry& x : p_.excTable) {
            if (x.start > x.end || x.end > n_) fail(d_, "exception range out of bounds");
            if (!x.handlerBody && x.handler >= n_) fail(d_, "exception handler out of bounds");
        }
    }

    // ------------------------------------------------------------- fresh containers
    enum Fresh : uint8_t { kOther = 0, kList = 1, kDict = 2 };

    void freshContainers() {
        using State = std::vector<uint8_t>;
        std::vector<State> in(n_);
        std::vector<bool> seen(n_, false);
        std::vector<size_t> work;
        auto join = [&](size_t pc, const State& s) {
            if (!seen[pc]) {
                in[pc] = s;
                seen[pc] = true;
                work.push_back(pc);
                return;
            }
            bool changed = false;
            for (size_t r = 0; r < s.size(); ++r)
                if (in[pc][r] != s[r] && in[pc][r] != kOther) {
                    in[pc][r] = kOther;
                    changed = true;
                }
            if (changed) work.push_back(pc);
        };
        State none(p_.nregs, kOther);
        join(0, none);
        for (size_t h : handlers()) join(h, none);   // reached from anywhere in the range: assume nothing
        while (!work.empty()) {
            size_t pc = work.back();
            work.pop_back();
            State s = in[pc];
            uint32_t w = p_.code[pc];
            OpCode op = ins::op(w);
            unsigned a = ins::a(w);
            if (op == OpCode::APPEND) {
                if (s[a] != kList) fail(d_, pc, "APPEND to a list this function didn't just create");
                unsigned b = ins::b(w);
                s[b] = kOther;   // the element escapes into the list
                if (b == a) s[a] = kOther;
            } else if (op == OpCode::DICTSET) {
                if (s[a] != kDict) fail(d_, pc, "DICTSET on a dict this function didn't just create");
                s[ins::b(w)] = kOther;
                s[ins::c(w)] = kOther;
                if (ins::b(w) == a || ins::c(w) == a) s[a] = kOther;
            } else if (op == OpCode::PMAP) {
                unsigned self = p_.calls[ins::bx(w)].self;
                if (s[self] != kList) fail(d_, pc, "PMAP over rows this function didn't just build");
                s[self] = kOther;
                s[a] = kOther;
            } else {
                Effects e = effects(pc);
                for (unsigned r : e.reads) s[r] = kOther;   // anything else that reads it could let it escape
                for (unsigned r : e.writes) s[r] = kOther;
                if (op == OpCode::NEWLIST) s[a] = kList;
                if (op == OpCode::NEWDICT) s[a] = kDict;
            }
            for (size_t nx : successors(pc)) join(nx, s);
        }
    }

    const Def& d_;
    PCode& p_;
    bool module_;
    size_t n_;
};

// ===================================================================== analysis
//
// Mirrors analyzer.cpp on registers. Needs: builtins' declared needs, plus
// callees' (fixpoint over the call graph); a call through an unknown value is
// the worst case. Mutation: each mutation site (SETINDEX, append/extend/pop,
// passing an argument to a callee that mutates that parameter) "touches" a
// register; the touched object is traced to its root: a parameter (that
// parameter is mutated), a local whose every assignment creates a new object
// (harmless), or anything else (may mutate something the caller can see).

class Analysis {
public:
    explicit Analysis(Program& p) : p_(p) {
        for (size_t i = 0; i < p.defs.size(); ++i) index_[p.defs[i].get()] = i;
    }

    void run(bool trace) {
        size_t n = p_.defs.size();
        // Facts per function: which registers hold a known function (nested defs called by name).
        knownFn_.resize(n);
        writers_.resize(n);
        leader_.resize(n);
        for (size_t i = 0; i < n; ++i) scanWrites(i);

        // ---- needs
        std::vector<unsigned> needs(n, NeedNone);
        std::vector<std::set<size_t>> callees(n);
        for (size_t i = 0; i < n; ++i) {
            const PCode& pc = *p_.code[i];
            for (size_t k = 0; k < pc.code.size(); ++k) {
                OpCode op = ins::op(pc.code[k]);
                if (op != OpCode::CALL && op != OpCode::SPAWN && op != OpCode::PMAP) continue;
                const CallSite& cs = pc.calls[ins::bx(pc.code[k])];
                switch (cs.kind) {
                case CallSite::Kind::Builtin:
                    needs[i] |= cs.builtin->name == "Stream" ? streamCtorNeeds(i, k) : cs.builtin->needs;
                    break;
                case CallSite::Kind::Function: callees[i].insert(index_.at(cs.def)); break;
                case CallSite::Kind::Method:   // methods of builtin types are pure, except Stream's
                    if (isStreamMethod(cs.method)) needs[i] |= streamRegNeeds(i, k, cs.self);
                    if (isListenerMethod(cs.method)) needs[i] |= NeedNetwork;
                    if (isPathIOMethod(cs.method) || isArchiveMethod(cs.method)) needs[i] |= NeedIO;
                    break;
                case CallSite::Kind::Value: {
                    const Def* f = nullptr;
                    if (!knownCallee(i, k, cs.self, f)) needs[i] |= NeedAll;   // unknown callee: worst case
                    else if (f) callees[i].insert(index_.at(f));                // else an exception class: pure
                    break;
                }
                }
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 0; i < n; ++i) {
                unsigned v = needs[i];
                for (size_t c : callees[i]) v |= needs[c];
                if (v != needs[i]) {
                    needs[i] = v;
                    changed = true;
                }
            }
        }

        // ---- mutation
        fresh_.resize(n);
        for (size_t i = 0; i < n; ++i) freshLocals(i);
        mutated_.assign(n, {});
        other_.assign(n, false);
        for (size_t i = 0; i < n; ++i) mutated_[i].assign(p_.defs[i]->params.size(), false);
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 0; i < n; ++i) changed = sites(i) || changed;
        }

        // ---- combine with the claims: the more conservative wins
        for (size_t i = 0; i < n; ++i) {
            Def& d = *p_.defs[i];
            d.mutatesArgs = d.mutatesOther;   // from the claims as loaded
            for (bool m : d.mutatedParams) d.mutatesArgs = d.mutatesArgs || m;
            bool claimSafe = d.parallelSafe();
            if (trace && (needs[i] & ~d.needs)) traceGap(d, "needs");
            if (trace && other_[i] && !d.mutatesOther) traceGap(d, "mutates something other than its parameters");
            d.needs |= needs[i];
            d.mutatesOther = d.mutatesOther || other_[i];
            for (size_t k = 0; k < d.params.size(); ++k) {
                if (trace && mutated_[i][k] && !d.mutatedParams[k]) traceGap(d, "mutates parameter " + d.params[k].name);
                d.mutatedParams[k] = d.mutatedParams[k] || mutated_[i][k];
            }
            d.mutatesArgs = d.mutatesOther;
            for (bool m : d.mutatedParams) d.mutatesArgs = d.mutatesArgs || m;
            if (trace && claimSafe && !d.parallelSafe())
                std::cerr << "dcl verify: " << d.name << " claimed parallel-safe, recomputed: not\n";
        }
    }

private:
    static void traceGap(const Def& d, const std::string& what) {
        std::cerr << "dcl verify: " << d.name << ": recomputed analysis is more conservative (" << what << ")\n";
    }

    /// Does instruction @p w write register ins::a(w)? (FORCE keeps the same logical value.)
    static bool writesA(uint32_t w) {
        switch (ins::op(w)) {
        case OpCode::MOVE: case OpCode::LOADK: case OpCode::LOADNIL: case OpCode::LOADTRUE: case OpCode::LOADFALSE:
        case OpCode::GETG: case OpCode::NEWDICT: case OpCode::EXCBIND: case OpCode::NOT: case OpCode::NEG:
        case OpCode::POS: case OpCode::INV: case OpCode::FORNEXT: case OpCode::NEWLIST: case OpCode::INDEX:
        case OpCode::INDEXK: case OpCode::ATTR: case OpCode::FORMAT: case OpCode::SLICE: case OpCode::CONCAT:
        case OpCode::CALL: case OpCode::SPAWN: case OpCode::PMAP:
            return true;
        default: return ins::op(w) >= OpCode::ADD && ins::op(w) <= OpCode::NOTIN;
        }
    }

    /// The instruction that last wrote register @p r before @p pc in the same
    /// basic block, or SIZE_MAX if control may have entered the block from
    /// elsewhere first (then only the flow-insensitive summary applies).
    size_t defBefore(size_t i, size_t pc, unsigned r) const {
        const PCode& p = *p_.code[i];
        while (pc > 0 && !leader_[i][pc]) {
            --pc;
            if (writesA(p.code[pc]) && ins::a(p.code[pc]) == r) return pc;
        }
        return SIZE_MAX;
    }

    /// Instructions writing each register; a register written only by LOADK of one function holds that function.
    void scanWrites(size_t i) {
        const PCode& pc = *p_.code[i];
        // Basic-block leaders: entry, handlers, and every target other than plain fall-through.
        leader_[i].assign(pc.code.size(), false);
        leader_[i][0] = true;
        FunctionVerifier fv(*p_.defs[i], *p_.code[i], false);
        for (size_t h : fv.handlers()) leader_[i][h] = true;
        for (size_t k = 0; k < pc.code.size(); ++k)
            for (size_t nx : fv.successors(k))
                if (nx != k + 1 && nx < pc.code.size()) leader_[i][nx] = true;
        writers_[i].assign(pc.nregs, {});
        knownFn_[i].assign(pc.nregs, nullptr);
        std::vector<int> fnState(pc.nregs, 0);   // 0 unwritten, 1 one function, 2 anything else
        for (size_t k = 0; k < pc.code.size(); ++k) {
            uint32_t w = pc.code[k];
            OpCode op = ins::op(w);
            unsigned a = ins::a(w);
            if (!writesA(w)) continue;
            writers_[i][a].push_back(k);
            const Def* f = nullptr;
            if (op == OpCode::LOADK) {
                const Value& v = pc.consts[ins::bx(w)];
                if (v.is<std::shared_ptr<const Function>>()) f = v.as<std::shared_ptr<const Function>>()->def;
            }
            if (f && (fnState[a] == 0 || (fnState[a] == 1 && knownFn_[i][a] == f))) {
                fnState[a] = 1;
                knownFn_[i][a] = f;
            } else {
                fnState[a] = 2;
                knownFn_[i][a] = nullptr;
            }
        }
    }
    static constexpr unsigned kAnyStream = NeedIO | NeedNetwork;

    /// Needs of the `Stream(...)` call at @p k: exact when its argument is a string constant
    /// (analyzer.cpp streamCtorNeeds()).
    unsigned streamCtorNeeds(size_t i, size_t k) const {
        const PCode& p = *p_.code[i];
        const CallSite& cs = p.calls[ins::bx(p.code[k])];
        if (cs.args.size() != 1 || !cs.kwNames.empty()) return kAnyStream;
        size_t w = defBefore(i, k, cs.args[0]);
        if (w == SIZE_MAX || ins::op(p.code[w]) != OpCode::LOADK) return kAnyStream;
        const Value& v = p.consts[ins::bx(p.code[w])];
        return v.isStr() ? Stream::needsFor(v.s().s) : kAnyStream;
    }
    /// Needs of a Stream method on register @p r at @p pc: one kind if every value that can
    /// reach it comes from `Stream(constant)` of that kind (directly or through MOVEs).
    unsigned streamRegNeeds(size_t i, size_t pc, unsigned r, int depth = 0) const {
        if (depth > 8) return kAnyStream;
        const PCode& p = *p_.code[i];
        auto from = [&](size_t w) -> unsigned {
            uint32_t ins = p.code[w];
            if (ins::op(ins) == OpCode::MOVE) return streamRegNeeds(i, w, ins::b(ins), depth + 1);
            if (ins::op(ins) != OpCode::CALL) return kAnyStream;
            const CallSite& cs = p.calls[ins::bx(ins)];
            if (cs.kind != CallSite::Kind::Builtin) return kAnyStream;
            if (cs.builtin->name == "Listener") return NeedNetwork;
            if (cs.builtin->name == "Archive") return NeedIO;
            return cs.builtin->name == "Stream" ? streamCtorNeeds(i, w) : kAnyStream;
        };
        if (size_t w = defBefore(i, pc, r); w != SIZE_MAX) return from(w);
        if (r < p_.defs[i]->params.size() || writers_[i][r].empty()) return kAnyStream;   // the caller's, or unknown
        unsigned k = 0;
        for (size_t w : writers_[i][r]) {
            unsigned n = from(w);
            if (k && n != k) return kAnyStream;
            k = n;
        }
        return k;
    }

    /// What the callee register of a Value call at @p pc holds, if known:
    /// a function (@p fn set) or an exception class (returns true with fn null).
    bool knownCallee(size_t i, size_t pc, unsigned r, const Def*& fn) const {
        fn = nullptr;
        const PCode& p = *p_.code[i];
        size_t k = defBefore(i, pc, r);
        if (k != SIZE_MAX) {
            uint32_t w = p.code[k];
            if (ins::op(w) == OpCode::LOADK) {
                const Value& v = p.consts[ins::bx(w)];
                if (v.is<std::shared_ptr<const Function>>()) {
                    fn = v.as<std::shared_ptr<const Function>>()->def;
                    return true;
                }
                return v.is<std::shared_ptr<const ExcClass>>();
            }
            if (ins::op(w) != OpCode::MOVE) return false;
            r = ins::b(w);   // a copy: what the source register holds overall
        }
        if ((fn = knownFn_[i][r])) return true;
        return excClassReg(i, r);
    }

    /// Is register @p r only ever an exception class constant (e.g. `ValueError("...")`)?
    bool excClassReg(size_t i, unsigned r) const {
        const PCode& pc = *p_.code[i];
        if (writers_[i][r].empty()) return false;
        for (size_t k : writers_[i][r]) {
            uint32_t w = pc.code[k];
            if (ins::op(w) != OpCode::LOADK || !pc.consts[ins::bx(w)].is<std::shared_ptr<const ExcClass>>()) return false;
        }
        return true;
    }

    /// Does instruction @p k of function @p i produce a new object (analyzer.cpp isFresh)?
    bool freshWrite(size_t i, size_t k, int depth = 0) const {
        const PCode& pc = *p_.code[i];
        uint32_t w = pc.code[k];
        OpCode op = ins::op(w);
        switch (op) {
        case OpCode::LOADK: case OpCode::LOADNIL: case OpCode::LOADTRUE: case OpCode::LOADFALSE: case OpCode::NEWLIST:
        case OpCode::NEWDICT: case OpCode::CONCAT: case OpCode::FORMAT: case OpCode::SLICE: case OpCode::PMAP:
        case OpCode::NOT: case OpCode::NEG: case OpCode::POS: case OpCode::INV: case OpCode::EXCBIND:
            return true;
        case OpCode::CALL: {
            const CallSite& cs = pc.calls[ins::bx(w)];
            if (cs.kind == CallSite::Kind::Builtin) return true;
            if (cs.kind == CallSite::Kind::Method) return cs.method != Method::pop && cs.method != Method::get && cs.method != Method::None;
            return false;   // a user function may return one of its arguments
        }
        case OpCode::MOVE: {   // a copy of a temporary that holds a new object
            unsigned b = ins::b(w);
            if (b < static_cast<unsigned>(p_.defs[i]->numLocals) || depth > 8) return false;   // a local: an alias
            if (size_t k2 = defBefore(i, k, b); k2 != SIZE_MAX) return freshWrite(i, k2, depth + 1);
            if (writers_[i][b].empty()) return false;
            for (size_t k2 : writers_[i][b])
                if (!freshWrite(i, k2, depth + 1)) return false;
            return true;
        }
        default: return op >= OpCode::ADD && op <= OpCode::NOTIN;
        }
    }

    /// Locals (not parameters) all of whose assignments create new objects.
    void freshLocals(size_t i) {
        const Def& d = *p_.defs[i];
        fresh_[i].assign(static_cast<size_t>(d.numLocals), true);
        for (size_t r = 0; r < static_cast<size_t>(d.numLocals); ++r) {
            if (r < d.params.size()) {
                fresh_[i][r] = false;   // the caller's objects
                continue;
            }
            for (size_t k : writers_[i][r])
                if (!freshWrite(i, k)) fresh_[i][r] = false;
        }
    }

    /// Record that the object in register @p r of function @p i, as it is
    /// just before instruction @p pc, may be mutated: trace it to its root.
    void touch(size_t i, size_t pc, unsigned r, int depth = 0) {
        const Def& d = *p_.defs[i];
        const PCode& p = *p_.code[i];
        if (r < static_cast<unsigned>(d.numLocals)) {
            if (r < d.params.size()) mutated_[i][r] = true;
            else if (!fresh_[i][r]) other_[i] = true;
            return;
        }
        if (depth > 16) {
            other_[i] = true;
            return;
        }
        if (size_t k = defBefore(i, pc, r); k != SIZE_MAX) {   // the writer in this block
            touchWrite(i, k, depth);
            return;
        }
        // Entered from elsewhere: every value the temporary may hold.
        if (writers_[i][r].empty()) {
            other_[i] = true;
            return;
        }
        for (size_t k : writers_[i][r]) {
            OpCode op = ins::op(p.code[k]);
            if (op == OpCode::INDEXK || op == OpCode::ATTR) continue;   // its container is another writer of r
            touchWrite(i, k, depth);
        }
    }
    /// Touch the object written by instruction @p k.
    void touchWrite(size_t i, size_t k, int depth) {
        uint32_t w = p_.code[i]->code[k];
        switch (ins::op(w)) {
        case OpCode::MOVE: touch(i, k, ins::b(w), depth + 1); break;
        case OpCode::INDEX: touch(i, k, ins::b(w), depth + 1); break;   // an element: root is the container
        case OpCode::INDEXK: case OpCode::ATTR: touch(i, k, ins::a(w), depth + 1); break;   // rA = rA[k]
        default:
            if (!freshWrite(i, k)) other_[i] = true;
        }
    }

    /// Mutation sites of function @p i; returns whether anything new was found.
    bool sites(size_t i) {
        auto before = mutated_[i];
        bool otherBefore = other_[i];
        const PCode& pc = *p_.code[i];
        for (size_t k = 0; k < pc.code.size(); ++k) {
            uint32_t w = pc.code[k];
            OpCode op = ins::op(w);
            if (op == OpCode::SETINDEX) {
                touch(i, k, ins::a(w));
                continue;
            }
            if (op != OpCode::CALL && op != OpCode::SPAWN && op != OpCode::PMAP) continue;
            const CallSite& cs = pc.calls[ins::bx(w)];
            if (op == OpCode::PMAP) {   // arguments are in rows: can't trace them
                size_t c = index_.at(cs.def);
                bool any = other_[c];
                for (bool m : mutated_[c]) any = any || m;
                if (any || p_.defs[c]->mutatesArgs) other_[i] = true;
                continue;
            }
            switch (cs.kind) {
            case CallSite::Kind::Builtin: break;
            case CallSite::Kind::Method:
                if (cs.method == Method::append || cs.method == Method::extend || cs.method == Method::pop) touch(i, k, cs.self);
                break;
            case CallSite::Kind::Function: callArgs(i, k, cs, index_.at(cs.def)); break;
            case CallSite::Kind::Value: {
                const Def* f = nullptr;
                if (!knownCallee(i, k, cs.self, f)) {
                    for (uint8_t r : cs.args) touch(i, k, r);   // unknown callee: assume the worst
                    touch(i, k, cs.self);                       // a bound method mutates its receiver
                } else if (f) {
                    callArgs(i, k, cs, index_.at(f));
                }
                break;
            }
            }
        }
        return mutated_[i] != before || other_[i] != otherBefore;
    }
    void callArgs(size_t i, size_t pc, const CallSite& cs, size_t c) {
        const Def& callee = *p_.defs[c];
        bool all = other_[c] || callee.mutatesOther;
        size_t npos = cs.args.size() - cs.kwNames.size();
        for (size_t a = 0; a < npos; ++a)
            if (all || (a < mutated_[c].size() && (mutated_[c][a] || callee.mutatedParams[a]))) touch(i, pc, cs.args[a]);
        for (size_t a = npos; a < cs.args.size(); ++a) {
            const std::string& k = cs.kwNames[a - npos];
            for (size_t pi = 0; pi < callee.params.size(); ++pi)
                if (callee.params[pi].name == k && (all || mutated_[c][pi] || callee.mutatedParams[pi])) touch(i, pc, cs.args[a]);
        }
    }

    Program& p_;
    std::unordered_map<const Def*, size_t> index_;
    std::vector<std::vector<const Def*>> knownFn_;
    std::vector<std::vector<std::vector<size_t>>> writers_;
    std::vector<std::vector<bool>> leader_;   ///< Per function: basic-block leaders.
    std::vector<std::vector<bool>> fresh_;
    std::vector<std::vector<bool>> mutated_;
    std::vector<bool> other_;
};

} // namespace

void verifyProgram(Program& p) {
    for (size_t i = 0; i < p.defs.size(); ++i) {
        if (p.defs[i]->numLocals > static_cast<int>(p.code[i]->nregs)) throw DclError("verify: " + p.defs[i]->name + ": bad local count");
        FunctionVerifier(*p.defs[i], *p.code[i], false).run();
    }
    FunctionVerifier(*p.moduleDef, *p.init, true).run();
    const char* t = std::getenv("DOLPHIN_DCL_TRACE");
    Analysis(p).run(t && *t && *t != '0');
}

} // namespace dolphin
