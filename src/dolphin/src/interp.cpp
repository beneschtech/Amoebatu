/**
 * @file interp.cpp
 * @brief Interpreter core: statements, expressions, operators, calls.
 *
 * Builtin functions and methods live in builtins.cpp.
 *
 * Control flow: `return`, `break` and `continue` propagate as Flow values
 * returned up through execBlock(), not as C++ exceptions, because they're
 * common and cheap. Dolphin exceptions do use C++ exceptions (DolphinError),
 * because they must unwind any number of C++ frames to the nearest `try`.
 */
#include "interp.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace dolphin {

/// Maximum Dolphin call depth before RecursionError. Same default as CPython.
/// It guards the C++ stack, since each Dolphin call uses several C++ frames.
static constexpr int kMaxDepth = 1000;

Interp::Interp(const Module& m) : mod_(m) { installBuiltins(); }

void Interp::raise(const std::string& cls, const std::string& msg, int line) const {
    throw DolphinError{cls, msg, line};
}

int Interp::run(const std::vector<std::string>& args) {
    Frame g{&globals_};
    Value ret;
    try {
        execBlock(mod_.body, g, ret);
        auto it = globals_.find("main");
        if (it != globals_.end() && it->second.is<std::shared_ptr<const Function>>()) {
            Args a;
            for (auto& s : args) a.push_back(Value::str(s));
            Kwargs kw;
            Value r = callValue(it->second, a, kw, 0);
            if (r.isInt()) return static_cast<int>(r.as<int64_t>());
        }
        return 0;
    } catch (const DolphinError& e) {
        std::cerr << mod_.file << ":" << e.line << ": " << e.cls;
        if (!e.msg.empty()) std::cerr << ": " << e.msg;
        std::cerr << "\n";
        return 1;
    }
}

// --------------------------------------------------------------- statements

Interp::Flow Interp::execBlock(const Block& b, Frame& f, Value& ret) {
    for (auto& s : b) {
        Flow fl = exec(*s, f, ret);
        if (fl != Flow::Normal) return fl;
    }
    return Flow::Normal;
}

bool Interp::excMatches(const std::string& handlerCls, const std::string& errCls) const {
    for (std::string c = errCls; !c.empty();) {
        if (c == handlerCls) return true;
        auto it = excParent_.find(c);
        if (it == excParent_.end()) break;
        c = it->second;
    }
    return false;
}

Interp::Flow Interp::exec(const Stmt& s, Frame& f, Value& ret) {
    switch (s.k) {
    case Stmt::K::Expr:
        eval(*static_cast<const ExprStmt&>(s).e, f);
        return Flow::Normal;

    case Stmt::K::Assign: {
        auto& a = static_cast<const Assign&>(s);
        assign(*a.target, eval(*a.value, f), f);
        return Flow::Normal;
    }

    // `t op= v` evaluates t's current value, computes, and assigns back.
    // Note the target expression is evaluated twice (once to read, once to
    // write), which is only observable if it has side effects.
    case Stmt::K::AugAssign: {
        auto& a = static_cast<const AugAssign&>(s);
        Value cur = eval(*a.target, f);
        Value rhs = eval(*a.value, f);
        assign(*a.target, binary(a.op, cur, rhs, s.line), f);
        return Flow::Normal;
    }

    case Stmt::K::If: {
        auto& i = static_cast<const If&>(s);
        for (auto& [cond, body] : i.branches)
            if (truthy(eval(*cond, f))) return execBlock(body, f, ret);
        return execBlock(i.orelse, f, ret);
    }

    case Stmt::K::While: {
        auto& w = static_cast<const While&>(s);
        while (truthy(eval(*w.cond, f))) {
            Flow fl = execBlock(w.body, f, ret);
            if (fl == Flow::Break) break;
            if (fl == Flow::Return) return fl;
        }
        return Flow::Normal;
    }

    case Stmt::K::For: {
        auto& fo = static_cast<const For&>(s);
        for (auto& item : iterate(eval(*fo.iter, f), s.line)) {
            (*f.vars)[fo.var] = item;
            Flow fl = execBlock(fo.body, f, ret);
            if (fl == Flow::Break) break;
            if (fl == Flow::Return) return fl;
        }
        return Flow::Normal;
    }

    case Stmt::K::Def: {
        auto& d = static_cast<const Def&>(s);
        (*f.vars)[d.name] = Value(std::make_shared<const Function>(Function{&d}));
        return Flow::Normal;
    }

    case Stmt::K::Return: {
        auto& r = static_cast<const Return&>(s);
        ret = r.value ? eval(*r.value, f) : Value();
        return Flow::Return;
    }
    case Stmt::K::Break: return Flow::Break;
    case Stmt::K::Continue: return Flow::Continue;
    case Stmt::K::Pass: return Flow::Normal;

    // try / except (LANGUAGE.md §3.5).
    //
    // Decision 2, "variables the failing call would assign are left
    // untouched", falls out naturally here: in `x = f()`, if f raises, the
    // DolphinError unwinds before assign() runs, so x keeps its old value.
    //
    // handling_ records the exception being handled, so a bare `raise`
    // inside the handler can re-throw it. It's a stack because handlers can
    // nest.
    case Stmt::K::Try: {
        auto& t = static_cast<const Try&>(s);
        try {
            return execBlock(t.body, f, ret);
        } catch (const DolphinError& err) {
            for (auto& h : t.handlers) {
                if (!h.cls.empty()) {
                    Value hc = lookup(h.cls, f, h.line);
                    if (!hc.is<std::shared_ptr<const ExcClass>>())
                        raise("TypeError", "'" + h.cls + "' is not an exception class", h.line);
                    if (!excMatches(h.cls, err.cls)) continue;
                }
                if (!h.as.empty())
                    (*f.vars)[h.as] = Value(std::make_shared<const ExcValue>(ExcValue{err.cls, err.msg}));
                handling_.push_back(err);
                Flow fl;
                try {
                    fl = execBlock(h.body, f, ret);
                } catch (...) {
                    handling_.pop_back();
                    throw;
                }
                handling_.pop_back();
                return fl;
            }
            throw;
        }
    }

    case Stmt::K::Raise: {
        auto& r = static_cast<const Raise&>(s);
        if (!r.value) {
            if (handling_.empty()) raise("RuntimeError", "no active exception to re-raise", s.line);
            throw handling_.back();
        }
        Value v = eval(*r.value, f);
        if (v.is<std::shared_ptr<const ExcValue>>()) {
            auto& e = *v.as<std::shared_ptr<const ExcValue>>();
            raise(e.cls, e.msg, s.line);
        }
        if (v.is<std::shared_ptr<const ExcClass>>()) raise(v.as<std::shared_ptr<const ExcClass>>()->name, "", s.line);
        raise("TypeError", "can only raise exceptions, not " + typeName(v), s.line);
    }
    }
    return Flow::Normal;
}

void Interp::assign(const Expr& target, const Value& v, Frame& f) {
    switch (target.k) {
    case Expr::K::Name:
        (*f.vars)[static_cast<const Name&>(target).id] = v;
        return;
    case Expr::K::Index: {
        auto& ix = static_cast<const Index&>(target);
        Value obj = eval(*ix.obj, f);
        if (ix.index->k == Expr::K::Slice) raise("TypeError", "slice assignment is not supported", target.line);
        if (!obj.isList()) raise("TypeError", "'" + typeName(obj) + "' does not support item assignment", target.line);
        Value iv = eval(*ix.index, f);
        if (!iv.isInt()) raise("TypeError", "list indices must be int", target.line);
        auto& items = obj.l().items;
        int64_t i = iv.as<int64_t>();
        if (i < 0) i += static_cast<int64_t>(items.size());
        if (i < 0 || i >= static_cast<int64_t>(items.size()))
            raise("IndexError", "list assignment index out of range", target.line);
        items[static_cast<size_t>(i)] = v;
        return;
    }
    case Expr::K::Attr:
        raise("AttributeError", "objects with assignable attributes arrive with 'struct'", target.line);
    default:
        raise("SyntaxError", "cannot assign to expression", target.line);
    }
}

// -------------------------------------------------------------- expressions

Value Interp::lookup(const std::string& name, Frame& f, int line) {
    auto it = f.vars->find(name);
    if (it != f.vars->end()) return it->second;
    it = globals_.find(name);
    if (it != globals_.end()) return it->second;
    it = builtins_.find(name);
    if (it != builtins_.end()) return it->second;
    raise("NameError", "name '" + name + "' is not defined", line);
}

Value Interp::eval(const Expr& e, Frame& f) {
    switch (e.k) {
    case Expr::K::Int: return Value(static_cast<const IntLit&>(e).v);
    case Expr::K::Float: return Value(static_cast<const FloatLit&>(e).v);
    case Expr::K::Str: return Value::str(static_cast<const StrLit&>(e).v);
    case Expr::K::Bytes: return Value::bytes(static_cast<const BytesLit&>(e).v);
    case Expr::K::Bool: return Value(static_cast<const BoolLit&>(e).v);
    case Expr::K::None: return Value();
    case Expr::K::Name: return lookup(static_cast<const Name&>(e).id, f, e.line);

    case Expr::K::FStr: {
        std::string out;
        for (auto& p : static_cast<const FStr&>(e).parts) {
            if (!p.expr) {
                out += p.text;
                continue;
            }
            Value v = eval(*p.expr, f);
            out += p.spec.empty() ? toStr(v) : format(v, p.spec, e.line);
        }
        return Value::str(std::move(out));
    }

    case Expr::K::Unary: {
        auto& u = static_cast<const Unary&>(e);
        Value v = eval(*u.operand, f);
        if (u.op == "not") return Value(!truthy(v));
        if (v.is<bool>()) v = Value(static_cast<int64_t>(v.as<bool>()));
        if (u.op == "+" && v.isNum()) return v;
        if (u.op == "-") {
            if (v.isFloat()) return Value(-v.as<double>());
            if (v.isInt()) {
                if (v.as<int64_t>() == INT64_MIN) raise("OverflowError", "integer overflow", e.line);
                return Value(-v.as<int64_t>());
            }
        }
        if (u.op == "~" && v.isInt()) return Value(~v.as<int64_t>());
        raise("TypeError", "bad operand type for unary " + u.op + ": '" + typeName(v) + "'", e.line);
    }

    case Expr::K::Binary: {
        auto& b = static_cast<const Binary&>(e);
        Value l = eval(*b.lhs, f);
        Value r = eval(*b.rhs, f);
        return binary(b.op, l, r, e.line);
    }

    case Expr::K::BoolOp: {
        auto& b = static_cast<const BoolOp&>(e);
        Value l = eval(*b.lhs, f);
        if (b.op == "and") return truthy(l) ? eval(*b.rhs, f) : l;
        return truthy(l) ? l : eval(*b.rhs, f);
    }

    case Expr::K::Compare: {
        auto& c = static_cast<const Compare&>(e);
        Value l = eval(*c.first, f);
        for (auto& [op, rx] : c.rest) {
            Value r = eval(*rx, f);
            if (!compare(op, l, r, e.line)) return Value(false);
            l = r;
        }
        return Value(true);
    }

    case Expr::K::Call: {
        auto& c = static_cast<const Call&>(e);
        Value fn = eval(*c.fn, f);
        Args args;
        args.reserve(c.args.size());
        for (auto& a : c.args) args.push_back(eval(*a, f));
        Kwargs kw;
        for (auto& [k, v] : c.kwargs) kw.emplace_back(k, eval(*v, f));
        return callValue(fn, args, kw, e.line);
    }

    case Expr::K::Index: {
        auto& ix = static_cast<const Index&>(e);
        Value obj = eval(*ix.obj, f);
        return subscript(obj, *ix.index, f, e.line);
    }

    case Expr::K::Slice: raise("SyntaxError", "slice outside of subscript", e.line);

    case Expr::K::Attr: {
        auto& a = static_cast<const Attr&>(e);
        return attribute(eval(*a.obj, f), a.name, e.line);
    }

    case Expr::K::List: {
        std::vector<Value> items;
        for (auto& x : static_cast<const ListLit&>(e).elems) items.push_back(eval(*x, f));
        return Value::list(std::move(items));
    }

    case Expr::K::ListComp: {
        auto& c = static_cast<const ListComp&>(e);
        std::vector<Value> out;
        for (auto& item : iterate(eval(*c.iter, f), e.line)) {
            (*f.vars)[c.var] = item;
            if (c.cond && !truthy(eval(*c.cond, f))) continue;
            out.push_back(eval(*c.elt, f));
        }
        return Value::list(std::move(out));
    }
    }
    raise("RuntimeError", "unhandled expression kind", e.line);
}

// --------------------------------------------------------------- arithmetic

static int64_t asInt(const Value& v) { return v.is<bool>() ? (v.as<bool>() ? 1 : 0) : v.as<int64_t>(); }
static double asDouble(const Value& v) {
    if (v.isFloat()) return v.as<double>();
    return static_cast<double>(asInt(v));
}

// Operator semantics follow Python, with one deliberate difference: ints are
// 64-bit, and overflow raises OverflowError instead of silently wrapping
// (C++) or growing without bound (Python). Order of checks:
//   1. Sequence operators: str/bytes/list `+` (concatenate) and `*` (repeat).
//   2. If either side is float, or the op is true division `/`, use doubles.
//   3. Otherwise int64 with overflow checks via __builtin_*_overflow.
Value Interp::binary(const std::string& op, const Value& a, const Value& b, int line) const {
    auto typeErr = [&]() -> Value {
        raise("TypeError",
              "unsupported operand types for " + op + ": '" + typeName(a) + "' and '" + typeName(b) + "'", line);
    };

    // Sequences
    if (op == "+") {
        if (a.isStr() && b.isStr()) return Value::str(a.s().s + b.s().s);
        if (a.isBytes() && b.isBytes()) return Value::bytes(a.b().b + b.b().b);
        if (a.isList() && b.isList()) {
            auto items = a.l().items;
            items.insert(items.end(), b.l().items.begin(), b.l().items.end());
            return Value::list(std::move(items));
        }
    }
    if (op == "*") {
        const Value* seq = nullptr;
        const Value* n = nullptr;
        if ((a.isStr() || a.isBytes() || a.isList()) && b.isInt()) { seq = &a; n = &b; }
        if ((b.isStr() || b.isBytes() || b.isList()) && a.isInt()) { seq = &b; n = &a; }
        if (seq) {
            int64_t k = std::max<int64_t>(0, n->as<int64_t>());
            if (seq->isList()) {
                std::vector<Value> items;
                for (int64_t i = 0; i < k; ++i)
                    items.insert(items.end(), seq->l().items.begin(), seq->l().items.end());
                return Value::list(std::move(items));
            }
            const std::string& src = seq->isStr() ? seq->s().s : seq->b().b;
            std::string out;
            out.reserve(src.size() * static_cast<size_t>(k));
            for (int64_t i = 0; i < k; ++i) out += src;
            return seq->isStr() ? Value::str(std::move(out)) : Value::bytes(std::move(out));
        }
    }

    if (!a.isNum() || !b.isNum()) return typeErr();

    if (a.isFloat() || b.isFloat() || op == "/") {
        double x = asDouble(a), y = asDouble(b);
        if (op == "+") return Value(x + y);
        if (op == "-") return Value(x - y);
        if (op == "*") return Value(x * y);
        if (op == "**") return Value(std::pow(x, y));
        if (op == "/" || op == "//" || op == "%") {
            if (y == 0.0) raise("ZeroDivisionError", "division by zero", line);
            if (op == "/") return Value(x / y);
            if (op == "//") return Value(std::floor(x / y));
            double m = std::fmod(x, y);
            if (m != 0.0 && ((m < 0) != (y < 0))) m += y;
            return Value(m);
        }
        return typeErr();
    }

    int64_t x = asInt(a), y = asInt(b), r = 0;
    auto overflow = [&]() -> Value { raise("OverflowError", "integer overflow in " + op, line); };
    if (op == "+") { if (__builtin_add_overflow(x, y, &r)) return overflow(); return Value(r); }
    if (op == "-") { if (__builtin_sub_overflow(x, y, &r)) return overflow(); return Value(r); }
    if (op == "*") { if (__builtin_mul_overflow(x, y, &r)) return overflow(); return Value(r); }
    if (op == "//" || op == "%") {
        if (y == 0) raise("ZeroDivisionError", "integer division or modulo by zero", line);
        if (x == INT64_MIN && y == -1) return op == "%" ? Value(int64_t{0}) : overflow();
        int64_t q = x / y, m = x % y;
        // C++ truncates toward zero; Python floors. When the remainder is
        // nonzero and its sign differs from the divisor's, step the quotient
        // down one and move the remainder into the divisor's sign:
        //   -7 // 2 == -4 and -7 % 2 == 1   (C++ gives -3 and -1)
        if (m != 0 && ((m < 0) != (y < 0))) { --q; m += y; }
        return Value(op == "//" ? q : m);
    }
    if (op == "**") {
        if (y < 0) return Value(std::pow(static_cast<double>(x), static_cast<double>(y)));
        int64_t result = 1, base = x;
        while (y) {
            if (y & 1) { if (__builtin_mul_overflow(result, base, &result)) return overflow(); }
            y >>= 1;
            if (y && __builtin_mul_overflow(base, base, &base)) return overflow();
        }
        return Value(result);
    }
    if (op == "&") return Value(x & y);
    if (op == "|") return Value(x | y);
    if (op == "^") return Value(x ^ y);
    if (op == "<<" || op == ">>") {
        if (y < 0) raise("ValueError", "negative shift count", line);
        if (op == ">>") return Value(y >= 64 ? (x < 0 ? int64_t{-1} : int64_t{0}) : (x >> y));
        if (y >= 64 || (x != 0 && (y >= 63 || (x > 0 ? x > (INT64_MAX >> y) : x < (INT64_MIN >> y)))))
            return overflow();
        return Value(static_cast<int64_t>(static_cast<uint64_t>(x) << y));
    }
    return typeErr();
}

bool Interp::compare(const std::string& op, const Value& a, const Value& b, int line) const {
    if (op == "==") return valuesEqual(a, b);
    if (op == "!=") return !valuesEqual(a, b);
    if (op == "in" || op == "not in") {
        bool found = false;
        if (b.isStr()) {
            if (!a.isStr()) raise("TypeError", "'in <str>' requires str as left operand", line);
            found = b.s().s.find(a.s().s) != std::string::npos;
        } else if (b.isBytes()) {
            if (a.isInt()) {
                int64_t v = a.as<int64_t>();
                found = v >= 0 && v < 256 && b.b().b.find(static_cast<char>(v)) != std::string::npos;
            } else if (a.isBytes()) {
                found = b.b().b.find(a.b().b) != std::string::npos;
            } else {
                raise("TypeError", "'in <bytes>' requires int or bytes", line);
            }
        } else if (b.isList()) {
            for (auto& x : b.l().items)
                if (valuesEqual(a, x)) { found = true; break; }
        } else {
            raise("TypeError", "argument of type '" + typeName(b) + "' is not a container", line);
        }
        return op == "in" ? found : !found;
    }
    int c;
    if (a.isNum() && b.isNum()) {
        if (a.isFloat() || b.isFloat()) {
            double x = asDouble(a), y = asDouble(b);
            if (std::isnan(x) || std::isnan(y)) return false;
            c = x < y ? -1 : (x > y ? 1 : 0);
        } else {
            int64_t x = asInt(a), y = asInt(b);
            c = x < y ? -1 : (x > y ? 1 : 0);
        }
    } else if (a.isStr() && b.isStr()) {
        c = a.s().s.compare(b.s().s);   // UTF-8 byte order == code point order
    } else if (a.isBytes() && b.isBytes()) {
        c = a.b().b.compare(b.b().b);
    } else {
        raise("TypeError", "'" + op + "' not supported between '" + typeName(a) + "' and '" + typeName(b) + "'", line);
    }
    if (op == "<") return c < 0;
    if (op == "<=") return c <= 0;
    if (op == ">") return c > 0;
    return c >= 0;
}

// ---------------------------------------------------------------- subscripts

/**
 * @brief Python slice bound normalization.
 *
 * Missing bounds default to 0 and @p len; negative bounds count from the end;
 * everything is clamped to [0, len]; an inverted range becomes empty.
 * Slicing never raises IndexError.
 */
static void sliceBounds(int64_t len, const Value* lo, const Value* hi, int64_t& a, int64_t& b) {
    auto norm = [&](const Value* v, int64_t dflt) {
        if (!v || v->isNone()) return dflt;
        int64_t x = v->as<int64_t>();
        if (x < 0) x += len;
        return std::clamp<int64_t>(x, 0, len);
    };
    a = norm(lo, 0);
    b = norm(hi, len);
    if (b < a) b = a;
}

Value Interp::subscript(const Value& obj, const Expr& idx, Frame& f, int line) {
    int64_t len;
    if (obj.isStr()) len = static_cast<int64_t>(utf8Length(obj.s()));
    else if (obj.isBytes()) len = static_cast<int64_t>(obj.b().b.size());
    else if (obj.isList()) len = static_cast<int64_t>(obj.l().items.size());
    else raise("TypeError", "'" + typeName(obj) + "' is not subscriptable", line);

    if (idx.k == Expr::K::Slice) {
        auto& s = static_cast<const Slice&>(idx);
        Value lo, hi;
        if (s.lo) lo = eval(*s.lo, f);
        if (s.hi) hi = eval(*s.hi, f);
        for (auto* v : {&lo, &hi})
            if (!v->isNone() && !v->isInt()) raise("TypeError", "slice indices must be int", line);
        int64_t a, b;
        sliceBounds(len, &lo, &hi, a, b);
        if (obj.isStr()) {
            size_t ba = utf8Offset(obj.s(), static_cast<size_t>(a));
            size_t bb = utf8Offset(obj.s(), static_cast<size_t>(b));
            return Value::str(obj.s().s.substr(ba, bb - ba));
        }
        if (obj.isBytes()) return Value::bytes(obj.b().b.substr(static_cast<size_t>(a), static_cast<size_t>(b - a)));
        auto& items = obj.l().items;
        return Value::list(std::vector<Value>(items.begin() + a, items.begin() + b));
    }

    Value iv = eval(idx, f);
    if (!iv.isInt()) raise("TypeError", typeName(obj) + " indices must be int, not " + typeName(iv), line);
    int64_t i = iv.as<int64_t>();
    if (i < 0) i += len;
    if (i < 0 || i >= len) raise("IndexError", typeName(obj) + " index out of range", line);
    size_t u = static_cast<size_t>(i);
    if (obj.isStr()) {
        size_t ba = utf8Offset(obj.s(), u), bb = utf8Offset(obj.s(), u + 1);
        return Value::str(obj.s().s.substr(ba, bb - ba));
    }
    if (obj.isBytes()) return Value(static_cast<int64_t>(static_cast<unsigned char>(obj.b().b[u])));
    return obj.l().items[u];
}

std::vector<Value> Interp::iterate(const Value& v, int line) const {
    if (v.isList()) return v.l().items;   // snapshot: mutation during iteration is not visible
    std::vector<Value> out;
    if (v.isBytes()) {
        for (unsigned char c : v.b().b) out.emplace_back(static_cast<int64_t>(c));
        return out;
    }
    if (v.isStr()) {
        const std::string& s = v.s().s;
        for (size_t i = 0; i < s.size();) {
            size_t j = i + 1;
            while (j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) ++j;
            out.push_back(Value::str(s.substr(i, j - i)));
            i = j;
        }
        return out;
    }
    raise("TypeError", "'" + typeName(v) + "' is not iterable", line);
}

// --------------------------------------------------------------------- calls

Value Interp::callValue(const Value& fn, Args& args, Kwargs& kw, int line) {
    if (fn.is<std::shared_ptr<const Function>>())
        return callFunction(*fn.as<std::shared_ptr<const Function>>()->def, args, kw, line);
    if (fn.is<std::shared_ptr<const Builtin>>()) {
        if (!kw.empty()) raise("TypeError", "builtins do not take keyword arguments yet", line);
        return fn.as<std::shared_ptr<const Builtin>>()->fn(*this, args, line);
    }
    if (fn.is<std::shared_ptr<const BoundMethod>>()) {
        if (!kw.empty()) raise("TypeError", "methods do not take keyword arguments yet", line);
        return callMethod(*fn.as<std::shared_ptr<const BoundMethod>>(), args, line);
    }
    if (fn.is<std::shared_ptr<const ExcClass>>()) {
        if (args.size() > 1 || !kw.empty()) raise("TypeError", "exceptions take at most one argument", line);
        std::string msg = args.empty() ? "" : toStr(args[0]);
        return Value(std::make_shared<const ExcValue>(ExcValue{fn.as<std::shared_ptr<const ExcClass>>()->name, msg}));
    }
    raise("TypeError", "'" + typeName(fn) + "' object is not callable", line);
}

// Parameter binding: positional args fill parameters left to right; keyword
// args fill by name (unknown or duplicate names are TypeErrors); remaining
// parameters take their defaults. Unlike Python, defaults are evaluated at
// call time rather than once at definition time. Since values are immutable,
// that's only observable through lists, and it removes Python's classic
// shared-mutable-default bug.
Value Interp::callFunction(const Def& d, Args& args, Kwargs& kw, int line) {
    if (depth_ >= kMaxDepth) raise("RecursionError", "maximum recursion depth exceeded", line);
    if (args.size() > d.params.size())
        raise("TypeError", d.name + "() takes " + std::to_string(d.params.size()) + " arguments but " +
                               std::to_string(args.size()) + " were given", line);
    Scope locals;
    for (size_t i = 0; i < args.size(); ++i) locals[d.params[i].name] = args[i];
    for (auto& [k, v] : kw) {
        bool known = false;
        for (auto& p : d.params)
            if (p.name == k) known = true;
        if (!known) raise("TypeError", d.name + "() got an unexpected keyword argument '" + k + "'", line);
        if (locals.count(k)) raise("TypeError", d.name + "() got multiple values for argument '" + k + "'", line);
        locals[k] = v;
    }
    Frame g{&globals_};
    for (auto& p : d.params) {
        if (locals.count(p.name)) continue;
        if (!p.dflt) raise("TypeError", d.name + "() missing required argument '" + p.name + "'", line);
        locals[p.name] = eval(*p.dflt, g);   // evaluated per call; values are immutable anyway
    }

    Frame f{&locals};
    Value ret;
    ++depth_;
    try {
        execBlock(d.body, f, ret);
    } catch (...) {
        --depth_;
        throw;
    }
    --depth_;
    return ret;
}

} // namespace dolphin
