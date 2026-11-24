/**
 * @file analyzer.hpp
 * @brief Whole-program needs analysis (LANGUAGE.md §3.1, decision 1).
 *
 * Runs over the complete program before anything executes, after
 * resolveNames() has bound every name. A function's
 * needs are the union of:
 *  - the declared needs of every builtin it calls (builtinNeeds()), and
 *  - the needs of every user function it calls, transitively.
 *
 * Recursion and mutual recursion are handled by iterating to a fixpoint over
 * the call graph.
 *
 * Calls the analyzer can't resolve statically (a function held in a
 * variable or parameter) are treated as worst case (NeedAll).
 * That's the same rule as for an unlabeled `extern` (decision 4): the program
 * stays correct, but those calls get ordered and kept local, so it runs
 * slower. Each such call produces a warning, so the programmer can see why.
 *
 * The analyzer also decides Def::mutatesArgs: whether a function may mutate
 * an object it didn't create itself. Mutations of the function's own fresh
 * objects (built from literals, comprehensions, slices or builtin results)
 * are fine; mutating a parameter, a global, or anything that might alias one
 * is not. A pure function that doesn't mutate its arguments is
 * Def::parallelSafe(): its calls can run on another thread.
 *
 * Current limitations:
 *  - Method calls on builtin types are assumed pure (true for all of today's
 *    str / bytes / list methods).
 *  - The report is keyed by function name. If two functions share a name
 *    (e.g. nested helpers in different functions), their entries are merged
 *    and a warning is issued. Def::needs on each node is always exact.
 */
#pragma once
#include <map>
#include <string>
#include <vector>

#include "ast.hpp"

namespace dolphin {

/// @brief Result of analyzeNeeds().
struct NeedsReport {
    std::map<std::string, unsigned> functions;  ///< Function name → Need bitmask.
    std::map<std::string, bool> mutatesArgs;    ///< Function name → Def::mutatesArgs.
    unsigned topLevel = NeedNone;               ///< Needs of the top-level code, including an implicit main() call.
    std::vector<std::string> warnings;          ///< Human-readable warnings (unresolved calls, duplicates).
};

/**
 * @brief Infer the needs of every function in @p m.
 *
 * Also writes each result into Def::needs, so later passes can read it off
 * the tree.
 *
 * @pre resolveNames(m) has run.
 */
NeedsReport analyzeNeeds(Module& m);

} // namespace dolphin
