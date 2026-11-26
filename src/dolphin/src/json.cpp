/**
 * @file json.cpp
 * @brief JSON documents: see json.hpp.
 */
#include "json.hpp"

#include <json/json.h>

#include <charconv>
#include <cmath>
#include <memory>
#include <sstream>

#include "base64.hpp"
#include "interp.hpp"

namespace dolphin {

/// @brief The payload of a JSON value. Never changed after construction.
struct JsonDoc {
    Json::Value v;
};

namespace {

constexpr int kMaxDepth = 256;   ///< Deeper input is rejected rather than risking the C++ stack.

/// @brief MIME type from the first bytes, for the data URI.
const char* sniffMime(const std::string& b) {
    auto has = [&](size_t off, const char* sig, size_t n) { return b.size() >= off + n && b.compare(off, n, sig, n) == 0; };
    if (has(0, "\xFF\xD8\xFF", 3)) return "image/jpeg";
    if (has(0, "\x89PNG\r\n\x1A\n", 8)) return "image/png";
    if (has(0, "GIF87a", 6) || has(0, "GIF89a", 6)) return "image/gif";
    if (has(0, "RIFF", 4) && has(8, "WEBP", 4)) return "image/webp";
    if (has(0, "%PDF-", 5)) return "application/pdf";
    if (has(0, "\x1F\x8B", 2)) return "application/gzip";
    if (has(0, "PK\x03\x04", 4)) return "application/zip";
    if (has(0, "GRIB", 4)) return "application/x-grib";
    return "application/octet-stream";
}

Json::Value fromBytes(const std::string& b) {
    return Json::Value(std::string("data:") + sniffMime(b) + ";base64," + base64Encode(b));
}

/// @brief A scalar, or false if @p v isn't one.
bool scalar(const Value& v, Json::Value& out) {
    if (v.isNone()) out = Json::Value(Json::nullValue);
    else if (v.is<bool>()) out = Json::Value(v.as<bool>());
    else if (v.isInt()) out = Json::Value(static_cast<Json::Int64>(v.as<int64_t>()));
    else if (v.isFloat()) out = Json::Value(v.as<double>());
    else if (v.isStr()) out = Json::Value(v.s().s);
    else if (v.isBytes()) out = fromBytes(v.b().b);
    else return false;
    return true;
}

Json::Value fromValue(const Interp& in, const Value& v, int line, int depth) {
    if (depth > kMaxDepth) in.raise("ValueError", "value too deeply nested for JSON", line);
    Json::Value out;
    if (v.isFloat() && !std::isfinite(v.as<double>())) in.raise("ValueError", "JSON has no NaN or infinity", line);
    if (scalar(v, out)) return out;
    if (v.isList()) {
        out = Json::Value(Json::arrayValue);
        for (auto& x : v.l().items) out.append(fromValue(in, x, line, depth + 1));
        return out;
    }
    if (v.isDict()) {
        out = Json::Value(Json::objectValue);
        for (auto& [k, x] : v.d().entries) {
            if (!k.isStr()) in.raise("TypeError", "JSON object keys must be str, not " + typeName(k), line);
            out[k.s().s] = fromValue(in, x, line, depth + 1);
        }
        return out;
    }
    if (v.is<std::shared_ptr<const JsonDoc>>()) return v.as<std::shared_ptr<const JsonDoc>>()->v;
    in.raise("TypeError", "cannot convert " + typeName(v) + " to JSON", line);
}

Value toValue(const Json::Value& j) {
    switch (j.type()) {
    case Json::nullValue: return Value();
    case Json::booleanValue: return Value(j.asBool());
    case Json::intValue: return Value(static_cast<int64_t>(j.asInt64()));
    case Json::uintValue:
        if (j.isInt64()) return Value(static_cast<int64_t>(j.asInt64()));
        return Value(j.asDouble());   // above int64: as Python's json would overflow us, keep it as float
    case Json::realValue: return Value(j.asDouble());
    case Json::stringValue: {
        std::string s = j.asString();
        if (s.compare(0, 5, "data:") == 0) {   // our bytes encoding (any data URI marked base64)
            size_t comma = s.find(',');
            static const std::string tag = ";base64";
            std::string bytes;
            if (comma != std::string::npos && comma >= tag.size() && s.compare(comma - tag.size(), tag.size(), tag) == 0 &&
                base64Decode(s.substr(comma + 1), bytes))
                return Value::bytes(std::move(bytes));
        }
        return Value::str(std::move(s));
    }
    case Json::arrayValue: {
        std::vector<Value> items;
        items.reserve(j.size());
        for (auto& x : j) items.push_back(toValue(x));
        return Value::list(std::move(items));
    }
    case Json::objectValue: {
        Value d = Value::dict();
        for (auto it = j.begin(); it != j.end(); ++it) {
            Value k = Value::str(it.name());
            std::string ck;
            dictKey(k, ck);
            d.d().set(ck, k, toValue(*it));
        }
        return d;
    }
    }
    return Value();
}

/// jsoncpp reports "* Line 1, Column 7\n  Syntax error: ...\n" per problem; make that
/// "Line 1, Column 7: Syntax error: ..." (several problems joined with "; ").
std::string oneLine(const std::string& err) {
    std::string out, cur;
    std::istringstream ss(err);
    for (std::string ln; std::getline(ss, ln);) {
        size_t a = ln.find_first_not_of(" ");
        if (a == std::string::npos) continue;
        ln = ln.substr(a);
        if (ln.compare(0, 2, "* ") == 0) {
            if (!out.empty()) out += "; ";
            out += ln.substr(2);
            cur = ":";
        } else if (ln.compare(0, 4, "See ") != 0) {   // "See Line 3, Column 1 for detail." adds nothing
            out += cur.empty() ? (out.empty() ? "" : " ") : ": ";
            out += ln;
            cur.clear();
        }
    }
    return out;
}

/// Shortest text that reads back as the same double (Python's repr), with a ".0"
/// on whole numbers so they stay floats when read back.
std::string number(double d) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, d);
    std::string t(buf, r.ptr);
    if (t.find_first_of(".eEn") == std::string::npos) t += ".0";
    return t;
}

/// Write @p v: compact when @p depth < 0, else indented two spaces per level
/// (empty arrays and objects stay on one line). jsoncpp escapes the strings.
void write(const Json::Value& v, std::string& o, int depth, const Json::StreamWriterBuilder& str) {
    bool indent = depth >= 0;
    std::string pad = indent ? "\n" + std::string(static_cast<size_t>(depth + 1) * 2, ' ') : "";
    std::string close = indent ? "\n" + std::string(static_cast<size_t>(depth) * 2, ' ') : "";
    int inner = indent ? depth + 1 : -1;
    switch (v.type()) {
    case Json::realValue: o += number(v.asDouble()); return;
    case Json::arrayValue:
        if (v.empty()) { o += "[]"; return; }
        o += '[';
        for (Json::ArrayIndex i = 0; i < v.size(); ++i) {
            if (i) o += ',';
            o += pad;
            write(v[i], o, inner, str);
        }
        o += close + "]";
        return;
    case Json::objectValue: {
        if (v.empty()) { o += "{}"; return; }
        o += '{';
        bool first = true;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (!first) o += ',';
            first = false;
            o += pad + Json::writeString(str, Json::Value(it.name())) + (indent ? ": " : ":");
            write(*it, o, inner, str);
        }
        o += close + "}";
        return;
    }
    default: o += Json::writeString(str, v); return;   // null, bool, ints, strings
    }
}

Value make(Json::Value v) { return Value(std::make_shared<const JsonDoc>(JsonDoc{std::move(v)})); }

} // namespace

Value jsonEmpty() { return make(Json::Value(Json::nullValue)); }

Value jsonLoad(const Interp& in, const Value& src, int line) {
    if (src.isStr()) {
        Json::CharReaderBuilder b;
        b["collectComments"] = false;
        b["failIfExtra"] = true;           // "{} junk" is an error, not a silently ignored tail
        b["rejectDupKeys"] = false;
        b["stackLimit"] = kMaxDepth;
        std::unique_ptr<Json::CharReader> r(b.newCharReader());
        const std::string& s = src.s().s;
        Json::Value v;
        std::string err;
        if (!r->parse(s.data(), s.data() + s.size(), &v, &err)) in.raise("JSONError", oneLine(err), line);
        return make(std::move(v));
    }
    if (src.isBytes()) return make(fromBytes(src.b().b));
    if (src.isList()) {   // flat arrays only, for processes that expect them
        Json::Value a(Json::arrayValue);
        for (auto& x : src.l().items) {
            Json::Value s;
            if (x.isFloat() && !std::isfinite(x.as<double>())) in.raise("ValueError", "JSON has no NaN or infinity", line);
            if (!scalar(x, s))
                in.raise("TypeError", "a JSON array takes only None, bool, int, float, str and bytes, not " + typeName(x) +
                                          " (put nested data in a dict)", line);
            a.append(std::move(s));
        }
        return make(std::move(a));
    }
    if (src.isDict()) return make(fromValue(in, src, line, 0));
    if (src.is<std::shared_ptr<const JsonDoc>>()) return src;   // immutable: sharing is copying
    in.raise("TypeError", "JSON << takes str, bytes, list, dict or JSON, not " + typeName(src), line);
}

Value jsonValue(const JsonDoc& d) { return toValue(d.v); }

std::string jsonText(const JsonDoc& d, bool indented) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    b["emitUTF8"] = true;
    std::string o;
    write(d.v, o, indented ? 0 : -1, b);
    return o;
}

bool jsonEqual(const JsonDoc& a, const JsonDoc& b) { return a.v == b.v; }

} // namespace dolphin
