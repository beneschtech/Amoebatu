/**
 * @file interp.hpp
 * @brief The M0 tree-walking interpreter.
 *
 * It walks the AST directly and evaluates it. Sequential and single-threaded
 * on purpose: it is the reference semantics for the language. Every later
 * execution engine (futures and the thread pool at M2, LLVM JIT at M3,
 * cluster dispatch at M5) must produce exactly the same observable output
 * as this one. The golden tests in `tests/` pin that down.
 *
 * Parallel execution (M2, local threads only):
 *  - A call to a Def::parallelSafe() function may run on a pool thread:
 *    `x = f(...)` makes x a future, and `[f(v) for v in xs]` becomes a
 *    batched parallel map. Arguments are evaluated first, on the caller.
 *  - It runs inline instead whenever the pool is busy (ThreadPool::submit()
 *    refuses), so heavy load degrades to sequential execution.
 *  - Output is identical to sequential execution, by these rules:
 *    - Effects (builtins with needs) run only on the main thread, in program
 *      order. Before one runs, every future dispatched earlier must finish
 *      (barrier()).
 *    - Mutating a list or dict is also a barrier, so nothing is mutated while
 *      a call that might read it is running. Workers only ever mutate objects
 *      they created themselves (that's what parallelSafe() guarantees).
 *    - Join points: `try` entry, `try` exit, function return.
 *    - If several calls fail, the first failure to *return* wins
 *      (LANGUAGE.md decision 17). Calls before it in program order still
 *      finish, as they would have sequentially; calls after it are cancelled.
 *      Variables the failed and cancelled calls were assigning keep their
 *      previous values (decision 2). With a single failure, the result is
 *      exactly what sequential execution gives.
 *
 * Scoping (deliberately simpler than Python's):
 *  - Module level: names live in the globals table.
 *  - Inside a function: assignment always creates or updates a local. Reads
 *    look in locals, then globals, then builtins. There is no `global`
 *    statement, so functions can't rebind module-level names, by design:
 *    shared mutable state goes through cluster globals, explicitly.
 *  - No closures yet: nested functions see their own locals and globals only.
 */
#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <memory>

#include "ast.hpp"
#include "costmodel.hpp"
#include "topology.hpp"
#include "runtime.hpp"
#include "value.hpp"

namespace dolphin {

/**
 * @brief The needs of every builtin function, keyed by name.
 *
 * This is the single source of truth (LANGUAGE.md decision 4: the standard
 * library ships with its needs declared). The analyzer reads it to infer
 * user functions' needs. The interpreter reads it when registering
 * builtins.
 */
const std::unordered_map<std::string, unsigned>& builtinNeeds();

/**
 * @brief Builtin exception classes as (name, parent) pairs, parents first.
 *
 * Shared by the resolver (which needs to know the names exist) and the
 * interpreter (which registers them). `Error` is the root and has an empty parent.
 */
const std::vector<std::pair<std::string, std::string>>& builtinExceptions();

/**
 * @brief Executes a parsed Module.
 *
 * @code
 *   Module m = parseSource(src, path);
 *   analyzeNeeds(m);
 *   Interp in(m);
 *   int rc = in.run(args);
 * @endcode
 *
 * The Module must outlive the Interp: functions are stored as pointers into
 * the AST.
 */
class Interp {
public:
    /**
     * @brief Bind to a module and register all builtins.
     * @param m        The parsed, resolved and analyzed program. Must outlive the Interp.
     * @param threads  Total threads including the main one. 1 = no parallel
     *                 execution at all (pure sequential reference semantics).
     *                 0 = one per hardware thread.
     * @param asyncOutput  Write program output from a printer thread
     *                 (OutputChannel) so print() never waits on the device.
     *                 False writes synchronously, for debugging.
     */
    explicit Interp(const Module& m, unsigned threads = 0, bool asyncOutput = true);

    /// @brief Write program output (what print() produces). Asynchronous unless disabled.
    void emit(std::string text);
    /// @brief Wait until all program output so far has reached the device.
    void flushOutput();
    /// @brief The async output channel, or null when output is synchronous.
    const OutputChannel* output() const { return out_.get(); }
    ~Interp();

    /// @brief Pool statistics, or null when running single-threaded.
    PoolStats* stats() { return pool_ ? &pool_->stats() : nullptr; }
    /// @brief Calls run inline because the pool refused them or they weren't parallel-safe at that point.
    uint64_t inlineCalls() const { return inlineCalls_.load(); }
    /// @brief Threads available (main + workers, active or parked).
    unsigned threads() const { return pool_ ? pool_->workers() + 1 : 1; }
    /// @brief Threads currently active (main + active workers).
    unsigned activeThreads() const { return pool_ ? pool_->active() + 1 : 1; }
    /// @brief The pool, if any (for `--stats`).
    const ThreadPool* pool() const { return pool_.get(); }
    /// @brief Topology used for the automatic thread count (empty basis if a count was given).
    const Topology& topology() const { return topo_; }
    /// @brief The cost model's current measurements (for `--stats`).
    const CostModel& costModel() const { return cost_; }
    /// @brief Use the old constant thresholds instead of measured ones (`--fixed-thresholds`).
    void setFixedThresholds(bool on) { cost_.setFixed(on); }
    /// @brief Log every dispatch decision, with its reason, to stderr (`dolphin --trace`).
    void setTrace(bool on) { trace_ = on; }

    /**
     * @brief Run the module.
     *
     * Executes the top-level statements. Then, if the module defines a
     * function named `main`, calls it with @p args as str values.
     *
     * @param args  Command-line arguments after the script name.
     * @return Process exit code: 0 on success; main()'s return value if it
     *         returns an int; 1 if an exception escaped (it is reported on
     *         stderr as `file:line: Class: message`).
     */
    int run(const std::vector<std::string>& args);

    /**
     * @brief Call any callable value: user function, builtin, bound method,
     *        or exception class (which constructs an exception instance).
     * @param fn    The callee.
     * @param args  Positional arguments.
     * @param kw    Keyword arguments (user functions only, for now).
     * @param line  Source line of the call, for error messages.
     * @throws DolphinError `TypeError` if @p fn isn't callable or the
     *         arguments don't fit; anything the callee raises.
     */
    Value callValue(const Value& fn, Args& args, Kwargs& kw, int line);

    /**
     * @brief Raise a Dolphin exception from C++.
     *
     * Use this for every runtime error, including from builtins, so that
     * Dolphin `try` / `except` can catch it.
     */
    [[noreturn]] void raise(const std::string& cls, const std::string& msg, int line) const;

    /**
     * @brief Apply an f-string format spec, e.g. `>8.2f`, `,d`, `^10`, `04x`.
     *
     * Grammar: `[[fill]align][sign][0][width][,][.precision][type]`, a
     * subset of Python's format mini-language. Types: `d x X o b f F e E g G s %`.
     * @throws DolphinError `ValueError` for a bad spec or a type mismatch.
     */
    std::string format(const Value& v, const std::string& spec, int line) const;

private:
    /// A name → value table: globals, one function's locals, or the builtins.
    using Scope = std::unordered_map<std::string, Value>;
    /// @brief A call dispatched from a frame that hasn't been joined yet.
    struct Pending {
        uint64_t seq;        ///< Dispatch order (TaskCtx::nextSeq); lower = earlier in program order.
        FutureP fut;
        std::string name;    ///< Variable the result is assigned to; empty for `f(x)` as a statement.
        Value prev;          ///< That variable's value before the assignment, restored if the call fails.
        bool hadPrev = false;
    };
    /// @brief Execution context for one function activation (or the module).
    struct Frame {
        Scope* vars;                   ///< Where assignments go.
        bool isModule = false;         ///< Module-level code never dispatches (it may write globals).
        std::vector<Pending> pending;  ///< Dispatched calls not yet joined, in dispatch order.
        explicit Frame(Scope* v, bool mod = false) : vars(v), isModule(mod) {}
    };
    /**
     * @brief Per-task state: the stack of frames this task is executing.
     *
     * The main program is one task; each call or batch run on the pool is
     * another. Kept in a thread_local pointer, saved and restored when a
     * waiting thread helps run someone else's task.
     */
    struct TaskCtx {
        std::vector<Frame*> frames;   ///< Innermost last.
        uint64_t nextSeq = 0;         ///< Source of Pending::seq.
        size_t pendingCount = 0;      ///< Total pending across frames (fast path for barrier()).
        /// Calls abandoned after a failure (cancelled, but possibly still running).
        /// Nobody wants their results, but barrier() waits for them, so nothing
        /// they might be reading is mutated under them.
        std::vector<FutureP> detached;
    };
    /// @brief Thrown by barrier() when the failure that must be raised belongs
    ///        to an outer frame. Unwinds (uncatchable by Dolphin `try`) to that frame.
    struct DeferredFailure { Frame* target; };
    /// @brief How a statement finished. Non-Normal values unwind to the enclosing loop or function.
    enum class Flow { Normal, Return, Break, Continue };

    /// @name Statement execution
    /// @{
    /// @brief Run statements in order until one doesn't finish Normal.
    /// @param[out] ret  Set by a `return` statement.
    Flow execBlock(const Block& b, Frame& f, Value& ret);
    /// @brief Run one statement.
    Flow exec(const Stmt& s, Frame& f, Value& ret);
    /// @brief Bind @p v to an assignment target (name, list element).
    void assign(const Expr& target, const Value& v, Frame& f);
    /// @}

    /// @name Expression evaluation
    /// @{
    Value eval(const Expr& e, Frame& f);                         ///< Evaluate an expression.
    Value lookup(const std::string& name, Frame& f, int line);   ///< locals → globals → builtins, else NameError.
    /// @brief Arithmetic, bitwise and sequence operators, with Python semantics
    ///        (floor division, sign of modulo) and int64 overflow detection.
    Value binary(Op op, const Value& a, const Value& b, int line) const;
    /// @brief One link of a comparison chain, including `in` / `not in`.
    bool compare(Op op, const Value& a, const Value& b, int line) const;
    /// @brief `obj[i]` or `obj[lo:hi]` on str, bytes or list.
    Value subscript(const Value& obj, const Expr& idx, Frame& f, int line);
    /// @brief `obj.name`: a BoundMethod, or a field of an exception.
    Value attribute(const Value& obj, const std::string& name, int line) const;
    /// @}

    /// @name Calls
    /// @{
    Value callMethod(const Value& self, Method m, Args& args, int line);        ///< Built-in methods of str, bytes, list, dict.
    /// @brief Is @p m a method of @p obj's type? (What makes `obj.m` valid.)
    static bool hasMethod(const Value& obj, Method m);
    Value callFunction(const Def& d, Args& args, Kwargs& kw, int line);        ///< Bind parameters, run a user function body.
    /// @}

    /// @name Parallel execution
    /// @{
    /// @brief Can this call be dispatched from frame @p f? (parallel-safe callee, not module level, pool present)
    const Def* dispatchTarget(const Call& c, const Frame& f) const;
    /// @brief Find assignments worth dispatching (see overlapAssigns_), and
    ///        create a CostStat for every parallel-safe function. Run once, at construction.
    void findOverlap(const Block& b);
    /// @brief Evaluate a call's arguments on this thread (forcing any futures they read).
    void evalArgs(const Call& c, Frame& f, Args& args, Kwargs& kw);
    /// @brief Try to run `d(args)` on the pool. Returns null if refused (caller runs it inline).
    FutureP dispatch(const Def& d, Args args, Kwargs kw, int line);
    /// @brief Run @p body as a separate task with its own TaskCtx (on whichever
    ///        thread calls this). Returns the operations it executed (cost model).
    uint64_t runAsTask(const std::function<void()>& body);
    /// @brief Add a joined task's operations to the current call's work count (once).
    void absorbOps(Future& fut);
    /// @brief Record a dispatched call in frame @p f, binding @p name (if any) to the future.
    void addPending(Frame& f, FutureP fut, const std::string& name);
    /// @brief Wait for a future, helping with queued work meanwhile.
    void waitFuture(Future& fut);
    /// @brief Read a variable's value, waiting for it if it's a future.
    Value force(const Value& v, Frame& f);
    /// @brief Wait until all of @p futs are done or one has failed. Returns the
    ///        failed one that returned first, or null if all succeeded.
    FutureP waitAllOrFirstFailure(const std::vector<FutureP>& futs);
    /**
     * @brief Give up on frame @p f's pending calls after a failure.
     * @param f        The frame whose calls are abandoned.
     * @param failSeq  Dispatch seq of the failed call (UINT64_MAX for an error
     *                 raised inline just now). Calls dispatched before it are
     *                 allowed to finish; calls from it on are cancelled and detached.
     * @param err      The failure. The result is whichever failure returned first.
     * @return The error to raise. Assignment targets of failed and cancelled calls are restored.
     */
    DolphinError abandonFrame(Frame& f, uint64_t failSeq, DolphinError err);
    /// @brief Cancel and detach every pending call of @p f (the frame is being unwound).
    void detachAll(Frame& f);
    /// @brief Join @p f. If any call failed, throw the first failure to return.
    void settle(Frame& f);
    /// @brief After @p err was raised in @p f: let earlier calls finish; a failure among them that returned first wins.
    DolphinError preferEarlier(Frame& f, const DolphinError& err);
    /// @brief Before an effect or a mutation: wait for every call this task has in flight.
    void barrier();
    /// @brief Can every call in @p e run early and out of order? (used for comprehension conditions/arguments)
    bool exprIsReorderable(const Expr& e) const;
    /// @brief The parallel path for `[f(x) for x in xs if c]`. Returns false if it doesn't apply.
    bool parallelComprehension(const ListComp& c, Frame& f, Value& out);
    /// @}

    /// @brief Materialize the items of a list, str (code points) or bytes (ints).
    std::vector<Value> iterate(const Value& v, int line) const;
    /// @brief Does an exception of class @p errCls match a handler for @p handlerCls,
    ///        directly or through a parent class?
    bool excMatches(const std::string& handlerCls, const std::string& errCls) const;

    /// @name Builtin registration (builtins.cpp)
    /// @{
    void installBuiltins();  ///< Register every builtin function and exception class.
    /// @brief Register one builtin. Aborts if @p name isn't in builtinNeeds().
    void addBuiltin(const std::string& name, std::function<Value(Interp&, Args&, int)> fn);
    /// @brief Register an exception class under @p parent.
    void addException(const std::string& name, const std::string& parent);
    /// @}

    const Module& mod_;                                    ///< The program.
    Scope globals_;                                        ///< Module-level names.
    Scope builtins_;                                       ///< Builtin functions and exception classes.
    std::unordered_map<std::string, std::string> excParent_;  ///< Exception class → parent class.
    std::unique_ptr<ThreadPool> pool_;                     ///< Null when single-threaded.
    std::unique_ptr<OutputChannel> out_;                   ///< Printer thread for stdout; null when synchronous.
    std::atomic<uint64_t> inlineCalls_{0};                 ///< Parallel-safe calls that ran inline (pool busy).
    static thread_local TaskCtx* tlCtx_;                   ///< The task this thread is currently executing.
    /// `x = f(...)` statements whose next statement doesn't read x: there's
    /// work to overlap with the call. Others run inline, since dispatching and
    /// immediately waiting only adds overhead. Read-only after construction.
    std::unordered_set<const Stmt*> overlapAssigns_;

    /// Work, rate, submit-cost and wake-latency measurements behind every dispatch decision
    /// (costmodel.hpp). Tracks every parallel-safe function, registered at construction.
    Topology topo_;   ///< Set when the thread count is automatic.
    CostModel cost_;
    /// @brief Is a single call to @p d expensive enough to be worth a thread?
    bool worthDispatching(const Def& d) const;
    /// @brief Dispatch `d(args)` if it's worth it and the pool has room; null means run it inline.
    ///        Logs the decision when tracing.
    FutureP tryDispatch(const Def& d, Args& args, Kwargs& kw, int line);
    /// @brief Write one line to stderr, prefixed with `trace:` and the thread (only when tracing).
    void trace(const std::string& msg) const;
    bool trace_ = false;
    // Per-thread state (current task, call depth, exceptions being handled)
    // lives in thread_local variables in interp.cpp.
};

} // namespace dolphin
