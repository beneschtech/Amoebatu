/**
 * @file dcl.cpp
 * @brief `.dcl` writer and loader (format: doc/PCODE.md §6). The verifier is in verify.cpp.
 *
 * Encoding inside sections: unsigned integers are LEB128 varints, signed ones
 * zigzag varints, floats their 8 IEEE bytes, strings indexes into STRS.
 * Instruction words live in CODE as little-endian u32s, so code could be
 * mapped and run in place; functions refer to (offset, length) in it.
 *
 * The loader treats the file as hostile: every count is checked against the
 * bytes left before anything is allocated, every index against its table,
 * and every section must be consumed exactly.
 */
#include "dcl.hpp"

#include <cstring>
#include <map>
#include <set>

#include "value.hpp"

namespace dolphin {

namespace {

// ------------------------------------------------------------------ constants

constexpr unsigned char kMagic[8] = {0x89, 'D', 'C', 'L', '\r', '\n', 0x1a, '\n'};
constexpr size_t kHeaderSize = 64;
constexpr size_t kDirEntrySize = 24;
constexpr unsigned kMaxSections = 16;
/// Identifies the compiler that wrote a file (8 bytes in the header).
constexpr char kCompilerId[8] = {'d', 'o', 'l', 'p', 'h', 'n', '0', '2'};

constexpr uint32_t fourcc(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[0])) |
           static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 8 |
           static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 16 |
           static_cast<uint32_t>(static_cast<unsigned char>(s[3])) << 24;
}
constexpr uint32_t kMETA = fourcc("META"), kSTRS = fourcc("STRS"), kCODE = fourcc("CODE"), kFUNC = fourcc("FUNC"),
                   kIMPT = fourcc("IMPT"), kEXPT = fourcc("EXPT"), kDEBG = fourcc("DEBG"), kSIGN = fourcc("SIGN");

/// Header flags.
constexpr uint32_t kFlagDebug = 1u << 0, kFlagSigned = 1u << 1;

/// Constant tags (low 7 bits); bit 7 marks a constant used as a dict key by INDEXK.
enum ConstTag : uint8_t { kNone = 0, kFalse, kTrue, kInt, kFloat, kStr, kBytes, kBuiltin, kFunction };
constexpr uint8_t kKeyed = 0x80;

/// FUNC entry flags.
constexpr uint64_t kFnModule = 1u << 0;

// ------------------------------------------------------------------ writing

struct Out {
    std::string b;
    void u8(uint8_t v) { b.push_back(static_cast<char>(v)); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }
    void varint(uint64_t v) {
        while (v >= 0x80) {
            u8(static_cast<uint8_t>(v | 0x80));
            v >>= 7;
        }
        u8(static_cast<uint8_t>(v));
    }
    void zz(int64_t v) { varint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
    void raw(const void* p, size_t n) { b.append(static_cast<const char*>(p), n); }
};

/// Deduplicating string table.
struct StrTable {
    std::vector<std::string> list;
    std::unordered_map<std::string, uint64_t> index;
    uint64_t add(const std::string& s) {
        auto it = index.find(s);
        if (it != index.end()) return it->second;
        list.push_back(s);
        return index[s] = list.size() - 1;
    }
};

/// Every Def in the module, in source (pre-)order: the function table.
void collectDefs(const Block& b, std::vector<const Def*>& out) {
    for (auto& s : b) {
        switch (s->k) {
        case Stmt::K::Def: {
            auto& d = static_cast<const Def&>(*s);
            out.push_back(&d);
            collectDefs(d.body, out);
            break;
        }
        case Stmt::K::If:
            for (auto& [c, body] : static_cast<const If&>(*s).branches) collectDefs(body, out);
            collectDefs(static_cast<const If&>(*s).orelse, out);
            break;
        case Stmt::K::While: collectDefs(static_cast<const While&>(*s).body, out); break;
        case Stmt::K::For: collectDefs(static_cast<const For&>(*s).body, out); break;
        case Stmt::K::Try:
            collectDefs(static_cast<const Try&>(*s).body, out);
            for (auto& h : static_cast<const Try&>(*s).handlers) collectDefs(h.body, out);
            break;
        default: break;
        }
    }
}

/// A parameter default as a constant, or throw: a .dcl stores defaults as values.
Value defaultValue(const Def& d, const Param& p) {
    const Expr& e = *p.dflt;
    bool neg = false;
    const Expr* x = &e;
    if (x->k == Expr::K::Unary && static_cast<const Unary&>(*x).op == "-") {
        neg = true;
        x = static_cast<const Unary&>(*x).operand.get();
    }
    switch (x->k) {
    case Expr::K::Int: return Value(neg ? -static_cast<const IntLit&>(*x).v : static_cast<const IntLit&>(*x).v);
    case Expr::K::Float: return Value(neg ? -static_cast<const FloatLit&>(*x).v : static_cast<const FloatLit&>(*x).v);
    default: break;
    }
    if (!neg) {
        switch (x->k) {
        case Expr::K::Str: return Value::str(static_cast<const StrLit&>(*x).v);
        case Expr::K::Bytes: return Value::bytes(static_cast<const BytesLit&>(*x).v);
        case Expr::K::Bool: return Value(static_cast<const BoolLit&>(*x).v);
        case Expr::K::None: return Value();
        default: break;
        }
    }
    throw DclError("line " + std::to_string(d.line) + ": default for parameter '" + p.name + "' of " + d.name +
                   "() must be a literal to compile to .dcl");
}

class Writer {
public:
    Writer(const Module& m, const std::unordered_map<const Def*, std::unique_ptr<PCode>>& fns, const PCode* init)
        : m_(m), fns_(fns), init_(init) {
        collectDefs(m.body, defs_);
        for (size_t i = 0; i < defs_.size(); ++i) index_[defs_[i]] = i;
    }

    std::string run(const std::string& source, bool debug) {
        if (!init_) throw DclError("module-level code can't be compiled to p-code");
        std::vector<const PCode*> code;
        for (const Def* d : defs_) {
            auto it = fns_.find(d);
            if (it == fns_.end())
                throw DclError("line " + std::to_string(d->line) + ": function " + d->name +
                               "() can't be compiled to p-code (slice or attribute assignment, or more than 256 registers)");
            code.push_back(it->second.get());
        }

        // FUNC: functions in table order, then the module entry.
        Out func;
        func.varint(defs_.size());
        for (size_t i = 0; i < defs_.size(); ++i) function(func, *defs_[i], *code[i], false);
        function(func, *init_->def, *init_, true);

        Out meta;
        meta.varint(strs_.add(m_.file));
        std::string stem = m_.file;
        if (auto sl = stem.find_last_of('/'); sl != std::string::npos) stem = stem.substr(sl + 1);
        if (auto dot = stem.rfind('.'); dot != std::string::npos) stem = stem.substr(0, dot);
        meta.varint(strs_.add(stem));

        Out impt;
        impt.varint(0);   // no imports until the language has modules
        Out expt;
        std::vector<std::pair<uint64_t, uint64_t>> exports;
        for (size_t i = 0; i < defs_.size(); ++i)
            if (defs_[i]->slot < 0) exports.emplace_back(strs_.add(defs_[i]->name), i);
        expt.varint(exports.size());
        for (auto& [n, i] : exports) {
            expt.varint(n);
            expt.varint(i);
        }

        Out debg;
        if (debug) {
            auto names = [&](const PCode& p) {
                debg.varint(p.regNames.size());
                for (auto& n : p.regNames) debg.varint(n.empty() ? 0 : strs_.add(n) + 1);
            };
            for (const PCode* p : code) names(*p);
            names(*init_);
        }

        Out strs;   // last: everything above added its strings
        strs.varint(strs_.list.size());
        for (auto& s : strs_.list) {
            strs.varint(s.size());
            strs.raw(s.data(), s.size());
        }

        std::vector<std::pair<uint32_t, const std::string*>> sections = {
            {kMETA, &meta.b}, {kSTRS, &strs.b}, {kCODE, &code_.b}, {kFUNC, &func.b}, {kIMPT, &impt.b}, {kEXPT, &expt.b}};
        if (debug) sections.emplace_back(kDEBG, &debg.b);

        Out file;
        file.raw(kMagic, 8);
        file.u16(kDclMajor);
        file.u16(kDclMinor);
        file.u32(debug ? kFlagDebug : 0);
        auto h = sha256(source);
        file.raw(h.data(), h.size());
        file.raw(kCompilerId, 8);
        file.u32(static_cast<uint32_t>(sections.size()));
        file.u32(0);
        size_t off = kHeaderSize + sections.size() * kDirEntrySize;
        auto align = [](size_t x) { return (x + 7) & ~size_t{7}; };
        off = align(off);
        for (auto& [type, data] : sections) {
            file.u32(type);
            file.u32(0);
            file.u64(off);
            file.u64(data->size());
            off = align(off + data->size());
        }
        for (auto& [type, data] : sections) {
            file.b.resize(align(file.b.size()), '\0');
            file.b += *data;
        }
        return std::move(file.b);
    }

private:
    void konst(Out& o, const Value& v, bool keyed) {
        uint8_t k = keyed ? kKeyed : 0;
        if (v.isNone()) {
            o.u8(k | kNone);
        } else if (v.is<bool>()) {
            o.u8(k | (v.as<bool>() ? kTrue : kFalse));
        } else if (v.isInt()) {
            o.u8(k | kInt);
            o.zz(v.as<int64_t>());
        } else if (v.isFloat()) {
            o.u8(k | kFloat);
            uint64_t bits;
            double d = v.as<double>();
            std::memcpy(&bits, &d, 8);
            o.u64(bits);
        } else if (v.isStr()) {
            o.u8(k | kStr);
            o.varint(strs_.add(v.s().s));
        } else if (v.isBytes()) {
            o.u8(k | kBytes);
            o.varint(strs_.add(v.b().b));
        } else if (v.is<std::shared_ptr<const Builtin>>()) {
            o.u8(k | kBuiltin);
            o.varint(strs_.add(v.as<std::shared_ptr<const Builtin>>()->name));
        } else if (v.is<std::shared_ptr<const ExcClass>>()) {
            o.u8(k | kBuiltin);
            o.varint(strs_.add(v.as<std::shared_ptr<const ExcClass>>()->name));
        } else if (v.is<std::shared_ptr<const Function>>()) {
            o.u8(k | kFunction);
            o.varint(defIndex(v.as<std::shared_ptr<const Function>>()->def));
        } else {
            throw DclError("internal: constant of type " + typeName(v) + " can't be written");
        }
    }
    uint64_t defIndex(const Def* d) const {
        auto it = index_.find(d);
        if (it == index_.end()) throw DclError("internal: reference to a function outside the module");
        return it->second;
    }

    void function(Out& o, const Def& d, const PCode& p, bool module) {
        o.varint(strs_.add(d.name));
        o.zz(d.line);
        o.zz(d.slot);
        o.varint(module ? kFnModule : 0);
        o.varint(d.params.size());
        for (auto& par : d.params) {
            o.varint(strs_.add(par.name));
            o.u8(par.dflt ? 1 : 0);
            if (par.dflt) konst(o, defaultValue(d, par), false);
        }
        o.varint(static_cast<uint64_t>(module ? 0 : d.numLocals));
        o.varint(p.nregs);
        o.varint(p.niters);
        uint64_t codeOff = code_.b.size() / 4;
        for (uint32_t w : p.code) code_.u32(w);
        o.varint(codeOff);
        o.varint(p.code.size());

        o.varint(p.consts.size());
        for (size_t i = 0; i < p.consts.size(); ++i) konst(o, p.consts[i], !p.constKeys[i].empty());
        o.varint(p.names.size());
        for (auto& n : p.names) o.varint(strs_.add(n));

        o.varint(p.calls.size());
        for (const CallSite& cs : p.calls) {
            o.u8(static_cast<uint8_t>(cs.kind));
            switch (cs.kind) {
            case CallSite::Kind::Builtin: o.varint(strs_.add(cs.builtin->name)); break;
            case CallSite::Kind::Function:
                o.varint(defIndex(cs.def));
                o.varint(strs_.add(cs.name));
                break;
            case CallSite::Kind::Method: o.varint(strs_.add(cs.name)); break;
            case CallSite::Kind::Value: break;
            }
            o.u8(cs.self);   // receiver (Method), callee (Value), argument rows (PMAP)
            o.varint(cs.args.size());
            for (uint8_t r : cs.args) o.u8(r);
            o.varint(cs.kwNames.size());
            for (auto& k : cs.kwNames) o.varint(strs_.add(k));
            o.u8(cs.discard ? 1 : 0);
        }

        o.varint(p.excTable.size());
        for (const ExcEntry& x : p.excTable) {
            o.varint(x.start);
            o.varint(x.end);
            o.varint(x.handler);
            o.u8(x.handlerBody ? 1 : 0);
        }

        int prev = 0;
        for (int line : p.lines) {
            o.zz(static_cast<int64_t>(line) - prev);
            prev = line;
        }

        // Claims; the loader recomputes these and keeps the more conservative.
        o.varint(module ? 0 : d.needs);
        o.u8(d.mutatesOther ? 1 : 0);
        for (size_t i = 0; i < d.params.size(); ++i)
            o.u8(i < d.mutatedParams.size() && d.mutatedParams[i] ? 1 : 0);
    }

    const Module& m_;
    const std::unordered_map<const Def*, std::unique_ptr<PCode>>& fns_;
    const PCode* init_;
    std::vector<const Def*> defs_;
    std::unordered_map<const Def*, size_t> index_;
    StrTable strs_;
    Out code_;
};

// ------------------------------------------------------------------ reading

/// Bounds-checked reader over one section.
class In {
public:
    In(const uint8_t* p, size_t n, const char* what) : p_(p), end_(p + n), what_(what) {}
    [[noreturn]] void fail(const std::string& why) const { throw DclError(std::string(what_) + ": " + why); }
    uint8_t u8() {
        if (p_ >= end_) fail("truncated");
        return *p_++;
    }
    uint64_t u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(u8()) << (8 * i);
        return v;
    }
    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0;; shift += 7) {
            uint8_t b = u8();
            if (shift == 63 && (b & 0x7e)) fail("varint overflow");
            v |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
            if (shift >= 63) fail("varint too long");
        }
    }
    int64_t zz() {
        uint64_t u = varint();
        return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
    }
    /// A varint that must be <= @p max.
    uint64_t upTo(uint64_t max, const char* field) {
        uint64_t v = varint();
        if (v > max) fail(std::string(field) + " out of range");
        return v;
    }
    /// An element count; each element takes at least @p minBytes, so a count
    /// larger than the bytes left is rejected before anything is allocated.
    size_t count(size_t minBytes, const char* field) {
        uint64_t n = varint();
        if (minBytes && n > left() / minBytes) fail(std::string(field) + " count exceeds section");
        return static_cast<size_t>(n);
    }
    int intRange(int64_t lo, int64_t hi, const char* field) {
        int64_t v = zz();
        if (v < lo || v > hi) fail(std::string(field) + " out of range");
        return static_cast<int>(v);
    }
    std::string bytes(size_t n) {
        if (n > left()) fail("truncated");
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }
    size_t left() const { return static_cast<size_t>(end_ - p_); }
    void done() const {
        if (p_ != end_) fail("unexpected trailing bytes");
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    const char* what_;
};

class Loader {
public:
    Loader(const std::string& img, const LoadHooks& hooks)
        : img_(reinterpret_cast<const uint8_t*>(img.data())), size_(img.size()), hooks_(hooks) {}

    std::unique_ptr<Program> run() {
        header();
        strings();
        code();
        meta();
        imports();
        functions();
        exports();
        debugInfo();
        verifyProgram(*prog_);
        return std::move(prog_);
    }

private:
    struct Section {
        const uint8_t* p = nullptr;
        size_t n = 0;
        bool present = false;
    };

    [[noreturn]] static void fail(const std::string& why) { throw DclError(why); }

    void header() {
        if (size_ < kHeaderSize) fail("not a .dcl file (too short)");
        if (std::memcmp(img_, kMagic, 8) != 0) fail("not a .dcl file (bad magic number)");
        In h(img_ + 8, kHeaderSize - 8, "header");
        uint16_t major = static_cast<uint16_t>(h.u8() | h.u8() << 8);
        uint16_t minor = static_cast<uint16_t>(h.u8() | h.u8() << 8);
        if (major != kDclMajor)
            fail("format version " + std::to_string(major) + "." + std::to_string(minor) + " is not supported (this Dolphin reads " +
                 std::to_string(kDclMajor) + ".x)");
        if (minor > kDclMinor)
            fail("format version " + std::to_string(major) + "." + std::to_string(minor) + " is newer than this Dolphin (" +
                 std::to_string(kDclMajor) + "." + std::to_string(kDclMinor) + ")");
        uint32_t flags = 0;
        for (int i = 0; i < 4; ++i) flags |= static_cast<uint32_t>(h.u8()) << (8 * i);
        if (flags & ~(kFlagDebug | kFlagSigned)) fail("unknown header flags");
        if (flags & kFlagSigned) fail("signed .dcl files are not supported yet");
        prog_ = std::make_unique<Program>();
        for (auto& b : prog_->sourceHash) b = h.u8();
        h.bytes(8);   // compiler id: informational
        uint32_t nsec = 0;
        for (int i = 0; i < 4; ++i) nsec |= static_cast<uint32_t>(h.u8()) << (8 * i);
        if (nsec == 0 || nsec > kMaxSections) fail("bad section count");
        size_t dirEnd = kHeaderSize + nsec * kDirEntrySize;
        if (dirEnd > size_) fail("section directory runs past the end of the file");

        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        In d(img_ + kHeaderSize, nsec * kDirEntrySize, "section directory");
        for (uint32_t i = 0; i < nsec; ++i) {
            uint32_t type = 0;
            for (int k = 0; k < 4; ++k) type |= static_cast<uint32_t>(d.u8()) << (8 * k);
            d.bytes(4);
            uint64_t off = d.u64(), n = d.u64();
            if (off % 8 || off < dirEnd || off > size_ || n > size_ - off) fail("section out of bounds");
            for (auto& [o2, n2] : ranges)
                if (off < o2 + n2 && o2 < off + n) fail("sections overlap");
            ranges.emplace_back(off, n);
            Section* s = nullptr;
            if (type == kMETA) s = &meta_;
            else if (type == kSTRS) s = &strs_;
            else if (type == kCODE) s = &code_;
            else if (type == kFUNC) s = &func_;
            else if (type == kIMPT) s = &impt_;
            else if (type == kEXPT) s = &expt_;
            else if (type == kDEBG) s = &debg_;
            else if (type == kSIGN) fail("signed .dcl files are not supported yet");
            else fail("unknown section type");
            if (s->present) fail("duplicate section");
            *s = Section{img_ + off, static_cast<size_t>(n), true};
        }
        for (const Section* s : {&meta_, &strs_, &code_, &func_, &impt_, &expt_})
            if (!s->present) fail("missing a required section");
        if (((flags & kFlagDebug) != 0) != debg_.present) fail("debug flag doesn't match the sections");
    }

    void strings() {
        In s(strs_.p, strs_.n, "STRS");
        size_t n = s.count(1, "string");
        strings_.reserve(n);
        for (size_t i = 0; i < n; ++i) strings_.push_back(s.bytes(s.count(0, "string length")));
        s.done();
        strCache_.resize(strings_.size());
    }
    const std::string& str(In& in) {
        uint64_t i = in.varint();
        if (i >= strings_.size()) in.fail("string index out of range");
        return strings_[i];
    }
    uint64_t strIndex(In& in) {
        uint64_t i = in.varint();
        if (i >= strings_.size()) in.fail("string index out of range");
        return i;
    }

    void code() {
        if (code_.n % 4) fail("CODE: size is not a whole number of instructions");
        words_.resize(code_.n / 4);
        for (size_t i = 0; i < words_.size(); ++i) {
            const uint8_t* b = code_.p + 4 * i;
            words_[i] = static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 |
                        static_cast<uint32_t>(b[2]) << 16 | static_cast<uint32_t>(b[3]) << 24;
        }
    }

    void meta() {
        In m(meta_.p, meta_.n, "META");
        prog_->file = str(m);
        str(m);   // module name: used once imports exist
        m.done();
    }

    void imports() {
        In m(impt_.p, impt_.n, "IMPT");
        if (m.varint() != 0) m.fail("imports are not supported yet");
        m.done();
    }

    Value konst(In& in, bool* keyed) {
        uint8_t tag = in.u8();
        if (keyed) *keyed = (tag & kKeyed) != 0;
        else if (tag & kKeyed) in.fail("unexpected key flag");
        switch (tag & ~kKeyed) {
        case kNone: return Value();
        case kFalse: return Value(false);
        case kTrue: return Value(true);
        case kInt: return Value(in.zz());
        case kFloat: {
            uint64_t bits = in.u64();
            double d;
            std::memcpy(&d, &bits, 8);
            return Value(d);
        }
        case kStr: {
            uint64_t i = strIndex(in);
            if (!strCache_[i]) {
                strCache_[i] = std::make_shared<const Str>(strings_[i]);
                prog_->strs.push_back(strCache_[i]);
            }
            return Value::borrowedStr(strCache_[i].get());   // uncounted, as compiled string literals are
        }
        case kBytes: return Value::bytes(str(in));
        case kBuiltin: {
            const std::string& n = str(in);
            const Value* v = hooks_.builtin ? hooks_.builtin(n) : nullptr;
            if (!v) in.fail("unknown builtin '" + n + "'");
            return *v;
        }
        case kFunction: {
            if (prog_->defs.empty()) in.fail("function index out of range");
            uint64_t i = in.upTo(prog_->defs.size() - 1, "function index");
            return Value(std::make_shared<const Function>(Function{prog_->defs[i].get()}));
        }
        default: in.fail("unknown constant tag");
        }
    }

    /// A default value back as a literal expression (the call path evaluates defaults per call).
    static ExprP defaultExpr(const Value& v, int line, In& in) {
        if (v.isNone()) return std::make_unique<NoneLit>(line);
        if (v.is<bool>()) return std::make_unique<BoolLit>(line, v.as<bool>());
        if (v.isInt()) return std::make_unique<IntLit>(line, v.as<int64_t>());
        if (v.isFloat()) return std::make_unique<FloatLit>(line, v.as<double>());
        if (v.isStr()) return std::make_unique<StrLit>(line, v.s().s);
        if (v.isBytes()) return std::make_unique<BytesLit>(line, v.b().b);
        in.fail("parameter default must be a literal");
    }

    void functions() {
        In f(func_.p, func_.n, "FUNC");
        size_t n = f.count(8, "function");
        if (n > 65536) f.fail("too many functions");
        for (size_t i = 0; i < n; ++i) {
            prog_->defs.push_back(std::make_unique<Def>(0));
            prog_->code.push_back(std::make_unique<PCode>());
        }
        for (size_t i = 0; i < n; ++i) function(f, *prog_->defs[i], *prog_->code[i], false);
        prog_->moduleDef = std::make_unique<Def>(0);
        prog_->init = std::make_unique<PCode>();
        function(f, *prog_->moduleDef, *prog_->init, true);
        f.done();
    }

    void function(In& f, Def& d, PCode& p, bool module) {
        d.name = str(f);
        d.line = f.intRange(0, 1 << 30, "line");
        d.slot = f.intRange(-1, 255, "slot");
        uint64_t flags = f.varint();
        if (flags & ~kFnModule) f.fail("unknown function flags");
        if (((flags & kFnModule) != 0) != module) f.fail("module entry out of place");
        size_t np = f.count(2, "parameter");
        if (np > 255 || (module && np)) f.fail("bad parameter count");
        for (size_t i = 0; i < np; ++i) {
            Param par;
            par.name = str(f);
            par.slot = static_cast<int>(i);
            uint8_t has = f.u8();
            if (has > 1) f.fail("bad default flag");
            if (has) par.dflt = defaultExpr(konst(f, nullptr), d.line, f);
            d.params.push_back(std::move(par));
        }
        d.numLocals = static_cast<int>(f.upTo(256, "locals"));
        if (static_cast<size_t>(d.numLocals) < np || (module && d.numLocals)) f.fail("bad local count");
        p.def = &d;
        p.nparams = static_cast<unsigned>(np);
        p.nregs = static_cast<unsigned>(f.upTo(256, "register count"));
        if (p.nregs < static_cast<unsigned>(d.numLocals)) f.fail("fewer registers than locals");
        p.niters = static_cast<unsigned>(f.upTo(256, "iterator count"));
        uint64_t off = f.varint(), len = f.varint();
        if (len == 0 || off > words_.size() || len > words_.size() - off) f.fail("code range out of bounds");
        p.code.assign(words_.begin() + static_cast<long>(off), words_.begin() + static_cast<long>(off + len));

        size_t nk = f.count(1, "constant");
        if (nk > 65536) f.fail("too many constants");
        for (size_t i = 0; i < nk; ++i) {
            bool keyed = false;
            p.consts.push_back(konst(f, &keyed));
            std::string key;
            if (keyed && !dictKey(p.consts.back(), key)) f.fail("key constant is not hashable");
            p.constHashes.push_back(keyed ? Dict::hashKey(key) : 0);
            p.constKeys.push_back(std::move(key));
        }
        size_t nn = f.count(1, "name");
        if (nn > 65536) f.fail("too many names");
        for (size_t i = 0; i < nn; ++i) p.names.push_back(str(f));

        size_t nc = f.count(4, "call site");
        if (nc > 65536) f.fail("too many call sites");
        for (size_t i = 0; i < nc; ++i) {
            CallSite cs;
            uint8_t kind = f.u8();
            if (kind > static_cast<uint8_t>(CallSite::Kind::Value)) f.fail("unknown call site kind");
            cs.kind = static_cast<CallSite::Kind>(kind);
            switch (cs.kind) {
            case CallSite::Kind::Builtin: {
                const std::string& bn = str(f);
                const Value* v = hooks_.builtin ? hooks_.builtin(bn) : nullptr;
                if (!v || !v->is<std::shared_ptr<const Builtin>>()) f.fail("unknown builtin '" + bn + "'");
                cs.builtinValue = *v;
                cs.builtin = v->as<std::shared_ptr<const Builtin>>().get();
                cs.name = bn;
                break;
            }
            case CallSite::Kind::Function: {
                if (prog_->defs.empty()) f.fail("function index out of range");
                cs.def = prog_->defs[f.upTo(prog_->defs.size() - 1, "function index")].get();
                cs.name = str(f);
                break;
            }
            case CallSite::Kind::Method:
                cs.name = str(f);
                cs.method = methodByName(cs.name);
                break;
            case CallSite::Kind::Value: break;
            }
            cs.self = f.u8();
            size_t na = f.count(1, "argument");
            if (na > 255) f.fail("too many arguments");
            for (size_t k = 0; k < na; ++k) cs.args.push_back(f.u8());
            size_t nkw = f.count(1, "keyword");
            if (nkw > na) f.fail("more keywords than arguments");
            for (size_t k = 0; k < nkw; ++k) cs.kwNames.push_back(str(f));
            uint8_t disc = f.u8();
            if (disc > 1) f.fail("bad discard flag");
            cs.discard = disc != 0;
            p.calls.push_back(std::move(cs));
        }

        size_t ne = f.count(4, "exception entry");
        for (size_t i = 0; i < ne; ++i) {
            ExcEntry x{};
            x.start = static_cast<uint32_t>(f.upTo(len, "exception range"));
            x.end = static_cast<uint32_t>(f.upTo(len, "exception range"));
            x.handler = static_cast<uint32_t>(f.upTo(len, "exception handler"));
            uint8_t hb = f.u8();
            if (hb > 1) f.fail("bad handler flag");
            x.handlerBody = hb != 0;
            p.excTable.push_back(x);
        }

        int64_t line = 0;
        p.lines.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            line += f.zz();
            if (line < 0 || line > (1 << 30)) f.fail("line out of range");
            p.lines.push_back(static_cast<int>(line));
        }

        d.needs = static_cast<unsigned>(f.upTo(0xffffffffu, "needs"));
        uint8_t mo = f.u8();
        if (mo > 1) f.fail("bad mutation flag");
        d.mutatesOther = mo != 0;
        d.mutatedParams.assign(np, false);
        for (size_t i = 0; i < np; ++i) {
            uint8_t m = f.u8();
            if (m > 1) f.fail("bad mutation flag");
            d.mutatedParams[i] = m != 0;
        }
        p.regNames.assign(p.nregs, "");
        for (size_t i = 0; i < np && i < p.nregs; ++i) p.regNames[i] = d.params[i].name;

        p.globalCache = std::make_unique<std::atomic<const Value*>[]>(p.names.size());
        for (size_t i = 0; i < p.names.size(); ++i) p.globalCache[i].store(nullptr);
        p.calleeBound = std::make_unique<std::atomic<bool>[]>(p.calls.size());
        for (size_t i = 0; i < p.calls.size(); ++i) p.calleeBound[i].store(false);
    }

    void exports() {
        In e(expt_.p, expt_.n, "EXPT");
        size_t n = e.count(2, "export");
        std::set<std::string> seen;
        for (size_t i = 0; i < n; ++i) {
            const std::string& name = str(e);
            if (prog_->defs.empty()) e.fail("export of a missing function");
            e.upTo(prog_->defs.size() - 1, "export index");
            if (!seen.insert(name).second) e.fail("duplicate export");
        }
        e.done();
    }

    void debugInfo() {
        if (!debg_.present) return;
        In d(debg_.p, debg_.n, "DEBG");
        auto names = [&](PCode& p) {
            size_t n = d.count(1, "register name");
            if (n != p.nregs) d.fail("register name count doesn't match");
            for (size_t i = 0; i < n; ++i) {
                uint64_t s = d.varint();
                if (s > strings_.size()) d.fail("string index out of range");
                if (s) p.regNames[i] = strings_[s - 1];
            }
        };
        for (auto& p : prog_->code) names(*p);
        names(*prog_->init);
        d.done();
    }

    const uint8_t* img_;
    size_t size_;
    const LoadHooks& hooks_;
    std::unique_ptr<Program> prog_;
    Section meta_, strs_, code_, func_, impt_, expt_, debg_;
    std::vector<std::string> strings_;
    std::vector<std::shared_ptr<const Str>> strCache_;
    std::vector<uint32_t> words_;
};

// ------------------------------------------------------------------ SHA-256 (FIPS 180-4)

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

} // namespace

std::array<uint8_t, 32> sha256(const std::string& data) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string m = data;
    uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    m.push_back(static_cast<char>(0x80));
    while (m.size() % 64 != 56) m.push_back('\0');
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<char>(bits >> (8 * i)));
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto* b = reinterpret_cast<const uint8_t*>(m.data() + off + 4 * i);
            w[i] = static_cast<uint32_t>(b[0]) << 24 | static_cast<uint32_t>(b[1]) << 16 | static_cast<uint32_t>(b[2]) << 8 | b[3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 4; ++k) out[static_cast<size_t>(4 * i + k)] = static_cast<uint8_t>(h[i] >> (24 - 8 * k));
    return out;
}

bool isDcl(const std::string& data) { return data.size() >= 8 && std::memcmp(data.data(), kMagic, 8) == 0; }

std::string writeDcl(const Module& m, const std::unordered_map<const Def*, std::unique_ptr<PCode>>& fns,
                     const PCode* init, const std::string& source, bool debug) {
    return Writer(m, fns, init).run(source, debug);
}

std::unique_ptr<Program> loadDcl(const std::string& image, const LoadHooks& hooks) {
    return Loader(image, hooks).run();
}

} // namespace dolphin
