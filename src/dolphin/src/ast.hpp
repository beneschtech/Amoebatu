/**
 * @file ast.hpp
 * @brief The abstract syntax tree: what the parser produces and every later
 *        stage (analyzer, interpreter, and later the IR generator) consumes.
 *
 * Design:
 *  - Two node families, Expr and Stmt, each with a `K` kind enum. Code
 *    switches on `k` and `static_cast`s to the concrete node. That's
 *    deliberate instead of a visitor or `std::variant`: it keeps each pass a
 *    single readable switch, and adding a pass doesn't touch the node
 *    classes.
 *  - Children are owned through std::unique_ptr (ExprP / StmtP), so the whole
 *    tree is owned by its Module and freed with it.
 *  - Every node records the source line it started on, for error messages.
 *  - Nodes are immutable after parsing, with one exception: Def::needs,
 *    which the analyzer fills in.
 */
#pragma once
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace dolphin {

// ============================================================ expressions

/// @brief Base class of all expression nodes.
struct Expr {
    /// @brief Concrete node type; tells you which subclass to `static_cast` to.
    enum class K { Int, Float, Str, FStr, Bytes, Bool, None, Name, Unary, Binary, BoolOp, Compare,
                   Call, Index, Slice, Attr, List, ListComp, Dict };
    K k;       ///< Node kind.
    int line;  ///< Source line.
    Expr(K k, int line) : k(k), line(line) {}
    virtual ~Expr() = default;
};
using ExprP = std::unique_ptr<Expr>;  ///< Owning pointer to an expression.

/**
 * @brief Operator, decoded once by the parser.
 *
 * Nodes keep the spelling too (for `--ast` and error messages), but the
 * interpreter dispatches on this code. Comparing operator strings on every
 * evaluation cost about a tenth of all instructions.
 */
enum class Op : uint8_t {
    Add, Sub, Mul, Div, FloorDiv, Mod, Pow, BitAnd, BitOr, BitXor, Shl, Shr,   ///< Binary arithmetic and bitwise.
    Eq, Ne, Lt, Le, Gt, Ge, In, NotIn,                                         ///< Comparisons.
    Not, Neg, Pos, Invert,                                                     ///< Unary (`not - + ~`).
    And, Or,                                                                   ///< Short-circuit.
};
/// @brief Binary/comparison/boolean operator for a spelling (`+`, `not in`, `and`, ...).
///        Unary operators: see unaryOp().
Op binaryOp(const std::string& spelling);
/// @brief Unary operator for a spelling: `not`, `-`, `+` or `~`.
Op unaryOp(const std::string& spelling);
/// @brief The operator's source spelling.
const char* opSpelling(Op op);

/**
 * @brief Built-in method, decoded once by the parser from `obj.name`.
 *
 * Method calls dispatch on this code instead of looking the name up and
 * comparing strings on every call. Method::None means "not a built-in method
 * name" (the attribute is then looked up the slow way, which reports errors).
 */
enum class Method : uint8_t {
    None,
    upper, lower, strip, lstrip, rstrip, split, join,
    startswith, endswith, find, replace, count, isdigit,
    isalpha, isspace, encode, decode, hex, append, pop,
    extend, index, get, keys, values,
    // Stream (stream.hpp): IO or Network, see isStreamMethod(). read/write/close
    // are shared with Archive, whose receivers the analyzer tracks the same way.
    open, read, readline, write, seek, tell, close, count_lines, set_timeout,
    // JSON (json.hpp): pure.
    value, toString, prettyPrint,
    // Listener (listener.hpp): Network. set_timeout above is shared with Stream.
    bind, threads, per_connection, limit, drain, start, stop, wait,
    // Path (path.hpp), pure: they only compute new paths.
    with_suffix, with_name, is_absolute, joinpath,
    // Path, touching the filesystem: IO, see isPathIOMethod().
    exists, is_dir, is_file, iterdir, glob, rglob, mkdir, rmdir, unlink, rename,
    stat, chmod, touch, resolve, read_bytes, read_text, write_bytes, write_text,
    // Archive (archive.hpp): IO, see isArchiveMethod().
    namelist, infolist, extractall, writestr,
};
/// @brief Number of Method values (for per-type method sets).
constexpr unsigned kMethodCount = static_cast<unsigned>(Method::writestr) + 1;
/// @brief Is @p m a method only Listener has? Those need Network.
inline bool isListenerMethod(Method m) { return m >= Method::bind && m <= Method::wait; }
/// @brief Is @p m a method of Stream? Those have needs (IO or Network) that depend on the receiver.
inline bool isStreamMethod(Method m) { return m >= Method::open && m <= Method::set_timeout; }
/// @brief Is @p m a Path method that touches the filesystem? Those need IO.
inline bool isPathIOMethod(Method m) { return m >= Method::exists && m <= Method::write_text; }
/// @brief Is @p m a method only Archive has? Those need IO.
inline bool isArchiveMethod(Method m) { return m >= Method::namelist && m <= Method::writestr; }
/// @brief Does calling @p m have an effect (any needs)? Every other method is pure.
inline bool isEffectMethod(Method m) {
    return isStreamMethod(m) || isListenerMethod(m) || isPathIOMethod(m) || isArchiveMethod(m);
}
/// @brief The Method named @p name, or Method::None.
Method methodByName(const std::string& name);
/// @brief A method's name (empty for Method::None).
const char* methodName(Method m);

/// @brief Integer literal. Range is int64.
struct IntLit : Expr { int64_t v; IntLit(int l, int64_t v) : Expr(K::Int, l), v(v) {} };
/// @brief Floating-point literal (IEEE double).
struct FloatLit : Expr { double v; FloatLit(int l, double v) : Expr(K::Float, l), v(v) {} };
/// @brief String literal; @c v is UTF-8 with escapes already decoded.
struct Str;   // value.hpp
/**
 * @brief String literal; @c v is UTF-8 with escapes already decoded.
 *
 * The runtime string is built once, here, and every evaluation hands out an
 * uncounted reference to it (Value::borrowedStr()). Allocating a fresh string
 * per evaluation was 1.6 million allocations on the METAR benchmark, and a
 * shared counted copy would have every thread bumping the same counter.
 * Values from literals therefore must not outlive the AST (the Module), which
 * owns every Interp.
 */
struct StrLit : Expr {
    std::string v;
    std::shared_ptr<const Str> str;   ///< The runtime string (owned here).
    StrLit(int l, std::string v);     // ast.cpp: needs the complete Str
};
/// @brief bytes literal; @c v holds raw bytes.
struct BytesLit : Expr { std::string v; BytesLit(int l, std::string v) : Expr(K::Bytes, l), v(std::move(v)) {} };
/// @brief `True` or `False`.
struct BoolLit : Expr { bool v; BoolLit(int l, bool v) : Expr(K::Bool, l), v(v) {} };
/// @brief `None`.
struct NoneLit : Expr { explicit NoneLit(int l) : Expr(K::None, l) {} };
struct Def;

/**
 * @brief What a name refers to, decided by the resolver (resolver.hpp).
 *
 * Resolution is static and follows Python's rule: a name assigned anywhere
 * in a function body is local to the whole function.
 */
enum class Bind {
    Unresolved,  ///< Not yet resolved (only before resolveNames() runs).
    Local,       ///< Parameter or local variable of the enclosing function; see Name::slot.
    Global,      ///< Module-level variable.
    Function,    ///< Module-level name bound only by a single `def`; see Name::def.
    Builtin,     ///< Builtin function (see builtinNeeds()).
    Exception,   ///< Builtin exception class.
};

/// @brief Short name of a binding kind, for `--ast`.
const char* bindName(Bind b);

/// @brief A variable or function name.
struct Name : Expr {
    std::string id;
    Bind bind = Bind::Unresolved;  ///< Set by the resolver.
    int slot = -1;                 ///< Local slot number when bind == Local (parameters come first).
    /// The function this name calls, when statically known: set for Bind::Function,
    /// and for a Local bound only by a nested `def`. Lets the analyzer follow the call.
    const Def* def = nullptr;
    unsigned needs = 0;            ///< For Bind::Builtin: the builtin's declared needs.
    Name(int l, std::string id) : Expr(K::Name, l), id(std::move(id)) {}
};

/**
 * @brief One piece of an f-string: either literal text or an expression field.
 *
 * `f"T={t:.1f}K"` becomes three parts: text `"T="`, field `t` with spec
 * `".1f"`, text `"K"`.
 */
struct FStrPart {
    std::string text;  ///< Literal text; used only when @c expr is null.
    ExprP expr;        ///< Field expression, or null for a text part.
    std::string spec;  ///< Format spec after `:` (see Interp::format()); empty for none.
};
/// @brief f-string: a sequence of text and expression parts.
struct FStr : Expr { std::vector<FStrPart> parts; explicit FStr(int l) : Expr(K::FStr, l) {} };

/// @brief Prefix operator: `-x`, `+x`, `~x`, `not x`.
struct Unary : Expr {
    std::string op;  ///< One of `-`, `+`, `~`, `not`.
    Op code;         ///< Decoded @c op.
    ExprP operand;
    Unary(int l, std::string op, ExprP e)
        : Expr(K::Unary, l), op(std::move(op)), code(unaryOp(this->op)), operand(std::move(e)) {}
};
/// @brief Arithmetic or bitwise infix operator, e.g. `a + b`, `a ** b`, `a << b`.
struct Binary : Expr {
    std::string op;  ///< Operator spelling: `+ - * / // % ** & | ^ << >>`.
    Op code;         ///< Decoded @c op.
    ExprP lhs, rhs;
    Binary(int l, std::string op, ExprP a, ExprP b)
        : Expr(K::Binary, l), op(std::move(op)), code(binaryOp(this->op)), lhs(std::move(a)), rhs(std::move(b)) {}
};
/// @brief Short-circuit `and` / `or`. Returns one of its operands, as in Python.
struct BoolOp : Expr {
    std::string op;  ///< `and` or `or`.
    Op code;         ///< Decoded @c op.
    ExprP lhs, rhs;
    BoolOp(int l, std::string op, ExprP a, ExprP b)
        : Expr(K::BoolOp, l), op(std::move(op)), code(binaryOp(this->op)), lhs(std::move(a)), rhs(std::move(b)) {}
};
/**
 * @brief Comparison chain: `a < b <= c` means `a < b and b <= c`, with each
 *        operand evaluated once.
 */
struct Compare : Expr {
    ExprP first;  ///< Leftmost operand.
    /// Each following (operator, operand) pair. Operators: `== != < <= > >= in` and `not in`.
    std::vector<std::pair<std::string, ExprP>> rest;
    std::vector<Op> ops;   ///< Decoded operators, parallel to @c rest. Use add() to append.
    Compare(int l, ExprP f) : Expr(K::Compare, l), first(std::move(f)) {}
    /// @brief Append an (operator, operand) pair.
    void add(std::string op, ExprP e) {
        ops.push_back(binaryOp(op));
        rest.emplace_back(std::move(op), std::move(e));
    }
};
/// @brief Function or method call with positional and keyword arguments.
struct Call : Expr {
    ExprP fn;                                            ///< What's being called.
    std::vector<ExprP> args;                             ///< Positional arguments, in order.
    std::vector<std::pair<std::string, ExprP>> kwargs;   ///< `name=value` arguments, in order.
    Call(int l, ExprP f) : Expr(K::Call, l), fn(std::move(f)) {}
};
/// @brief `lo:hi` inside a subscript. Only valid as Index::index.
struct Slice : Expr {
    ExprP lo, hi;  ///< Either may be null (omitted bound).
    explicit Slice(int l) : Expr(K::Slice, l) {}
};
/// @brief Subscript `obj[index]` or slice `obj[lo:hi]` (index is then a Slice).
struct Index : Expr {
    ExprP obj, index;
    /// When @c index is a str or int literal: its dict key (dictKey()), so
    /// `r["station"]` on a dict looks up without building anything.
    std::string constKey;
    uint64_t constHash = 0;   ///< Dict::hashKey(constKey), computed once.
    bool hasConstKey = false;
    Index(int l, ExprP o, ExprP i);   // ast.cpp
};
/// @brief Attribute access `obj.name`. Today only methods and exception fields.
struct Attr : Expr {
    ExprP obj;
    std::string name;
    Method method;   ///< methodByName(name): Method::None if it isn't a built-in method name.
    Attr(int l, ExprP o, std::string n)
        : Expr(K::Attr, l), obj(std::move(o)), name(std::move(n)), method(methodByName(this->name)) {}
};
/// @brief List display `[a, b, c]`.
struct ListLit : Expr { std::vector<ExprP> elems; explicit ListLit(int l) : Expr(K::List, l) {} };
/// @brief Dict display `{k: v, ...}`. Keys and values are evaluated left to right.
struct DictLit : Expr {
    std::vector<std::pair<ExprP, ExprP>> items;  ///< (key, value) in source order.
    explicit DictLit(int l) : Expr(K::Dict, l) {}
};
/**
 * @brief List comprehension `[elt for var in iter if cond]`.
 *
 * This is the task-parallel map of the language (LANGUAGE.md §3.3): from M2
 * on, each evaluation of @c elt that is a pure call becomes a future.
 */
struct ListComp : Expr {
    ExprP elt;        ///< Element expression.
    std::string var;  ///< Loop variable.
    int slot = -1;    ///< Local slot of @c var (-1 at module level, where it's a global).
    ExprP iter;       ///< Iterable.
    ExprP cond;       ///< Optional filter; null if absent.
    explicit ListComp(int l) : Expr(K::ListComp, l) {}
};

// ============================================================= statements

struct Stmt;
using StmtP = std::unique_ptr<Stmt>;  ///< Owning pointer to a statement.
using Block = std::vector<StmtP>;     ///< A statement sequence (one indented block).

/// @brief Base class of all statement nodes.
struct Stmt {
    /// @brief Concrete node type; tells you which subclass to `static_cast` to.
    enum class K { Expr, Assign, AugAssign, If, While, For, Def, Return, Break, Continue, Pass, Try, Raise };
    K k;       ///< Node kind.
    int line;  ///< Source line.
    Stmt(K k, int line) : k(k), line(line) {}
    virtual ~Stmt() = default;
};

/// @brief An expression evaluated for its effect, e.g. a call.
struct ExprStmt : Stmt { ExprP e; ExprStmt(int l, ExprP e) : Stmt(K::Expr, l), e(std::move(e)) {} };
/// @brief `target = value` or annotated `name: type = value`.
struct Assign : Stmt {
    ExprP target;  ///< Name, Index or Attr.
    ExprP annot;   ///< Type annotation, or null. Parsed, not yet checked.
    ExprP value;
    Assign(int l) : Stmt(K::Assign, l) {}
};
/// @brief Augmented assignment, e.g. `off += n`.
struct AugAssign : Stmt {
    ExprP target;
    std::string op;      ///< Operator without the `=`, e.g. `+`.
    Op code = Op::Add;   ///< Decoded @c op (set with @c op by the parser).
    ExprP value;
    AugAssign(int l) : Stmt(K::AugAssign, l) {}
};
/// @brief `if` / `elif` chain with optional `else`.
struct If : Stmt {
    std::vector<std::pair<ExprP, Block>> branches;  ///< (condition, body) for `if` then each `elif`.
    Block orelse;                                   ///< `else` body; empty if none.
    explicit If(int l) : Stmt(K::If, l) {}
};
/// @brief `while cond:` loop.
struct While : Stmt { ExprP cond; Block body; explicit While(int l) : Stmt(K::While, l) {} };
/// @brief `for var in iter:` loop.
struct For : Stmt {
    std::string var;
    int slot = -1;   ///< Local slot of @c var (-1 at module level).
    ExprP iter;
    Block body;
    explicit For(int l) : Stmt(K::For, l) {}
};

/**
 * @brief What a function needs from the system, as inferred by the analyzer
 *        (LANGUAGE.md §3.1).
 *
 * A bitmask: a function's needs are the OR of everything it calls. A pure
 * function (NeedNone) can be dispatched early, reordered, retried or cached.
 * The other bits tell the scheduler what ordering or which node capability
 * the call requires.
 */
enum Need : unsigned {
    NeedNone = 0,        ///< Pure: no ordering constraints at all.
    NeedIO = 1,          ///< File or console IO; ordered with other IO in program order.
    NeedDatabase = 2,    ///< Database access; needs a node with that capability.
    NeedNetwork = 4,     ///< Outbound network.
    NeedLocalOnly = 8,   ///< Must run on the calling node.
    NeedGlobals = 16,    ///< Touches cluster globals (ownership-token cost).
    NeedAll = 31,        ///< Worst case: used for calls the analyzer can't see through.
};

/// @brief Render a needs mask as `pure` or a comma list like `IO,Network`.
std::string needsToString(unsigned n);

/// @brief One function parameter.
struct Param {
    std::string name;
    int slot = -1;  ///< Local slot; parameter i gets slot i.
    ExprP annot;  ///< Type annotation, or null. Parsed, not yet checked.
    ExprP dflt;   ///< Default value expression, or null.
};
/// @brief Function definition.
struct Def : Stmt {
    std::string name;
    std::vector<Param> params;
    ExprP ret;                   ///< Return annotation (`-> type`), or null.
    Block body;
    unsigned needs = NeedNone;   ///< Inferred by analyzeNeeds(); a Need bitmask.
    int numLocals = 0;           ///< Parameters + locals, set by the resolver.
    /// May this function mutate an object it didn't create (an argument, a
    /// global, or an alias of either)? Set by the analyzer. A function that
    /// does must run inline: on another thread it would race with its caller.
    bool mutatesArgs = false;
    /// Which parameters (by position) the function may mutate. Callers use it
    /// to know which of their arguments a call can change.
    std::vector<bool> mutatedParams;
    /// May mutate something other than its parameters: a global, or an object
    /// the analyzer couldn't trace. Callers then treat every argument as mutated.
    bool mutatesOther = false;
    /// Can a call to this function run on another thread? (pure and doesn't mutate its arguments)
    bool parallelSafe() const { return needs == NeedNone && !mutatesArgs; }
    int slot = -1;               ///< Slot of this def's name in the enclosing function (-1 at module level).
    explicit Def(int l) : Stmt(K::Def, l) {}
};
/// @brief `return [value]`. A null value returns None.
struct Return : Stmt { ExprP value; explicit Return(int l) : Stmt(K::Return, l) {} };
/// @brief `break`.
struct Break : Stmt { explicit Break(int l) : Stmt(K::Break, l) {} };
/// @brief `continue`.
struct Continue : Stmt { explicit Continue(int l) : Stmt(K::Continue, l) {} };
/// @brief `pass`.
struct Pass : Stmt { explicit Pass(int l) : Stmt(K::Pass, l) {} };
/// @brief One `except` clause of a Try.
struct Handler {
    std::string cls;  ///< Exception class name to match; empty for a bare `except:`.
    std::string as;   ///< Name bound to the exception (`as e`); empty if none.
    int asSlot = -1;  ///< Local slot of @c as (-1 at module level or if none).
    Block body;
    int line = 0;
};
/// @brief `try:` with one or more `except` clauses (no `finally` yet).
struct Try : Stmt {
    Block body;
    std::vector<Handler> handlers;  ///< Tried in order; first match wins.
    explicit Try(int l) : Stmt(K::Try, l) {}
};
/// @brief `raise expr`, or bare `raise` (null value) to re-raise inside a handler.
struct Raise : Stmt { ExprP value; explicit Raise(int l) : Stmt(K::Raise, l) {} };

/// @brief A parsed source file: the root of the tree.
struct Module {
    std::string file;  ///< File name, for error messages.
    Block body;        ///< Top-level statements in order.
};

/**
 * @brief Print the tree in the style of `clang -Xclang -ast-dump`
 *        (`dolphin --ast`): one node per line, children indented under
 *        `|-` / `` `- `` connectors, with line numbers, resolved bindings and
 *        inferred needs.
 *
 * The format is for humans and tests, not a stable interchange format.
 */
void dump(const Module& m, std::ostream& os);
/// @brief An expression rendered back as compact source text, e.g. `list[bytes]`.
///        Used for type annotations in the dump.
std::string exprToSource(const Expr& e);

} // namespace dolphin
