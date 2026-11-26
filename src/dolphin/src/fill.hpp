/**
 * @file fill.hpp
 * @brief Parallel fills: `for i in ...: out[i] = f(...)` into a preallocated list, in parallel.
 *
 * A decoder that knows its record count up front can allocate the result
 * once and fill it by index:
 *
 *     out = array(n)                       # or [None] * n
 *     for i in range(n):
 *         out[i] = decode(buf, offsets[i])
 *
 * When that loop is safe to run in parallel, it is rewritten (after the needs
 * analysis, before either engine sees the tree) into
 *
 *     $f = False
 *     try:
 *         $items = [i for i in range(n)]             # the loop's values, once
 *         $keys = [i for i in $items]                # every index, in order
 *         $vals = [decode(buf, offsets[i]) for i in $items]   # a parallel map
 *         $f = __fill_ok(out, $keys)                 # would every store succeed?
 *     except:
 *         pass
 *     if $f:
 *         __fill(out, $keys, $vals)                  # store them all, in order
 *     else:
 *         for i in range(n):                         # the original loop, unchanged
 *             out[i] = decode(buf, offsets[i])
 *
 * The calls run as a batched parallel map (Interp::parallelMap()), so they
 * get the same cost model and load-based admission as comprehensions. The
 * results are stored once the map is done, in loop order, by native code.
 *
 * **Why it's exactly the sequential loop.** The loop qualifies only if:
 *  - its body is the single statement `NAME[index] = f(args)`;
 *  - `f` is parallel-safe (pure, mutates nothing it didn't create);
 *  - the iterable, the index and the arguments are reorderable
 *    (isReorderableExpr(): no effects, no mutation), and the index and
 *    arguments don't mention NAME, so no call can see an earlier store;
 *  - NAME is a local of the function that holds a list or dict it created
 *    (every assignment is a literal, a comprehension, `array()`, `range()`,
 *    `sorted()`, an operator or a slice copy) and never escapes: it's never
 *    assigned to another name, stored in a container, or passed to a user
 *    function. So no argument and no global can reach it either.
 * Every value the parallel path computes is pure, so if anything goes wrong
 * before the stores (a call raises, an index is out of range, NAME isn't a
 * list) nothing observable has happened, and the original loop runs instead
 * and fails exactly where it would have. The loop variable ends with the last
 * item either way. Module-level loops are never rewritten: module code never
 * dispatches.
 */
#pragma once
#include "ast.hpp"

namespace dolphin {

/**
 * @brief Rewrite every qualifying loop in @p m.
 * @return How many loops were rewritten. If nonzero, run resolveNames() and
 *         analyzeNeeds() again: the new code introduces names.
 * Requires resolveNames() and analyzeNeeds() to have run.
 */
int rewriteParallelFills(Module& m);

} // namespace dolphin
