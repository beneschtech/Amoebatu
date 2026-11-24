# Dolphin — Amoebatu Language Design Draft v0.2

*Status: draft for discussion. Driving workload: GRIB2 decoding.*

*Dolphin: smaller and smarter than the whales.*

---

## 1. Why GRIB2 is the right forcing function

A GRIB2 file exercises almost every feature the language needs, at real scale:

| GRIB2 property | What it forces in the language |
|---|---|
| File = concatenation of independent messages | Task parallelism. One `decode(msg)` per message is the natural dispatch unit. |
| Big-endian fixed-width fields, sign-magnitude ints (scale factors E, D) | Byte/bit-level primitives and declarative binary layouts. |
| N-bit packed values (template 5.0) | Bit-extraction intrinsic that lowers to tight native code. |
| `Y = (R + X·2^E) / 10^D` per point | Elementwise `map` over arrays — data parallelism. |
| Bitmap (section 6) | Prefix-sum + scatter — `scan` primitive. |
| Complex packing + spatial differencing (5.3) | Group decoding + cumulative sum — `scan` again, with real dependencies. |
| Grid templates (3.0, 3.20, 3.30, 3.40…) and product templates (4.x) | Tagged unions and `match`. |
| JPEG2000 / PNG / CCSDS packing (5.40/5.41/5.42) | C FFI, gated by node capability (`requires: openjpeg`). |
| Lat/lon per grid point via projection | FP-heavy embarrassingly parallel kernels. |
| ~1–2M points per field, hundreds of fields per file | Forces a real cost model: what goes over the wire vs. stays on-node. |

It also has a perfect correctness oracle: **eccodes**. Every milestone is validated by comparing decoded values against `grib_get_data` on the same file.

## 2. Two axes of parallelism (the key thing the workload clarified)

Decoding one 1M-point field is a few milliseconds of work. Shipping its 4 MB of output over 10GbE is ~3 ms. So not everything should be dispatched across the cluster.

```
Task parallelism  — function calls → futures → cluster dispatch
                    unit: decode(msg), process_file(path)
                    decided by: adaptive scheduler (learns cost)

Data parallelism  — map / scan / reduce over arrays → node-local
                    unit: unpack loop, bitmap expand, projection
                    executed by: thread pool + SIMD, later OpenCL
```

The language makes the two syntactically distinct so neither the compiler nor the scheduler has to guess. Function calls *may* go remote. `map`/`scan`/`reduce` never leave the node (until an explicit OpenCL/GPU target exists, at which point "the node with the GPU" is just a capability).

**Argument passing (decided):**
- **Same node:** arguments are references to a local shared-memory object, so there's no copy. That's safe because values are immutable (3.2).
- **Remote node:** arguments are serialized into the Execute message.

The worst case is an IO-bound call that sends a large payload to a remote host, which costs about what it costs today. The scheduler sees argument sizes in its latency statistics, so it learns to keep large-argument calls near their data without any special mechanism.

## 3. Core semantics

### 3.1 Whole-program analysis determines each function's needs (decided)

Source is examined in full before anything runs. It is not streamable. There is no effect keyword: every function is a plain `def`, and the analyzer infers what it needs, transitively through everything it calls.

| Need | Meaning for the runtime |
|---|---|
| *(none)* — pure | Dispatch early, speculate, reorder, retry on node failure, memoize by (function hash, argument hashes). |
| `IO` | File/console access. Ordered relative to other `IO` in program order. |
| `Database` | Database access. Ordered per connection. Only goes to nodes with that capability. |
| `Network` | Outbound network. Ordered per endpoint. |
| `LocalOnly` | Must run on the calling node (local shared memory, display, local device). |
| `Globals` | Touches cluster globals. Pays the ownership-token cost. Visible to the scheduler. |

The inferred set goes into the function object next to the `TypeSignature`, so the scheduler matches it against node capabilities exactly like a `requires:`.

Blocking then happens only where something actually needs one of these. Pure work is always a future.

What this rules out, because the analyzer must see every call target:
- No `eval`, no loading code at runtime.
- Higher-order calls (`map(f, …)`, lambdas) are fine as long as the function passed in is known statically. Function values passed through data structures get the union of every function that could flow there.
- **C functions can't be analyzed, so their needs are declared:**
  - **Standard library:** written in C/C++ underneath, and ships with every function's needs already declared and reviewed. Most programs never touch FFI directly.
  - **User FFI:** an unlabeled `extern` is treated as worst case, i.e. it gets every need (IO, Database, Network, LocalOnly, Globals). It still runs correctly, just ordered and pinned locally, so it's slower. Labeling it correctly is the programmer's job, and a wrong label is a bug in their code. Dolphin is a tool for structuring work differently, not a way to fix bad code on the fly.

  ```python
  extern "libaec" def aec_buffer_decode(strm: ptr) -> i32           # unlabeled: worst case
  extern "libaec" def aec_buffer_decode(strm: ptr) -> i32 needs ()  # labeled pure
  ```

### 3.2 Names rebind; values don't mutate

Python feel is preserved: `off += n` is fine. Rebinding a local name is not mutation — the compiler converts to SSA, so it creates no hazards for dependency analysis.

What is restricted is *object* mutation:

- Values are immutable by default (`bytes`, `array[T]`, structs, lists).
- `mut` containers exist for building results inside a function (`mut array[f32](n)`), cannot be captured by a dispatched call, and are frozen on return.

Immutable values are what make passing unresolved futures into remote calls (Option B) safe: no aliasing, no write races.

### 3.3 Implicit futures

Every call to a function returns a future. A future is forced only when its value is consumed: arithmetic, branch, indexing, passing to a function with an IO/Database/Network need, or returning it from `main`.

```python
a = decode(m1)          # dispatched
b = decode(m2)          # dispatched
c = summarize(a, b)     # Option A: blocks for a, b, then dispatches
                        # Option B: dispatches now with future refs
```

Plan: Option A first (milestones 2–5), Option B later (milestone 6). The `isFuture` flag in the Execute message is already the hook for this.

Comprehensions are the parallel map at task level: `[decode(m) for m in msgs]` dispatches all of them and returns a list of futures; iterating it blocks per element in order.

### 3.4 Static types, inferred locally, explicit at function boundaries

Function signatures must be fully typed: they become the `TypeSignature` in the function object and are what dependency analysis and the JIT work from. Types inside function bodies are inferred.

Primitive types: `u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 bool str bytes`
Compound: `array[T]` (dense, contiguous), `list[T]`, `struct`, tagged unions (`A | B | C`), `Option[T]`.

No duck typing, no `__add__` overloading in v0. That's what keeps dependency analysis static and the JIT simple.

### 3.5 Exceptions (decided)

Exceptions propagate up to the nearest handler, even if execution has already moved past the failing call. The variables that call would have assigned are left untouched (they keep their previous binding).

```python
try:
    grid  = parse_grid(s3)        # dispatched
    vals  = unpack(s7, s5)        # dispatched
    log_progress(n)               # IO — may run before either finishes
    f = Field(grid=grid, values=vals)   # blocks on both
except DecodeError as e:
    # grid / vals keep whatever they held before the try
    # log_progress has already happened, and is not rolled back
    skip(msg, e)
```

What this requires of the runtime:

- **Every `try` block and every function body is a join point.** A `try` can't complete until every call started inside it has resolved, or an exception would have no handler left to reach. A function's own future doesn't resolve until all the calls it started have resolved. This is structured concurrency: no call outlives the scope that started it.
- **Side effects already performed are not undone.** Independent work that ran speculatively stays done. This is the deliberate departure from sequential semantics.
- **Several failures in one `try`:** proposed rule is that the one earliest in *program order* is raised, and the others are attached to it (like Python's `__context__` / exception groups). That keeps the handler deterministic no matter which node finished first.

A corrupt GRIB message only fails the `try` around its own decode.

### 3.6 Strings

Text matters as much as binary: WMO bulletin headers, METAR/TAF, code tables, parameter names, logs, output.

- **`str` is immutable UTF-8.** `bytes` is a separate type, and there's no implicit conversion between them: `s.encode()` / `b.decode()`.
- **`len`, indexing and slicing work on code points**, as in Python. Each string carries an ASCII flag, so the common case (nearly all weather text) is O(1) byte indexing.
- **f-strings with format specs**: `f"{name:<12} {val:8.2f} {code:04d}"`.
- **Methods**: `split` `join` `strip`/`lstrip`/`rstrip` `upper` `lower` `find` `replace` `startswith` `endswith` `isdigit` `isalpha` `isspace`, plus `int()` / `float()` / `str()` / `ord()` / `chr()`.
- **All string operations are pure.** Strings move like any other value: a shared-memory reference locally, serialized when they go remote.
- **Later**: `re` (pure, backed by PCRE2), and a tokenizer helper for METAR/TAF-style space-delimited groups.

## 4. Binary layouts

GRIB, BUFR, and most of LEADS' ingest formats are fixed-layout big-endian. A declarative layout is clearer than hand-written offset arithmetic and lowers to straight-line loads.

```python
@endian(big)
layout Section0:
    magic:      bytes[4] == b"GRIB"
    reserved:   u16
    discipline: u8
    edition:    u8 == 2
    total_len:  u64

@endian(big)
layout Section5_0:            # simple packing
    length:     u32
    number:     u8 == 5
    npoints:    u32
    template:   u16 == 0
    ref_value:  f32           # R
    bin_scale:  i16sm         # E, sign-magnitude
    dec_scale:  i16sm         # D, sign-magnitude
    nbits:      u8
    orig_type:  u8
```

- `== value` is a checked constraint. Failure is a decode error.
- `i16sm`/`i32sm` are sign-magnitude integers, a field type that exists only for layouts.
- `Section0.read(buf, off)` returns a struct. Prior art: Erlang bit syntax, Kaitai Struct.

## 5. Worked example: the target program

```python
from grib.layouts import Section0, SectionHeader, Section5_0
from grib.grids import GridDef, parse_grid

struct Field:
    discipline: u8
    category:   u8
    number:     u8
    grid:       GridDef
    values:     array[f32]

def split_messages(buf: bytes) -> list[bytes]:
    out = mut list[bytes]()
    off = 0
    while off < len(buf):
        s0 = Section0.read(buf, off)
        out.append(buf[off : off + s0.total_len])
        off += s0.total_len
    return out

def unpack_simple(data: bytes, s5: Section5_0) -> array[f32]:
    e = 2.0 ** s5.bin_scale
    d = 10.0 ** -s5.dec_scale
    n = s5.nbits
    return map(range(s5.npoints),
               lambda i: (s5.ref_value + bits(data, i * n, n) * e) * d)

def decode(msg: bytes) -> Field:
    sections = walk_sections(msg)              # dict of section number -> bytes
    grid = parse_grid(sections[3])             # returns GridDef (tagged union)
    s5 = sections[5]
    values = match template_of(s5):
        case 0:  unpack_simple(sections[7], Section5_0.read(s5, 0))
        case 3:  unpack_complex_sd(sections[7], s5)
        case 40: unpack_jpeg2000(sections[7], s5)   # FFI, requires: openjpeg
    values = apply_bitmap(values, sections.get(6))
    return Field(..., grid=grid, values=values)

def read_file(path: str) -> bytes        # stdlib; analyzer marks it IO

def main(path: str):                      # inferred: IO
    msgs   = split_messages(read_file(path))
    fields = [decode(m) for m in msgs]          # all dispatched immediately
    for f in fields:                            # blocks per field, in order
        print(f.discipline, f.category, f.number, min(f.values), max(f.values))
```

What the runtime does with this:

1. `read_file` is the only blocking point before any work starts.
2. `split_messages` is a cheap serial scan of headers.
3. Every `decode` is dispatched immediately. On one node, they go to the thread pool. On the cluster, the scheduler spreads them out and learns which nodes are fast.
4. Inside `decode`, `parse_grid` and the unpack branch are independent, so they overlap (scan-ahead within the callee — Option B territory).
5. `map` in `unpack_simple` runs data-parallel on whatever node `decode` landed on.
6. `print` is inferred `IO`, so output stays in message order even though the decodes finish out of order.

## 6. Compiler pipeline

```
source (.dol)
   │  indentation-aware lexer (INDENT/DEDENT), recursive descent + Pratt
   ▼
AST
   │  name resolution, type inference within bodies, effect checking
   ▼
Typed IR (ours) — SSA, explicit futures, effect edges, dependency graph
   │  ← dispatch / scan-ahead decisions live here, above LLVM
   ├──► IR interpreter                (M0–M2)
   └──► LLVM IR per function → ORC LLJIT on receiving node   (M3+)
```

- **Source-only distribution holds** (existing decision): function objects carry signed source and the signature. The receiving node compiles and caches by content hash. The typed IR never goes over the wire.
- The typed IR exists because LLVM IR is too low to express "this is a future" or "this edge is an effect ordering". LLVM sees one function body at a time.
- Runtime and compiler are C++17, linked against one pinned LLVM major version.

## 7. Milestones (each validated against eccodes)

| # | Deliverable | Language features proven | Validation |
|---|---|---|---|
| M0 | `grib_ls` clone: list messages and section 0/1/4 identifiers for a GFS file | lexer, parser, types, layouts, IR interpreter | Matches `grib_ls` keys. **Skeleton done:** lexer, parser, needs analyzer, tree-walking interpreter; runs `examples/grib_ls.dol` on a synthetic fixture. Remaining: `layout`, `struct`, `dict`, type checking, real GFS file |
| M1 | Decode simple packing + bitmap | `bits`, `map`, `scan`, `mut` arrays | Values match `grib_get_data` (≤1 ulp f32) |
| M2 | Implicit futures, local thread pool; parallel decode of a whole file on one node | futures, whole-program needs analysis, effect ordering, try/function join points | Same output; measure speedup vs. serial |
| M3 | LLVM lowering + ORC JIT for pure numeric functions | codegen | Same output; measure vs. interpreter and vs. eccodes |
| M4 | Complex packing + spatial differencing (5.3); lat/lon for 3.0 and 3.30 (Lambert, RRFS) | `match`, tagged unions, real `scan` dependencies | eccodes values + coordinates |
| M5 | Cluster dispatch of `decode` via Execute/Result, Option A futures | function objects, signing, remote compile cache | 4-node run output == 1-node run output |
| M6 | Option B futures, scan-ahead inside callees | `isFuture` path | Same output; latency profile |
| M7 | FFI codecs (5.40/5.42), capability-gated dispatch | FFI, `requires:` | eccodes values |

M0–M4 need only one machine, so language work isn't blocked on getting cluster hardware.

## 8. Decisions log

| # | Question | Decision |
|---|---|---|
| 1 | How are effects known? | Whole-program static analysis before running. Needs (IO, Database, Network, LocalOnly, Globals) are inferred, not declared. FFI declares its own. |
| 2 | Failure under speculation | Exceptions propagate to the nearest handler. Assignment targets are left untouched. Independent effects that already ran stay done. |
| 3 | Argument passing | Local: shared-memory reference. Remote: serialized. |
| 4 | C functions | Stdlib ships with needs declared. Unlabeled user `extern` = worst case (correct but slow). |
| 5 | Name | Dolphin. |

## 9. Open decisions

1. **Several failures in one `try`**: the earliest in program order wins, with the others attached (proposed).
2. **File extension**: `.dol` (proposed).
3. **Standard library scope for M0–M4** (proposed, driven by the GRIB target):
   - `io`: files, mmap, stdout
   - `str`: UTF-8 text, f-strings, methods above; `re` later
   - `bytes`: slicing, endian reads, `bits()`, layouts
   - `math`: scalar float/int functions
   - `array`: `map` / `scan` / `reduce`, min/max/mean, scatter/gather
   - `proj`: lat/lon for regular, Lambert, polar stereographic and Gaussian grids
   - `codec`: wrappers for libaec, openjpeg and libpng, with needs declared
   - `db`, `net`: later, after M5

## 10. Prior art to borrow from

- **Orca** (Bal, Kaashoek, Tanenbaum — Amoeba's parallel language; see `cacm90.pdf`, `ukuug90.pdf`, `comcom91.pdf`, `cpe94.pdf`):
  - Model: explicit `fork` of processes that share passive *shared data-objects* (abstract data types). Operations on an object are atomic and sequentially consistent.
  - Runtime: replicates each object to every machine that uses it. Reads run locally; writes go out by reliable broadcast.
  - The compiler/runtime decided per object whether to replicate it and where to put it.
  - Takeaways: Orca's shared objects are the closest ancestor of our cluster globals. Its read/write split is worth copying for the ownership-token design. The difference is that Orca made parallelism explicit (`fork`, Replicated Worker pattern). Here it's implicit and found by the analyzer.
- **Erlang bit syntax**: binary layouts.
- **Cilk / Intel TBB**: work-stealing for the local pool.
- **Halide**: separating *what* a pipeline computes from *where/how* it runs, the same split as task vs. data parallelism here.
- **Koka / effect systems**: effect inference. Same goal, reached through types.
- **Mojo, Codon**: Python-shaped syntax with static types, compiled through LLVM. Proof that the syntax/typing trade-off works.
- **Dask**: futures over a cluster, and the cost-model problems that come with them.
