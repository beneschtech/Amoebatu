/**
 * @file value.cpp
 * @brief Value helpers: str()/repr() formatting, equality, truthiness, UTF-8.
 */
#include "value.hpp"
#include "json.hpp"
#include "listener.hpp"
#include "stream.hpp"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dolphin {

Str::Str(std::string v) : s(std::move(v)), ascii(true) {
    for (unsigned char c : s)
        if (c >= 0x80) { ascii = false; break; }
}

// Code points are counted by counting bytes that aren't UTF-8 continuation
// bytes (10xxxxxx). The input is assumed valid UTF-8: source text and
// bytes.decode() both guarantee it.
size_t utf8Length(const Str& s) {
    if (s.ascii) return s.s.size();
    size_t n = 0;
    for (unsigned char c : s.s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

size_t utf8Offset(const Str& s, size_t cp) {
    if (s.ascii) return cp < s.s.size() ? cp : s.s.size();
    size_t n = 0;
    for (size_t i = 0; i < s.s.size(); ++i) {
        if ((static_cast<unsigned char>(s.s[i]) & 0xC0) != 0x80) {
            if (n == cp) return i;
            ++n;
        }
    }
    return s.s.size();
}

std::string utf8Encode(uint32_t cp) {
    std::string o;
    if (cp < 0x80) {
        o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return o;
}

uint64_t Dict::hashKey(const std::string& ck) { return std::hash<std::string>{}(ck); }

long Dict::find(const std::string& ck, uint64_t h) const {
    if (table.empty()) {   // small: scan the stored hashes
        for (size_t i = 0; i < hashes.size(); ++i)
            if (hashes[i] == h && ckeys[i] == ck) return static_cast<long>(i);
        return -1;
    }
    size_t mask = table.size() - 1;
    for (size_t s = h & mask;; s = (s + 1) & mask) {
        int32_t pos = table[s];
        if (pos < 0) return -1;
        if (hashes[static_cast<size_t>(pos)] == h && ckeys[static_cast<size_t>(pos)] == ck) return pos;
    }
}

void Dict::set(const std::string& ck, uint64_t h, const Value& key, const Value& value) {
    long pos = find(ck, h);
    if (pos >= 0) {
        entries[static_cast<size_t>(pos)].second = value;
        return;
    }
    entries.emplace_back(key, value);
    ckeys.push_back(ck);
    hashes.push_back(h);
    if (entries.size() > kLinear) {
        if (table.size() < entries.size() * 2) {
            rebuildTable();   // grow (or create) at most half full
        } else {
            size_t mask = table.size() - 1;
            size_t s = h & mask;
            while (table[s] >= 0) s = (s + 1) & mask;
            table[s] = static_cast<int32_t>(entries.size() - 1);
        }
    }
}

void Dict::rebuildTable() {
    size_t cap = 16;
    while (cap < entries.size() * 2) cap *= 2;
    table.assign(cap, -1);
    size_t mask = cap - 1;
    for (size_t i = 0; i < entries.size(); ++i) {
        size_t s = hashes[i] & mask;
        while (table[s] >= 0) s = (s + 1) & mask;
        table[s] = static_cast<int32_t>(i);
    }
}

void Dict::erase(size_t pos) {
    entries.erase(entries.begin() + static_cast<long>(pos));
    ckeys.erase(ckeys.begin() + static_cast<long>(pos));
    hashes.erase(hashes.begin() + static_cast<long>(pos));
    if (entries.size() > kLinear) rebuildTable();
    else table.clear();
}

// A type tag plus the value. Integral floats use the int form so 1.0 finds 1;
// other floats use their bit pattern. Function identity uses the Def address.
bool dictKey(const Value& v, std::string& out) {
    if (v.isNone()) { out = "N"; return true; }
    if (v.is<bool>()) { out = v.as<bool>() ? "I1" : "I0"; return true; }
    if (v.isInt()) { out = "I" + std::to_string(v.as<int64_t>()); return true; }
    if (v.isFloat()) {
        double d = v.as<double>();
        if (std::isfinite(d) && d == std::floor(d) && d >= -9.2e18 && d <= 9.2e18) {
            out = "I" + std::to_string(static_cast<int64_t>(d));
        } else {
            uint64_t bits;
            std::memcpy(&bits, &d, sizeof bits);
            out = "F" + std::to_string(bits);
        }
        return true;
    }
    if (v.isStr()) { out = "S" + v.s().s; return true; }
    if (v.isBytes()) { out = "B" + v.b().b; return true; }
    if (v.is<std::shared_ptr<const Function>>()) {
        out = "P" + std::to_string(reinterpret_cast<uintptr_t>(v.as<std::shared_ptr<const Function>>()->def));
        return true;
    }
    if (v.is<std::shared_ptr<const ExcClass>>()) { out = "C" + v.as<std::shared_ptr<const ExcClass>>()->name; return true; }
    return false;   // list, dict, builtins, methods, exception instances
}

std::string typeName(const Value& v) {
    struct V {
        std::string operator()(NoneT) const { return "None"; }
        std::string operator()(bool) const { return "bool"; }
        std::string operator()(int64_t) const { return "int"; }
        std::string operator()(double) const { return "float"; }
        std::string operator()(const std::shared_ptr<const Str>&) const { return "str"; }
        std::string operator()(const std::shared_ptr<const Bytes>&) const { return "bytes"; }
        std::string operator()(const std::shared_ptr<List>&) const { return "list"; }
        std::string operator()(const std::shared_ptr<Dict>&) const { return "dict"; }
        std::string operator()(const std::shared_ptr<const Function>&) const { return "function"; }
        std::string operator()(const std::shared_ptr<const Builtin>&) const { return "builtin"; }
        std::string operator()(const std::shared_ptr<const ExcClass>&) const { return "exception class"; }
        std::string operator()(const std::shared_ptr<const ExcValue>& e) const { return e->cls; }
        std::string operator()(const std::shared_ptr<const BoundMethod>&) const { return "method"; }
        std::string operator()(const std::shared_ptr<Future>&) const { return "future"; }
        std::string operator()(const std::shared_ptr<Stream>&) const { return "Stream"; }
        std::string operator()(const std::shared_ptr<Response>&) const { return "Response"; }
        std::string operator()(const std::shared_ptr<const JsonDoc>&) const { return "JSON"; }
        std::string operator()(const std::shared_ptr<Listener>&) const { return "Listener"; }
    };
    return std::visit(V{}, v.v);
}

bool truthy(const Value& v) {
    if (v.isNone()) return false;
    if (v.is<bool>()) return v.as<bool>();
    if (v.isInt()) return v.as<int64_t>() != 0;
    if (v.isFloat()) return v.as<double>() != 0.0;
    if (v.isStr()) return !v.s().s.empty();
    if (v.isBytes()) return !v.b().b.empty();
    if (v.isList()) return !v.l().items.empty();
    if (v.isDict()) return !v.d().entries.empty();
    return true;
}

/// @brief Format a double exactly as CPython's repr() does (tests compare against CPython).
static std::string fmtFloat(double d) {
    // Python repr: shortest digits that round-trip; positional notation when the
    // decimal exponent is in [-4, 16), scientific otherwise.
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    if (d == 0.0) return std::signbit(d) ? "-0.0" : "0.0";
    char buf[64];
    int p = 1;
    for (; p < 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*e", p - 1, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    std::snprintf(buf, sizeof buf, "%.*e", p - 1, d);
    int exp = std::atoi(std::strchr(buf, 'e') + 1);
    if (exp < -4 || exp >= 16) return buf;
    std::snprintf(buf, sizeof buf, "%.*f", std::max(0, p - 1 - exp), d);
    std::string s = buf;
    if (s.find('.') == std::string::npos) s += ".0";
    return s;
}

/// @brief Quote @p s with quote char @p q, escaping as Python's repr does.
static std::string quoteStr(const std::string& s, char q) {
    std::string o(1, q);
    for (unsigned char c : s) {
        if (c == static_cast<unsigned char>(q) || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\r') o += "\\r";
        else if (c < 0x20 || c == 0x7f) {
            char buf[5];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            o += buf;
        } else o += static_cast<char>(c);
    }
    return o + q;
}

std::string toRepr(const Value& v) {
    if (v.isStr()) {   // Python's rule: prefer '...', use "..." if the text has ' but no "
        const std::string& t = v.s().s;
        bool sq = t.find('\'') != std::string::npos, dq = t.find('"') != std::string::npos;
        return quoteStr(t, sq && !dq ? '"' : '\'');
    }
    if (v.isBytes()) {
        std::string o = "b'";
        for (unsigned char c : v.b().b) {
            if (c == '\'' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
            else if (c >= 0x20 && c < 0x7f) o += static_cast<char>(c);
            else {
                char buf[5];
                std::snprintf(buf, sizeof buf, "\\x%02x", c);
                o += buf;
            }
        }
        return o + "'";
    }
    return toStr(v);
}

std::string toStr(const Value& v) {
    if (v.isNone()) return "None";
    if (v.is<bool>()) return v.as<bool>() ? "True" : "False";
    if (v.isInt()) return std::to_string(v.as<int64_t>());
    if (v.isFloat()) return fmtFloat(v.as<double>());
    if (v.isStr()) return v.s().s;
    if (v.isBytes()) return toRepr(v);
    if (v.isList()) {
        std::string o = "[";
        bool first = true;
        for (auto& x : v.l().items) {
            if (!first) o += ", ";
            o += toRepr(x);
            first = false;
        }
        return o + "]";
    }
    if (v.isDict()) {
        std::string o = "{";
        bool first = true;
        for (auto& [k, x] : v.d().entries) {
            if (!first) o += ", ";
            o += toRepr(k) + ": " + toRepr(x);
            first = false;
        }
        return o + "}";
    }
    if (v.is<std::shared_ptr<const Function>>()) return "<function " + v.as<std::shared_ptr<const Function>>()->def->name + ">";
    if (v.is<std::shared_ptr<const Builtin>>()) return "<builtin " + v.as<std::shared_ptr<const Builtin>>()->name + ">";
    if (v.is<std::shared_ptr<const ExcClass>>()) return "<class " + v.as<std::shared_ptr<const ExcClass>>()->name + ">";
    if (v.is<std::shared_ptr<const ExcValue>>()) return v.as<std::shared_ptr<const ExcValue>>()->msg;
    if (v.is<std::shared_ptr<const BoundMethod>>()) return "<method " + std::string(methodName(v.as<std::shared_ptr<const BoundMethod>>()->method)) + ">";
    if (v.isStream()) return "<Stream " + v.as<std::shared_ptr<Stream>>()->target() + ">";
    if (v.isResponse()) return "<Response>";
    if (v.isListener()) {
        auto& l = *v.as<std::shared_ptr<Listener>>();
        return l.running() ? "<Listener " + l.describe() + ">" : "<Listener>";
    }
    if (v.isJson()) return jsonText(*v.as<std::shared_ptr<const JsonDoc>>(), false);
    return "?";
}

// Numbers compare by value across int/float/bool (1 == 1.0 == True). When
// both are ints, compare as int64 rather than through double, so large
// integers above 2^53 don't falsely compare equal.
bool valuesEqual(const Value& a, const Value& b) {
    if (a.isNum() && b.isNum()) {
        auto num = [](const Value& x) -> long double {
            if (x.is<bool>()) return x.as<bool>() ? 1 : 0;
            if (x.isInt()) return static_cast<long double>(x.as<int64_t>());
            return x.as<double>();
        };
        if (a.isFloat() || b.isFloat()) return num(a) == num(b);
        int64_t ai = a.is<bool>() ? a.as<bool>() : a.as<int64_t>();
        int64_t bi = b.is<bool>() ? b.as<bool>() : b.as<int64_t>();
        return ai == bi;
    }
    if (a.isNone() && b.isNone()) return true;
    if (a.isStr() && b.isStr()) return a.s().s == b.s().s;
    if (a.isBytes() && b.isBytes()) return a.b().b == b.b().b;
    if (a.isList() && b.isList()) {
        auto& x = a.l().items;
        auto& y = b.l().items;
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); ++i)
            if (!valuesEqual(x[i], y[i])) return false;
        return true;
    }
    if (a.isDict() && b.isDict()) {   // same keys mapping to equal values; order doesn't matter
        auto& x = a.d();
        auto& y = b.d();
        if (x.entries.size() != y.entries.size()) return false;
        for (auto& [k, val] : x.entries) {
            std::string ck;
            dictKey(k, ck);
            long pos = y.find(ck);
            if (pos < 0 || !valuesEqual(val, y.entries[static_cast<size_t>(pos)].second)) return false;
        }
        return true;
    }
    if (a.v.index() != b.v.index()) return false;
    if (a.is<std::shared_ptr<const Function>>())
        return a.as<std::shared_ptr<const Function>>()->def == b.as<std::shared_ptr<const Function>>()->def;
    if (a.isStream()) return a.as<std::shared_ptr<Stream>>() == b.as<std::shared_ptr<Stream>>();
    if (a.isListener()) return a.as<std::shared_ptr<Listener>>() == b.as<std::shared_ptr<Listener>>();
    if (a.isJson()) return jsonEqual(*a.as<std::shared_ptr<const JsonDoc>>(), *b.as<std::shared_ptr<const JsonDoc>>());
    if (a.isResponse()) return a.as<std::shared_ptr<Response>>() == b.as<std::shared_ptr<Response>>();
    if (a.is<std::shared_ptr<const ExcClass>>())
        return a.as<std::shared_ptr<const ExcClass>>()->name == b.as<std::shared_ptr<const ExcClass>>()->name;
    return false;
}

} // namespace dolphin
