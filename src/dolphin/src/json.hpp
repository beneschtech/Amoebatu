/**
 * @file json.hpp
 * @brief The `JSON` builtin type: a JSON document (jsoncpp underneath).
 *
 * A JSON value is immutable, like str: `j << x` builds a new document and
 * returns it. The statement `j << x` rebinds `j` (parser.cpp rewrites
 * `name << expr` as an assignment), so it reads as loading `x` into `j`.
 *
 * | Expression        | Result                                                     |
 * |-------------------|------------------------------------------------------------|
 * | `JSON()`          | empty document (`null`); `JSON(x)` is `JSON() << x`        |
 * | `j << str`        | parse JSON text; JSONError (a ValueError) if malformed     |
 * | `j << bytes`      | a `data:<mime>;base64,...` string (the web form)           |
 * | `j << list`       | array of simple values only (None/bool/int/float/str/bytes)|
 * | `j << dict`       | object, recursively: any nesting of dicts and lists        |
 * | `j << other JSON` | a copy                                                     |
 * | `x << j`          | `j.value()` (statement `d << j`: `d = j.value()`)          |
 * | `j.value()`       | dict, list or scalar; `data:...;base64,` strings → bytes   |
 * | `j.toString()`    | compact text; `j.prettyPrint()` indented                   |
 *
 * jsoncpp keeps object members sorted by key, so output is in key order,
 * not insertion order.
 */
#pragma once
#include <string>

#include "value.hpp"

namespace dolphin {

class Interp;
struct JsonDoc;

/// @brief `j << src`: a new JSON document from @p src (raises JSONError / TypeError).
Value jsonLoad(const Interp& in, const Value& src, int line);
/// @brief `j.value()`.
Value jsonValue(const JsonDoc& d);
/// @brief `j.toString()` (compact) or `j.prettyPrint()`.
std::string jsonText(const JsonDoc& d, bool pretty);
/// @brief Same document?
bool jsonEqual(const JsonDoc& a, const JsonDoc& b);
/// @brief An empty (null) document.
Value jsonEmpty();

} // namespace dolphin
