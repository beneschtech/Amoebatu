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
#include <vector>

#include "ast.hpp"
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
    /// @brief Bind to a module and register all builtins.
    explicit Interp(const Module& m);

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
    /// @brief Execution context for one block: which scope assignments go to.
    struct Frame { Scope* vars; };
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
    Value binary(const std::string& op, const Value& a, const Value& b, int line) const;
    /// @brief One link of a comparison chain, including `in` / `not in`.
    bool compare(const std::string& op, const Value& a, const Value& b, int line) const;
    /// @brief `obj[i]` or `obj[lo:hi]` on str, bytes or list.
    Value subscript(const Value& obj, const Expr& idx, Frame& f, int line);
    /// @brief `obj.name`: a BoundMethod, or a field of an exception.
    Value attribute(const Value& obj, const std::string& name, int line) const;
    /// @}

    /// @name Calls
    /// @{
    Value callMethod(const BoundMethod& m, Args& args, int line);              ///< Built-in methods of str, bytes, list.
    Value callFunction(const Def& d, Args& args, Kwargs& kw, int line);        ///< Bind parameters, run a user function body.
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
    std::vector<DolphinError> handling_;                   ///< Exceptions currently being handled (for bare `raise`).
    int depth_ = 0;                                        ///< Current call depth (recursion limit).
};

} // namespace dolphin
