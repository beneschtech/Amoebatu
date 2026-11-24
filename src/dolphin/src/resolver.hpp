/**
 * @file resolver.hpp
 * @brief Static name resolution: decides what every name in the program refers to.
 *
 * Runs after parsing and before the needs analyzer. For every Name node it
 * sets Name::bind (and Name::slot / Name::def where relevant), and it numbers
 * the locals of every function (Def::numLocals, Param::slot, For::slot, …).
 *
 * Rules (Python's, minus `global`/`nonlocal` and closures):
 *  - Inside a function, a name that is a parameter or is bound anywhere in
 *    the body (assignment, `for` variable, comprehension variable,
 *    `except … as`, nested `def`) is local to the whole function. Reading it
 *    before it's assigned raises UnboundLocalError at run time.
 *  - Otherwise, if the module binds the name, it's a global. If the module
 *    binds it only with a single `def`, it's a Function, and the call target
 *    is known statically.
 *  - Otherwise it's a builtin function or builtin exception class.
 *  - Otherwise it's a compile-time NameError. The whole program is examined
 *    before it runs (LANGUAGE.md §3.1), so a misspelled name never gets as far
 *    as running.
 *
 * Closures aren't supported yet. A nested function that reads a local of its
 * enclosing function is rejected with a message saying so, rather than
 * silently reading a global of the same name.
 *
 * Comprehension variables are locals of the enclosing function (at module
 * level, globals). Unlike Python 3, they don't get their own scope.
 */
#pragma once
#include "ast.hpp"

namespace dolphin {

/**
 * @brief Resolve every name in @p m in place.
 * @throws SyntaxError with kind `NameError` for an undefined name or an
 *         unsupported closure reference, or kind `SyntaxError` for a
 *         non-exception name in an `except` clause.
 */
void resolveNames(Module& m);

} // namespace dolphin
