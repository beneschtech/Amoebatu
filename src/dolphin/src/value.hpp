/**
 * @file value.hpp
 * @brief Runtime values for the M0 tree-walking interpreter.
 *
 * At M0, Dolphin is dynamically typed at runtime: every value is a Value,
 * a tagged union (std::variant) of the types below. Once the static type
 * checker and the LLVM path arrive (M3), compiled code works on raw
 * machine types and most of this is only used by the interpreter.
 *
 * Memory model:
 *  - Scalars (bool, int64, double) are stored inline.
 *  - str, bytes and the callable types are held through
 *    `std::shared_ptr<const T>`: immutable and shared, so copying a Value
 *    is cheap and never copies the payload. This mirrors the language rule
 *    that values are immutable (LANGUAGE.md §3.2), which is also what makes
 *    them safe to share between threads later.
 *  - list is the one mutable type and is held through a non-const
 *    `std::shared_ptr<List>`. Two Values pointing at the same List see each
 *    other's appends, as in Python.
 */
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ast.hpp"

namespace dolphin {

struct Value;
class Interp;

/**
 * @brief Immutable UTF-8 string.
 *
 * Stores an "all ASCII" flag computed once at construction. Indexing and
 * length are defined in code points (LANGUAGE.md §3.6); when @c ascii is true
 * those are O(1) byte operations, which covers almost all weather text.
 */
struct Str {
    std::string s;  ///< UTF-8 bytes.
    bool ascii;     ///< True if every byte is < 0x80.
    explicit Str(std::string v);
};
/// @brief Immutable byte string.
struct Bytes { std::string b; };
/// @brief Mutable list of values.
struct List { std::vector<Value> items; };

/// @brief A user-defined function: a pointer back into the AST.
/// The Def is owned by the Module, which outlives the interpreter.
struct Function { const Def* def; };
struct Builtin;
/// @brief An exception class such as `ValueError`. @c parent is empty only for `Error`.
struct ExcClass { std::string name; std::string parent; };
/// @brief An exception instance: class name plus message.
struct ExcValue { std::string cls; std::string msg; };
struct BoundMethod;
/// @brief Tag type for `None`.
struct NoneT {};

/**
 * @brief Any Dolphin runtime value.
 *
 * Constructors take the natural C++ type (`Value(int64_t{3})`,
 * `Value(2.5)`, `Value(true)`). Use the static factories str(), bytes() and
 * list() for heap types. Query with the `isX()` helpers and read with
 * s(), b(), l() or as<T>().
 *
 * @warning There is no `Value(size_t)` constructor. Passing a size_t is
 *          ambiguous between the bool and int overloads. Cast to int64_t.
 */
struct Value {
    /// The underlying variant. Index order is not significant.
    using V = std::variant<NoneT, bool, int64_t, double,
                           std::shared_ptr<const Str>, std::shared_ptr<const Bytes>, std::shared_ptr<List>,
                           std::shared_ptr<const Function>, std::shared_ptr<const Builtin>,
                           std::shared_ptr<const ExcClass>, std::shared_ptr<const ExcValue>,
                           std::shared_ptr<const BoundMethod>>;
    V v;  ///< Storage.

    Value() : v(NoneT{}) {}                                ///< None.
    Value(bool b) : v(b) {}                                ///< bool.
    Value(int64_t i) : v(i) {}                             ///< int.
    Value(int i) : v(static_cast<int64_t>(i)) {}           ///< int (convenience for literals).
    Value(double d) : v(d) {}                              ///< float.
    /// Any immutable heap type, e.g. `std::make_shared<const Function>(...)`.
    template <class T> Value(std::shared_ptr<T> p) : v(std::shared_ptr<const T>(std::move(p))) {}
    Value(std::shared_ptr<List> p) : v(std::move(p)) {}    ///< list (mutable, shared).

    /// @brief Make a str value from UTF-8 bytes.
    static Value str(std::string s) { return Value(std::make_shared<const Str>(std::move(s))); }
    /// @brief Make a bytes value.
    static Value bytes(std::string b) { return Value(std::make_shared<const Bytes>(Bytes{std::move(b)})); }
    /// @brief Make a new list.
    static Value list(std::vector<Value> items = {}) { return Value(std::make_shared<List>(List{std::move(items)})); }

    template <class T> bool is() const { return std::holds_alternative<T>(v); }  ///< Holds alternative T?
    template <class T> const T& as() const { return std::get<T>(v); }            ///< Get alternative T (throws std::bad_variant_access if wrong).

    bool isNone() const { return is<NoneT>(); }
    bool isInt() const { return is<int64_t>(); }
    bool isFloat() const { return is<double>(); }
    /// True for int, float **and bool**: bool takes part in arithmetic as 0/1, as in Python.
    bool isNum() const { return isInt() || isFloat() || is<bool>(); }
    bool isStr() const { return is<std::shared_ptr<const Str>>(); }
    bool isBytes() const { return is<std::shared_ptr<const Bytes>>(); }
    bool isList() const { return is<std::shared_ptr<List>>(); }

    const Str& s() const { return *as<std::shared_ptr<const Str>>(); }      ///< The str payload. Check isStr() first.
    const Bytes& b() const { return *as<std::shared_ptr<const Bytes>>(); }  ///< The bytes payload. Check isBytes() first.
    List& l() const { return *as<std::shared_ptr<List>>(); }                ///< The list payload (mutable). Check isList() first.
};

using Args = std::vector<Value>;                             ///< Positional call arguments.
using Kwargs = std::vector<std::pair<std::string, Value>>;   ///< Keyword call arguments, in call order.

/**
 * @brief A function implemented in C++.
 *
 * Every builtin carries its Need mask. The needs are declared once, in
 * builtinNeeds(); Interp::addBuiltin() refuses a builtin missing from that
 * table, so no builtin can exist with undeclared needs.
 */
struct Builtin {
    std::string name;
    unsigned needs;  ///< Need bitmask.
    /// Implementation. Receives the interpreter (for raise()), the arguments
    /// and the call's source line (for error messages).
    std::function<Value(Interp&, Args&, int line)> fn;
};
/// @brief A method looked up on a value, e.g. `s.split`, waiting to be called.
struct BoundMethod {
    Value self;        ///< Receiver.
    std::string name;  ///< Method name; validated when the attribute is read.
};

/**
 * @brief A Dolphin exception travelling through the C++ stack.
 *
 * Interp::raise() throws one. A `try` statement in Dolphin catches it in
 * Interp::exec(). If none does, Interp::run() reports it. Kept separate from
 * SyntaxError, which is compile-time only.
 */
struct DolphinError {
    std::string cls;  ///< Exception class name, e.g. `ValueError`.
    std::string msg;  ///< Message; may be empty.
    int line;         ///< Source line where it was raised (0 if unknown).
};

/// @brief Dolphin type name of a value (`int`, `str`, …; an exception's class name).
std::string typeName(const Value& v);
/// @brief Python truthiness: None, False, 0, 0.0 and empty str/bytes/list are false.
bool truthy(const Value& v);
/// @brief `str(x)`: what print() shows.
std::string toStr(const Value& v);
/// @brief `repr(x)`: like str() but quotes strings, matching CPython's quoting rules.
std::string toRepr(const Value& v);
/// @brief `==` semantics: numeric across int/float/bool, deep for lists, identity for functions.
bool valuesEqual(const Value& a, const Value& b);

/// @name UTF-8 helpers
/// @{
/// @brief Number of code points in @p s.
size_t utf8Length(const Str& s);
/// @brief Byte offset of code point index @p cp; clamped to the string length.
size_t utf8Offset(const Str& s, size_t cp);
/// @brief Encode one code point as UTF-8. The caller validates the range.
std::string utf8Encode(uint32_t cp);
/// @}

} // namespace dolphin
