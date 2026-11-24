/**
 * @file builtins.cpp
 * @brief Builtin functions, methods of str/bytes/list, and f-string formatting.
 *
 * ### Adding a builtin
 * 1. Add its name and Need mask to the table in builtinNeeds(). Be honest
 *    about needs: a builtin marked pure gets dispatched early and reordered.
 * 2. Register the implementation with addBuiltin() in installBuiltins().
 * 3. Report every error with `in.raise(...)` so Dolphin code can catch it.
 *    Never throw raw C++ exceptions.
 *
 * Step 2 without step 1 aborts at startup, deliberately: a builtin with
 * undeclared needs would silently make the needs analysis wrong.
 *
 * ### Adding a method
 * Add the name to kMethods (that's what makes `obj.name` resolve), then
 * implement it in Interp::callMethod(). All methods today are pure; the
 * analyzer relies on that (see analyzer.hpp).
 */
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "interp.hpp"

namespace dolphin {

// The single source of truth for builtin needs; see "Adding a builtin" above.
const std::unordered_map<std::string, unsigned>& builtinNeeds() {
    static const std::unordered_map<std::string, unsigned> table = {
        // IO
        {"print", NeedIO}, {"read_file", NeedIO},
        // pure: conversions and helpers
        {"len", NeedNone}, {"range", NeedNone}, {"str", NeedNone}, {"repr", NeedNone}, {"int", NeedNone},
        {"float", NeedNone}, {"bool", NeedNone}, {"abs", NeedNone}, {"min", NeedNone}, {"max", NeedNone},
        {"sum", NeedNone}, {"ord", NeedNone}, {"chr", NeedNone}, {"type", NeedNone},
        // pure: binary decoding
        {"bits", NeedNone}, {"be_u8", NeedNone}, {"be_u16", NeedNone}, {"be_u32", NeedNone},
        {"be_u64", NeedNone}, {"be_i16sm", NeedNone}, {"be_i32sm", NeedNone}, {"be_f32", NeedNone},
        {"be_f64", NeedNone},
    };
    return table;
}

void Interp::addBuiltin(const std::string& name, std::function<Value(Interp&, Args&, int)> fn) {
    auto it = builtinNeeds().find(name);
    if (it == builtinNeeds().end()) {
        std::cerr << "internal error: builtin '" << name << "' has no declared needs\n";
        std::abort();
    }
    builtins_[name] = Value(std::make_shared<const Builtin>(Builtin{name, it->second, std::move(fn)}));
}

void Interp::addException(const std::string& name, const std::string& parent) {
    builtins_[name] = Value(std::make_shared<const ExcClass>(ExcClass{name, parent}));
    if (!parent.empty()) excParent_[name] = parent;
}

// ------------------------------------------------------------------ helpers

/// @brief Raise TypeError unless the argument count is between @p lo and @p hi inclusive.
static void arity(const Interp& in, const char* fn, const Args& a, size_t lo, size_t hi, int line) {
    if (a.size() < lo || a.size() > hi) {
        std::string want = lo == hi ? std::to_string(lo) : std::to_string(lo) + ".." + std::to_string(hi);
        in.raise("TypeError", std::string(fn) + "() takes " + want + " arguments, got " + std::to_string(a.size()), line);
    }
}
/// @brief The value as int64 (bool counts as 0/1), or raise TypeError naming @p what.
static int64_t needInt(const Interp& in, const Value& v, const char* what, int line) {
    if (v.is<bool>()) return v.as<bool>();
    if (!v.isInt()) in.raise("TypeError", std::string(what) + " must be int, not " + typeName(v), line);
    return v.as<int64_t>();
}
/// @brief The value's UTF-8 bytes if it's a str, else raise TypeError.
static const std::string& needStr(const Interp& in, const Value& v, const char* what, int line) {
    if (!v.isStr()) in.raise("TypeError", std::string(what) + " must be str, not " + typeName(v), line);
    return v.s().s;
}
/// @brief The value's bytes if it's a bytes, else raise TypeError.
static const std::string& needBytes(const Interp& in, const Value& v, const char* what, int line) {
    if (!v.isBytes()) in.raise("TypeError", std::string(what) + " must be bytes, not " + typeName(v), line);
    return v.b().b;
}
/// @brief The value as a double (int/bool/float accepted), else raise TypeError.
static double needNum(const Interp& in, const Value& v, const char* what, int line) {
    if (v.isFloat()) return v.as<double>();
    return static_cast<double>(needInt(in, v, what, line));
}

/**
 * @brief Shared body of the `be_*` builtins: read @p n bytes big-endian
 *        from `args[0]` at offset `args[1]`.
 *
 * Bounds-checked: reading past the end raises IndexError with the offset and
 * buffer size, which is what you want when a corrupt GRIB message claims a
 * section longer than the file.
 */
static uint64_t readBE(const Interp& in, const Args& a, size_t n, const char* fn, int line) {
    arity(in, fn, a, 2, 2, line);
    const std::string& b = needBytes(in, a[0], "buffer", line);
    int64_t off = needInt(in, a[1], "offset", line);
    if (off < 0 || static_cast<uint64_t>(off) + n > b.size())
        in.raise("IndexError", std::string(fn) + ": read of " + std::to_string(n) + " bytes at offset " +
                                   std::to_string(off) + " past end of " + std::to_string(b.size()) + "-byte buffer",
                 line);
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | static_cast<unsigned char>(b[static_cast<size_t>(off) + i]);
    return v;
}

/// @brief Strict int parse for `int(str)`: surrounding whitespace allowed,
///        anything else unparsed is a failure. False on overflow.
static bool parseInt(const std::string& s, int base, int64_t& out) {
    std::string t = s;
    t.erase(0, t.find_first_not_of(" \t\n\r"));
    t.erase(t.find_last_not_of(" \t\n\r") + 1);
    if (t.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(t.c_str(), &end, base);
    if (errno || !end || *end) return false;
    out = v;
    return true;
}

// ------------------------------------------------------------------ builtins

void Interp::installBuiltins() {
    // Exception hierarchy: everything derives from Error.
    addException("Error", "");
    for (const char* e : {"ValueError", "TypeError", "IndexError", "KeyError", "NameError", "AttributeError",
                          "ZeroDivisionError", "OverflowError", "IOError", "DecodeError", "RecursionError",
                          "RuntimeError", "SyntaxError"})
        addException(e, "Error");

    addBuiltin("print", [](Interp&, Args& a, int) {
        std::string out;
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) out += ' ';
            out += toStr(a[i]);
        }
        out += '\n';
        std::cout << out;
        return Value();
    });

    addBuiltin("read_file", [](Interp& in, Args& a, int line) {
        arity(in, "read_file", a, 1, 1, line);
        const std::string& path = needStr(in, a[0], "path", line);
        std::ifstream f(path, std::ios::binary);
        if (!f) in.raise("IOError", "cannot open '" + path + "': " + std::strerror(errno), line);
        std::ostringstream ss;
        ss << f.rdbuf();
        return Value::bytes(ss.str());
    });

    addBuiltin("len", [](Interp& in, Args& a, int line) {
        arity(in, "len", a, 1, 1, line);
        const Value& v = a[0];
        if (v.isStr()) return Value(static_cast<int64_t>(utf8Length(v.s())));
        if (v.isBytes()) return Value(static_cast<int64_t>(v.b().b.size()));
        if (v.isList()) return Value(static_cast<int64_t>(v.l().items.size()));
        in.raise("TypeError", "object of type '" + typeName(v) + "' has no len()", line);
    });

    addBuiltin("range", [](Interp& in, Args& a, int line) {
        arity(in, "range", a, 1, 3, line);
        int64_t start = 0, stop, step = 1;
        if (a.size() == 1) stop = needInt(in, a[0], "stop", line);
        else {
            start = needInt(in, a[0], "start", line);
            stop = needInt(in, a[1], "stop", line);
            if (a.size() == 3) step = needInt(in, a[2], "step", line);
        }
        if (step == 0) in.raise("ValueError", "range() step must not be zero", line);
        std::vector<Value> out;
        for (int64_t i = start; step > 0 ? i < stop : i > stop; i += step) out.emplace_back(i);
        return Value::list(std::move(out));
    });

    addBuiltin("str", [](Interp& in, Args& a, int line) {
        arity(in, "str", a, 0, 1, line);
        return Value::str(a.empty() ? "" : toStr(a[0]));
    });
    addBuiltin("repr", [](Interp& in, Args& a, int line) {
        arity(in, "repr", a, 1, 1, line);
        return Value::str(toRepr(a[0]));
    });
    addBuiltin("type", [](Interp& in, Args& a, int line) {
        arity(in, "type", a, 1, 1, line);
        return Value::str(typeName(a[0]));
    });

    addBuiltin("int", [](Interp& in, Args& a, int line) {
        arity(in, "int", a, 1, 2, line);
        const Value& v = a[0];
        if (v.isStr()) {
            int base = a.size() == 2 ? static_cast<int>(needInt(in, a[1], "base", line)) : 10;
            int64_t out;
            if (!parseInt(v.s().s, base, out))
                in.raise("ValueError", "invalid literal for int(): " + toRepr(v), line);
            return Value(out);
        }
        if (a.size() == 2) in.raise("TypeError", "int() base is only valid with a str", line);
        if (v.isFloat()) {
            double d = v.as<double>();
            if (!std::isfinite(d) || d >= 9.2233720368547758e18 || d < -9.2233720368547758e18)
                in.raise("OverflowError", "float out of int range", line);
            return Value(static_cast<int64_t>(d));
        }
        return Value(needInt(in, v, "argument", line));
    });

    addBuiltin("float", [](Interp& in, Args& a, int line) {
        arity(in, "float", a, 1, 1, line);
        const Value& v = a[0];
        if (v.isStr()) {
            std::string t = v.s().s;
            t.erase(0, t.find_first_not_of(" \t\n\r"));
            t.erase(t.find_last_not_of(" \t\n\r") + 1);
            char* end = nullptr;
            double d = std::strtod(t.c_str(), &end);
            if (t.empty() || !end || *end) in.raise("ValueError", "could not convert string to float: " + toRepr(v), line);
            return Value(d);
        }
        return Value(needNum(in, v, "argument", line));
    });

    addBuiltin("bool", [](Interp& in, Args& a, int line) {
        arity(in, "bool", a, 1, 1, line);
        return Value(truthy(a[0]));
    });

    addBuiltin("abs", [](Interp& in, Args& a, int line) {
        arity(in, "abs", a, 1, 1, line);
        if (a[0].isFloat()) return Value(std::fabs(a[0].as<double>()));
        int64_t v = needInt(in, a[0], "argument", line);
        if (v == INT64_MIN) in.raise("OverflowError", "integer overflow", line);
        return Value(v < 0 ? -v : v);
    });

    auto minmax = [](bool isMax) {
        return [isMax](Interp& in, Args& a, int line) {
            const char* fn = isMax ? "max" : "min";
            if (a.empty()) in.raise("TypeError", std::string(fn) + "() expected at least 1 argument", line);
            std::vector<Value> items = a.size() == 1 && a[0].isList() ? a[0].l().items : a;
            if (items.empty()) in.raise("ValueError", std::string(fn) + "() arg is an empty sequence", line);
            Value best = items[0];
            for (size_t i = 1; i < items.size(); ++i) {
                const Value& x = items[i];
                bool better;
                if (x.isNum() && best.isNum()) {
                    double xd = x.isFloat() ? x.as<double>() : static_cast<double>(needInt(in, x, fn, line));
                    double bd = best.isFloat() ? best.as<double>() : static_cast<double>(needInt(in, best, fn, line));
                    better = isMax ? xd > bd : xd < bd;
                } else if (x.isStr() && best.isStr()) {
                    better = isMax ? x.s().s > best.s().s : x.s().s < best.s().s;
                } else {
                    in.raise("TypeError", std::string(fn) + "() of mixed or unorderable types", line);
                }
                if (better) best = x;
            }
            return best;
        };
    };
    addBuiltin("min", minmax(false));
    addBuiltin("max", minmax(true));

    addBuiltin("sum", [](Interp& in, Args& a, int line) {
        arity(in, "sum", a, 1, 1, line);
        if (!a[0].isList()) in.raise("TypeError", "sum() needs a list", line);
        Value acc(int64_t{0});
        Kwargs none;
        for (auto& x : a[0].l().items) {
            if (!x.isNum()) in.raise("TypeError", "sum() of non-numeric value", line);
            if (acc.isFloat() || x.isFloat()) acc = Value(needNum(in, acc, "sum", line) + needNum(in, x, "sum", line));
            else {
                int64_t r;
                if (__builtin_add_overflow(acc.as<int64_t>(), needInt(in, x, "sum", line), &r))
                    in.raise("OverflowError", "integer overflow in sum()", line);
                acc = Value(r);
            }
        }
        return acc;
    });

    addBuiltin("ord", [](Interp& in, Args& a, int line) {
        arity(in, "ord", a, 1, 1, line);
        const std::string& s = needStr(in, a[0], "argument", line);
        if (utf8Length(a[0].s()) != 1) in.raise("TypeError", "ord() expected a single character", line);
        auto c = static_cast<unsigned char>(s[0]);
        uint32_t cp;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 6) cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        else if ((c >> 4) == 14) cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        else cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return Value(static_cast<int64_t>(cp));
    });

    addBuiltin("chr", [](Interp& in, Args& a, int line) {
        arity(in, "chr", a, 1, 1, line);
        int64_t v = needInt(in, a[0], "argument", line);
        if (v < 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) in.raise("ValueError", "chr() arg not in range", line);
        return Value::str(utf8Encode(static_cast<uint32_t>(v)));
    });

    // ---- binary decoding (GRIB/BUFR): big-endian, bounds-checked.
    // These are the M0 stand-ins for `layout` declarations (LANGUAGE.md §4).
    // Once layouts exist, the compiler emits the same loads directly.
    addBuiltin("be_u8", [](Interp& in, Args& a, int line) { return Value(static_cast<int64_t>(readBE(in, a, 1, "be_u8", line))); });
    addBuiltin("be_u16", [](Interp& in, Args& a, int line) { return Value(static_cast<int64_t>(readBE(in, a, 2, "be_u16", line))); });
    addBuiltin("be_u32", [](Interp& in, Args& a, int line) { return Value(static_cast<int64_t>(readBE(in, a, 4, "be_u32", line))); });
    addBuiltin("be_u64", [](Interp& in, Args& a, int line) {
        uint64_t v = readBE(in, a, 8, "be_u64", line);
        if (v > static_cast<uint64_t>(INT64_MAX)) in.raise("OverflowError", "be_u64 value exceeds int range", line);
        return Value(static_cast<int64_t>(v));
    });
    // GRIB sign-magnitude integers: top bit is the sign, the rest is magnitude.
    addBuiltin("be_i16sm", [](Interp& in, Args& a, int line) {
        uint64_t v = readBE(in, a, 2, "be_i16sm", line);
        int64_t mag = static_cast<int64_t>(v & 0x7FFF);
        return Value((v & 0x8000) ? -mag : mag);
    });
    addBuiltin("be_i32sm", [](Interp& in, Args& a, int line) {
        uint64_t v = readBE(in, a, 4, "be_i32sm", line);
        int64_t mag = static_cast<int64_t>(v & 0x7FFFFFFF);
        return Value((v & 0x80000000u) ? -mag : mag);
    });
    addBuiltin("be_f32", [](Interp& in, Args& a, int line) {
        auto u = static_cast<uint32_t>(readBE(in, a, 4, "be_f32", line));
        float f;
        std::memcpy(&f, &u, sizeof f);
        return Value(static_cast<double>(f));
    });
    addBuiltin("be_f64", [](Interp& in, Args& a, int line) {
        uint64_t u = readBE(in, a, 8, "be_f64", line);
        double d;
        std::memcpy(&d, &u, sizeof d);
        return Value(d);
    });
    // bits(buf, bit_offset, nbits): unsigned big-endian bit field, nbits 0..63
    addBuiltin("bits", [](Interp& in, Args& a, int line) {
        arity(in, "bits", a, 3, 3, line);
        const std::string& b = needBytes(in, a[0], "buffer", line);
        int64_t off = needInt(in, a[1], "bit offset", line);
        int64_t n = needInt(in, a[2], "nbits", line);
        if (n < 0 || n > 63) in.raise("ValueError", "bits(): nbits must be 0..63", line);
        if (off < 0 || static_cast<uint64_t>(off + n) > static_cast<uint64_t>(b.size()) * 8)
            in.raise("IndexError", "bits(): read past end of buffer", line);
        uint64_t v = 0;
        for (int64_t i = 0; i < n; ++i) {
            int64_t bit = off + i;
            unsigned char byte = static_cast<unsigned char>(b[static_cast<size_t>(bit >> 3)]);
            v = (v << 1) | ((byte >> (7 - (bit & 7))) & 1u);
        }
        return Value(static_cast<int64_t>(v));
    });
}

// ------------------------------------------------------------------ methods

/// Method names per type. attribute() consults this, so a misspelled method
/// fails at the `.name` lookup with a clear AttributeError, not at the call.
static const std::unordered_map<std::string, std::vector<std::string>> kMethods = {
    {"str", {"upper", "lower", "strip", "lstrip", "rstrip", "split", "join", "startswith", "endswith", "find",
             "replace", "count", "isdigit", "isalpha", "isspace", "encode"}},
    {"bytes", {"decode", "find", "startswith", "endswith", "hex"}},
    {"list", {"append", "pop", "extend", "index", "count"}},
};

Value Interp::attribute(const Value& obj, const std::string& name, int line) const {
    if (obj.is<std::shared_ptr<const ExcValue>>()) {
        auto& e = *obj.as<std::shared_ptr<const ExcValue>>();
        if (name == "message") return Value::str(e.msg);
        if (name == "kind") return Value::str(e.cls);
    }
    auto it = kMethods.find(typeName(obj));
    if (it != kMethods.end() && std::find(it->second.begin(), it->second.end(), name) != it->second.end())
        return Value(std::make_shared<const BoundMethod>(BoundMethod{obj, name}));
    raise("AttributeError", "'" + typeName(obj) + "' has no attribute '" + name + "'", line);
}

/// @brief Shared body of strip / lstrip / rstrip: drop characters in @p chars from the chosen ends.
static std::string stripChars(const std::string& s, bool left, bool right, const std::string& chars) {
    size_t a = 0, b = s.size();
    if (left) while (a < b && chars.find(s[a]) != std::string::npos) ++a;
    if (right) while (b > a && chars.find(s[b - 1]) != std::string::npos) --b;
    return s.substr(a, b - a);
}

Value Interp::callMethod(const BoundMethod& m, Args& a, int line) {
    const Value& self = m.self;
    const std::string& n = m.name;

    if (self.isStr()) {
        const std::string& s = self.s().s;
        if (n == "upper" || n == "lower") {
            arity(*this, n.c_str(), a, 0, 0, line);
            std::string o = s;   // ASCII case mapping only; full Unicode casing is a later stdlib item
            for (auto& c : o) c = static_cast<char>(n == "upper" ? std::toupper(static_cast<unsigned char>(c))
                                                                 : std::tolower(static_cast<unsigned char>(c)));
            return Value::str(o);
        }
        if (n == "strip" || n == "lstrip" || n == "rstrip") {
            arity(*this, n.c_str(), a, 0, 1, line);
            std::string chars = a.empty() ? std::string(" \t\n\r\f\v") : needStr(*this, a[0], "chars", line);
            return Value::str(stripChars(s, n != "rstrip", n != "lstrip", chars));
        }
        if (n == "split") {
            arity(*this, "split", a, 0, 1, line);
            std::vector<Value> out;
            if (a.empty() || a[0].isNone()) {   // whitespace split, runs collapsed
                std::istringstream ss(s);
                std::string w;
                while (ss >> w) out.push_back(Value::str(w));
            } else {
                const std::string& sep = needStr(*this, a[0], "separator", line);
                if (sep.empty()) raise("ValueError", "empty separator", line);
                size_t start = 0, pos;
                while ((pos = s.find(sep, start)) != std::string::npos) {
                    out.push_back(Value::str(s.substr(start, pos - start)));
                    start = pos + sep.size();
                }
                out.push_back(Value::str(s.substr(start)));
            }
            return Value::list(std::move(out));
        }
        if (n == "join") {
            arity(*this, "join", a, 1, 1, line);
            if (!a[0].isList()) raise("TypeError", "join() needs a list of str", line);
            std::string o;
            bool first = true;
            for (auto& x : a[0].l().items) {
                if (!first) o += s;
                o += needStr(*this, x, "join() item", line);
                first = false;
            }
            return Value::str(o);
        }
        if (n == "startswith" || n == "endswith") {
            arity(*this, n.c_str(), a, 1, 1, line);
            const std::string& p = needStr(*this, a[0], "prefix", line);
            if (p.size() > s.size()) return Value(false);
            return Value(n == "startswith" ? s.compare(0, p.size(), p) == 0
                                           : s.compare(s.size() - p.size(), p.size(), p) == 0);
        }
        if (n == "find") {
            arity(*this, "find", a, 1, 1, line);
            size_t pos = s.find(needStr(*this, a[0], "substring", line));
            if (pos == std::string::npos) return Value(int64_t{-1});
            // convert byte offset to code point index
            return Value(static_cast<int64_t>(utf8Length(Str(s.substr(0, pos)))));
        }
        if (n == "count") {
            arity(*this, "count", a, 1, 1, line);
            const std::string& sub = needStr(*this, a[0], "substring", line);
            if (sub.empty()) return Value(static_cast<int64_t>(utf8Length(self.s()) + 1));
            int64_t c = 0;
            for (size_t pos = s.find(sub); pos != std::string::npos; pos = s.find(sub, pos + sub.size())) ++c;
            return Value(c);
        }
        if (n == "replace") {
            arity(*this, "replace", a, 2, 2, line);
            const std::string& from = needStr(*this, a[0], "old", line);
            const std::string& to = needStr(*this, a[1], "new", line);
            if (from.empty()) raise("ValueError", "replace() with empty pattern", line);
            std::string o;
            size_t start = 0, pos;
            while ((pos = s.find(from, start)) != std::string::npos) {
                o.append(s, start, pos - start);
                o += to;
                start = pos + from.size();
            }
            o.append(s, start, std::string::npos);
            return Value::str(o);
        }
        if (n == "isdigit" || n == "isalpha" || n == "isspace") {
            arity(*this, n.c_str(), a, 0, 0, line);
            if (s.empty()) return Value(false);
            for (unsigned char c : s) {
                bool ok = n == "isdigit" ? std::isdigit(c) : n == "isalpha" ? std::isalpha(c) : std::isspace(c);
                if (!ok) return Value(false);
            }
            return Value(true);
        }
        if (n == "encode") {
            arity(*this, "encode", a, 0, 0, line);
            return Value::bytes(s);   // UTF-8 only
        }
    }

    if (self.isBytes()) {
        const std::string& b = self.b().b;
        if (n == "decode") {
            arity(*this, "decode", a, 0, 0, line);
            // validate UTF-8
            for (size_t i = 0; i < b.size();) {
                auto c = static_cast<unsigned char>(b[i]);
                size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
                if (!len || i + len > b.size()) raise("DecodeError", "invalid UTF-8 at byte " + std::to_string(i), line);
                for (size_t k = 1; k < len; ++k)
                    if ((static_cast<unsigned char>(b[i + k]) & 0xC0) != 0x80)
                        raise("DecodeError", "invalid UTF-8 at byte " + std::to_string(i), line);
                i += len;
            }
            return Value::str(b);
        }
        if (n == "find") {
            arity(*this, "find", a, 1, 2, line);
            const std::string& sub = needBytes(*this, a[0], "needle", line);
            size_t start = a.size() == 2 ? static_cast<size_t>(std::max<int64_t>(0, needInt(*this, a[1], "start", line))) : 0;
            size_t pos = b.find(sub, start);
            return Value(pos == std::string::npos ? int64_t{-1} : static_cast<int64_t>(pos));
        }
        if (n == "startswith" || n == "endswith") {
            arity(*this, n.c_str(), a, 1, 1, line);
            const std::string& p = needBytes(*this, a[0], "prefix", line);
            if (p.size() > b.size()) return Value(false);
            return Value(n == "startswith" ? b.compare(0, p.size(), p) == 0
                                           : b.compare(b.size() - p.size(), p.size(), p) == 0);
        }
        if (n == "hex") {
            arity(*this, "hex", a, 0, 0, line);
            static const char* d = "0123456789abcdef";
            std::string o;
            for (unsigned char c : b) { o += d[c >> 4]; o += d[c & 15]; }
            return Value::str(o);
        }
    }

    if (self.isList()) {
        auto& items = self.l().items;
        if (n == "append") {
            arity(*this, "append", a, 1, 1, line);
            items.push_back(a[0]);
            return Value();
        }
        if (n == "extend") {
            arity(*this, "extend", a, 1, 1, line);
            auto more = iterate(a[0], line);
            items.insert(items.end(), more.begin(), more.end());
            return Value();
        }
        if (n == "pop") {
            arity(*this, "pop", a, 0, 1, line);
            if (items.empty()) raise("IndexError", "pop from empty list", line);
            int64_t i = a.empty() ? -1 : needInt(*this, a[0], "index", line);
            if (i < 0) i += static_cast<int64_t>(items.size());
            if (i < 0 || i >= static_cast<int64_t>(items.size())) raise("IndexError", "pop index out of range", line);
            Value v = items[static_cast<size_t>(i)];
            items.erase(items.begin() + i);
            return v;
        }
        if (n == "index") {
            arity(*this, "index", a, 1, 1, line);
            for (size_t i = 0; i < items.size(); ++i)
                if (valuesEqual(items[i], a[0])) return Value(static_cast<int64_t>(i));
            raise("ValueError", "value is not in list", line);
        }
        if (n == "count") {
            arity(*this, "count", a, 1, 1, line);
            int64_t c = 0;
            for (auto& x : items)
                if (valuesEqual(x, a[0])) ++c;
            return Value(c);
        }
    }
    raise("AttributeError", "'" + typeName(self) + "' has no method '" + n + "'", line);
}

// --------------------------------------------------------------- formatting
//
// Format spec: [[fill]align][sign][0][width][,][.precision][type]
//   align: < > ^     sign: + - space     type: d x X o b f F e E g G s %

std::string Interp::format(const Value& v, const std::string& spec, int line) const {
    size_t i = 0;
    char fill = ' ', align = 0, sign = '-', type = 0;
    bool zero = false, comma = false;
    int width = 0, prec = -1;
    auto isAlign = [](char c) { return c == '<' || c == '>' || c == '^'; };
    if (spec.size() >= 2 && isAlign(spec[1])) { fill = spec[0]; align = spec[1]; i = 2; }
    else if (!spec.empty() && isAlign(spec[0])) { align = spec[0]; i = 1; }
    if (i < spec.size() && (spec[i] == '+' || spec[i] == '-' || spec[i] == ' ')) sign = spec[i++];
    if (i < spec.size() && spec[i] == '0') { zero = true; ++i; }
    while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) width = width * 10 + (spec[i++] - '0');
    if (i < spec.size() && spec[i] == ',') { comma = true; ++i; }
    if (i < spec.size() && spec[i] == '.') {
        ++i;
        if (i >= spec.size() || !std::isdigit(static_cast<unsigned char>(spec[i])))
            raise("ValueError", "format spec: missing precision", line);
        prec = 0;
        while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) prec = prec * 10 + (spec[i++] - '0');
    }
    if (i < spec.size()) type = spec[i++];
    if (i != spec.size()) raise("ValueError", "invalid format spec '" + spec + "'", line);

    std::string body, signStr;
    bool numeric = v.isNum();
    if (type == 0 && v.isFloat() && prec >= 0) type = 'g';
    if (type == 0) type = v.isInt() || v.is<bool>() ? 'd' : (v.isFloat() ? 'r' : 's');

    auto addCommas = [](std::string digits) {
        std::string o;
        int n = 0;
        for (size_t k = digits.size(); k-- > 0;) {
            o.insert(o.begin(), digits[k]);
            if (++n % 3 == 0 && k > 0) o.insert(o.begin(), ',');
        }
        return o;
    };

    switch (type) {
    case 's':
        if (numeric && spec.find('s') != std::string::npos) raise("ValueError", "format 's' needs a str", line);
        body = toStr(v);
        if (prec >= 0) body = body.substr(0, utf8Offset(Str(body), static_cast<size_t>(prec)));
        numeric = false;
        break;
    case 'r': body = toStr(v); break;
    case 'd': case 'x': case 'X': case 'o': case 'b': {
        if (!(v.isInt() || v.is<bool>())) raise("ValueError", std::string("format '") + type + "' needs an int", line);
        int64_t x = v.is<bool>() ? v.as<bool>() : v.as<int64_t>();
        uint64_t mag = x < 0 ? (~static_cast<uint64_t>(x) + 1) : static_cast<uint64_t>(x);
        if (x < 0) signStr = "-";
        int base = type == 'd' ? 10 : (type == 'o' ? 8 : (type == 'b' ? 2 : 16));
        const char* dig = type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        do { body.insert(body.begin(), dig[mag % static_cast<unsigned>(base)]); mag /= static_cast<unsigned>(base); } while (mag);
        if (comma && type == 'd') body = addCommas(body);
        break;
    }
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case '%': {
        if (!numeric) raise("ValueError", std::string("format '") + type + "' needs a number", line);
        double d = v.isFloat() ? v.as<double>() : static_cast<double>(v.is<bool>() ? v.as<bool>() : v.as<int64_t>());
        if (type == '%') d *= 100.0;
        if (std::signbit(d) && !std::isnan(d)) { signStr = "-"; d = -d; }
        char cfmt[8] = {'%', '.', '*', type == '%' ? 'f' : type, 0};
        char buf[512];
        std::snprintf(buf, sizeof buf, cfmt, prec < 0 ? 6 : prec, d);
        body = buf;
        if (comma) {
            size_t dot = body.find_first_not_of("0123456789");
            body = addCommas(body.substr(0, dot)) + (dot == std::string::npos ? "" : body.substr(dot));
        }
        if (type == '%') body += '%';
        break;
    }
    default: raise("ValueError", std::string("unknown format type '") + type + "'", line);
    }

    if (numeric && signStr.empty() && (sign == '+' || sign == ' ')) signStr = std::string(1, sign);

    size_t len = utf8Length(Str(signStr + body));
    size_t w = static_cast<size_t>(width);
    if (len >= w) return signStr + body;
    size_t pad = w - len;
    if (zero && !align && numeric) return signStr + std::string(pad, '0') + body;
    if (!align) align = numeric ? '>' : '<';
    std::string f(1, fill);
    auto rep = [&](size_t k) { std::string o; for (size_t j = 0; j < k; ++j) o += f; return o; };
    if (align == '<') return signStr + body + rep(pad);
    if (align == '>') return rep(pad) + signStr + body;
    return rep(pad / 2) + signStr + body + rep(pad - pad / 2);
}

} // namespace dolphin
