# Dolphin p-code and the `.dcl` file format — design sketch v0.1

*Status: sketch for markup. Implements decision 21 (LANGUAGE.md). File extension `.dcl` (no conflict that matters on Linux: AutoCAD dialog files and OpenVMS scripts use it too; the magic number tells them apart).*

## 1. Goals

1. **Remove the tree-walker's runtime costs**:
   - Locals become register slots, so there are no name lookups.
   - Instructions are contiguous, so evaluation doesn't chase pointers between nodes.
   - Constants are never freed, so threads reading them share no reference counts.
2. **Keep everything M2 built.** Dispatch, futures, barriers, join points, decision 17 failure handling and the cost model all carry over. Output stays identical to the tree-walker at every thread count; the existing golden tests and CPython oracle are the acceptance test.
3. **One format, two delivery paths** (decision 21):
   - **Standalone:** a `.dcl` file run directly, Java-jar style.
   - **Pool:** p-code is a per-node cache of signed source and never travels between nodes.
4. **A good base for M3.** Register form maps straight onto SSA for LLVM lowering, and the JIT compiles from p-code.

Not goals for v1: optimizations beyond simple ones (constant folding, jump threading), a stable public ABI for third-party compilers, or debugger stepping. The line table makes stepping possible later.

## 2. Execution model

- **A call gets a register file:** `Value regs[nregs]`, allocated per call (from a per-thread stack, so there's no malloc per call). Parameters arrive in `r0..r(n-1)`. Locals have fixed registers after them, then temporaries. The resolver already assigns slots; p-code uses them directly.
- **Futures live in registers.** A register assigned from a dispatched call may hold a Future. The compiler knows which locals can (they're targets of `SPAWN`), and puts `FORCE r` before each read of those locals. `FORCE` is a single type check when the value is already resolved. Every other register is never a future, so ordinary reads cost nothing extra.
- **Pending calls are tracked per register,** not per name. Decision 17's "restore the targets" rule becomes "restore these registers", using the frame's pending list.
- **Constants** live in one pool per module, owned by the loaded module for the life of the process. Loading a constant gives an uncounted reference (as `Value::borrowedStr` does today), so no allocation and no shared counter.
- **Globals** are a slot array indexed by number. After module initialization they are never rebound (decision 6: functions can't rebind globals). See §8 for replicating read-mostly globals per thread.

## 3. Instruction encoding

Fixed 32-bit little-endian words, as in Lua 5. Every instruction decodes with shifts and masks, the stream is 4-byte aligned, and a file can be memory-mapped and run without copying the code.

| Format | Bits | Used for |
|---|---|---|
| `ABC` | op:8 A:8 B:8 C:8 | three registers (arithmetic, compare, index) |
| `ABx` | op:8 A:8 Bx:16 | register + constant, global, function or call-site index |
| `AsBx` | op:8 A:8 sBx:16 | register + signed jump offset, or small int |
| `sJ` | op:8 sJ:24 | unconditional jump (±8M instructions) |

Limits: 256 registers per function, 65,536 constants per module, and 65,536 call sites per function. Exceeding one is a compile error with a clear message ("function too large: split it"), not a silent wrap. Wider forms can come in a later minor version if real code needs them.

## 4. Instruction set (v1)

**Loads and moves**

| Op | Form | Meaning |
|---|---|---|
| `MOVE` | AB | rA = rB |
| `LOADK` | ABx | rA = K[Bx] (uncounted reference) |
| `LOADI` | AsBx | rA = sBx (small int) |
| `LOADNIL` / `LOADTRUE` / `LOADFALSE` | A | None / True / False |
| `GETGLOBAL` / `SETGLOBAL` | ABx | global slot Bx. `SETGLOBAL` is valid only in module init code. |
| `GETFUNC` | ABx | rA = function Bx of this module (only when used as a value; direct calls use `CALL`) |

**Arithmetic, comparison, unary** (operands in registers, result in rA; semantics exactly as `Interp::binary` and `Interp::compare` today)

`ADD SUB MUL DIV FDIV MOD POW BAND BOR BXOR SHL SHR` · `EQ NE LT LE GT GE IN NOTIN` · `NEG POS NOT INV`

One opcode per operator, so the interpreter's `switch` jumps straight to it, with no second dispatch on an `Op` code.

**Control**

| Op | Form | Meaning |
|---|---|---|
| `JMP` | sJ | pc += sJ |
| `JMPIF` / `JMPIFNOT` | AsBx | if truthy(rA) / not truthy(rA): pc += sBx |
| `ITER` | AB | rA = iterator over rB (§9, open question 2) |
| `FORNEXT` | AsBx | advance iterator rA into r(A+1); when exhausted, pc += sBx |
| `RETURN` | A | join this frame's pending calls, return rA |
| `RETURNNIL` | — | join, return None |

`and`/`or` and comparison chains compile to jumps, so there are no instructions for them.

**Containers and strings**

| Op | Form | Meaning |
|---|---|---|
| `NEWLIST` | ABC | rA = [rB .. r(B+C-1)] |
| `NEWDICT` | ABC | rA = {rB: r(B+1), ...}, C pairs |
| `INDEX` | ABC | rA = rB[rC] |
| `INDEXK` | ABC | rA = rB[K[C]]. The constant's dict key is precomputed at load (today's `Index::constKey`). |
| `SETINDEX` | ABC | rA[rB] = rC (barrier first, as today) |
| `SLICE` | ABC | rA = rB[rC : r(C+1)] (None for an omitted bound) |
| `FORMAT` | ABC | rA = format(rB, K[C]) (one f-string field with its spec) |
| `CONCAT` | ABC | rA = rB + ... + r(B+C-1) as strings (assembles an f-string in one allocation) |

**Calls.** Call details live in a per-function call-site table, not in the instruction. The instruction stays one word, and the table carries the metadata that's decided at compile time:

| Op | Form | Meaning |
|---|---|---|
| `CALL` | ABx | call site Bx. Arguments in rA, r(A+1), ...; result into rA. Runs inline. |
| `SPAWN` | ABx | same, but may dispatch (`tryDispatch`); rA may receive a Future. Emitted only where today's `overlapAssigns_` would allow it. |
| `FORCE` | A | if rA is a Future, wait for it (and rethrow its failure); replace rA with the value |
| `PMAP` | ABx | rA = parallel map; call site Bx names the function, rA+1 holds the list of argument lists |
| `BARRIER` | — | wait for everything this task dispatched (§2 rules of M2) |
| `JOIN` | — | join point: try entry and try exit |

A call-site entry holds:
- **Target kind:** module function index, builtin index, Method code, or "callable in a register".
- **Argument count**, and keyword arguments already bound to positions when the target is known at compile time.
- **Source line.**
- **Flags:** needs a barrier (an effectful builtin, or a mutating method); parallel-safe target.
- **Cost-model slot.**

Because barriers are decided at compile time, the runtime no longer checks each call's needs.

**Exceptions** use an exception table (start pc, end pc, handler pc, class constant), as the JVM and CPython 3.11 do. Entering a `try` costs only its `JOIN`, with no handler push.

| Op | Form | Meaning |
|---|---|---|
| `RAISE` | A | raise rA |
| `RERAISE` | — | bare `raise` inside a handler |
| `EXCBIND` | A | rA = the exception being handled (`except E as e`) |

## 5. Example

```python
def parse_temp(s: str) -> int:
    if s.startswith("M"):
        return -int(s[1:])
    return int(s)
```

```
parse_temp  params 1  regs 4  needs ()  parallel-safe
  K0 "M"   K1 1
  site0  method startswith, 1 arg          line 53
  site1  builtin int, 1 arg                line 54
  site2  builtin int, 1 arg                line 55

 0  MOVE      r1 r0          ; self
 1  LOADK     r2 K0
 2  CALL      r1 site0       ; r1 = s.startswith("M")
 3  JMPIFNOT  r1 +6          ; -> 10
 4  LOADI     r2 1
 5  LOADNIL   r3
 6  SLICE     r1 r0 r2       ; r1 = s[1:]
 7  CALL      r1 site1       ; r1 = int(r1)
 8  NEG       r1 r1
 9  RETURN    r1
10  MOVE      r1 r0
11  CALL      r1 site2
12  RETURN    r1
```

The tree-walker makes a recursive `eval`/`exec` call per node and hashes the name `s` on every read. P-code runs at most 10 instructions on either path, reads `s` by register index, and only allocates the results themselves (the slice and the int).

## 6. The `.dcl` file

Little-endian throughout. Sections are 8-byte aligned, so code can be memory-mapped and run in place.

```
Header (64 bytes)
  magic        8   89 44 43 4C 0D 0A 1A 0A   ("\x89DCL\r\n\x1a\n", PNG-style:
                                             catches text-mode transfers and
                                             distinguishes from AutoCAD .dcl)
  major        2   format major version: a runtime refuses majors it doesn't know
  minor        2   additions only: older runtimes may refuse a newer minor
  flags        4   bit 0 debug info present, bit 1 signed
  sourceHash  32   SHA-256 of the source it was compiled from
  compilerId   8   compiler version that produced it
  nsections    4
  reserved     4
Section directory: nsections × {type u32, reserved u32, offset u64, size u64}
Sections:
  STRS   string table (UTF-8, length-prefixed). Names, capability strings, messages.
  CONS   constant pool: tagged entries (int, float, str→STRS, bytes)
  GLOB   global slots: name (STRS), init order
  FUNC   per function: name, params (names, defaults as constants), nregs,
         needs bitmask + capability strings, flags (parallel-safe, mutates
         args, per-param mutation), code, call-site table, exception table,
         line table (delta-encoded pc→line)
  DEBG   optional, strippable: source file name, register names per function
  SIGN   optional, last: OpenPGP detached signature (Ed25519 key) over every
         preceding byte
```

- **Pool mode** caches p-code in exactly this format, keyed by (sourceHash, compilerId). A node that gets new signed source misses the cache and compiles.
- **Signing uses OpenPGP (GPG) with Ed25519 keys.** Publishers sign with standard `gpg`; a pool trusts a publisher by importing its published public key, as RHEL/Alma admins already do for package repos (`rpm --import`). Expiry, revocation and air-gapped key distribution come with it. The loader verifies through a small, maintained library (GPGME, or Sequoia as RPM now uses) rather than its own OpenPGP parser, to keep the code that untrusted files reach small.
- **Stripping** (`DEBG` removed) is for shipping without register names. Line numbers stay, so error messages still point somewhere useful.

## 7. Loading and verifying

A standalone `.dcl` is untrusted input. The loader rejects, before running anything:

- A bad magic number, an unknown major version, or sections that overlap or run past the file.
- A register index ≥ nregs, a constant index past the pool, or an unknown function, global, call site or string index.
- A jump target outside the code, and an exception table range or handler outside the code.
- A function whose code can fall off its end (every path must reach `RETURN` or `RAISE`).
- `SETGLOBAL` outside module init code.

**Needs and parallel safety are recomputed, not trusted.** A file could claim an IO function is pure so it gets dispatched to threads. The analyzer runs on p-code (calls and their call-site flags are all it needs), so the loader recomputes needs and compares. A mismatch is rejected. A pool may later choose to skip the recomputation for files signed by a key it trusts (§9).

## 8. Globals: replicate, don't share (proposal)

Today every worker reading `WX_CODES` updates the same reference counts, which ping-pong between CPU caches. P-code removes this for literals (constant pool). For globals holding containers:

- After module init, each worker thread gets its own copy of a global the first time it reads it, so its reference counts are its own.
- A version counter per global is bumped when main mutates it (always behind a barrier, so no worker is mid-read). A worker re-copies on its next read when the version moved.
- This is Orca's replicated shared-object design (LANGUAGE.md §10) applied to threads. Remote nodes need replicas anyway, so it's the same mechanism at two scales.

Cost: one copy per thread per global per mutation. Read-mostly tables, the common case in decoders, are copied once per thread.

## 8a. Runtime under load

Measured 2026-11-24 on an i5-12600K (6 performance cores with SMT + 4 efficiency cores: 10 cores, 16 hardware threads).

**First pass (mixed thread counts, ten runs at once).** Halving dispatches saved at most 5% CPU, so dispatch thresholds are not the lever. Per-run CPU rose from 1430 ms idle to 2167 ms under load, and a 16-thread run used 51% more CPU than a 1-thread run.

**Second pass (ten identical runs at once, per configuration)** separated the causes:

| Ten copies at | Last finishes | User CPU (total) | Sys CPU |
|---|---|---|---|
| 1 thread | 2335–2402 ms | 18.3 s | 0.8 s |
| 2 threads | 2167–2275 ms | 28.3 s | 1.0 s |
| 4 threads | 2164–2192 ms | 31.5 s (user+sys) | |
| 16 threads | 2201–2465 ms | 30.9 s | 1.2 s |
| automatic | 2206–2447 ms | 30.1 s | 1.1 s |

- **The extra CPU is the hardware, not Dolphin.** Ten single-threaded copies are ten threads on ten physical cores. Anything more puts SMT siblings on shared cores, and each thread runs slower, so the same instructions take more CPU time. The extra is user time, not system time (no extra polling or locking), and it doesn't grow past 2 threads per copy. The first pass's "threads interfering through shared reference counts" was mostly this.
- **Once the machine is saturated, thread count barely matters.** Every configuration finishes within about ±5% and uses about the same total CPU, because the machine does the same work either way. CPU time isn't a fair cost measure under SMT: it counts hardware-thread time, and a hardware thread sharing a core does less per second.
- **Load makes every thread slower** (shared cores, E-cores, lower turbo). Nothing in Dolphin can change that.

So the adaptive pool (R1, built) is about *not wasting* resources rather than winning under saturation:

- **Automatic thread count** (no `--threads`): all workers are created, the topology's suggestion starts active (P-core threads on Intel hybrid, the fastest group on asymmetric ARM, one per physical core elsewhere), capped by the affinity mask and cgroup quota.
- **AIMD on contention** (run-queue wait from `/proc/thread-self/schedstat`): above 25%, park a quarter of the workers; below 10% with work waiting, add one.
- **Every addition is checked against throughput:** if it didn't add half an average worker's share, it's taken back and becomes a ceiling for 10 s. This handles SMT, E-cores, Bulldozer modules and NUMA without per-vendor rules.
- **Idle machine result:** about 3% slower than `--threads 16` for about 11% less CPU (469–482 ms, 3.5–3.6 s CPU vs 452–470 ms, 4.0–4.1 s). On this machine it started at 12, tried 13 and 14, and settled at 13.
- **Saturated:** the same as fixed counts (table above).
- **Not tested here:** NUMA (multi-socket Xeon/Opteron), Bulldozer modules and big.LITTLE need other hardware. The rule doesn't depend on them, but it should be measured there.

Followed by §8b: a per-machine profile in shared memory.

## 8b. Machine profile in shared memory (R2, built)

- **Segment:** `/dev/shm/dolphin-<uid>-v1`, mode 0600, 16 KB. `/dev/shm` is writable by everyone, so it's used only if it is owned by this user and private; otherwise Dolphin runs without it. The layout version is in the name. Everything read from it is clamped and treated as a hint. `DOLPHIN_SHM=0` turns it off.
- **Live coordination:** one cache-line slot per running Dolphin process (pid, active threads, heartbeat), updated with lock-free atomics at each control step. A new process starts at its fair share: the larger of cores ÷ live Dolphin processes and the cores the others leave free. A running process won't grow past that. Contention still drives cutting. Slots of dead processes are reclaimed (`kill(pid, 0)`), and slots without a heartbeat for 2 s don't count.
- **Learned limit:** when a throughput check fails on an uncontended machine, the limit is stored, and later runs start no higher than it. It only lowers the topology's guess (where that guess is too high: NUMA, shared-unit modules, quotas). Passed checks aren't recorded: passing can be noise, and an early version that recorded them crept up one thread per run.
- **Same change:** the pool can now park every worker, so a program runs on its main thread alone. Threads submitting work also run control steps, so it can grow back. The cost model's rate needs 8 clean samples before it's trusted. Under 50-program load it had read 0.0 Mops/s from one or two samples.

Results on the 12600K, compared with fixed thread counts:

| Load | Automatic (R2) | Best fixed choice | Before R2 |
|---|---|---|---|
| idle, 1 program | 478–496 ms, starts 12, settles 13 | `--threads 16`: 452–470 ms, ~11% more CPU | same |
| 4 programs | 1008–1061 ms, CPU 11.4–12.1 s | `--threads 16`: 1011–1036 ms, CPU 13.1–13.2 s | 1009–1035 ms, CPU 12.5–12.7 s |
| 50 programs | 10.17–10.33 s, CPU 154–156 s; most start and stay at 1 thread | `--threads 1`: 10.05 s, CPU 151 s | 10.47–10.49 s, CPU 160–161 s |

So with no flags, Dolphin is within about 1–3% of the best fixed choice at each load, and uses less CPU than the fixed count that matches its speed.

**Later:** neighbord exchanges the same figures between nodes, so the local profile becomes the node's entry in the cluster's view.

## 9. Open questions

1. **Register width.** 8-bit registers (256 per function) and a 16-bit constant index. Generous for hand-written code; generated code might hit them.
2. **Iteration semantics.** Today `for x in lst` iterates a snapshot (mutation during the loop isn't seen). Python sees mutation. P-code can do either cheaply: an index walk over the live list, or a snapshot taken only if the loop body can mutate the list (the compiler knows). Proposal: keep snapshot semantics and copy only when needed.
3. ~~Signed standalone files in a pool.~~ **Decided:** a trusted pool runs a `.dcl` signed by a key it has imported (OpenPGP, §6), without source. Open: whether needs are still recomputed for trusted signers (§7). Recomputing is cheap, so the default is yes.
4. **Developer PCs.** Which keys a pool trusts, and whether unsigned work-in-progress may run on pool servers (LANGUAGE.md §9).
5. **Global replication (§8):** accept as the design, or keep shared counts and measure first?

## 10. Plan

| Step | Deliverable | Done when |
|---|---|---|
| R1 | Load-adaptive worker count and topology-based default thread count (§8a) | **Built.** Idle: within ~3% of 16 threads for ~11% less CPU. Saturated: same as fixed counts |
| R2 | Machine profile and live coordination in shared memory (§8b) | **Built.** Within 1–3% of the best fixed thread count at 1, 4 and 50 programs |
| P1a | Compiler (AST → p-code) and sequential p-code interpreter behind `--engine pcode`; `--disasm` | **Built.** Every test passes on both engines (the runner now runs both); ASan/TSan clean. METAR single-threaded: 23% fewer instructions, ~10% faster on the 12600K (1312–1358 ms vs 1458–1506 ms), identical output |
| P1b | Dispatch, futures, barriers, parallel maps and decision 17 as instructions | **Built.** Same output at every thread count and in automatic mode; ASan/TSan clean. METAR on the 12600K: 1 thread −17% (1268–1285 vs 1522–1531 ms), 4 threads −10%, 16 threads −11%, automatic ~−10%, with 12–13% less CPU in parallel |
| O1 | Optimizations (`--opt 1`, default; `--opt 0` keeps the plain form and the test runner checks both): definite-assignment removal of redundant `CHKB`s, fused compare-and-branch (`JLT a b k` + jump word, one dispatch) with `and`/`or`/`not` compiled as jump chains, integer fast paths in arithmetic and comparisons | **Built.** METAR p-code: 1,595 → 1,321 static instructions, `CHKB` 269 → 0; 6% fewer machine instructions. On the 12600K: 1 thread 1198–1216 ms (`--opt 0`: 1294–1308, tree-walker: 1562–1565), 4 threads −6% vs `--opt 0`, 16 threads ~−5% |
| P2 | `.dcl` writer, loader, verifier; `dolphin -c file.dol` → `file.dcl`; `dolphin file.dcl`; `--disasm` | Round trip: compile, write, load, run gives identical output. Fuzzed files are rejected, never crash. |
| P3 | Global replication (§8), if measurements support it | Speedup at 8–16 threads improves on METAR |
| P4 | Remove the tree-walker | p-code is the only engine |
| — | Signing, pool cache | With M5 |

**P1a notes.**
- Function bodies run as p-code; module-level code still runs on the tree-walker (it runs once). A function the compiler can't handle (more than 256 registers, slice or attribute assignment) stays on the tree-walker.
- P1b: `SPAWN` for calls the tree-walker would dispatch (pending calls record the target register, so decision 17 restores registers as it restores variables). `FORCE` goes only before reads of locals a `SPAWN` writes. A comprehension that can run as a parallel map compiles twice, with `JMPNOPOOL` choosing: the sequential loop when there's no pool, or argument lists collected first and then `PMAP` running them through the batching code shared with the tree-walker (`Interp::parallelMap`).
- Calls between p-code functions with positional arguments copy registers straight into the callee's registers (no argument vector), and small functions keep their registers inline (no allocation). Global reads cache the global's slot after the first lookup. Together: about 3% fewer instructions.
- Operators, builtins, methods and formatting are shared with the tree-walker, not reimplemented. So results and error messages are the same.
- Known differences, only when two things fail at once:
  - `obj[f()]` where `obj` isn't subscriptable: the tree-walker reports the object first, p-code evaluates `f()` first.
  - A call to a module function that isn't defined yet: p-code evaluates the arguments before raising the NameError.
- Next costs, from the profile: dict lookups hash their string key every time (constant keys could carry a precomputed hash), Value copies (reference counting) on register moves, and method calls still building an argument vector. Then loop-invariant hoisting (`len(msg)` in a `while` test), which needs the analyzer's mutation information, and repeat `FORCE` removal (same analysis as `CHKB`).
