# Dolphin

The Amoebatu language. Python-shaped, statically analyzed, built for implicit
parallelism across a cluster. Smaller and smarter than the whales.

Design: [`doc/LANGUAGE.md`](../../doc/LANGUAGE.md).

## Status: M0 complete, M2 (local threads) working

A tree-walking interpreter that defines the language's semantics, now able to
run independent calls on local threads. `--threads 1` is the sequential
reference; any other thread count must produce byte-identical output, and
the test suite checks that at 1, 2, 4 and 8 threads and in automatic mode, on both
engines (tree-walker and p-code) and from compiled `.dcl` files.

| Piece | State |
|---|---|
| Lexer: indentation, str/bytes/f-string literals, hex/bin/octal ints | done |
| Parser: def (typed params, defaults, kwargs), if/elif/else, while, for, try/except/raise, list comprehensions, slicing | done |
| Resolver: binds every name (local slot, global, function, builtin, exception); undefined names are compile errors | done |
| Needs analyzer: whole-program, transitive, fixpoint; unresolved calls = worst case | done |
| Interpreter: int64 with overflow errors, float, str (UTF-8, code-point indexing), bytes, list, dict | done |
| Exceptions: hierarchy under `Error`, assignment targets untouched on failure | done |
| Builtins: strings, f-string format specs, `sorted`, `be_u8..be_u64`, `be_i16sm/i32sm`, `be_f32/f64`, `bits` | done |
| `struct`, `layout`, `match`, tuples, `x if c else y`, `lambda`, `import`, `extern`, closures | not yet implemented (keywords reserved) |
| Static type checking (annotations are parsed but not enforced) | not yet |
| Local parallel execution: futures, batched parallel map, measured-cost dispatch, load-based admission | done (M2) |
| Remote dispatch | M5 |

## Build

```sh
mkdir build && cd build
cmake ..                       # -DDOLPHIN_SANITIZE=ON (ASan + UBSan) or =thread (TSan)
                               # -DDOLPHIN_ALLOCATOR=auto|mimalloc|jemalloc|system (default auto)
make -j$(nproc)
ctest --output-on-failure
```

C++17 or later (the Amoebatu tree builds it as C++20), no dependencies. Inside the Amoebatu tree it builds with everything else; `ctest` from the top-level build runs its tests.

## Use

```sh
dolphin file.dol [args...]     # analyze, run; calls main(*args) if defined
dolphin --needs file.dol       # needs per function, and whether its calls can run in parallel
dolphin --threads N file.dol   # exactly N threads (1 = sequential)
dolphin file.dol               # automatic: sized from the CPU topology, adapts to load
dolphin --engine pcode f.dol   # run function bodies as p-code (doc/PCODE.md); default: tree-walker
dolphin --disasm file.dol      # print the module's and every function's p-code
dolphin -c file.dol            # compile to file.dcl (-o out.dcl, --strip drops register names)
dolphin file.dcl [args...]     # run a compiled file: no source needed (doc/PCODE.md §6–7)
dolphin --stats file.dol       # wall time, dispatch counts and output batching, on stderr
dolphin --sync-output file.dol # print() writes directly instead of via the printer thread
dolphin --trace file.dol       # every dispatch decision and why, failures, parallel maps (stderr)
dolphin --ast file.dol         # clang-style tree: bindings, slots, needs
dolphin --tokens file.dol      # token stream
dolphin --check file.dol       # parse + analyze only
```

Two example programs, one per kind of weather data:

- `examples/metar.dol` decodes a METAR/SPECI bulletin: groups, remarks, flight category.
  Irregular text with optional groups and variable-length reports.
- `examples/grib_ls.dol` lists and decodes simple-packed GRIB2 messages. Regular binary.

```sh
$ ./build/dolphin examples/metar.dol tests/data/metar_bulletin.txt
Bulletin SAUS99 KWBC 231510: 10 reports
STN   TIME  FL WIND          VIS WX       SKY                  T/TD C      ALT   CAT
K6L4  1515Z A  310@8      0.25sm FG       OVC002               9/9         30.09 LIFR
KBCB  1515Z A  calm         10sm -        BKN034 OVC050        14.5/12.7   30.06 VFR
...
$ ./build/dolphin --needs examples/metar.dol | grep -v pure
main            IO
<module>        IO
```

Every decoding function is pure, so each report's decode can run in parallel at M2. `--ast`
shows how each name resolved:

```
|-FunctionDef `flight_category` <line:203> needs=pure locals=4
| |-Param `r` slot=0 : dict
| |-Returns : str
| `-Body
|   |-Assign <line:204>
|   | |-Name `ceiling` local slot=1
|   | `-IntLit 100000
```

```sh
$ ./build/dolphin examples/grib_ls.dol tests/data/synthetic.grib2
  1  TMP    center=7   2026-11-23 12Z +6h  4x3  min=  250.0 max=  287.0
  2  UGRD   center=7   2026-11-23 12Z +6h  4x3  min=  250.0 max=  482.1
2 messages

$ ./build/dolphin --needs examples/grib_ls.dol
describe        pure
main            IO
param_name      pure
split_messages  pure
unpack_simple   pure
walk_sections   pure
<module>        IO
```

Only `main` blocks: everything it calls is pure, so every `describe(m)` can become a future at M2.

## Parallel execution (M2)

The order of preference is to run a call inline, then on a free local thread, and
only later (M5) on another node:

- **What can run on a thread.** The analyzer marks a function parallel-safe when it has
  no needs *and* never mutates an object it didn't create (an argument, a global, or
  something that might alias one). `--needs` shows the verdict per function.
- **Where it happens.** `x = f(...)` becomes a future when the next statement doesn't
  touch `x`, so there's work to overlap. `[f(v) for v in xs]` becomes a batched parallel map.
- **Only when worth it.** (`src/costmodel.hpp`) Three rolling measurements, all taken from
  ordinary execution:
  - **work** per function: operations (statements and expressions) per call, averaged
    over its last 8 calls. It depends only on the code and its input, so it's the same on
    any machine and under any load.
  - **rate**: operations per nanosecond across recent calls that never waited on another
    thread. This is how fast this machine runs right now.
  - **submit cost**: the submitting thread's time to hand a task to the pool. This is
    overhead; the submitter does nothing useful meanwhile.
  - **wake latency**: time until an idle worker starts the task. This is only delay; the
    submitter keeps working. It's bimodal (a just-idle worker starts in a few µs, a
    sleeping one in tens of µs), so it's a trimmed mean over 32 samples, not a median.

  Predicted time = work ÷ rate. A single call is dispatched if it's predicted to take at
  least 4 × (submit + wake): the caller may have to wait and be woken itself. A parallel
  map splits into about 4 batches per thread for load balance, each at least 16 submits
  and one wakeup long. Load changes all of these, so the thresholds follow it, with no
  per-machine constants. A function never measured runs inline once first. If no wakeup
  has been measured in the last 64 inline decisions, the next call worth at least one
  wakeup is dispatched as a probe, so a busy spell can't lock dispatch off. `--stats`
  shows the current measurements and thresholds, and `--trace` shows each decision with
  its numbers. `--fixed-thresholds` restores the old constants (calls ≥ 50 µs, batches
  ~250 µs) for A/B comparison.
- **How many threads.** (`src/topology.hpp`, `ThreadPool` in `src/runtime.hpp`) Without
  `--threads`, the pool creates a worker per CPU it may use (affinity mask and cgroup
  quota) and starts with the topology's suggestion active: P-core threads on Intel hybrid
  chips, one per physical core elsewhere. Every 100 ms it adapts, using figures from
  running tasks only: run-queue wait above 25% parks a quarter of the workers; below 10%
  with work waiting, it adds one, and takes it back if throughput didn't rise. It can park
  every worker and run on the main thread alone. Dolphin programs on one machine share a
  per-user profile in shared memory (`src/sharedprofile.hpp`): a new one starts at its
  fair share of the cores, and a limit found by one run lowers the next run's starting
  point. `DOLPHIN_SHM=0` turns that off. `--stats` shows the range used and the other
  programs seen; `--trace` logs each change.
- **Only when there's room.** If the pool's queue is backed up, the call runs inline.
  Under heavy load this degrades to sequential execution instead of piling up threads.
  A thread waiting on a future that's still queued runs that task itself, but never
  unrelated work: helping with someone else's long task delays your own result
  (priority inversion).
- **Same output as sequential.** Effects (IO) run only on the main thread, in order.
  Before an effect, or before any list/dict mutation, everything dispatched earlier
  must finish. `try` entry and exit and function return are join points.
- **Failures (LANGUAGE.md decision 17).** If several calls fail, the first failure to
  *return* is reported. Calls before it in program order still finish and keep their
  results; calls after it are cancelled, and their assignments don't happen. With a single
  failure that's exactly sequential behaviour. With several, parallel runs can report
  different ones (`examples/first_failure.dol` shows it).

- **Output never waits on the device.** `print()` formats its line and appends it to a
  queue. A printer thread writes everything queued in one `fwrite`, lingering up to 2 ms
  (or until 64 KB is queued) so bursts coalesce: 9,850 METAR lines go out in about 60 writes.
  Order is program order, because only the main thread prints. If the program gets more than
  8 MB ahead of a slow consumer, `print()` blocks until it catches up. Output is drained
  before error messages, `--stats` and exit, so stdout and stderr stay in order.

Checked with ThreadSanitizer and AddressSanitizer as well as the golden tests.

## Tests

`tests/run_tests.sh` runs golden tests from `tests/cases/`:

- `X.out`: expected stdout. `arith`, `strings`, `floats` and `control` are also valid Python, and their `.out` files are generated by CPython. So the Python-shaped core is checked against real Python, not against itself.
- `X.err`: expected error message and a nonzero exit.
- `X.needs`: expected `--needs` output.

Every check runs in four modes: the tree-walker, p-code, p-code without
optimizations, and `dcl` (compiled with `-c`, then the `.dcl` file run). Set
`ENGINE` to run one, and `THREADS` to choose thread counts.

`tests/fuzz_dcl.sh BUILD_DIR [N] [SEED]` fuzzes the `.dcl` loader and verifier
(build the `dcl_fuzz` target first, ideally with `-DDOLPHIN_SANITIZE=ON`):
mutated files must be rejected with a reason or load, and the ones that load
must run without crashing.

`tests/py_oracle.py program.dol args...` runs a program under CPython instead, supplying
`read_file` and `DecodeError`. The expected output of `examples/metar.dol` comes from it.
Anything written in the Python-compatible subset of Dolphin should be tested this way.

METAR test data:

- `tests/data/metar_bulletin.txt`: a real bulletin (SAUS99 KWBC, 10 reports).
- `tests/data/metar_edge.txt`: a synthetic bulletin with the awkward cases: SOH/ETX framing and
  CRLF line ends, `1 1/2SM`, `M1/4SM`, gusts, variable winds, RVR, `+TSRA`, `VV`, `CB`
  layers, a missing dewpoint, `Q` altimeter, SPECI, COR, and a NOSIG trend.

`tests/data/synthetic.grib2` comes from `tests/make_fixture.py`: two 4×3 simple-packed GRIB2 messages. eccodes reads it as `2t` and `u` with the same min/max Dolphin reports. It's synthetic. M1 switches to real NOAA files, checked against `grib_get_data`.

## Documentation

The code is documented for Doxygen. With Doxygen installed:

```sh
cmake --build . --target dolphin_docs     # from the build directory
# or: cd src/dolphin && doxygen
```

Open `src/dolphin/build-docs/html/index.html`. The design doc (`doc/LANGUAGE.md`) is included.

Conventions for new code:

- Every file starts with a `/** @file ... @brief ... */` block saying what it's for.
- Public API is documented in the header: `@brief`, then `@param`, `@return` and
  `@throws` where they add something. If a function documents any parameter, it
  documents all of them; Doxygen warns otherwise.
- Non-obvious implementation choices get plain `//` comments in the `.cpp`
  explaining **why**, e.g. why floor division needs a correction step, or why
  defaults are evaluated per call. Don't narrate what the code already says.
- Point at the design doc for language-level decisions (`LANGUAGE.md §3.5`).
- `doxygen` should run with zero warnings.

## Layout

```
src/lexer.*      tokens, INDENT/DEDENT, literals
src/parser.*     recursive descent + Pratt expressions, f-string fields
src/ast.*        tree, needs bitmask, --ast dump
src/resolver.*   static name binding (locals/slots, globals, builtins)
src/analyzer.*   whole-program needs inference
src/value.*      runtime values, repr/str, UTF-8 helpers
src/interp.*     tree-walking evaluator, p-code interpreter, dispatch/join/barrier logic
src/pcode.*      AST → p-code compiler, optimizations, disassembler
src/dcl.*        .dcl writer and loader, SHA-256
src/verify.cpp   .dcl verifier: bounds, control flow, fresh containers, recomputed analysis
src/runtime.*    thread pool and futures
src/builtins.cpp builtins + needs table, methods, format specs
src/main.cpp     driver
```

`builtinNeeds()` in `builtins.cpp` is the single table of what each builtin needs.
Adding a builtin that isn't in it aborts at startup, so nothing ships with undeclared needs.
