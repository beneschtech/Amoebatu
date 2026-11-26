# Dolphin Language Reference

*What the interpreter actually does today. Last updated 2026-11-26: paths and directories,
compression, archives, `count_lines()`, `array()` and parallel fills.*

`doc/LANGUAGE.md` is the design draft: why things are the way they are, and
what is planned. This file is the other half: the language as implemented,
checked against the source (`src/dolphin/src/`) and by running programs. When
the two disagree, this file describes current behaviour and LANGUAGE.md
describes intent.

Dolphin looks like Python and most small Python programs run unchanged. The
differences are listed first, because they're what trips you up.

---

## 1. Coming from Python: the differences

### Not in the language (yet)

| Python | Dolphin | What to write instead |
|---|---|---|
| `x is None`, `is not` | no `is` operator (syntax error) | `x == None`, `x != None` |
| `a if c else b` | no conditional expression | an `if`/`else` statement |
| `a, b = 1, 2`, `return a, b`, `(1, 2)` | no tuples, no unpacking | return a list: `return [a, b]`, then `r[0]`, `r[1]` |
| `for k, v in d.items()` | no `items()`, no unpacking | `for k in d:` then `d[k]` |
| `global X` / `nonlocal` | no such statements | see §6.2 (mutate a global list/dict, or return values) |
| `lambda`, closures | reserved / rejected | a module-level `def`, passing what it needs as arguments |
| `class` | no classes, structs or objects yet | dicts for records; `struct` is reserved for later |
| `import` | reserved, not implemented | one file per program for now |
| `os.listdir`, `os.makedirs`, `os.stat`, `glob.glob` | no `os` module | `Path` (§9.7), which works like `pathlib` |
| `gzip.open`, `zipfile`, `tarfile` | no modules | `decompress(read_file(p))` (§9.8), `Archive` (§9.9) |
| `try` … `finally` / `else` | not supported | `except` clauses only |
| `except (A, B):` | no tuple of classes | two `except` clauses, or catch a common parent |
| user-defined exceptions | not possible (no classes) | raise a builtin one with a message (`DecodeError("...")`) |
| `s[::2]`, any slice step | not supported | a loop or comprehension |
| `x[1:3] = ...` | no slice assignment | build a new list |
| `del x[k]` | no `del` | `d.pop(k)` / `lst.pop(i)` |
| sets, `{x for ...}`, dict comprehensions, generator expressions | not supported | lists, dicts built in a loop |
| `a = b = 0` | no chained assignment | two statements |
| `a = 1; b = 2` | no `;` | one statement per line |
| `while … else`, `for … else` | no loop `else` | a flag variable |
| `*args`, `**kwargs`, `f(*lst)` | not supported | pass a list or dict |
| `"%d" % n`, `"{}".format(n)` | neither | f-strings |
| `f"{x!r}"`, `f"{x=}"` | no conversions or `=` | `f"{repr(x)}"` |
| `print(a, end="")`, `sorted(xs, key=...)` | builtins and methods take **no keyword arguments** | build the whole line, then `print` it |
| `"é"`, `"\N{...}"` | no `\u`/`\N` escapes (SyntaxError) | put the character in the source (UTF-8), or `chr(0xE9)` |
| `obj.attr = v` | no attribute assignment | dict item assignment |

Missing builtins people reach for: `enumerate`, `zip`, `map`, `filter`, `any`,
`all`, `round`, `list()`, `dict()`, `isinstance`, `hex`, `input`, `open`. Use
`range(len(xs))` for `enumerate`, `int(x + 0.5)` or an f-string spec for
rounding, `lst[0:]` to copy a list, `type(x) == "int"` for type tests, and
`Stream` / `read_file` / `Path` for files. Missing methods: `str.rjust/ljust/zfill/
partition/index/title`, `list.insert/remove/sort/reverse`, `dict.items/update`.
Use f-string alignment for padding (`f"{s:>8}"`) and `sorted(xs)` for sorting.

### Things that behave differently

- **Integers are 64-bit.** Overflow raises `OverflowError`; it never wraps and
  never grows into a bignum. `2 ** 63` raises.
- **`name << expr` as a statement assigns.** `n << 3` on a line by itself
  *shifts `n` in place* (`n = n << 3`). In Python that line does nothing. It
  exists for JSON (§9.4), but it applies to ints too.
- **Default arguments are evaluated on every call**, in the module scope. So
  `def f(x, acc=[])` gets a fresh list each call (no shared-default bug), and a
  default can refer to globals but not to earlier parameters.
- **Comprehension variables don't get their own scope.** `[x for x in xs]`
  binds `x` in the enclosing function (or as a global at module level).
- **`"\xHH"` in a `str` is a raw byte**, not code point U+00HH. Fine below
  `\x80`; above that you can create invalid UTF-8. Use `chr()` for non-ASCII.
- **`type(x)` returns a string**: `"int"`, `"str"`, `"list"`, `"KeyError"`...
- **`dict.keys()` / `values()` return lists** (copies), not views.
- **`range()` returns a real list.** `range(10_000_000)` allocates it.
- **`str.upper/lower/isdigit/isalpha/isspace` are ASCII-only.**
- **Names are checked before the program runs.** A misspelled variable or
  function anywhere in the file is a compile error, even in code that would
  never execute.
- **f-string fields can't contain the f-string's own quote character.**
  `f"{d["k"]}"` is an error; write `f"{d['k']}"`. Format specs are literal
  text: `f"{x:{w}}"` (a computed width) is not supported.

---

## 2. Running programs

```sh
dolphin prog.dol [args...]      # run; calls main(*args) if main is defined
dolphin --check prog.dol        # parse + resolve + analyze only (finds every NameError)
dolphin --needs prog.dol        # what each function needs, and whether it can run in parallel
dolphin --threads 1 prog.dol    # sequential reference execution
dolphin --engine pcode prog.dol # p-code engine (same semantics, faster)
dolphin -c prog.dol             # compile to prog.dcl; `dolphin prog.dcl` runs it
dolphin --ast / --tokens / --disasm / --stats / --trace prog.dol
```

### Program structure

1. The whole file is parsed, every name is resolved, and the needs analysis
   runs. Any syntax error or undefined name stops here, before anything runs.
2. Module-level statements run top to bottom (they can print, assign globals,
   loop, catch exceptions, and so on).
3. If the module defined a function `main`, it is called with the command-line
   arguments, **each as a `str`**. Too few arguments for `main`'s parameters
   (without defaults) is a `TypeError`. Extra ones are also a `TypeError`.
4. If `main` returns an `int`, that is the process exit code; otherwise 0.
5. An uncaught exception prints `file.dol:LINE: Kind: message` on stderr (after
   all earlier output has been flushed) and exits with 1.

```python
def main(path, limit="10"):      # dolphin prog.dol data.txt  /  dolphin prog.dol data.txt 50
    n = int(limit)
    ...
    return 0
```

---

## 3. Lexical structure

### Lines and indentation

- Blocks are defined by indentation, as in Python. **Spaces only**: a tab in
  indentation is a syntax error. Any width works, as long as a dedent returns
  to a width that was used before.
- A block is either an indented suite or **one simple statement** on the same
  line: `if x < 0: return 0`. (A compound statement like `if` can't be the
  one-liner.)
- Inside `()`, `[]`, `{}` newlines and indentation are ignored, so expressions
  can span lines. A backslash at the end of a line also joins it to the next.
- `#` starts a comment that runs to the end of the line.
- No semicolons.

### Identifiers and keywords

Identifiers: ASCII letter or `_`, then letters, digits, `_`. Case-sensitive.

Keywords:

```
def return if elif else while for in break continue pass try except as
raise and or not True False None
```

Reserved, can't be used as names, not implemented yet:

```
lambda import from struct layout extern needs mut
```

Note that `needs` and `mut` are reserved, so they can't be variable names.

### Numbers

| Form | Example | Type |
|---|---|---|
| decimal | `42`, `1_000_000` | int |
| hex / binary / octal | `0xFF`, `0b1010`, `0o755` | int |
| float | `3.14`, `.5`, `1.`, `1e-3`, `6.02E23` | float |

Underscores are digit separators anywhere in the digits. There are no negative
literals: `-5` is unary minus applied to `5`. An integer literal above
`9223372036854775807` is a syntax error.

### Strings and bytes

```python
'single'   "double"   '''triple
can span lines'''   """also triple"""
r"raw \n stays two characters"
b"bytes"   rb"raw bytes"   br"same"
f"formatted {x}"   rf"raw formatted {x}"
"adjacent " "literals " f"concatenate {n}"
```

- Prefixes (`b`, `r`, `f`, upper or lower case, up to two of them) go directly
  before the quote. `bf"..."` is an error.
- Escapes: `\n \t \r \0 \\ \' \" \xHH`, and backslash-newline (the newline is
  dropped). **Any other escape is a syntax error**, including `\u`, `\a`,
  `\b`, `\f`, `\v`. Raw strings keep backslashes as written.
- A single-quoted string can't contain a raw newline; a triple-quoted one can.
- Bytes literals may only contain ASCII in the source; use `\xHH` for the rest.
- Adjacent string literals concatenate. If any piece is an f-string, the whole
  thing is one f-string.

---

## 4. Values and types

| Type | `type(x)` | Mutable? | Notes |
|---|---|---|---|
| None | `"None"` | | the single value `None` |
| bool | `"bool"` | | `True`, `False`; acts as 1/0 in arithmetic |
| int | `"int"` | | signed 64-bit; overflow raises `OverflowError` |
| float | `"float"` | | IEEE double; prints like Python (`0.1`, `2.0`, `1e+20`) |
| str | `"str"` | no | UTF-8 text; length and indices count **code points** |
| bytes | `"bytes"` | no | raw bytes; indexing gives an int 0..255 |
| list | `"list"` | **yes** | shared by reference |
| dict | `"dict"` | **yes** | shared by reference; keeps insertion order |
| function | `"function"` | | a `def`; can be passed around and called |
| builtin | `"builtin"` | | e.g. `len` |
| exception class | `"exception class"` | | e.g. `ValueError` |
| exception | its class name | | what `except E as e` binds |
| Path | `"Path"` | no | a filesystem path, §9.7 |
| Stream, Response, JSON, Listener, Archive | same | | I/O objects, §9 |

### References and copying

Assignment never copies. `b = a` makes both names refer to the same list or
dict, so `b.append(1)` is visible through `a`. Strings, bytes and numbers can't
change, so sharing them is invisible.

- Copy a list: `copy = lst[0:]` (or `lst[:]`).
- Copy a dict: there's no `copy()`; build one: `c = {}` then
  `for k in d: c[k] = d[k]`.

### Truthiness

False: `None`, `False`, `0`, `0.0`, `""`, `b""`, `[]`, `{}`.
Everything else is true, including every Stream, function and exception.

### Equality

- Numbers compare by value across int, float and bool: `1 == 1.0 == True`.
  Two ints compare exactly (no rounding through double).
- Lists are equal if they have equal elements in the same order. Dicts are
  equal if they have the same keys mapping to equal values (order ignored).
- `str` and `bytes` are never equal to each other: `"a" != b"a"`.
- Different types are simply unequal (no error).

### Ordering (`<`, `<=`, `>`, `>=`)

Allowed between numbers (int/float/bool mixed), between two `str` (code point
order), and between two `bytes`. Anything else, including `None` or lists,
raises `TypeError`. Comparisons involving NaN are false.

### Dict keys

Keys can be `None`, bool, int, float, str, bytes, functions and exception
classes. A list or dict as a key is `TypeError: unhashable type`. Numbers that
are equal are the **same key**: `d[1]`, `d[1.0]` and `d[True]` are one entry.
Iteration order is insertion order; reassigning an existing key keeps its
position.

---

## 5. Expressions

### Operator precedence (lowest to highest)

| Level | Operators | Notes |
|---|---|---|
| 1 | `or` | short-circuit; returns an operand, not a bool |
| 2 | `and` | short-circuit; returns an operand |
| 3 | `not` | prefix |
| 4 | `==` `!=` `<` `<=` `>` `>=` `in` `not in` | chain: `a < b < c` means `a < b and b < c` |
| 5 | `\|` | |
| 6 | `^` | |
| 7 | `&` | |
| 8 | `<<` `>>` | |
| 9 | `+` `-` | |
| 10 | `*` `/` `//` `%` | |
| 11 | unary `-` `+` `~` | `-2 ** 2 == -4` |
| 12 | `**` | right-associative: `2 ** 3 ** 2 == 512` |
| 13 | call `f(x)`, index `a[i]`, slice `a[i:j]`, attribute `a.b` | left to right |

Parentheses group. `(a, b)` is an error ("tuples are not supported yet").

### Arithmetic

| Expression | Result |
|---|---|
| `int + - * int` | int, `OverflowError` on overflow |
| anything with a float | float |
| `a / b` | **always float** (`7 / 2 == 3.5`, `4 / 2 == 2.0`) |
| `a // b` | floor division: `7 // 2 == 3`, `-7 // 2 == -4` (float if either is float) |
| `a % b` | result takes the divisor's sign: `-7 % 2 == 1` |
| `int ** int` | int if the exponent is ≥ 0, float if negative (`2 ** -1 == 0.5`) |
| `x / 0`, `x // 0`, `x % 0` | `ZeroDivisionError`, for floats too |
| `& \| ^ ~ << >>` | ints only. `<<` raises `OverflowError` if bits are lost; negative shift counts raise `ValueError` |
| `str + str`, `bytes + bytes`, `list + list` | concatenation (new object) |
| `str * int`, `bytes * int`, `list * int` (either order) | repetition; a negative count gives empty |
| `True + 1` | `2` (bools are ints in arithmetic) |

Mixing `str` and numbers (`"a" + 1`) is a `TypeError`. Convert with `str()`.

### Membership: `in` / `not in`

| Right side | Left side | Meaning |
|---|---|---|
| str | str | substring test (`"KT" in g`) |
| bytes | bytes or int | sub-bytes, or a byte value |
| list | anything | some element `==` it |
| dict | a valid key | key is present |

### Indexing and slicing

```python
s[0]   s[-1]          # str: a 1-character str; bytes: an int; list: the element
d["key"]              # dict: KeyError if missing (use d.get("key") for None)
s[2:5]  s[:3]  s[3:]  s[-2:]  s[:]   # new str / bytes / list
```

- Negative indices count from the end. An out-of-range index raises
  `IndexError`.
- Slices take `lo` and `hi` only (no step). They clamp to the sequence and
  never raise: `"abc"[1:100] == "bc"`, `"abc"[5:9] == ""`.
- String indices and slices count code points, not bytes.
- Assignment: `lst[i] = v` and `d[k] = v` only. No slice assignment, and str
  and bytes can't be modified.

### Calls

```python
f(1, 2)          f(1, b=2)          f(a=1, b=2)
```

- User functions take positional arguments, then keyword arguments.
  Duplicates, unknown names and missing arguments are `TypeError`.
- **Builtins and methods take positional arguments only.**
- Calling a function stored in a variable works, but the analyzer can't see
  through it, so the caller is treated as needing everything (a warning is
  printed, and that function won't run in parallel).

### List comprehensions

```python
[expr for name in iterable]
[expr for name in iterable if condition]
```

One `for` and at most one `if`. No nesting of `for`s inside one comprehension
(nest whole comprehensions instead). When `expr` is a call to a pure function,
the comprehension becomes a parallel map (§10).

### f-strings

```python
f"{station:<5} {temp:6.1f} {count:,} {flag}"
```

Each `{...}` holds an expression, optionally followed by `:spec`. `{{` and `}}`
are literal braces. Without a spec, the value is shown as `str()` would show
it. The spec is the format mini-language below.

### Format spec

`[[fill]align][sign][0][width][,][.precision][type]`

| Part | Values |
|---|---|
| align | `<` left, `>` right, `^` center. Default: numbers right, text left |
| fill | any one character before the align: `f"{x:*^9}"` |
| sign | `+` always, `-` negatives only (default), space: space for positives |
| `0` | zero-pad numbers after the sign: `f"{7:03d}" == "007"` |
| width | minimum width in code points |
| `,` | thousands separators: `f"{1234567:,}" == "1,234,567"` |
| .precision | digits after the point (`f`, `e`, `%`), significant digits (`g`), max characters (`s`) |
| type | `d` `x` `X` `o` `b` for ints; `f` `F` `e` `E` `g` `G` `%` for numbers; `s` for strings |

A float with a precision but no type uses `g`. Using `d` on a float, or `f` on
a string, raises `ValueError`.

```python
f"{3.14159:.2f}"   # 3.14
f"{3.14159:8.3f}"  # '   3.142'
f"{42:05d}"        # 00042
f"{255:x}"         # ff
f"{0.5:%}"         # 50.000000%
f"{-3:+d}"         # -3
f"{'s':<4}|"       # 's   |'
```

---

## 6. Statements

### 6.1 The statement list

```python
x = expr                      # assignment: target is a name, a[i], or d[k]
x: int = expr                 # annotation (parsed, not enforced); needs a value
x += expr                     # also -= *= /= //= %= **= &= |= ^= <<= >>=
name << expr                  # assigns, see §1 and §9.4
expr                          # expression statement (usually a call)
pass
break
continue
return [expr]                 # no value: returns None
raise [expr]                  # §8

if cond:  ...
elif cond: ...
else: ...

while cond: ...

for name in iterable: ...     # exactly one name

def name(params) -> ret: ...  # §7

try: ...
except ClassName as e: ...
except ClassName: ...
except: ...                   # catches everything
```

`for` iterates over:

| Iterable | Yields |
|---|---|
| list | its elements (a snapshot taken when the loop starts) |
| dict | its keys in insertion order (also a snapshot) |
| str | 1-character strings (code points) |
| bytes | ints 0..255 |

Changing a list or dict while looping over it doesn't affect the loop. The
loop variable keeps its last value after the loop.

### 6.2 Scope

There are exactly two levels: the module (globals) and the function (locals).

- **Inside a function, any name assigned anywhere in it is local throughout
  it.** That includes `for` variables, comprehension variables, `except … as`
  names and nested `def`s. Reading a local before it's assigned raises
  `UnboundLocalError`.
- Everything else in a function refers to the module. Module-level names are
  visible from every function, including ones defined further down the file.
- **There is no `global` statement**, so a function can't rebind a global:

  ```python
  COUNT = 0
  def bump():
      COUNT += 1      # UnboundLocalError: COUNT is local to bump
  ```

  To share state, either return values (preferred, and it keeps the function
  parallel), or put the state in a global list or dict and mutate it
  (`STATS["count"] += 1`), which works but makes the function inline-only (§10).
- Reading a global before the module code has assigned it raises
  `NameError: name 'X' is not defined yet`.
- Nested `def`s are allowed, but **no closures**: a nested function that reads
  a local of the function around it is a compile error. Pass the value in.
- Undefined names are compile errors (`--check` finds them all).

---

## 7. Functions

```python
def decode(line: str, strict=False, width: int = 80) -> dict:
    ...
    return result
```

- Parameters may have annotations and defaults. Annotations, including the
  `-> type`, are parsed and kept but **not checked** yet.
- A parameter without a default can't follow one with a default.
- Defaults are evaluated on every call, in the module scope.
- Falling off the end, or a bare `return`, returns `None`.
- Functions are values: `handler = decode` then `handler(x)` works (§5 Calls
  explains why you might not want to).
- Recursion depth is limited to 1000 calls (`RecursionError`).
- Defining two functions with the same name is allowed (the later one wins
  when it runs), but the name stops being statically known, which costs
  analysis precision. Don't.

---

## 8. Exceptions

### Hierarchy

```
Error
├── ValueError
│   └── JSONError
├── TypeError
├── IndexError
├── KeyError
├── NameError
│   └── UnboundLocalError
├── AttributeError
├── ZeroDivisionError
├── OverflowError
├── IOError
├── DecodeError
├── RecursionError
├── RuntimeError
└── SyntaxError
```

`except Error` catches everything that can be raised. `DecodeError` is meant
for your own parsers ("this input is malformed"); `bytes.decode()` also raises
it for invalid UTF-8.

### Raising

```python
raise ValueError("bad wind group: " + g)   # class called with one argument (converted with str)
raise DecodeError                          # no message
raise                                      # inside an except block: re-raise the current one
```

Only builtin exception classes exist; you can't define new ones. Exception
constructors take at most one argument.

### Catching

```python
try:
    v = int(field)
except ValueError as e:
    print(e.kind, e.message)    # "ValueError", "invalid literal for int(): 'x'"
    v = None
```

- Clauses are tried in order; the first whose class matches (the error's class
  or any ancestor) runs. A bare `except:` matches anything.
- `e.kind` is the class name, `e.message` the message. `str(e)` is the message.
  `type(e)` is the class name too.
- If nothing matches, the exception keeps propagating.
- **A failed assignment leaves its target untouched.** In `x = f()`, if `f`
  raises, `x` keeps whatever value it had. Handlers can rely on that.
- There's no `finally` and no `else` clause.

### Errors the runtime raises

| Situation | Exception |
|---|---|
| `int("abc")`, `float("x")` | `ValueError` |
| wrong argument types, `"a" + 1`, calling a non-function | `TypeError` |
| list/str/bytes index out of range, `be_u32` past the end | `IndexError` |
| missing dict key with `d[k]` | `KeyError` (message is the key's repr) |
| `x.nosuchmethod` | `AttributeError` |
| int overflow | `OverflowError` |
| file or socket failure | `IOError` |
| invalid UTF-8 in `bytes.decode()` | `DecodeError` |

---

## 9. Builtins and methods

### 9.1 Builtin functions

| Function | Meaning |
|---|---|
| `print(a, b, ...)` | `str()` of each, space-separated, plus a newline. `print()` prints an empty line. No `end=`/`sep=`. |
| `len(x)` | str (code points), bytes, list, dict |
| `range(stop)`, `range(start, stop[, step])` | a **list** of ints; step 0 raises `ValueError` |
| `str(x)` | text form (`str()` with no argument is `""`). `str(b"x")` is `"b'x'"`; use `.decode()` for text |
| `repr(x)` | quoted form: `repr("a") == "'a'"` |
| `int(x)` | from int, bool, float (truncates toward zero), or str |
| `int(s, base)` | str in a base; `int("ff", 16) == 255`, `int("0x1f", 16) == 31` |
| `float(x)` | from a number or str (`"nan"`, `"inf"` accepted) |
| `bool(x)` | truthiness |
| `abs(x)` | int or float |
| `min(a, b, ...)`, `min(list)`; `max` likewise | numbers or strings, not mixed; empty list raises `ValueError` |
| `sum(list)` | numbers; int unless a float is involved |
| `sorted(list)`, `sorted(dict)` | new ascending list (dict: its keys); all numbers or all strings; stable. No `key`/`reverse` |
| `ord(c)`, `chr(n)` | code point of a 1-character str, and back |
| `type(x)` | type name as a str |
| `read_file(path)` | the whole file as **bytes** (`IOError` if it can't be read) |
| `array(n[, fill])` | a list of `n` copies of `fill` (default `None`), allocated once. Same as `[fill] * n`. A list or dict `fill` is shared by every slot, as in Python. See §10 for filling it in parallel |
| `compress(data, format[, level])`, `decompress(data[, format])`, `compressors()` | §9.8 |
| `Path(...)`, `Stream(target)`, `JSON([x])`, `Listener(...)`, `Archive(path[, mode])` | objects, below |

`int(s)` and `float(s)` accept surrounding whitespace but nothing else extra:
`int(" 42 ")` works, `int("42KT")` raises `ValueError`. `int("012")` is 12.

### 9.2 Binary decoding (big-endian, bounds-checked)

All take `(buf: bytes, offset: int)` and raise `IndexError` with the offset and
buffer size if the read runs past the end.

| Function | Reads |
|---|---|
| `be_u8`, `be_u16`, `be_u32`, `be_u64` | unsigned ints (`be_u64` raises `OverflowError` above 2^63−1) |
| `be_i16sm`, `be_i32sm` | sign-magnitude ints (GRIB style: top bit is the sign) |
| `be_f32`, `be_f64` | IEEE floats |
| `bits(buf, bit_offset, nbits)` | unsigned big-endian bit field, `nbits` 0..63 |

`examples/grib_ls.dol` uses all of these.

### 9.3 Methods

**str** (returns new strings; the original never changes)

| Method | Notes |
|---|---|
| `upper()`, `lower()` | ASCII only |
| `strip([chars])`, `lstrip([chars])`, `rstrip([chars])` | default: whitespace. `chars` is a set of (ASCII) characters |
| `split()` | on runs of whitespace, no empty pieces |
| `split(sep)` | on exact `sep`; keeps empty pieces (`"a,,b".split(",") == ["a", "", "b"]`). No `maxsplit` |
| `sep.join(list_of_str)` | |
| `startswith(s)`, `endswith(s)` | one string, not a tuple |
| `find(s)` | code point index or -1. No start argument |
| `count(s)` | non-overlapping occurrences |
| `replace(old, new)` | all occurrences; empty `old` raises |
| `isdigit()`, `isalpha()`, `isspace()` | ASCII; false for `""` |
| `encode()` | UTF-8 bytes |

**bytes**: `decode()` (UTF-8, `DecodeError` if invalid), `find(sub[, start])`
(byte offset or -1), `startswith(b)`, `endswith(b)`, `hex()`.

**list**: `append(x)`, `extend(iterable)`, `pop([i])` (default last),
`index(x)` (`ValueError` if absent), `count(x)`.

**dict**: `get(k[, default])` (default `None`), `pop(k[, default])` (removes;
`KeyError` without a default), `keys()`, `values()` (both lists).

**Path**, **Stream**, **Archive**: see §9.5, §9.7 and §9.9.

Exception values have the fields `kind` and `message`.

### 9.4 JSON

`JSON` is an immutable document. `JSON()` is null; `JSON(x)` loads `x`.

```python
j = JSON()
j << text                 # parse a str; JSONError (a ValueError) with line/column if malformed
j << {"stn": "KBCB", "t": 14.5}   # or load a dict / list / scalar
d << j                    # d = j.value(): dict, list or scalar (d needn't exist yet)
j.toString()              # compact text
j.prettyPrint()           # indented two spaces
```

Keys must be strings and come out sorted. Bytes become `data:<mime>;base64,...`
strings and turn back into bytes when read. NaN or infinity raises
`ValueError`. Details: `src/dolphin/README.md`, section JSON.

### 9.5 Stream: files and network

`Stream(target)` is the one I/O object. The target decides what it is:

| Target | Kind | Needs |
|---|---|---|
| `"path"`, `"file://path"` or a `Path` | file, **opened read-only immediately** | IO |
| `"http://..."`, `"https://..."` | HTTP: GET on the first read, POST on write | Network |
| `"tcp://host:port"`, `"unix:///path"` | connected socket | Network |

```python
f = Stream("obs.txt")
if not f.valid:                 # opening never raises: check valid / error
    print("cannot open:", f.error)
    return 1
line = f.readline()             # bytes including the "\n"; b"" at end of file
chunk = f.read(4096)            # up to n bytes; b"" at end of file
rest = f.read()                 # everything that's left
f.close()

out = Stream("result.txt")      # invalid until opened for writing:
out.open("wc")                  # r, rw, wc (create/truncate), a (append). Returns the new valid
out.write("text\n")             # str or bytes; returns the byte count
out.seek(0, 0); out.tell()      # files only (IOError on pipes and sockets)

n = Stream("obs.txt").count_lines()   # how many lines readline() would return
```

`count_lines()` counts every `"\n"`, plus a last line that doesn't end with
one (so `"a\nb"` is 2 lines and `""` is 0). A file is counted from its start
however much of it has been read, and the read position is left where it
was. It reads in 64 KB blocks without building any strings, so a million-line
file takes milliseconds. On an HTTP stream it counts the rest of the body;
on a socket it raises `IOError` (a socket has no end to count to).

- Fields: `valid`, `error`, `code` (HTTP status), `seekable`, `timeout`,
  `name`. Reading a field never raises.
- Needs: a string-literal target or `Stream(Path(...))` is known (IO for a file,
  Network otherwise). A target computed any other way, such as `Stream(d / "x.txt")`
  or a variable, counts as `IO,Network`. That's safe, just less precise.
- An operation on an invalid stream, or a failing one, raises `IOError`.
- `s.set_timeout(seconds)` for sockets and HTTP (default 30).
- An HTTP `write(dict)` posts form fields and returns a `Response` with
  `ready` (never blocks) and `code`, `body`, `valid`, `error` (these wait).

### 9.6 Listener: servers

`Listener(port)` / `Listener("tcp://host:port")` / `Listener("unix:///path")`,
configure (`bind`, `threads`, `per_connection`, `limit`, `set_timeout`,
`drain`), then `start(handler)` and `wait()`. Each connection calls
`handler(conn)` with a connected `Stream` on a worker thread. A handler that
might modify a global list or dict is refused at `start()` (`ValueError`).
Full description in `src/dolphin/README.md`; example in
`examples/echo_server.dol`.

### 9.7 Path: files and directories

`Path` works like Python's `pathlib.Path`. A Path is an immutable value, and
building or taking one apart never touches the disk. The methods that do
touch the disk need IO.

```python
d = Path("data") / "2026" / "11"       # join with /; Path("data", "2026", "11") is the same
d.mkdir()                              # creates missing parents too; fine if it exists
for p in d.iterdir():                  # sorted
    if p.suffix == ".txt" and p.is_file():
        print(p.name, p.stat()["size"])
for p in Path("data").rglob("*.grib2"):   # recursive, sorted
    ...
f = Stream(d / "obs.txt")              # Stream, Archive and read/write methods accept a Path
```

**Text** is kept normalized, with forward slashes: `Path("a//b/./c/")` is
`a/b/c`, and `Path()` is `.`. `..` is kept as written (only `resolve()` can
remove it, because that needs the filesystem). `str(p)` gives the text and
`repr(p)` gives `Path('a/b/c')`. Paths compare and sort by their text, can be
dict keys, and `sorted()` takes a list of them.

**Pure (no filesystem access):**

| | `Path("data/obs.tar.gz")` gives |
|---|---|
| `p.name` | `"obs.tar.gz"` |
| `p.stem` | `"obs.tar"` |
| `p.suffix` | `".gz"` (`""` for `.bashrc` or `README`) |
| `p.parent` | `Path('data')` (`Path("a").parent` is `.`, `Path("/").parent` is `/`) |
| `p.parts` | `["data", "obs.tar.gz"]` (an absolute path starts with `"/"`) |
| `p / "x"`, `"dir" / p`, `p / q` | joined; an absolute right side replaces the left, as in pathlib |
| `p.joinpath(a, b, ...)` | same as `p / a / b` |
| `p.with_suffix(".zip")` | `Path('data/obs.tar.zip')`; `""` removes the suffix |
| `p.with_name("x.csv")` | `Path('data/x.csv')` |
| `p.is_absolute()` | `False` |

**Filesystem (IO):**

| Method | Returns | Notes |
|---|---|---|
| `exists()`, `is_dir()`, `is_file()` | bool | never raise |
| `iterdir()` | list of Path | entries of a directory, **sorted** |
| `glob(pattern)` | list of Path, sorted | `*`, `?`, `[abc]`, `[a-z]`, `[!x]` within a name; `**` for any number of directories (`"**/*.csv"`); `/` between levels (`"*/obs/*.txt"`). `*` also matches names starting with `.` |
| `rglob(pattern)` | list of Path, sorted | `glob("**/" + pattern)`: everywhere below |
| `mkdir([mode])` | bool: whether it was created | creates parents; an existing directory is fine (`False`); an existing *file* raises. `mode` (e.g. `0o750`) applies to the new directory |
| `rmdir()` | None | empty directories only |
| `unlink()` | None | files and symlinks; a directory raises (use `rmdir()`) |
| `rename(target)` | the new Path | `target` is str or Path |
| `touch()` | None | creates an empty file, or updates the modification time |
| `stat()` | dict | see below |
| `chmod(mode)` | None | `p.chmod(0o640)` |
| `resolve()` | Path | absolute, with symlinks and `..` resolved |
| `read_bytes()`, `read_text()` | bytes / str | `read_text()` raises `DecodeError` if the file isn't UTF-8 |
| `write_bytes(b)`, `write_text(s)` | int: bytes / characters written | create or replace the file |

`stat()` returns a dict with `size`, `mode` (permission bits as an int:
`f"{st['mode']:o}"` prints `640`), `perms` (ls-style text: `"-rw-r-----"`,
`"drwxr-xr-x"`), `mtime`, `atime`, `ctime` (float seconds since the epoch),
`uid`, `gid` (0 on Windows), `is_dir`, `is_file`, `is_link`.

Every failure raises `IOError`, with the path and the reason in the message
(`"data/x: No such file or directory"`).

Differences from pathlib to know about:
- `mkdir()` always behaves like `mkdir(parents=True, exist_ok=True)`, which is
  what scripts want nearly every time (methods take no keyword arguments).
- `iterdir()`, `glob()` and `rglob()` return **sorted lists**. pathlib returns
  generators in directory order, which varies between machines; sorting keeps
  output reproducible.
- There's no `Path.cwd()` or `Path.home()` (no static methods): use
  `Path(".").resolve()`. There's no `open()` method: use `Stream(p)`.
- There's no recursive delete (`shutil.rmtree`). Remove files with `unlink()`,
  then directories deepest first with `rmdir()`.

### 9.8 Compression

```python
raw = decompress(read_file("obs.csv.gz"))         # format detected from the data
z = compress(text, "zstd")                        # str is compressed as its UTF-8 bytes
z = compress(data, "xz", 9)                       # optional level
print(compressors())                              # what this build supports
```

| Format | Files | Level (default) |
|---|---|---|
| `"gzip"` | `.gz` | 0..9 (6) |
| `"zlib"` | zlib streams (Python's `zlib.compress`) | 0..9 (6) |
| `"deflate"` | raw deflate, no header | 0..9 (6) |
| `"bz2"` | `.bz2` | 1..9 (9) |
| `"xz"` | `.xz` | 0..9 (6) |
| `"zstd"` | `.zst` | 1..22 (3) |

- `compress()` takes str or bytes and returns bytes. `decompress()` returns
  bytes: add `.decode()` for text.
- `decompress(data)` without a format recognizes gzip, zlib, bz2, xz and zstd
  from their first bytes. Raw deflate has no header, so pass `"deflate"`.
- Files made by concatenating compressed files (`cat a.gz b.gz > c.gz`, and
  the same for bz2, xz and zstd) decompress to the concatenated contents, as
  `gunzip` does.
- Corrupt or truncated data raises `DecodeError` ("gzip: truncated data"). An
  unknown format, a format missing from this build, or a bad level raises
  `ValueError`.
- **Which formats exist depends on the build**: each needs its library
  (zlib, bzip2, liblzma, libzstd) at compile time. `compressors()` and
  `dolphin --features` list them, and asking for a missing one says it
  "was not compiled into this dolphin".
- These functions are **pure**, so `[decompress(b) for b in blobs]` runs in
  parallel.
- Everything happens in memory. A multi-gigabyte `.gz` needs that much RAM;
  streaming decompression through `Stream` isn't there yet.

### 9.9 Archive: zip, tar, 7z

`Archive` reads and writes archive files. The method names are Python's
`zipfile` ones. It needs libarchive at compile time (`dolphin --features`
lists `archive`); without it every Archive is invalid and says why.

```python
a = Archive("obs.tar.gz")                # read: any format libarchive knows
if not a.valid:
    print(a.error)
a.namelist()                             # ["2026/", "2026/obs.txt", ...] in archive order
a.infolist()                             # [{"name", "size", "mode", "mtime", "is_dir"}, ...]
data = a.read("2026/obs.txt")            # bytes; KeyError if there's no such member
a.extractall("out")                      # extract everything; returns the names written
a.extractall("out", ["2026/obs.txt"])    # or only these members

w = Archive("bundle.zip", "w")           # write: the format comes from the name
w.writestr("notes.txt", "some text")     # a member from str or bytes
w.write("data/obs.txt")                  # a file from disk, stored under the same name
w.write(Path("data"), "obs")             # a directory, recursively, stored under "obs/"
w.close()                                # finishes the file: call it
```

- **Reading** recognizes zip, tar (plain, or compressed with gzip, bzip2, xz,
  zstd, lz4...), 7z, cpio, ISO, rar and others. Each `namelist()`,
  `infolist()` and `read()` scans the archive from the start, so reading many
  members of a big `.tar.gz` one by one is slow. `extractall()` makes one pass.
- **Writing**: the format comes from the file name: `.zip`, `.7z`, `.tar`,
  `.tar.gz`/`.tgz`, `.tar.bz2`/`.tbz2`, `.tar.xz`/`.txz`, `.tar.zst`/`.tzst`,
  `.tar.lz4`, `.cpio`. For any other name, give the format in the mode:
  `Archive("out.bin", "w:tar.gz")`. Directories are added in sorted order, so
  the same input gives the same archive. Absolute paths are stored without
  their leading `/`.
- **Safety**: `extractall()` refuses members that would land outside the
  destination (absolute paths, `..`, or a path through a symlink) and raises
  `IOError` instead.
- Like `Stream`, constructing one **never raises**: check `valid` and `error`.
  Methods on an invalid or closed archive, and failures, raise `IOError`.
  Fields: `valid`, `error`, `name`, `mode` (`"r"` or `"w"`).
- An unclosed archive is finished when it's freed, but an error at that point
  can't be reported. Call `close()`.
- For a single compressed file (`obs.txt.gz`, not a tar), use
  `decompress(read_file(...))`. An Archive is for containers.
- Every method needs IO.

---

## 10. Needs, purity and parallelism

You never write threads. The analyzer works out what each function *needs*,
and the runtime runs independent pure calls in parallel when that's worth it.
The output is always identical to sequential execution. To get parallelism,
you only need to know what makes a function pure.

### What a function needs

`dolphin --needs prog.dol` prints one line per function:

| Need | Comes from |
|---|---|
| `pure` | nothing below |
| `IO` | `print`, `read_file`, file streams, `Path` methods that touch the disk, `Archive` |
| `Network` | http/tcp/unix streams, `Listener` |

Needs are **transitive**: a function that calls a function that prints needs
`IO` too. Calls the analyzer can't resolve (through a variable) count as
needing everything.

### What can run in parallel

A function can run on another thread when it is `pure` **and** it never
modifies an object it didn't create itself (a parameter, a global, or anything
reachable from one). Building a new list or dict and filling it is fine.

```python
def decode(line):          # pure, builds its own dict: parallel
    r = {}
    r["stn"] = line[0:4]
    return r

def decode_into(line, r):  # modifies its parameter: always inline
    r["stn"] = line[0:4]

def decode_and_show(line): # prints: IO, inline, and so is everything that calls it
    print(line)
```

### Where parallelism happens

- `x = f(...)` where `f` is parallel-safe and the next statement doesn't use
  `x`: the call starts and `x` becomes a future. Reading `x` waits for it.
- `[f(v) for v in items]` with a parallel-safe `f`: a batched parallel map.
- A call runs inline when it's predicted to be too short to be worth a thread,
  or when the machine is busy. Effects (`print`, I/O) happen only on the main
  thread, in program order, after everything dispatched before them finishes.
- If several parallel calls fail, the first failure in program order is the
  one reported (decision 17 in LANGUAGE.md).

### Filling a preallocated array in parallel

When a decoder knows early how many records there are, it can allocate the
result once and fill it by index:

```python
def decode_all(buf, offsets):
    out = array(len(offsets))              # or [None] * len(offsets)
    for i in range(len(offsets)):
        out[i] = decode_record(buf, offsets[i])
    return out
```

If `decode_record` is parallel-safe, that loop runs as a parallel map: the
calls run in batches on the thread pool, then the results are stored into
`out` in loop order. The output is exactly what the sequential loop
produces, including on failure. If record 37 raises, slots 0..36 are filled,
37 onward are untouched, `i` is 37, and the error is record 37's. On a
16-core machine a 4,000-record fill with ~0.7 ms per record ran 6× faster on
8 threads.

A loop qualifies only when all of these hold. Otherwise it simply runs
sequentially, which is still correct:

- the loop body is the single statement `NAME[index] = f(args)`, inside a
  function (module-level code never runs in parallel);
- `f` is parallel-safe (`--needs` says `parallel`);
- the iterable, the index and the arguments have no effects (no `print`, no
  stream or path methods, no `append`), and **the index and arguments don't
  read `NAME`** (`out[i] = f(out[i - 1])` is a recurrence, so it stays
  sequential);
- `NAME` is a list or dict that **this function created**: every assignment
  to it is `array(...)`, `[...] * n`, a literal, a comprehension, `range()`,
  `sorted()` or a slice copy;
- `NAME` never escapes: it isn't assigned to another variable, stored in
  another list or dict, or passed to one of your functions (passing it to
  builtins like `len` or `print`, or returning it, is fine).

The last two rules mean no call can see the array while it's being filled. So
**create the array in the same function that fills it.** An array passed in
as a parameter disqualifies the loop, and so does a function that fills its
argument, which isn't parallel-safe anyway. `--trace` shows each parallel map
as it runs, with its line number and batch count; `--ast` shows how the loop
was rewritten (`src/dolphin/src/fill.hpp` explains the rewrite).

A comprehension, `out = [decode_record(buf, o) for o in offsets]`, gets the
same parallel map with less code. Use the fill form when the index isn't just
the position in a list, when filling a dict, or when the array exists before
the loop for other reasons.

**The practical rule: keep parsing and decoding in pure functions that take
text or bytes and return lists/dicts, and do all reading and printing in
`main`.** `examples/metar.dol` is built exactly that way.

---

## 11. Writing a parser for a NOAA text product

Here are the idioms I'd use for a line-oriented or fixed-column NOAA file:

```python
# noaa.dol: dolphin noaa.dol FILE

# Fixed-width columns: [start, end, name]. Slices clamp, so short lines give "".
COLUMNS = [[0, 4, "station"], [5, 11, "time"], [12, 18, "value"]]

def to_int(s):              # None for blank or missing fields instead of an exception
    t = s.strip()
    if t == "" or t == "M" or t == "MM":
        return None
    try:
        return int(t)
    except ValueError:
        return None

def to_float(s):
    t = s.strip()
    if t == "" or t == "M":
        return None
    try:
        return float(t)
    except ValueError:
        return None

def parse_line(line: str) -> dict:      # pure: runs in parallel over all lines
    r = {}
    for c in COLUMNS:
        r[c[2]] = line[c[0]:c[1]].strip()
    r["value"] = to_float(r["value"])
    return r

def is_data(line: str) -> bool:
    return line != "" and not line.startswith("#")

def main(path):
    text = read_file(path).decode()       # bytes -> str (DecodeError if not UTF-8)
    lines = [l.rstrip("\r") for l in text.split("\n")]
    data = [l for l in lines if is_data(l)]
    rows = [parse_line(l) for l in data]  # parallel map
    for r in rows:
        if r["value"] != None:
            print(f"{r['station']:<5} {r['time']:>6} {r['value']:8.2f}")
    print(f"{len(rows)} rows")
    return 0
```

Notes:

- `read_file()` gives bytes. Call `.decode()` once on the whole file, then work
  with `str`. If the file can contain non-UTF-8 bytes (old Latin-1 products),
  either work on bytes slices or catch `DecodeError`.
- Strip `"\r"` yourself: `split("\n")` leaves it on CRLF files, and the plain
  `split()` (whitespace) removes it as part of the run.
- Whitespace-separated fields: `line.split()`. Exact separators (CSV, `|`):
  `line.split(",")`, which keeps empty fields so column positions hold.
- Missing-value markers (`M`, `MM`, `9999`, `-9999.0`): convert to `None` in
  one helper and compare with `== None` later. There's no `is`.
- A "record" is a dict; a fixed pair is a two-element list. There are no tuples.
- To report a malformed record, `raise DecodeError(f"{stn}: bad group {g}")`
  and catch it in `main` if you want to skip the record and keep going.
- To keep the decoder parallel, don't `print` from inside it, and don't let it
  fill a dict or list that was passed in. Return new ones.
- Compressed input (`.gz`, `.bz2`, `.xz`, `.zst`): `decompress(read_file(path)).decode()`
  gives you the text in one step. A directory of daily files:
  `for p in Path("obs").glob("*.txt.gz"): ...` (sorted, so files are processed in date order
  when their names sort that way).
- If a header line tells you the record count, `out = array(n)` and an indexed
  fill loop run the decoding in parallel (§10). `Stream(p).count_lines()` is a
  cheap way to size arrays for line-per-record files.
- Test pieces in the Python-compatible subset with CPython via
  `src/dolphin/tests/py_oracle.py`, which supplies `read_file` and
  `DecodeError`, so you get a second opinion on the logic.
