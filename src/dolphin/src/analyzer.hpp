/**
 * @file analyzer.hpp
 * @brief Whole-program needs analysis (LANGUAGE.md §3.1, decision 1).
 *
 * Runs over the complete program before anything executes. A function's
 * needs are the union of:
 *  - the declared needs of every builtin it calls (builtinNeeds()), and
 *  - the needs of every user function it calls, transitively.
 *
 * Recursion and mutual recursion are handled by iterating to a fixpoint over
 * the call graph.
 *
 * Calls the analyzer can't resolve statically (a function held in a
 * variable, a name that isn't defined) are treated as worst case (NeedAll).
 * That's the same rule as for an unlabeled `extern` (decision 4): the program
 * stays correct, but those calls get ordered and kept local, so it runs
 * slower. Each such call produces a warning, so the programmer can see why.
 *
 * Current limitations:
 *  - Method calls on builtin types are assumed pure (true for all of today's
 *    str / bytes / list methods).
 *  - Function names are global: two functions with the same name collapse
 *    into one entry, and a warning is issued.
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
    unsigned topLevel = NeedNone;               ///< Needs of the top-level code, including an implicit main() call.
    std::vector<std::string> warnings;          ///< Human-readable warnings (unresolved calls, duplicates).
};

/**
 * @brief Infer the needs of every function in @p m.
 *
 * Also writes each result into Def::needs, so later passes can read it off
 * the tree.
 */
NeedsReport analyzeNeeds(Module& m);

} // namespace dolphin
