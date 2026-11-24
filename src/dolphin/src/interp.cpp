/**
 * @file interp.cpp
 * @brief Interpreter core: statements, expressions, operators, calls.
 *
 * Builtin functions and methods live in builtins.cpp.
 *
 * Control flow: `return`, `break` and `continue` propagate as Flow values
 * returned up through execBlock(), not as C++ exceptions, because they're
 * common and cheap. Dolphin exceptions do use C++ exceptions (DolphinError),
 * because they must unwind any number of C++ frames to the nearest `try`.
 */
#include "interp.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <thread>

namespace dolphin {

/// Maximum Dolphin call depth before RecursionError. Same default as CPython.
/// It guards the C++ stack, since each Dolphin call uses several C++ frames.
static constexpr int kMaxDepth = 1000;


// Per-thread interpreter state. A pool thread that helps with another task
// while waiting saves and restores these around it (runAsTask()).
thread_local Interp::TaskCtx* Interp::tlCtx_ = nullptr;   // current task's frame stack
static thread_local int tl_depth = 0;                    // Dolphin call depth (RecursionError)
// Cost model inputs. tl_ops counts operations (statements and expressions
// evaluated) on this thread; tl_waited records whether the current call has
// blocked waiting for another thread (then it's not a clean rate sample).
static thread_local uint64_t tl_ops = 0;
static thread_local bool tl_waited = false;
static thread_local std::vector<DolphinError> tl_handling;   // exceptions being handled, for bare `raise`

Interp::Interp(const Module& m, unsigned threads, bool asyncOutput) : mod_(m) {
    installBuiltins();
    if (asyncOutput) out_ = std::make_unique<OutputChannel>(stdout);
    // threads == 0: automatic. Create a worker for every CPU we may use, start
    // with the topology's suggestion active, and let the pool adapt to load.
    // An explicit count is fixed (reproducible benchmarks).
    if (threads == 0) {
        topo_ = Topology::detect();
        if (topo_.hardLimit > 1) {
            // Starting point: what an earlier run found useful on this machine
            // (shared profile), else the topology's guess; then no more than
            // a fair share given the Dolphin programs already running.
            auto shared = std::make_unique<SharedProfile>();
            if (shared->ok()) {
                // The learned figure only lowers the start (a quota, NUMA, modules
                // that share units); growth above the start happens within the run.
                if (unsigned learned = shared->learnedCeiling(); learned && learned < topo_.start) {
                    topo_.start = learned;
                    topo_.basis = "learned by earlier runs";
                }
                SharedProfile::Others o = shared->others();
                unsigned fair = std::max(1u, topo_.hardLimit / (o.processes + 1));
                unsigned free = o.threads < topo_.hardLimit ? topo_.hardLimit - o.threads : 0;
                unsigned cap = std::max(fair, free);
                if (cap < topo_.start) {
                    topo_.start = cap;
                    topo_.basis += ", fair share with " + std::to_string(o.processes) + " other program(s)";
                }
                shared->publish(topo_.start);
            } else {
                shared.reset();
            }
            pool_ = std::make_unique<ThreadPool>(topo_.hardLimit - 1, topo_.start - 1, true);   // main is the other thread
            if (shared) pool_->adoptShared(std::move(shared));
        }
    } else if (threads > 1) {
        pool_ = std::make_unique<ThreadPool>(threads - 1);
    }
    if (pool_) {
        pool_->setTimingSinks([this](uint64_t ns) { cost_.recordSubmit(ns); },
                              [this](uint64_t ns) { cost_.recordWake(ns); });
        pool_->setTraceSink([this](const std::string& m) { if (trace_) trace(m); });
    }
    findOverlap(m.body);
}

namespace {
/// Does @p s mention the name @p id anywhere (reads, writes, nested blocks)?
bool mentions(const Stmt& s, const std::string& id);
bool mentions(const Expr& e, const std::string& id) {
    switch (e.k) {
    case Expr::K::Name: return static_cast<const Name&>(e).id == id;
    case Expr::K::FStr:
        for (auto& p : static_cast<const FStr&>(e).parts)
            if (p.expr && mentions(*p.expr, id)) return true;
        return false;
    case Expr::K::Unary: return mentions(*static_cast<const Unary&>(e).operand, id);
    case Expr::K::Binary: return mentions(*static_cast<const Binary&>(e).lhs, id) || mentions(*static_cast<const Binary&>(e).rhs, id);
    case Expr::K::BoolOp: return mentions(*static_cast<const BoolOp&>(e).lhs, id) || mentions(*static_cast<const BoolOp&>(e).rhs, id);
    case Expr::K::Compare: {
        auto& c = static_cast<const Compare&>(e);
        if (mentions(*c.first, id)) return true;
        for (auto& [op, r] : c.rest)
            if (mentions(*r, id)) return true;
        return false;
    }
    case Expr::K::Call: {
        auto& c = static_cast<const Call&>(e);
        if (mentions(*c.fn, id)) return true;
        for (auto& a : c.args)
            if (mentions(*a, id)) return true;
        for (auto& [k, v] : c.kwargs)
            if (mentions(*v, id)) return true;
        return false;
    }
    case Expr::K::Index: return mentions(*static_cast<const Index&>(e).obj, id) || mentions(*static_cast<const Index&>(e).index, id);
    case Expr::K::Slice: {
        auto& s = static_cast<const Slice&>(e);
        return (s.lo && mentions(*s.lo, id)) || (s.hi && mentions(*s.hi, id));
    }
    case Expr::K::Attr: return mentions(*static_cast<const Attr&>(e).obj, id);
    case Expr::K::List:
        for (auto& x : static_cast<const ListLit&>(e).elems)
            if (mentions(*x, id)) return true;
        return false;
    case Expr::K::Dict:
        for (auto& [k, v] : static_cast<const DictLit&>(e).items)
            if (mentions(*k, id) || mentions(*v, id)) return true;
        return false;
    case Expr::K::ListComp: {
        auto& c = static_cast<const ListComp&>(e);
        return c.var == id || mentions(*c.iter, id) || mentions(*c.elt, id) || (c.cond && mentions(*c.cond, id));
    }
    default: return false;
    }
}
bool mentions(const Block& b, const std::string& id) {
    for (auto& s : b)
        if (mentions(*s, id)) return true;
    return false;
}
bool mentions(const Stmt& s, const std::string& id) {
    switch (s.k) {
    case Stmt::K::Expr: return mentions(*static_cast<const ExprStmt&>(s).e, id);
    case Stmt::K::Assign: return mentions(*static_cast<const Assign&>(s).target, id) || mentions(*static_cast<const Assign&>(s).value, id);
    case Stmt::K::AugAssign: return mentions(*static_cast<const AugAssign&>(s).target, id) || mentions(*static_cast<const AugAssign&>(s).value, id);
    case Stmt::K::If:
        for (auto& [c, b] : static_cast<const If&>(s).branches)
            if (mentions(*c, id) || mentions(b, id)) return true;
        return mentions(static_cast<const If&>(s).orelse, id);
    case Stmt::K::While: return mentions(*static_cast<const While&>(s).cond, id) || mentions(static_cast<const While&>(s).body, id);
    case Stmt::K::For: {
        auto& f = static_cast<const For&>(s);
        return f.var == id || mentions(*f.iter, id) || mentions(f.body, id);
    }
    case Stmt::K::Return: return static_cast<const Return&>(s).value && mentions(*static_cast<const Return&>(s).value, id);
    case Stmt::K::Raise: return static_cast<const Raise&>(s).value && mentions(*static_cast<const Raise&>(s).value, id);
    case Stmt::K::Try: {
        auto& t = static_cast<const Try&>(s);
        if (mentions(t.body, id)) return true;
        for (auto& h : t.handlers)
            if (h.as == id || mentions(h.body, id)) return true;
        return false;
    }
    default: return false;   // Def, pass, break, continue
    }
}
} // namespace

// Scan ahead (statically, once): `x = f(...)` is worth dispatching only if the
// statement after it doesn't touch x, so the caller has something to do while
// f runs. When the very next statement reads x, dispatching would just add a
// queue round trip before the same work happens on this thread anyway.
void Interp::findOverlap(const Block& b) {
    for (size_t i = 0; i < b.size(); ++i) {
        const Stmt& s = *b[i];
        if (s.k == Stmt::K::Assign) {
            auto& a = static_cast<const Assign&>(s);
            if (a.target->k == Expr::K::Name && a.value->k == Expr::K::Call && i + 1 < b.size() &&
                !mentions(*b[i + 1], static_cast<const Name&>(*a.target).id))
                overlapAssigns_.insert(&s);
        }
        if (s.k == Stmt::K::Expr && i + 1 < b.size()) overlapAssigns_.insert(&s);   // `f(x)` statement: always independent
        switch (s.k) {
        case Stmt::K::If:
            for (auto& [c, body] : static_cast<const If&>(s).branches) findOverlap(body);
            findOverlap(static_cast<const If&>(s).orelse);
            break;
        case Stmt::K::While: findOverlap(static_cast<const While&>(s).body); break;
        case Stmt::K::For: findOverlap(static_cast<const For&>(s).body); break;
        case Stmt::K::Def: {
            auto& d = static_cast<const Def&>(s);
            if (d.parallelSafe()) cost_.track(&d);
            findOverlap(d.body);
            break;
        }
        case Stmt::K::Try:
            findOverlap(static_cast<const Try&>(s).body);
            for (auto& h : static_cast<const Try&>(s).handlers) findOverlap(h.body);
            break;
        default: break;
        }
    }
}

// Stop the pool first: abandoned (detached) calls may still be running, and
// they use members declared after pool_ (the cost table) that would otherwise
// be destroyed before them. out_ drains and joins its own thread.
Interp::~Interp() { pool_.reset(); }

void Interp::emit(std::string text) {
    if (out_) {
        out_->write(std::move(text));
    } else {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
    }
}

void Interp::flushOutput() {
    if (out_) out_->drain();
}

void Interp::raise(const std::string& cls, const std::string& msg, int line) const {
    throw DolphinError{cls, msg, line};
}

int Interp::run(const std::vector<std::string>& args) {
    TaskCtx mainTask;
    tlCtx_ = &mainTask;
    Frame g(&globals_, true);
    mainTask.frames.push_back(&g);
    Value ret;
    try {
        execBlock(mod_.body, g, ret);
        auto it = globals_.find("main");
        int rc = 0;
        if (it != globals_.end() && it->second.is<std::shared_ptr<const Function>>()) {
            Args a;
            for (auto& s : args) a.push_back(Value::str(s));
            Kwargs kw;
            Value r = callValue(it->second, a, kw, 0);
            if (r.isInt()) rc = static_cast<int>(r.as<int64_t>());
        }
        flushOutput();
        return rc;
    } catch (const DolphinError& e) {
        flushOutput();   // everything printed before the error appears before its message
        std::cerr << mod_.file << ":" << e.line << ": " << e.cls;
        if (!e.msg.empty()) std::cerr << ": " << e.msg;
        std::cerr << "\n";
        return 1;
    } catch (const DeferredFailure&) {   // can't happen: every target frame is below the module frame
        flushOutput();
        std::cerr << mod_.file << ": internal error: unhandled deferred failure\n";
        return 70;
    }
}

// ======================================================= parallel execution
//
// See the overview in interp.hpp. Invariants that make this race-free:
//  1. Only parallel-safe functions run on the pool. They have no needs and
//     only mutate objects they create, so a task never writes anything
//     another thread can see.
//  2. A thread never mutates a list/dict, or performs an effect, while it
//     has calls in flight (barrier()). So nothing a task might read changes
//     under it.
//  3. Module-level code never dispatches, and every function joins its own
//     calls before returning, so globals are only written while nothing is
//     in flight.

bool Interp::worthDispatching(const Def& d) const {
    return cost_.predictNs(&d) >= cost_.callThresholdNs();   // unmeasured (-1) runs inline first
}

void Interp::absorbOps(Future& fut) { tl_ops += fut.ops.exchange(0, std::memory_order_acq_rel); }

void Interp::trace(const std::string& msg) const {
    if (!trace_) return;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    std::fprintf(stderr, "trace: [%s] %s\n", tlCtx_ && tlCtx_->frames.size() && tlCtx_->frames[0]->isModule ? "main" : "task", msg.c_str());
}

FutureP Interp::tryDispatch(const Def& d, Args& args, Kwargs& kw, int line) {
    double pred = cost_.predictNs(&d);
    auto where = d.name + "() line " + std::to_string(line);
    if (pred < 0) {
        if (trace_) trace(where + ": inline (never measured; this call measures it)");
        return nullptr;
    }
    bool probe = false;
    if (pred < cost_.callThresholdNs() && !(probe = cost_.shouldProbe(pred))) {
        if (trace_) {
            char buf[200];
            std::snprintf(buf, sizeof buf, ": inline (predicted %.1f us = %.0f ops / %.1f Mops/s < %.0f x (submit %.1f + wake %.1f us))",
                          pred / 1e3, cost_.avgOps(&d), cost_.rate() * 1e3, CostModel::kCallFactor, cost_.submitNs() / 1e3,
                          cost_.wakeNs() / 1e3);
            trace(where + buf);
        }
        return nullptr;
    }
    FutureP fut = dispatch(d, args, kw, line);
    if (trace_) trace(where + (fut ? (probe ? ": probe dispatch (re-measuring wake latency) as seq " : ": dispatched as seq ") + std::to_string(fut->seq)
                                   : ": inline (pool busy)"));
    return fut;
}

const Def* Interp::dispatchTarget(const Call& c, const Frame& f) const {
    if (!pool_ || f.isModule || c.fn->k != Expr::K::Name) return nullptr;
    const Def* d = static_cast<const Name&>(*c.fn).def;
    return d && d->parallelSafe() ? d : nullptr;
}

void Interp::evalArgs(const Call& c, Frame& f, Args& args, Kwargs& kw) {
    args.reserve(c.args.size());
    for (auto& a : c.args) args.push_back(eval(*a, f));
    for (auto& [k, v] : c.kwargs) kw.emplace_back(k, eval(*v, f));
}

uint64_t Interp::runAsTask(const std::function<void()>& body) {
    TaskCtx task;
    TaskCtx* savedCtx = tlCtx_;
    int savedDepth = tl_depth;
    uint64_t savedOps = tl_ops;
    bool savedWaited = tl_waited;
    std::vector<DolphinError> savedHandling;
    savedHandling.swap(tl_handling);
    tlCtx_ = &task;
    tl_depth = 0;
    tl_ops = 0;
    tl_waited = false;
    body();
    uint64_t ops = tl_ops;
    tlCtx_ = savedCtx;
    tl_depth = savedDepth;
    tl_ops = savedOps;
    tl_waited = savedWaited;
    tl_handling.swap(savedHandling);
    return ops;
}

FutureP Interp::dispatch(const Def& d, Args args, Kwargs kw, int line) {
    auto fut = std::make_shared<Future>();
    fut->seq = ++tlCtx_->nextSeq;
    fut->line = line;
    const Def* def = &d;
    bool accepted = pool_->submit([this, def, args = std::move(args), kw = std::move(kw), fut, line]() mutable {
        if (fut->cancelled()) {   // abandoned before it started (decision 17)
            fut->fail(DolphinError{"Cancelled", "", line});
            return;
        }
        // Outcome first, then record the work, then complete the future: a
        // caller joining it must find the operation count already there.
        Value result;
        std::unique_ptr<DolphinError> err;
        uint64_t ops = runAsTask([&] {
            try {
                result = callFunction(*def, args, kw, line);
            } catch (const DolphinError& e) {
                err = std::make_unique<DolphinError>(e);
                if (trace_) trace("seq " + std::to_string(fut->seq) + " (" + def->name + ") failed: " + e.cls + " " + e.msg);
            } catch (const DeferredFailure&) {   // can't happen: parallel-safe code has no barriers that defer
                err = std::make_unique<DolphinError>(DolphinError{"RuntimeError", "internal: deferred failure escaped a task", line});
            } catch (const std::exception& e) {
                err = std::make_unique<DolphinError>(DolphinError{"RuntimeError", std::string("internal: ") + e.what(), line});
            }
        });
        fut->ops = ops;
        pool_->noteOps(ops);
        if (err) fut->fail(*err);
        else fut->resolve(std::move(result));
    }, fut.get());
    if (!accepted) {
        inlineCalls_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    return fut;
}

void Interp::addPending(Frame& f, FutureP fut, const std::string& name) {
    Pending p;
    p.seq = fut->seq;
    p.fut = fut;
    p.name = name;
    if (!name.empty()) {
        auto it = f.vars->find(name);
        if (it != f.vars->end()) {
            p.prev = it->second;
            p.hadPrev = true;
        }
        (*f.vars)[name] = Value(fut);
    }
    f.pending.push_back(std::move(p));
    ++tlCtx_->pendingCount;
}

void Interp::waitFuture(Future& fut) {
    if (!fut.ready()) tl_waited = true;   // this call's time now includes waiting
    if (pool_) pool_->wait(fut);
    absorbOps(fut);
}

FutureP Interp::waitAllOrFirstFailure(const std::vector<FutureP>& futs) {
    for (;;) {
        for (auto& fu : futs) {   // finished ones: their work counts toward this call
            if (fu->ready()) absorbOps(*fu);
            else tl_waited = true;
        }
        FutureP first;
        bool allDone = true;
        for (auto& fu : futs) {
            if (!fu->ready()) {
                allDone = false;
            } else if (fu->failed() && (!first || fu->doneAt() < first->doneAt())) {
                first = fu;
            }
        }
        if (first || allDone) return first;
        // Run one of *our* calls that's still queued, if any; otherwise doze on
        // one that's running elsewhere. Never pick up unrelated work (see ThreadPool).
        bool ranOwn = false;
        for (auto& fu : futs)
            if (!fu->ready() && pool_ && pool_->runOwn(fu.get())) {
                ranOwn = true;
                break;
            }
        if (ranOwn) continue;
        for (auto& fu : futs)
            if (!fu->ready()) {
                fu->waitFor(200);
                break;
            }
    }
}

// Decision 17: the failure that returned first wins. To keep single-failure
// programs exactly sequential, calls *before* the failed one in program order
// still finish (they'd have run first sequentially), and their assignments
// stand. Calls from the failed one on never "happened": they're cancelled,
// and whatever they were assigning gets its previous value back.
DolphinError Interp::abandonFrame(Frame& f, uint64_t failSeq, DolphinError err) {
    if (trace_) trace("abandoning frame: failure " + err.msg + (failSeq == UINT64_MAX ? std::string(" (raised inline)") : " from seq " + std::to_string(failSeq)));
    uint64_t errAt = err.at ? err.at : Future::tick();
    std::vector<Pending> pending;
    pending.swap(f.pending);
    tlCtx_->pendingCount -= pending.size();

    for (auto& p : pending) {
        if (p.seq < failSeq) waitFuture(*p.fut);   // earlier: sequentially it completed
        else if (!p.fut->ready()) {                // later: cancel; track it until it stops
            p.fut->cancel();
            tlCtx_->detached.push_back(p.fut);
        }
        if (p.fut->ready() && p.fut->failed() && p.fut->doneAt() < errAt) {
            err = p.fut->error();   // returned before the one we were going to raise
            errAt = p.fut->doneAt();
        }
    }
    // Restore targets. Later calls unwind in reverse, so a variable assigned
    // twice ends up with the value from before the first discarded call.
    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
        const Pending& p = *it;
        if (p.name.empty() || p.seq < failSeq) continue;
        if (p.hadPrev) (*f.vars)[p.name] = p.prev;
        else f.vars->erase(p.name);
    }
    for (auto& p : pending) {   // earlier calls that failed too: their assignment didn't happen either
        if (p.name.empty() || p.seq >= failSeq || !p.fut->failed()) continue;
        auto cur = f.vars->find(p.name);
        if (cur == f.vars->end() || !cur->second.isFuture() || cur->second.as<FutureP>() != p.fut) continue;
        if (p.hadPrev) cur->second = p.prev;
        else f.vars->erase(cur);
    }
    return err;
}

void Interp::detachAll(Frame& f) {
    for (auto& p : f.pending) {
        if (!p.fut->ready()) {
            p.fut->cancel();
            tlCtx_->detached.push_back(p.fut);
        }
    }
    tlCtx_->pendingCount -= f.pending.size();
    f.pending.clear();
}

void Interp::settle(Frame& f) {
    if (f.pending.empty()) return;
    std::vector<FutureP> futs;
    for (auto& p : f.pending) futs.push_back(p.fut);
    FutureP first = waitAllOrFirstFailure(futs);
    if (!first) {   // all succeeded; the variables keep their (resolved) futures
        tlCtx_->pendingCount -= f.pending.size();
        f.pending.clear();
        return;
    }
    throw abandonFrame(f, first->seq, first->error());
}

DolphinError Interp::preferEarlier(Frame& f, const DolphinError& err) {
    if (f.pending.empty()) return err;
    return abandonFrame(f, UINT64_MAX, err);   // every pending call is earlier than err's statement
}

Value Interp::force(const Value& v, Frame& f) {
    if (!v.isFuture()) return v;
    Future& fut = *v.as<FutureP>();
    waitFuture(fut);
    if (fut.failed()) throw abandonFrame(f, fut.seq, fut.error());
    return fut.value();
}

// An effect or a mutation is about to happen on this thread. Sequentially,
// every call before it has completed, so wait for all of them. If one failed,
// its exception must surface *instead of* the effect, and from the frame that
// made the call (so the right `try` handles it): raise directly if that's the
// current frame, or unwind to it with DeferredFailure.
void Interp::barrier() {
    TaskCtx* ctx = tlCtx_;
    if (!ctx || (ctx->pendingCount == 0 && ctx->detached.empty())) return;
    if (ctx->pendingCount) {
        std::vector<FutureP> futs;
        for (Frame* fr : ctx->frames)
            for (auto& p : fr->pending) futs.push_back(p.fut);
        if (FutureP first = waitAllOrFirstFailure(futs)) {
            Frame* owner = nullptr;
            for (Frame* fr : ctx->frames)
                for (auto& p : fr->pending)
                    if (p.fut == first) owner = fr;
            if (owner == ctx->frames.back()) throw abandonFrame(*owner, first->seq, first->error());
            throw DeferredFailure{owner};
        }
        for (Frame* fr : ctx->frames) fr->pending.clear();   // all succeeded
        ctx->pendingCount = 0;
    }
    for (auto& fu : ctx->detached) waitFuture(*fu);   // abandoned calls may still be reading something
    ctx->detached.clear();
}

// Comprehension conditions and arguments run early (all of them before the
// calls), so they must not have effects or mutate anything.
bool Interp::exprIsReorderable(const Expr& e) const {
    switch (e.k) {
    case Expr::K::Call: {
        auto& c = static_cast<const Call&>(e);
        if (c.fn->k == Expr::K::Name) {
            auto& n = static_cast<const Name&>(*c.fn);
            if (n.bind == Bind::Builtin && n.needs != NeedNone) return false;
            if (n.def && !n.def->parallelSafe()) return false;
            if (!n.def && n.bind != Bind::Builtin && n.bind != Bind::Exception) return false;
        } else if (c.fn->k == Expr::K::Attr) {
            const std::string& m = static_cast<const Attr&>(*c.fn).name;
            if (m == "append" || m == "extend" || m == "pop") return false;
            if (!exprIsReorderable(*static_cast<const Attr&>(*c.fn).obj)) return false;
        } else {
            return false;
        }
        for (auto& a : c.args)
            if (!exprIsReorderable(*a)) return false;
        for (auto& [k, v] : c.kwargs)
            if (!exprIsReorderable(*v)) return false;
        return true;
    }
    case Expr::K::FStr:
        for (auto& p : static_cast<const FStr&>(e).parts)
            if (p.expr && !exprIsReorderable(*p.expr)) return false;
        return true;
    case Expr::K::Unary: return exprIsReorderable(*static_cast<const Unary&>(e).operand);
    case Expr::K::Binary:
        return exprIsReorderable(*static_cast<const Binary&>(e).lhs) && exprIsReorderable(*static_cast<const Binary&>(e).rhs);
    case Expr::K::BoolOp:
        return exprIsReorderable(*static_cast<const BoolOp&>(e).lhs) && exprIsReorderable(*static_cast<const BoolOp&>(e).rhs);
    case Expr::K::Compare: {
        auto& c = static_cast<const Compare&>(e);
        if (!exprIsReorderable(*c.first)) return false;
        for (auto& [op, r] : c.rest)
            if (!exprIsReorderable(*r)) return false;
        return true;
    }
    case Expr::K::Index:
        return exprIsReorderable(*static_cast<const Index&>(e).obj) && exprIsReorderable(*static_cast<const Index&>(e).index);
    case Expr::K::Slice: {
        auto& s = static_cast<const Slice&>(e);
        return (!s.lo || exprIsReorderable(*s.lo)) && (!s.hi || exprIsReorderable(*s.hi));
    }
    case Expr::K::Attr: return exprIsReorderable(*static_cast<const Attr&>(e).obj);
    case Expr::K::List:
        for (auto& x : static_cast<const ListLit&>(e).elems)
            if (!exprIsReorderable(*x)) return false;
        return true;
    case Expr::K::Dict:
        for (auto& [k, v] : static_cast<const DictLit&>(e).items)
            if (!exprIsReorderable(*k) || !exprIsReorderable(*v)) return false;
        return true;
    case Expr::K::ListComp: return false;   // keep nesting simple: the inner one may parallelize on its own
    default: return true;                   // literals and names
    }
}

// [f(args) for v in xs if cond] with f parallel-safe: evaluate every
// condition and argument list first (in order, on this thread), then run the
// calls in batches on the pool, then collect results in element order. If
// elements fail, the first failure to return wins and the other batches stop
// (decision 17); with one thread that's the first failing element.
//
// Batch size: about four batches per thread, enough to even out uneven work
// (one long METAR among short ones) without paying per-element dispatch
// overhead. LANGUAGE.md §2.1: this becomes a learned, per-function size later.
bool Interp::parallelComprehension(const ListComp& c, Frame& f, Value& out) {
    if (c.elt->k != Expr::K::Call) return false;
    auto& call = static_cast<const Call&>(*c.elt);
    const Def* d = dispatchTarget(call, f);
    if (!d) return false;
    for (auto& a : call.args)
        if (!exprIsReorderable(*a)) return false;
    for (auto& [k, v] : call.kwargs)
        if (!exprIsReorderable(*v)) return false;
    if (c.cond && !exprIsReorderable(*c.cond)) return false;

    // Everything a batch touches is owned here, shared with the batches: an
    // abandoned batch (decision 17) may still be running after we return.
    struct Item { Args args; Kwargs kw; };
    struct Shared {
        std::vector<Item> items;
        std::vector<Value> results;
        std::atomic<bool> abort{false};   ///< Set by the first failing element; other batches stop early.
    };
    auto sh = std::make_shared<Shared>();
    for (auto& v : iterate(eval(*c.iter, f), c.line)) {
        (*f.vars)[c.var] = v;
        if (c.cond && !truthy(eval(*c.cond, f))) continue;
        Item it;
        evalArgs(call, f, it.args, it.kw);
        sh->items.push_back(std::move(it));
    }
    size_t n = sh->items.size();
    sh->results.resize(n);

    // One batch: run elements in order; stop at the first failure (later
    // elements of the batch wouldn't have run sequentially) or when another
    // batch has already failed. Result or error goes into the batch's future.
    auto runBatch = [this, d, sh, line = c.line](size_t lo, size_t hi, const FutureP& fut) {
        if (fut->cancelled()) {
            fut->fail(DolphinError{"Cancelled", "", line});
            return;
        }
        std::unique_ptr<DolphinError> err;   // outcome first, then ops, then complete (see dispatch())
        uint64_t ops = runAsTask([&] {
            try {
                for (size_t i = lo; i < hi && !sh->abort.load(std::memory_order_relaxed); ++i)
                    sh->results[i] = callFunction(*d, sh->items[i].args, sh->items[i].kw, line);
            } catch (const DolphinError& e) {
                sh->abort.store(true, std::memory_order_relaxed);
                err = std::make_unique<DolphinError>(e);
            } catch (const std::exception& e) {
                sh->abort.store(true, std::memory_order_relaxed);
                err = std::make_unique<DolphinError>(DolphinError{"RuntimeError", std::string("internal: ") + e.what(), line});
            }
        });
        fut->ops = ops;
        if (pool_) pool_->noteOps(ops);
        if (err) fut->fail(*err);
        else fut->resolve(Value());
    };

    std::vector<FutureP> futs;
    // Batch size from measured cost: never measured -> run the first element
    // here (it measures the function), then decide for the rest.
    size_t start = 0;
    if (cost_.predictNs(d) < 0 && n > 0) {
        auto fut = std::make_shared<Future>();
        runBatch(0, 1, fut);
        absorbOps(*fut);
        if (fut->failed()) throw fut->error();
        start = 1;
    }
    double avg = std::max(cost_.predictNs(d), 1.0);   // predicted ns per element: work / current rate
    size_t remaining = n - start;
    // About kBatchesPerThread batches per thread evens out uneven items; each
    // must still be big enough to be worth its submit and wakeup (CostModel).
    size_t maxBatches = static_cast<size_t>(activeThreads()) * CostModel::kBatchesPerThread;
    size_t byCost = static_cast<size_t>(static_cast<double>(remaining) * avg / cost_.minBatchNs());
    size_t batches = std::min({remaining, maxBatches, std::max<size_t>(byCost, 1)});
    size_t per = batches ? (remaining + batches - 1) / batches : 0;
    for (size_t lo = start; lo < n; lo += per) {
        size_t hi = std::min(n, lo + per);
        auto fut = std::make_shared<Future>();
        futs.push_back(fut);
        bool last = lo + per >= n;   // the last batch always runs here: this thread would only wait otherwise
        if (!last && pool_->submit([runBatch, lo, hi, fut]() { runBatch(lo, hi, fut); }, fut.get())) continue;
        if (!last) inlineCalls_.fetch_add(1, std::memory_order_relaxed);
        runBatch(lo, hi, fut);
        absorbOps(*fut);
    }

    if (trace_) trace(d->name + "() line " + std::to_string(c.line) + ": parallel map, " + std::to_string(n) + " items in " + std::to_string(futs.size()) + " batches");
    if (FutureP first = waitAllOrFirstFailure(futs)) {
        // The first failure to return wins. Stop the rest; any still running are
        // tracked so the next mutation or effect waits for them.
        sh->abort.store(true, std::memory_order_relaxed);
        for (auto& fu : futs)
            if (!fu->ready()) {
                fu->cancel();
                tlCtx_->detached.push_back(fu);
            }
        throw first->error();
    }
    out = Value::list(std::move(sh->results));
    return true;
}

// --------------------------------------------------------------- statements

Interp::Flow Interp::execBlock(const Block& b, Frame& f, Value& ret) {
    for (auto& s : b) {
        Flow fl = exec(*s, f, ret);
        if (fl != Flow::Normal) return fl;
    }
    return Flow::Normal;
}

bool Interp::excMatches(const std::string& handlerCls, const std::string& errCls) const {
    for (std::string c = errCls; !c.empty();) {
        if (c == handlerCls) return true;
        auto it = excParent_.find(c);
        if (it == excParent_.end()) break;
        c = it->second;
    }
    return false;
}

Interp::Flow Interp::exec(const Stmt& s, Frame& f, Value& ret) {
    ++tl_ops;   // one operation per statement executed (cost model)
    switch (s.k) {
    case Stmt::K::Expr: {
        auto& es = static_cast<const ExprStmt&>(s);
        if (es.e->k == Expr::K::Call && overlapAssigns_.count(&s)) {   // `f(x)` as a statement: run it, keep only a possible failure
            auto& c = static_cast<const Call&>(*es.e);
            if (const Def* d = dispatchTarget(c, f)) {
                Args args;
                Kwargs kw;
                evalArgs(c, f, args, kw);
                if (FutureP fut = tryDispatch(*d, args, kw, c.line)) {
                    addPending(f, fut, "");
                    return Flow::Normal;
                }
                callFunction(*d, args, kw, c.line);
                return Flow::Normal;
            }
        }
        eval(*es.e, f);
        return Flow::Normal;
    }

    case Stmt::K::Assign: {
        auto& a = static_cast<const Assign&>(s);
        // `x = f(...)` with f parallel-safe: x becomes a future.
        if (overlapAssigns_.count(&s)) {
            auto& c = static_cast<const Call&>(*a.value);
            if (const Def* d = dispatchTarget(c, f)) {
                Args args;
                Kwargs kw;
                evalArgs(c, f, args, kw);
                if (FutureP fut = tryDispatch(*d, args, kw, c.line)) {
                    addPending(f, fut, static_cast<const Name&>(*a.target).id);
                    return Flow::Normal;
                }
                assign(*a.target, callFunction(*d, args, kw, c.line), f);
                return Flow::Normal;
            }
        }
        assign(*a.target, eval(*a.value, f), f);
        return Flow::Normal;
    }

    // `t op= v` evaluates t's current value, computes, and assigns back.
    // Note the target expression is evaluated twice (once to read, once to
    // write), which is only observable if it has side effects.
    case Stmt::K::AugAssign: {
        auto& a = static_cast<const AugAssign&>(s);
        Value cur = eval(*a.target, f);
        Value rhs = eval(*a.value, f);
        assign(*a.target, binary(a.code, cur, rhs, s.line), f);
        return Flow::Normal;
    }

    case Stmt::K::If: {
        auto& i = static_cast<const If&>(s);
        for (auto& [cond, body] : i.branches)
            if (truthy(eval(*cond, f))) return execBlock(body, f, ret);
        return execBlock(i.orelse, f, ret);
    }

    case Stmt::K::While: {
        auto& w = static_cast<const While&>(s);
        while (truthy(eval(*w.cond, f))) {
            Flow fl = execBlock(w.body, f, ret);
            if (fl == Flow::Break) break;
            if (fl == Flow::Return) return fl;
        }
        return Flow::Normal;
    }

    case Stmt::K::For: {
        auto& fo = static_cast<const For&>(s);
        for (auto& item : iterate(eval(*fo.iter, f), s.line)) {
            (*f.vars)[fo.var] = item;
            Flow fl = execBlock(fo.body, f, ret);
            if (fl == Flow::Break) break;
            if (fl == Flow::Return) return fl;
        }
        return Flow::Normal;
    }

    case Stmt::K::Def: {
        auto& d = static_cast<const Def&>(s);
        (*f.vars)[d.name] = Value(std::make_shared<const Function>(Function{&d}));
        return Flow::Normal;
    }

    case Stmt::K::Return: {
        auto& r = static_cast<const Return&>(s);
        ret = r.value ? eval(*r.value, f) : Value();
        return Flow::Return;
    }
    case Stmt::K::Break: return Flow::Break;
    case Stmt::K::Continue: return Flow::Continue;
    case Stmt::K::Pass: return Flow::Normal;

    // try / except (LANGUAGE.md §3.5).
    //
    // Decision 2, "variables the failing call would assign are left
    // untouched", falls out naturally here: in `x = f()`, if f raises, the
    // DolphinError unwinds before assign() runs, so x keeps its old value.
    //
    // handling_ records the exception being handled, so a bare `raise`
    // inside the handler can re-throw it. It's a stack because handlers can
    // nest.
    case Stmt::K::Try: {
        auto& t = static_cast<const Try&>(s);
        // Join points. On entry: a failure from before the `try` must not be
        // caught by it. On exit (inside the C++ try): a failure from inside must be.
        settle(f);
        try {
            Flow fl = execBlock(t.body, f, ret);
            settle(f);
            return fl;
        } catch (const DolphinError& raised) {
            const DolphinError err = preferEarlier(f, raised);
            for (auto& h : t.handlers) {
                if (!h.cls.empty()) {
                    Value hc = lookup(h.cls, f, h.line);
                    if (!hc.is<std::shared_ptr<const ExcClass>>())
                        raise("TypeError", "'" + h.cls + "' is not an exception class", h.line);
                    if (!excMatches(h.cls, err.cls)) continue;
                }
                if (!h.as.empty())
                    (*f.vars)[h.as] = Value(std::make_shared<const ExcValue>(ExcValue{err.cls, err.msg}));
                tl_handling.push_back(err);
                Flow fl;
                try {
                    fl = execBlock(h.body, f, ret);
                } catch (...) {
                    tl_handling.pop_back();
                    throw;
                }
                tl_handling.pop_back();
                return fl;
            }
            throw err;
        }
    }

    case Stmt::K::Raise: {
        auto& r = static_cast<const Raise&>(s);
        if (!r.value) {
            if (tl_handling.empty()) raise("RuntimeError", "no active exception to re-raise", s.line);
            throw tl_handling.back();
        }
        Value v = eval(*r.value, f);
        if (v.is<std::shared_ptr<const ExcValue>>()) {
            auto& e = *v.as<std::shared_ptr<const ExcValue>>();
            raise(e.cls, e.msg, s.line);
        }
        if (v.is<std::shared_ptr<const ExcClass>>()) raise(v.as<std::shared_ptr<const ExcClass>>()->name, "", s.line);
        raise("TypeError", "can only raise exceptions, not " + typeName(v), s.line);
    }
    }
    return Flow::Normal;
}

void Interp::assign(const Expr& target, const Value& v, Frame& f) {
    switch (target.k) {
    case Expr::K::Name:
        (*f.vars)[static_cast<const Name&>(target).id] = v;
        return;
    case Expr::K::Index: {
        auto& ix = static_cast<const Index&>(target);
        Value obj = eval(*ix.obj, f);
        if (ix.index->k == Expr::K::Slice) raise("TypeError", "slice assignment is not supported", target.line);
        if (obj.isDict()) {
            Value k = eval(*ix.index, f);
            std::string ck;
            if (!dictKey(k, ck)) raise("TypeError", "unhashable type: '" + typeName(k) + "'", target.line);
            barrier();   // mutation: nothing in flight may be reading it
            obj.d().set(ck, k, v);
            return;
        }
        if (!obj.isList()) raise("TypeError", "'" + typeName(obj) + "' does not support item assignment", target.line);
        Value iv = eval(*ix.index, f);
        if (!iv.isInt()) raise("TypeError", "list indices must be int", target.line);
        auto& items = obj.l().items;
        int64_t i = iv.as<int64_t>();
        if (i < 0) i += static_cast<int64_t>(items.size());
        if (i < 0 || i >= static_cast<int64_t>(items.size()))
            raise("IndexError", "list assignment index out of range", target.line);
        barrier();   // mutation: nothing in flight may be reading it
        items[static_cast<size_t>(i)] = v;
        return;
    }
    case Expr::K::Attr:
        raise("AttributeError", "objects with assignable attributes arrive with 'struct'", target.line);
    default:
        raise("SyntaxError", "cannot assign to expression", target.line);
    }
}

// -------------------------------------------------------------- expressions

Value Interp::lookup(const std::string& name, Frame& f, int line) {
    auto it = f.vars->find(name);
    if (it != f.vars->end()) return it->second;
    it = globals_.find(name);
    if (it != globals_.end()) return it->second;
    it = builtins_.find(name);
    if (it != builtins_.end()) return it->second;
    raise("NameError", "name '" + name + "' is not defined", line);
}

Value Interp::eval(const Expr& e, Frame& f) {
    ++tl_ops;   // one operation per expression evaluated (cost model)
    switch (e.k) {
    case Expr::K::Int: return Value(static_cast<const IntLit&>(e).v);
    case Expr::K::Float: return Value(static_cast<const FloatLit&>(e).v);
    case Expr::K::Str: return Value::borrowedStr(static_cast<const StrLit&>(e).str.get());   // no allocation, no refcount
    case Expr::K::Bytes: return Value::bytes(static_cast<const BytesLit&>(e).v);
    case Expr::K::Bool: return Value(static_cast<const BoolLit&>(e).v);
    case Expr::K::None: return Value();
    case Expr::K::Name: {
        // The resolver has already decided which table the name lives in.
        auto& n = static_cast<const Name&>(e);
        switch (n.bind) {
        case Bind::Local: {
            auto it = f.vars->find(n.id);
            if (it == f.vars->end())
                raise("UnboundLocalError", "local variable '" + n.id + "' referenced before assignment", e.line);
            if (it->second.isFuture()) {   // a call still running: wait for it
                Value v = force(it->second, f);
                (*f.vars)[n.id] = v;      // later reads don't need to wait
                return v;
            }
            return it->second;
        }
        case Bind::Global:
        case Bind::Function: {
            auto it = globals_.find(n.id);
            if (it == globals_.end()) raise("NameError", "name '" + n.id + "' is not defined yet", e.line);
            return it->second;
        }
        case Bind::Builtin:
        case Bind::Exception: return builtins_.at(n.id);
        case Bind::Unresolved: break;
        }
        return lookup(n.id, f, e.line);
    }

    case Expr::K::FStr: {
        std::string out;
        for (auto& p : static_cast<const FStr&>(e).parts) {
            if (!p.expr) {
                out += p.text;
                continue;
            }
            Value v = eval(*p.expr, f);
            out += p.spec.empty() ? toStr(v) : format(v, p.spec, e.line);
        }
        return Value::str(std::move(out));
    }

    case Expr::K::Unary: {
        auto& u = static_cast<const Unary&>(e);
        Value v = eval(*u.operand, f);
        if (u.code == Op::Not) return Value(!truthy(v));
        if (v.is<bool>()) v = Value(static_cast<int64_t>(v.as<bool>()));
        if (u.code == Op::Pos && v.isNum()) return v;
        if (u.code == Op::Neg) {
            if (v.isFloat()) return Value(-v.as<double>());
            if (v.isInt()) {
                if (v.as<int64_t>() == INT64_MIN) raise("OverflowError", "integer overflow", e.line);
                return Value(-v.as<int64_t>());
            }
        }
        if (u.code == Op::Invert && v.isInt()) return Value(~v.as<int64_t>());
        raise("TypeError", "bad operand type for unary " + u.op + ": '" + typeName(v) + "'", e.line);
    }

    case Expr::K::Binary: {
        auto& b = static_cast<const Binary&>(e);
        Value l = eval(*b.lhs, f);
        Value r = eval(*b.rhs, f);
        return binary(b.code, l, r, e.line);
    }

    case Expr::K::BoolOp: {
        auto& b = static_cast<const BoolOp&>(e);
        Value l = eval(*b.lhs, f);
        if (b.code == Op::And) return truthy(l) ? eval(*b.rhs, f) : l;
        return truthy(l) ? l : eval(*b.rhs, f);
    }

    case Expr::K::Compare: {
        auto& c = static_cast<const Compare&>(e);
        Value l = eval(*c.first, f);
        for (size_t i = 0; i < c.rest.size(); ++i) {
            Value r = eval(*c.rest[i].second, f);
            if (!compare(c.ops[i], l, r, e.line)) return Value(false);
            l = r;
        }
        return Value(true);
    }

    case Expr::K::Call: {
        auto& c = static_cast<const Call&>(e);
        // Fast path for statically resolved callees: call straight through
        // without copying the function's shared handle. Copying it would bump
        // one shared reference count from every thread, and that cache-line
        // ping-pong is what limits parallel speedup in a tree-walker.
        if (c.fn->k == Expr::K::Name) {
            auto& n = static_cast<const Name&>(*c.fn);
            if (n.bind == Bind::Builtin) {
                auto& b = *builtins_.find(n.id)->second.as<std::shared_ptr<const Builtin>>();
                Args args;
                Kwargs kw;
                evalArgs(c, f, args, kw);
                if (!kw.empty()) raise("TypeError", "builtins do not take keyword arguments yet", e.line);
                if (b.needs != NeedNone) barrier();
                return b.fn(*this, args, e.line);
            }
            if (n.bind == Bind::Function && n.def && globals_.count(n.id)) {
                Args args;
                Kwargs kw;
                evalArgs(c, f, args, kw);
                return callFunction(*n.def, args, kw, e.line);
            }
        }
        // Fast path for built-in methods (`s.strip()`, `d.get(k)`): dispatch on
        // the parser's Method code, without building a bound-method object.
        if (c.fn->k == Expr::K::Attr) {
            auto& at = static_cast<const Attr&>(*c.fn);
            if (at.method != Method::None) {
                Value self = eval(*at.obj, f);
                if (hasMethod(self, at.method)) {
                    Args args;
                    Kwargs kw;
                    evalArgs(c, f, args, kw);
                    if (!kw.empty()) raise("TypeError", "methods do not take keyword arguments yet", e.line);
                    return callMethod(self, at.method, args, e.line);
                }
                Args args;   // not a method of this type: the general path reports it
                Kwargs kw;
                Value fn = attribute(self, at.name, e.line);
                evalArgs(c, f, args, kw);
                return callValue(fn, args, kw, e.line);
            }
        }
        Value fn = eval(*c.fn, f);
        Args args;
        args.reserve(c.args.size());
        for (auto& a : c.args) args.push_back(eval(*a, f));
        Kwargs kw;
        for (auto& [k, v] : c.kwargs) kw.emplace_back(k, eval(*v, f));
        return callValue(fn, args, kw, e.line);
    }

    case Expr::K::Index: {
        auto& ix = static_cast<const Index&>(e);
        Value obj = eval(*ix.obj, f);
        if (ix.hasConstKey && obj.isDict()) {   // r["station"]: key precomputed by the parser
            long pos = obj.d().find(ix.constKey);
            if (pos >= 0) return obj.d().entries[static_cast<size_t>(pos)].second;
        }
        return subscript(obj, *ix.index, f, e.line);   // also raises the KeyError
    }

    case Expr::K::Slice: raise("SyntaxError", "slice outside of subscript", e.line);

    case Expr::K::Attr: {
        auto& a = static_cast<const Attr&>(e);
        return attribute(eval(*a.obj, f), a.name, e.line);
    }

    case Expr::K::List: {
        std::vector<Value> items;
        for (auto& x : static_cast<const ListLit&>(e).elems) items.push_back(eval(*x, f));
        return Value::list(std::move(items));
    }

    case Expr::K::Dict: {
        Value d = Value::dict();
        for (auto& [kx, vx] : static_cast<const DictLit&>(e).items) {
            Value k = eval(*kx, f);
            Value v = eval(*vx, f);
            std::string ck;
            if (!dictKey(k, ck)) raise("TypeError", "unhashable type: '" + typeName(k) + "'", e.line);
            d.d().set(ck, k, v);
        }
        return d;
    }

    case Expr::K::ListComp: {
        auto& c = static_cast<const ListComp&>(e);
        Value par;
        if (parallelComprehension(c, f, par)) return par;
        std::vector<Value> out;
        for (auto& item : iterate(eval(*c.iter, f), e.line)) {
            (*f.vars)[c.var] = item;
            if (c.cond && !truthy(eval(*c.cond, f))) continue;
            out.push_back(eval(*c.elt, f));
        }
        return Value::list(std::move(out));
    }
    }
    raise("RuntimeError", "unhandled expression kind", e.line);
}

// --------------------------------------------------------------- arithmetic

static int64_t asInt(const Value& v) { return v.is<bool>() ? (v.as<bool>() ? 1 : 0) : v.as<int64_t>(); }
static double asDouble(const Value& v) {
    if (v.isFloat()) return v.as<double>();
    return static_cast<double>(asInt(v));
}

// Operator semantics follow Python, with one deliberate difference: ints are
// 64-bit, and overflow raises OverflowError instead of silently wrapping
// (C++) or growing without bound (Python). Order of checks:
//   1. Sequence operators: str/bytes/list `+` (concatenate) and `*` (repeat).
//   2. If either side is float, or the op is true division `/`, use doubles.
//   3. Otherwise int64 with overflow checks via __builtin_*_overflow.
Value Interp::binary(Op op, const Value& a, const Value& b, int line) const {
    auto typeErr = [&]() -> Value {
        raise("TypeError",
              "unsupported operand types for " + std::string(opSpelling(op)) + ": '" + typeName(a) + "' and '" + typeName(b) + "'", line);
    };

    // Sequences
    if (op == Op::Add) {
        if (a.isStr() && b.isStr()) return Value::str(a.s().s + b.s().s);
        if (a.isBytes() && b.isBytes()) return Value::bytes(a.b().b + b.b().b);
        if (a.isList() && b.isList()) {
            auto items = a.l().items;
            items.insert(items.end(), b.l().items.begin(), b.l().items.end());
            return Value::list(std::move(items));
        }
    }
    if (op == Op::Mul) {
        const Value* seq = nullptr;
        const Value* n = nullptr;
        if ((a.isStr() || a.isBytes() || a.isList()) && b.isInt()) { seq = &a; n = &b; }
        if ((b.isStr() || b.isBytes() || b.isList()) && a.isInt()) { seq = &b; n = &a; }
        if (seq) {
            int64_t k = std::max<int64_t>(0, n->as<int64_t>());
            if (seq->isList()) {
                std::vector<Value> items;
                for (int64_t i = 0; i < k; ++i)
                    items.insert(items.end(), seq->l().items.begin(), seq->l().items.end());
                return Value::list(std::move(items));
            }
            const std::string& src = seq->isStr() ? seq->s().s : seq->b().b;
            std::string out;
            out.reserve(src.size() * static_cast<size_t>(k));
            for (int64_t i = 0; i < k; ++i) out += src;
            return seq->isStr() ? Value::str(std::move(out)) : Value::bytes(std::move(out));
        }
    }

    if (!a.isNum() || !b.isNum()) return typeErr();

    if (a.isFloat() || b.isFloat() || op == Op::Div) {
        double x = asDouble(a), y = asDouble(b);
        if (op == Op::Add) return Value(x + y);
        if (op == Op::Sub) return Value(x - y);
        if (op == Op::Mul) return Value(x * y);
        if (op == Op::Pow) return Value(std::pow(x, y));
        if (op == Op::Div || op == Op::FloorDiv || op == Op::Mod) {
            if (y == 0.0) raise("ZeroDivisionError", "division by zero", line);
            if (op == Op::Div) return Value(x / y);
            if (op == Op::FloorDiv) return Value(std::floor(x / y));
            double m = std::fmod(x, y);
            if (m != 0.0 && ((m < 0) != (y < 0))) m += y;
            return Value(m);
        }
        return typeErr();
    }

    int64_t x = asInt(a), y = asInt(b), r = 0;
    auto overflow = [&]() -> Value { raise("OverflowError", std::string("integer overflow in ") + opSpelling(op), line); };
    if (op == Op::Add) { if (__builtin_add_overflow(x, y, &r)) return overflow(); return Value(r); }
    if (op == Op::Sub) { if (__builtin_sub_overflow(x, y, &r)) return overflow(); return Value(r); }
    if (op == Op::Mul) { if (__builtin_mul_overflow(x, y, &r)) return overflow(); return Value(r); }
    if (op == Op::FloorDiv || op == Op::Mod) {
        if (y == 0) raise("ZeroDivisionError", "integer division or modulo by zero", line);
        if (x == INT64_MIN && y == -1) return op == Op::Mod ? Value(int64_t{0}) : overflow();
        int64_t q = x / y, m = x % y;
        // C++ truncates toward zero; Python floors. When the remainder is
        // nonzero and its sign differs from the divisor's, step the quotient
        // down one and move the remainder into the divisor's sign:
        //   -7 // 2 == -4 and -7 % 2 == 1   (C++ gives -3 and -1)
        if (m != 0 && ((m < 0) != (y < 0))) { --q; m += y; }
        return Value(op == Op::FloorDiv ? q : m);
    }
    if (op == Op::Pow) {
        if (y < 0) return Value(std::pow(static_cast<double>(x), static_cast<double>(y)));
        int64_t result = 1, base = x;
        while (y) {
            if (y & 1) { if (__builtin_mul_overflow(result, base, &result)) return overflow(); }
            y >>= 1;
            if (y && __builtin_mul_overflow(base, base, &base)) return overflow();
        }
        return Value(result);
    }
    if (op == Op::BitAnd) return Value(x & y);
    if (op == Op::BitOr) return Value(x | y);
    if (op == Op::BitXor) return Value(x ^ y);
    if (op == Op::Shl || op == Op::Shr) {
        if (y < 0) raise("ValueError", "negative shift count", line);
        if (op == Op::Shr) return Value(y >= 64 ? (x < 0 ? int64_t{-1} : int64_t{0}) : (x >> y));
        if (y >= 64 || (x != 0 && (y >= 63 || (x > 0 ? x > (INT64_MAX >> y) : x < (INT64_MIN >> y)))))
            return overflow();
        return Value(static_cast<int64_t>(static_cast<uint64_t>(x) << y));
    }
    return typeErr();
}

bool Interp::compare(Op op, const Value& a, const Value& b, int line) const {
    if (op == Op::Eq) return valuesEqual(a, b);
    if (op == Op::Ne) return !valuesEqual(a, b);
    if (op == Op::In || op == Op::NotIn) {
        bool found = false;
        if (b.isStr()) {
            if (!a.isStr()) raise("TypeError", "'in <str>' requires str as left operand", line);
            found = b.s().s.find(a.s().s) != std::string::npos;
        } else if (b.isBytes()) {
            if (a.isInt()) {
                int64_t v = a.as<int64_t>();
                found = v >= 0 && v < 256 && b.b().b.find(static_cast<char>(v)) != std::string::npos;
            } else if (a.isBytes()) {
                found = b.b().b.find(a.b().b) != std::string::npos;
            } else {
                raise("TypeError", "'in <bytes>' requires int or bytes", line);
            }
        } else if (b.isList()) {
            for (auto& x : b.l().items)
                if (valuesEqual(a, x)) { found = true; break; }
        } else if (b.isDict()) {
            std::string ck;
            if (!dictKey(a, ck)) raise("TypeError", "unhashable type: '" + typeName(a) + "'", line);
            found = b.d().find(ck) >= 0;
        } else {
            raise("TypeError", "argument of type '" + typeName(b) + "' is not a container", line);
        }
        return op == Op::In ? found : !found;
    }
    int c;
    if (a.isNum() && b.isNum()) {
        if (a.isFloat() || b.isFloat()) {
            double x = asDouble(a), y = asDouble(b);
            if (std::isnan(x) || std::isnan(y)) return false;
            c = x < y ? -1 : (x > y ? 1 : 0);
        } else {
            int64_t x = asInt(a), y = asInt(b);
            c = x < y ? -1 : (x > y ? 1 : 0);
        }
    } else if (a.isStr() && b.isStr()) {
        c = a.s().s.compare(b.s().s);   // UTF-8 byte order == code point order
    } else if (a.isBytes() && b.isBytes()) {
        c = a.b().b.compare(b.b().b);
    } else {
        raise("TypeError", "'" + std::string(opSpelling(op)) + "' not supported between '" + typeName(a) + "' and '" + typeName(b) + "'", line);
    }
    if (op == Op::Lt) return c < 0;
    if (op == Op::Le) return c <= 0;
    if (op == Op::Gt) return c > 0;
    return c >= 0;
}

// ---------------------------------------------------------------- subscripts

/**
 * @brief Python slice bound normalization.
 *
 * Missing bounds default to 0 and @p len; negative bounds count from the end;
 * everything is clamped to [0, len]; an inverted range becomes empty.
 * Slicing never raises IndexError.
 */
static void sliceBounds(int64_t len, const Value* lo, const Value* hi, int64_t& a, int64_t& b) {
    auto norm = [&](const Value* v, int64_t dflt) {
        if (!v || v->isNone()) return dflt;
        int64_t x = v->as<int64_t>();
        if (x < 0) x += len;
        return std::clamp<int64_t>(x, 0, len);
    };
    a = norm(lo, 0);
    b = norm(hi, len);
    if (b < a) b = a;
}

Value Interp::subscript(const Value& obj, const Expr& idx, Frame& f, int line) {
    if (obj.isDict()) {
        if (idx.k == Expr::K::Slice) raise("TypeError", "dict cannot be sliced", line);
        Value k = eval(idx, f);
        std::string ck;
        if (!dictKey(k, ck)) raise("TypeError", "unhashable type: '" + typeName(k) + "'", line);
        long pos = obj.d().find(ck);
        if (pos < 0) raise("KeyError", toRepr(k), line);
        return obj.d().entries[static_cast<size_t>(pos)].second;
    }
    int64_t len;
    if (obj.isStr()) len = static_cast<int64_t>(utf8Length(obj.s()));
    else if (obj.isBytes()) len = static_cast<int64_t>(obj.b().b.size());
    else if (obj.isList()) len = static_cast<int64_t>(obj.l().items.size());
    else raise("TypeError", "'" + typeName(obj) + "' is not subscriptable", line);

    if (idx.k == Expr::K::Slice) {
        auto& s = static_cast<const Slice&>(idx);
        Value lo, hi;
        if (s.lo) lo = eval(*s.lo, f);
        if (s.hi) hi = eval(*s.hi, f);
        for (auto* v : {&lo, &hi})
            if (!v->isNone() && !v->isInt()) raise("TypeError", "slice indices must be int", line);
        int64_t a, b;
        sliceBounds(len, &lo, &hi, a, b);
        if (obj.isStr()) {
            size_t ba = utf8Offset(obj.s(), static_cast<size_t>(a));
            size_t bb = utf8Offset(obj.s(), static_cast<size_t>(b));
            return Value::str(obj.s().s.substr(ba, bb - ba));
        }
        if (obj.isBytes()) return Value::bytes(obj.b().b.substr(static_cast<size_t>(a), static_cast<size_t>(b - a)));
        auto& items = obj.l().items;
        return Value::list(std::vector<Value>(items.begin() + a, items.begin() + b));
    }

    Value iv = eval(idx, f);
    if (!iv.isInt()) raise("TypeError", typeName(obj) + " indices must be int, not " + typeName(iv), line);
    int64_t i = iv.as<int64_t>();
    if (i < 0) i += len;
    if (i < 0 || i >= len) raise("IndexError", typeName(obj) + " index out of range", line);
    size_t u = static_cast<size_t>(i);
    if (obj.isStr()) {
        size_t ba = utf8Offset(obj.s(), u), bb = utf8Offset(obj.s(), u + 1);
        return Value::str(obj.s().s.substr(ba, bb - ba));
    }
    if (obj.isBytes()) return Value(static_cast<int64_t>(static_cast<unsigned char>(obj.b().b[u])));
    return obj.l().items[u];
}

std::vector<Value> Interp::iterate(const Value& v, int line) const {
    if (v.isList()) return v.l().items;   // snapshot: mutation during iteration is not visible
    std::vector<Value> out;
    if (v.isDict()) {   // keys, in insertion order (also a snapshot)
        for (auto& [k, x] : v.d().entries) out.push_back(k);
        return out;
    }
    if (v.isBytes()) {
        for (unsigned char c : v.b().b) out.emplace_back(static_cast<int64_t>(c));
        return out;
    }
    if (v.isStr()) {
        const std::string& s = v.s().s;
        for (size_t i = 0; i < s.size();) {
            size_t j = i + 1;
            while (j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) ++j;
            out.push_back(Value::str(s.substr(i, j - i)));
            i = j;
        }
        return out;
    }
    raise("TypeError", "'" + typeName(v) + "' is not iterable", line);
}

// --------------------------------------------------------------------- calls

Value Interp::callValue(const Value& fn, Args& args, Kwargs& kw, int line) {
    if (fn.is<std::shared_ptr<const Function>>())
        return callFunction(*fn.as<std::shared_ptr<const Function>>()->def, args, kw, line);
    if (fn.is<std::shared_ptr<const Builtin>>()) {
        if (!kw.empty()) raise("TypeError", "builtins do not take keyword arguments yet", line);
        auto& b = *fn.as<std::shared_ptr<const Builtin>>();
        if (b.needs != NeedNone) barrier();   // an effect: everything before it must have happened
        return b.fn(*this, args, line);
    }
    if (fn.is<std::shared_ptr<const BoundMethod>>()) {
        if (!kw.empty()) raise("TypeError", "methods do not take keyword arguments yet", line);
        auto& m = *fn.as<std::shared_ptr<const BoundMethod>>();
        return callMethod(m.self, m.method, args, line);
    }
    if (fn.is<std::shared_ptr<const ExcClass>>()) {
        if (args.size() > 1 || !kw.empty()) raise("TypeError", "exceptions take at most one argument", line);
        std::string msg = args.empty() ? "" : toStr(args[0]);
        return Value(std::make_shared<const ExcValue>(ExcValue{fn.as<std::shared_ptr<const ExcClass>>()->name, msg}));
    }
    raise("TypeError", "'" + typeName(fn) + "' object is not callable", line);
}

// Parameter binding: positional args fill parameters left to right; keyword
// args fill by name (unknown or duplicate names are TypeErrors); remaining
// parameters take their defaults. Unlike Python, defaults are evaluated at
// call time rather than once at definition time. Since values are immutable,
// that's only observable through lists, and it removes Python's classic
// shared-mutable-default bug.
Value Interp::callFunction(const Def& d, Args& args, Kwargs& kw, int line) {
    if (tl_depth >= kMaxDepth) raise("RecursionError", "maximum recursion depth exceeded", line);
    if (args.size() > d.params.size())
        raise("TypeError", d.name + "() takes " + std::to_string(d.params.size()) + " arguments but " +
                               std::to_string(args.size()) + " were given", line);
    Scope locals;
    for (size_t i = 0; i < args.size(); ++i) locals[d.params[i].name] = args[i];
    for (auto& [k, v] : kw) {
        bool known = false;
        for (auto& p : d.params)
            if (p.name == k) known = true;
        if (!known) raise("TypeError", d.name + "() got an unexpected keyword argument '" + k + "'", line);
        if (locals.count(k)) raise("TypeError", d.name + "() got multiple values for argument '" + k + "'", line);
        locals[k] = v;
    }
    Frame g(&globals_);
    for (auto& p : d.params) {
        if (locals.count(p.name)) continue;
        if (!p.dflt) raise("TypeError", d.name + "() missing required argument '" + p.name + "'", line);
        locals[p.name] = eval(*p.dflt, g);   // evaluated per call; values are immutable anyway
    }

    Frame f(&locals);
    TaskCtx* ctx = tlCtx_;
    ctx->frames.push_back(&f);
    Value ret;
    ++tl_depth;
    // Function return is a join point. Every exit path joins this frame's
    // calls before popping it, so none outlive the frame they write into.
    auto leave = [&] {
        --tl_depth;
        ctx->frames.pop_back();
    };
    // Measure parallel-safe functions for the cost model: operations executed
    // (including joined work it dispatched) and wall time. A call that raises
    // still cost its time, so it's measured too; otherwise a function that
    // usually fails would never be considered worth a thread.
    bool measured = cost_.tracks(&d);
    auto t0 = measured ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    uint64_t ops0 = tl_ops;
    bool outerWaited = tl_waited;
    tl_waited = false;
    auto recordCost = [&] {
        bool waited = tl_waited;
        tl_waited = outerWaited || waited;   // the caller waited too, while this call did
        if (!measured) return;
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        cost_.recordCall(&d, tl_ops - ops0, static_cast<uint64_t>(ns), !waited);
    };
    try {
        execBlock(d.body, f, ret);
        settle(f);
        ret = force(ret, f);   // `return x` where x is a future
        recordCost();
    } catch (const DolphinError& raised) {
        DolphinError err = preferEarlier(f, raised);   // waits for this call's own earlier work...
        recordCost();                                  // ...which is part of what it cost
        leave();
        throw err;
    } catch (const DeferredFailure& df) {
        tl_waited = outerWaited || tl_waited;
        detachAll(f);   // dispatched after the failure: cancel them
        leave();
        if (!ctx->frames.empty() && df.target == ctx->frames.back()) settle(*df.target);   // throws in the caller
        throw;
    } catch (...) {
        tl_waited = outerWaited || tl_waited;
        detachAll(f);
        leave();
        throw;
    }
    leave();
    return ret;
}

} // namespace dolphin
