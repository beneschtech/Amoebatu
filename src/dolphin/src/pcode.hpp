/**
 * @file pcode.hpp
 * @brief Dolphin p-code: instruction set, compiled function format, compiler
 *        and disassembler (design: doc/PCODE.md).
 *
 * Every function body compiles to p-code and runs on Interp::execPcode()
 * when `--engine pcode` is given. Module-level code still runs on the
 * tree-walker (it runs once).
 *
 * ### Parallel execution (P1b)
 * The same decisions the tree-walker makes, made at compile time where they
 * can be:
 * - `x = f(...)` / `f(...)` that the tree-walker would try to dispatch become
 *   SPAWN. At run time the cost model still decides; if it dispatches, the
 *   local's register holds a Future, pending in the frame by register (so a
 *   failure can restore it, decision 17).
 * - Only locals a SPAWN writes can hold a Future, so only their reads get a
 *   FORCE. Every other read costs nothing extra.
 * - A comprehension the tree-walker could run as a parallel map compiles
 *   twice: JMPNOPOOL picks the sequential loop when there is no pool (so
 *   `--threads 1` evaluates in exactly the tree-walker's order); otherwise the
 *   argument lists are collected first and PMAP runs them with the shared
 *   batching code (Interp::parallelMap()).
 *
 * ### Encoding
 * Fixed 32-bit words, as in Lua 5:
 *
 * | Format | Bits | Fields |
 * |---|---|---|
 * | ABC | op:8 A:8 B:8 C:8 | three registers (or small operands) |
 * | ABx | op:8 A:8 Bx:16 | register + table index |
 * | AsBx | op:8 A:8 sBx:16 | register + signed jump offset |
 *
 * Jump offsets are relative to the instruction after the jump.
 *
 * ### Registers
 * A call gets `nregs` registers. Parameters are r0..r(n-1), then the
 * function's other locals (the resolver's slots), then temporaries. A local
 * may be unassigned (reading it raises UnboundLocalError), so each register
 * has a "bound" flag: every instruction that writes rA sets it, and `CHKB`
 * checks it before a local is read. Parameters are always bound, so they
 * need no check.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ast.hpp"
#include "value.hpp"

namespace dolphin {

/// @brief Opcodes. Comments give the format and meaning.
enum class OpCode : uint8_t {
    MOVE,       ///< AB    rA = rB
    LOADK,      ///< ABx   rA = K[Bx]
    LOADNIL,    ///< A     rA = None
    LOADTRUE,   ///< A     rA = True
    LOADFALSE,  ///< A     rA = False
    CHKB,       ///< A     raise UnboundLocalError if local rA was never assigned
    GETG,       ///< ABx   rA = global named N[Bx] (NameError "not defined yet" if absent)
    // Binary operators: rA = rB op rC (Interp::binary). Order matches Op::Add..Op::Shr.
    ADD, SUB, MUL, DIV, FDIV, MOD, POW, BAND, BOR, BXOR, SHL, SHR,
    // Comparisons: rA = bool(rB op rC) (Interp::compare). Order matches Op::Eq..Op::NotIn.
    EQ, NE, LT, LE, GT, GE, IN, NOTIN,
    NOT,        ///< AB    rA = not truthy(rB)
    NEG,        ///< AB    rA = -rB
    POS,        ///< AB    rA = +rB
    INV,        ///< AB    rA = ~rB
    JMP,        ///< sBx   pc += sBx
    JMPIF,      ///< AsBx  if truthy(rA): pc += sBx
    JMPIFNOT,   ///< AsBx  if not truthy(rA): pc += sBx
    ITER,       ///< AB    iterator slot A = snapshot of rB's items (Interp::iterate)
    FORNEXT,    ///< AB    if iterator B has an item: rA = it, skip the next instruction;
                ///<       else fall through to it (always a JMP out of the loop)
    NEWLIST,    ///< ABC   rA = [rB .. r(B+C-1)]
    APPEND,     ///< AB    append rB to the fresh list rA (comprehensions; no barrier needed)
    NEWDICT,    ///< A     rA = {}
    DICTSET,    ///< ABC   fresh dict rA[rB] = rC (dict displays; no barrier needed)
    INDEX,      ///< ABC   rA = rB[rC]
    INDEXK,     ///< ABx   rA = rA[K[Bx]], with K[Bx]'s dict key precomputed (Code::constKeys)
    SLICE,      ///< ABC   rA = rB[rC : r(C+1)]  (None = omitted bound)
    SETINDEX,   ///< ABC   rA[rB] = rC  (barrier first, as the tree-walker does)
    ATTR,       ///< ABx   rA = attribute N[Bx] of rA
    FORMAT,     ///< ABx   rA = format(rA, spec N[Bx]) (empty spec: str(rA))
    CONCAT,     ///< ABC   rA = rB + ... + r(B+C-1), all strings, one allocation
    CALL,       ///< ABx   rA = call site Bx (Code::calls)
    RETURN,     ///< A     return rA
    RETURNNIL,  ///<       return None
    RAISE,      ///< A     raise rA (exception value or class)
    RERAISE,    ///<       bare `raise`: re-raise the exception being handled
    EXCMATCH,   ///< Bx    does the caught exception match class N[Bx]? Yes: skip the next
                ///<       instruction. No: fall through to it (a JMP to the next handler).
    EXCBIND,    ///< A     rA = the caught exception, as a value (`except E as e`)
    HPUSH,      ///<       start handling the caught exception (for bare `raise`)
    HPOP,       ///<       finished handling it
    RETHROW,    ///<       no handler matched: raise the caught exception again
    SETTLE,     ///<       join point (try entry/exit): wait for this frame's calls
    SPAWN,      ///< ABx   like CALL, but the call may be dispatched: rA may receive a Future
                ///<       (pending in this frame). Call sites with `discard` keep no result.
    FORCE,      ///< A     if rA holds a Future, wait for it and replace it with its value
    JMPNOPOOL,  ///< sBx   if there's no thread pool: pc += sBx (to a comprehension's sequential form)
    PMAP,       ///< ABx   rA = parallel map of call site Bx over the argument lists in r(site.self)
    // Fused compare-and-branch (--opt 1): compare rA with rB; if the result equals C (0/1),
    // jump by the offset in the next word (a JMP), else skip it. One dispatch, no bool value.
    // Order matches Op::Eq..Op::NotIn.
    JEQ, JNE, JLT, JLE, JGT, JGE, JIN, JNOTIN,
};

/// @brief Mnemonic of an opcode, for the disassembler.
const char* opName(OpCode op);

/// @brief Instruction word helpers.
namespace ins {
inline uint32_t abc(OpCode op, unsigned a, unsigned b = 0, unsigned c = 0) {
    return static_cast<uint32_t>(op) | (a & 0xFF) << 8 | (b & 0xFF) << 16 | (c & 0xFF) << 24;
}
inline uint32_t abx(OpCode op, unsigned a, unsigned bx) { return static_cast<uint32_t>(op) | (a & 0xFF) << 8 | (bx & 0xFFFF) << 16; }
inline uint32_t asbx(OpCode op, unsigned a, int sbx) {
    return static_cast<uint32_t>(op) | (a & 0xFF) << 8 | (static_cast<uint32_t>(sbx) & 0xFFFF) << 16;
}
inline OpCode op(uint32_t w) { return static_cast<OpCode>(w & 0xFF); }
inline unsigned a(uint32_t w) { return (w >> 8) & 0xFF; }
inline unsigned b(uint32_t w) { return (w >> 16) & 0xFF; }
inline unsigned c(uint32_t w) { return w >> 24; }
inline unsigned bx(uint32_t w) { return w >> 16; }
inline int sbx(uint32_t w) { return static_cast<int16_t>(w >> 16); }
} // namespace ins

/// @brief A call, decided at compile time as far as possible. Instructions refer to it by index.
struct CallSite {
    enum class Kind : uint8_t {
        Builtin,    ///< builtin function; @c builtin
        Function,   ///< module-level function @c def, bound to global @c name
        Method,     ///< built-in method @c method on register @c self (attribute name @c name)
        Value,      ///< whatever value register @c self holds
    };
    Kind kind = Kind::Value;
    const Builtin* builtin = nullptr;
    Value builtinValue;                 ///< Keeps @c builtin alive.
    const Def* def = nullptr;
    Method method = Method::None;
    std::string name;                   ///< Global name (Function) or attribute name (Method).
    uint8_t self = 0;                   ///< Receiver (Method) or callee (Value) register.
    std::vector<uint8_t> args;          ///< Argument registers: positional, then keyword values.
    std::vector<std::string> kwNames;   ///< Names of the trailing keyword arguments.
    bool discard = false;               ///< SPAWN of a call statement: no result to keep.
};

/// @brief A handler range (exception table entry).
struct ExcEntry {
    uint32_t start, end;   ///< Instructions [start, end) covered.
    uint32_t handler;      ///< Where to jump (Try entries).
    bool handlerBody;      ///< True: a handler body; unwinding out of it ends handling (HPOP).
};

/// @brief One compiled function.
struct PCode {
    const Def* def = nullptr;
    unsigned nparams = 0;            ///< Always-bound registers r0..r(nparams-1).
    unsigned nregs = 0;              ///< Registers needed.
    unsigned niters = 0;             ///< Iterator slots needed (loop nesting depth).
    std::vector<uint32_t> code;
    std::vector<int> lines;          ///< Source line per instruction.
    std::vector<Value> consts;       ///< K[]. String literals are uncounted references into the AST.
    std::vector<std::string> constKeys;   ///< Precomputed dict keys for INDEXK (parallel to consts; empty if none).
    std::vector<uint64_t> constHashes;    ///< Dict::hashKey() of each constKeys entry.
    std::vector<std::string> names;  ///< N[]: global, attribute and exception class names, format specs.
    std::vector<std::string> regNames;    ///< Local names per register (for errors and the disassembler).
    std::vector<CallSite> calls;
    std::vector<ExcEntry> excTable;  ///< Innermost entries first.

    /// Per GETG instruction's name: the global's value slot, found on first use.
    /// Global table nodes never move or disappear (std::unordered_map keeps
    /// references stable; globals are never erased), so later reads skip the
    /// hash lookup. Atomic: functions run on several threads.
    std::unique_ptr<std::atomic<const Value*>[]> globalCache;
    /// Per call site (Function kind): set once the callee's global has been seen bound.
    std::unique_ptr<std::atomic<bool>[]> calleeBound;
};

/// @brief What the compiler needs from the interpreter.
struct CompileHooks {
    /// The value of a builtin name (builtin function or exception class).
    std::function<Value(const std::string&)> builtin;
    /// Would the tree-walker try to dispatch this statement (`x = f(...)` or `f(...)`
    /// whose result isn't needed by the next statement)? See Interp::findOverlap().
    std::function<bool(const Stmt&)> overlap;
    /// May this expression be evaluated ahead of its turn (no effects, no mutation)?
    /// Decides whether a comprehension can become a parallel map.
    std::function<bool(const Expr&)> reorderable;
    /// Optimize (`--opt 1`, the default): drop provably redundant CHKBs, compile
    /// conditions as fused compare-and-branch. `--opt 0` keeps the plain form,
    /// so the two can be compared.
    bool optimize = true;
};

/**
 * @brief Compile function @p d to p-code.
 * @return Null if the function can't be compiled (more than 256 registers or
 *         65,536 table entries); the caller then runs it on the tree-walker.
 */
std::unique_ptr<PCode> compileFunction(const Def& d, const CompileHooks& hooks);

/// @brief Human-readable listing of @p p (`dolphin --disasm`).
std::string disassemble(const PCode& p);

} // namespace dolphin
