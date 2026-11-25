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

### 2.1 The irregular case: METAR

GRIB is a few large, regular items: the parallelism is mostly *inside* each field. METAR is the opposite, and it's closer to most of the data Amoebatu will see (obs, TAFs, PIREPs, bulletins):

- **Many tiny items.** A bulletin holds dozens to hundreds of reports. A day of global METARs is hundreds of thousands. Decoding one report is microseconds.
- **Uneven cost.** A one-line `CLR` report and a report with weather, RVR, four cloud layers and a page of remarks differ several-fold in cost, and which is which isn't known until it's parsed.
- **Branchy, data-dependent control flow.** Which groups appear, and in what order, decides the path through the decoder. That's sequential *within* a report, and that's fine. The parallelism is *between* reports.
- **Still pure.** `decode_metar(report)` has no needs (see `examples/metar.dol`, `--needs`), so `[decode_metar(r) for r in reports]` is a parallel map.

What this forces on the runtime: **grain size.** One cluster dispatch per report (~100 µs round trip) would cost more than the decode. So a comprehension over a pure function is not "one future per element". The runtime splits it into **batches**:

- Batch size is learned per function from the same latency statistics the scheduler already keeps (cost per element versus dispatch overhead).
- Small lists, or cheap functions, run on the local thread pool, and work-stealing absorbs the uneven cost.
- Large lists send batches to other nodes.
- Results come back in element order regardless of which batch finished first, so output is identical to the sequential interpreter.

The language doesn't change for this: the programmer still writes a plain comprehension. Batching is a runtime decision, which is the point. See open decision 4.

### 2.2 Leaving the machine: capability, locality, capacity (M5)

The order of preference is fixed: **inline → a free local thread → another node.** Local threads are decided by measured cost and current load alone (M2: if the pool is busy, run inline). A remote node is the last resort, and the reason to use one is usually not speed. There are three reasons to send a call elsewhere, listed from most to least deterministic:

1. **Capability: the call *must* move.** It needs something this node doesn't have: a database, a GPU/OpenCL device, a display, a licensed codec, a particular instrument or feed. The analyzer already infers each function's needs (§3.1). Nodes advertise their capabilities through neighbord (the Capability/Load message). Placement is any node whose capabilities cover the call's needs. This part is deterministic and testable, so it comes first.
2. **Locality: the call *should* move.** The data lives elsewhere: a file on node B's disk, a feed that arrives at node C. Move the code to the data and ship back the (usually smaller) result.
3. **Capacity: the call *may* move.** Local cores are saturated and the call's measured cost is well above a network round trip (~100 µs on 10GbE) plus serialization. This extends the per-function cost statistics (M2's CostStat) with per-node latency. The adaptive scheduler's decay and exploration terms belong here.

The program doesn't name hosts:

```python
def ingest(paths: dict):
    m = decode_metars(paths["metar"])    # pure: runs near its file, or locally
    t = decode_tafs(paths["taf"])        # pure: same; overlaps with m
    s = decode_sigmets(paths["sigmet"])  # pure: same; overlaps with m and t
    store(m, t, s)                       # needs Database: runs on the node that has one
```

The three decodes overlap wherever they run. `store` waits for all three, then goes to the only node that can run it. An OpenCL kernel is the same case with a GPU need instead.

**A whole program is just a call.** Wrap `metar.dol`'s main body in `def decode_metars(path)` and it's an ordinary function with ordinary needs. Distribution at program granularity (METARs on host A, TAFs on host B, SIGMETs on host C, pooled on the database host) is the same mechanism as distribution at function granularity, just coarser.

**What travels:** the signed source of the function and everything it calls transitively (source-only distribution), plus serialized arguments (Cap'n Proto). The receiving node verifies, compiles and caches by content hash, and sends back the result or a DolphinError.

**What stays the same:** the M2 rules. A run spread across nodes must produce exactly the output of a single node that has every capability; that's the M5 test (for runs without failures; see decision 13 for the per-resource form). When calls fail, the first failure to return wins and assignment targets are restored (decision 17).

**What's new:**
- **Failure and retry (decision 15).** A pure call can always be retried, on the same node or another one. An effect is retried only if it's declared retryable. Retries are deferred: other runnable work goes first, and the retry waits for a natural pause, with backoff. An effect not declared retryable runs at most once and raises on failure.
- **Effect ordering becomes per resource (decision 13).** In M2 every effect runs on one thread, behind one barrier. With remote effects that would serialize the whole cluster behind its slowest call. Instead, each resource keeps its own order: the display/stdout channel among itself, each database among itself, each file among itself. An effect waits only for earlier calls on its own resource.

### 2.3 Output is a channel: toward the browser UI

M2 already routes `print()` through a queue and a printer thread, so the program never waits on the device. That's the shape of the final UI, not just an IO optimization:

- **Structured messages, not text.** In its final form, output is mostly not stdout. Producers emit structured messages (widget descriptions, updates). The channel's transport is the WebSocket to the browser, which renders natively (DESIGN.md: the server sends UI intent; the browser does fonts, layout and input).
- **The display is a capability.** A workstation with a browser attached is a node with a Display capability. UI calls have that need, so §2.2 places them there.
- **Ordering is per channel,** exactly as stdout is ordered today: program order on the producing side.
- **Callbacks make it interactive (decision 16).** Browser events come back as input, so a GUI program is an event loop. An outer program receives events and dispatches each one as a call, unconditionally. A slow handler never blocks the next event, and a failing one is reported instead of ending the program. Within a handler, work dispatches to threads or nodes like any other call. Display updates are ordered per handler only, so the browser side must handle concurrent updates to the same widget (each applied whole, last arrival wins).
- **Standard library work this implies:** a `ui` module (widgets, layout, events, bindings), a Display need, and the event-loop entry point (alternative to `main()`).

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
- **Several failures in one `try` (decision 17):** the first failure to *return* is raised, and the `try` aborts at once. The other calls' results are discarded, and calls not yet started are cancelled. With one thread that's program order; in parallel it can vary between runs, and that's accepted.

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
   ├──► p-code (register-based) → p-code interpreter   (replaces the M0–M2 tree-walker)
   └──► LLVM IR per function → ORC LLJIT on receiving node   (M3+)
```

- **P-code is the IR interpreter's format (decision 21).** Register-based: locals are register slots (no name lookups), constants live in a per-function pool and are never freed (so threads reading them share no reference counts), and each function carries a line table plus its needs and parallel-safety flags. Dispatch, futures, barriers and join points become instructions. Register form maps directly onto SSA for the LLVM step.
- **Two ways to run code (decision 21):** a standalone p-code file run directly, and the signed-source pool. In the pool, source is canonical: function objects carry signed source, and each receiving node compiles it and caches the p-code by content hash. P-code never goes over the wire between pool nodes.
- The typed IR exists because LLVM IR is too low to express "this is a future" or "this edge is an effect ordering". LLVM sees one function body at a time.
- Runtime and compiler are C++17, linked against one pinned LLVM major version.

## 7. Milestones (each validated against eccodes)

| # | Deliverable | Language features proven | Validation |
|---|---|---|---|
| M0 | `grib_ls` clone: list messages and section 0/1/4 identifiers for a GFS file; METAR bulletin decoder | lexer, parser, types, layouts, IR interpreter | Matches `grib_ls` keys; METAR output matches CPython. **Done so far:** lexer, parser, resolver, needs analyzer, tree-walking interpreter, dict; `examples/grib_ls.dol` (synthetic fixture) and `examples/metar.dol` (real bulletin plus a synthetic edge-case bulletin). Remaining: tuples, `layout`, `struct`, type checking, real GFS file |
| M1 | Decode simple packing + bitmap | `bits`, `map`, `scan`, `mut` arrays | Values match `grib_get_data` (≤1 ulp f32) |
| M2 | Implicit futures, local thread pool; parallel decode of a whole file on one node | futures, whole-program needs analysis, effect ordering, try/function join points | Same output; measure speedup vs. serial. **Working:** futures, batched parallel map, measured-cost dispatch, load-based admission; identical output at 1–8 threads; TSan clean |
| M3 | LLVM lowering + ORC JIT for pure numeric functions | codegen | Same output; measure vs. interpreter and vs. eccodes |
| M4 | Complex packing + spatial differencing (5.3); lat/lon for 3.0 and 3.30 (Lambert, RRFS) | `match`, tagged unions, real `scan` dependencies | eccodes values + coordinates |
| M5 | Capability-driven remote dispatch (§2.2): a call with a need the local node lacks (database first) runs on a node that has it, via Execute/Result | function objects, signing, remote compile cache, per-resource effect ordering | Multi-node output == output of one node with every capability |
| M5b | Locality and capacity placement (§2.2 reasons 2–3) | adaptive scheduler with per-node latency | Same output; latency/throughput profile under load |
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
| 6 | Name resolution | Static, before running. A name assigned anywhere in a function is local to the whole function (reading it first raises UnboundLocalError). Undefined names are compile errors. No `global` statement: functions can't rebind module-level names. |
| 7 | Local before remote | Prefer inline, then a free local thread, then a remote node (M5) last. Dispatch is admitted only when the pool has room, so heavy load degrades to sequential instead of thrashing. |
| 8 | What may run on a thread | Pure functions that never mutate an object they didn't create. Callers' mutations and effects wait for in-flight calls (barrier), so no copies are needed and aliasing behaves exactly as in Python. |
| 9 | Grain size | Measured at run time. Work is ops per call, from a rolling window of 8 calls per function. Rate is ops/ns from recent clean calls. Submit cost (submitter's overhead) and wake latency (delay until a worker starts) are measured separately, because only the first is overhead. A single call is dispatched if work ÷ rate ≥ 4 × (submit + wake). A parallel map makes ~4 batches per thread for load balance, each ≥ max(16 × submit, wake). Unmeasured functions run inline once. A probe dispatch keeps wake latency fresh. |
| 10 | Program output | Asynchronous. `print()` queues; a printer thread writes in coalesced batches (2 ms / 64 KB) with backpressure at 8 MB. Order is program order; output is drained before stderr messages and exit. |
| 11 | Why a call goes remote | Capability first (it can't run here), then locality (the data is there), then capacity (local cores saturated and the call is expensive). Never just because it's possible. |
| 12 | Output model | Output is a channel (queue + consumer). stdout is the first consumer; the browser UI over WebSocket is the target. The display is a capability. |
| 13 | Effect ordering across resources | Scoped to the resource. An effect waits only for earlier calls on the same resource (stdout/display, a database, a file), not for everything earlier in the program. Faster across a cluster; an unrelated failure surfaces later, and effects already done stay done (as decision 2). The guarantee: each resource's stream matches what a sequential run writes to it. |
| 14 | Capabilities | Free-form strings, matched by set containment (the node's set must contain the call's). Dolphin ships a default vocabulary (Database, GPU, Display, Network, …) but never interprets the strings: what counts as a separation of duties is the organization's call. The runtime warns at startup if a program needs a string no node advertises. |
| 15 | Failure and retry | Pure calls are always retryable. Effects are retryable only if declared so (same string mechanism). A failed retryable call is deferred: other runnable work goes first, and the retry happens at a natural pause (its result is needed, or nothing else can run), with backoff, since the fault has likely cleared by then. Later effects on the same resource wait behind it; nothing else does. Effects not declared retryable run at most once and raise on failure. |
| 16 | Event loop | An outer program receives events; each event is dispatched as a call, unconditionally. A slow or failing handler never blocks the loop or the next event; its failure is reported (UI or log), not fatal. Display updates are ordered per handler only; handlers rarely touch the same widget, and when they do, order rarely matters. Requirement on the front-end framework: it must accept concurrent updates to the same widget from different handlers without breaking (each update applied whole; the last one to arrive wins). |
| 17 | Several failures in one `try` | The first failure to *return* wins (completion order), not the earliest in program order. The `try` aborts as soon as any call inside it fails: the others' results are discarded, their assignments don't happen, and calls not yet started are cancelled. Not ideal, but it gives a definite answer quickly, and points the programmer at a real failure to debug. Sequential runs (`--threads 1`) remain deterministic, since there completion order is program order. Supersedes the "earliest in program order" rule in §3.5 and M2. |
| 18 | File extension | `.dol` |
| 19 | Standard library | Aim broad: draw use cases from science, finance, engineering, data processing and more, not just weather. It's the longest-running work on the project and grows continuously. |
| 20 | Closures | Allowed (still to be implemented), but a closure never leaves the host: calls through one are never dispatched to another node. Since they don't travel, closures capture by reference, as in Python. First version: calls through a closure also stay off local threads (inline only), since a captured variable could be rebound while a thread reads it. Loosen later if needed. |
| 21 | P-code and distribution | Code runs as register-based p-code, replacing the tree-walking interpreter. Two ways to deliver it, for two kinds of user. **Standalone:** a compiled p-code file (`.dcl`; design in PCODE.md) that `dolphin` runs directly, Java-jar style: copy one file onto a machine and try it, no pool or signing needed. Untrusted input, so the loader verifies it before running, and the format is versioned. **Pool:** shops that develop their own code, or run production data services on a server pool, use the signed-source model: source is the single source of truth, nodes compile and cache p-code by content hash, and upgrading means publishing new signed source. Developer PCs can join the pool alongside the servers. `--threads 1` remains the deterministic debugging mode; compiled code is not restricted to one thread. |
| 22 | Modules and applications | A source file is a module: the unit of compilation, of the `.dcl` file and of the pool cache. An application is either a folder of `.dcl` modules linked when loaded through a search path, or a bundle: one `.dcl` holding several module images and a manifest, signed as a whole. Imports bind by (module, name) to direct function indexes. Needs and parallel safety are recomputed across everything loaded; one module's claims about another are never trusted. Import cycles are not allowed. Static linking into one module may come later as an optimization. Format hooks (IMPT/EXPT) exist since P2; `import` itself comes later (PCODE.md §6a) |

## 9. Open decisions

1. **Standard library breadth** (decision 19). The scope below is the GRIB-driven starting point, not the limit:
   - `io`: files, mmap, stdout
   - `str`: UTF-8 text, f-strings, methods above; `re` later
   - `bytes`: slicing, endian reads, `bits()`, layouts
   - `math`: scalar float/int functions
   - `array`: `map` / `scan` / `reduce`, min/max/mean, scatter/gather
   - `proj`: lat/lon for regular, Lambert, polar stereographic and Gaussian grids
   - `codec`: wrappers for libaec, openjpeg and libpng, with needs declared
   - `db`, `net`: later, after M5
2. **Dispatch thresholds: probably the heart of the runtime's dynamic behaviour.** M2 now derives them from measurement (decision 9) instead of constants. Cost is split into **work** (operations per call, independent of machine and load) and **rate** (operations per ns, machine × load). Both are compared against the pool's measured **submit cost** (overhead) and **wake latency** (delay). Two things are still open:
   - **Persistence.** Work counts are portable, so they're what to cache between runs (and to send to other nodes, which predict their own time from their own rate). Rate and handoff are cheap to re-learn, because they come from ordinary calls rather than calibration runs. Cache location and invalidation (by source hash) haven't been decided.
   - **Under load, adapt thread count, not thresholds (PCODE.md §8a).** Halving dispatches saved at most 5% CPU. The extra CPU under load turned out to be SMT and hybrid-core sharing, not Dolphin overhead; once the machine is saturated, thread count barely changes throughput. The pool now sizes itself from the topology, adapts with AIMD on run-queue contention, checks each added worker against throughput, and coordinates with other Dolphin programs through a per-user shared-memory profile (§8b). Without flags it is within 1–3% of the best fixed thread count at 1, 4 and 50 concurrent programs. Persisting cost data across runs (work counts per function) is still open.
   - **The factors 4 and 16, and 4 batches per thread.** They are the only tuning constants left. They're ratios, so they should hold across machines, but they haven't been tested under the hundreds-of-programs load this is meant for.

3. **P-code (decision 21; PCODE.md).** Format and verifier are built (P2: `dolphin -c`, `.dcl` files, a verifier that recomputes parallel safety, fuzzed). Still open:
   - **Signing standalone files.** Optional for trying things out. Whether a pool should ever accept a signed `.dcl` instead of source (for example, from a vendor that doesn't ship source) is open.
   - **Developer PCs in the pool.** Which signing keys a pool trusts, and whether a developer's unsigned work-in-progress may run on pool servers or only on the developer's own machine.
   - **Modules (decision 22).** `import`, the search path, and bundles.

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
