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

## 8a. Runtime under load: fewer threads, not fewer dispatches

Measured 2026-11-24 on an i5-12600K (6 performance + 4 efficiency cores, 16 threads), with ten METAR runs at once (62 threads on the machine), comparing CPU time per run:

- **Dispatch count barely moves CPU cost.** At 8 threads the measured cost model dispatched 42% less than fixed thresholds (74 vs 128) and saved 5% CPU. At 16 threads it dispatched 25% less and used 3% *more*. Per-dispatch overhead is small.
- **Thread count does.** Under load, a 16-thread run cost 51% more CPU than a 1-thread run of the same work. The extra cost comes from threads interfering with each other (shared reference counts, memory traffic, SMT siblings), and it grows with the number of active threads.
- **Load makes every thread slower.** The same 1-thread run cost 1430 ms CPU idle and 2167 ms under load (+52%); the measured rate fell from ~33 to ~20 Mops/s. On a hybrid chip, loaded threads land on efficiency cores.

So the cost model detects load correctly (wake latency rises from tens of µs to milliseconds) but adjusts the wrong thing. Changes, as runtime work alongside P1:

1. **Adapt active workers to load.** When wake latency shows no free cores, park workers so fewer threads compete; unpark when it falls. Dispatch thresholds stay as they are.
2. **Default thread count from performance cores,** not hardware threads: on hybrid chips, count P-cores (and their SMT siblings only if measured to help). 6 or 12 on the 12600K, not 16.
3. **Measure with CPU time.** `--stats` reports it; under multi-program load it, not wall time, is the figure of merit, since Linux shares CPU per thread and wall time rewards a program for grabbing more threads.

## 9. Open questions

1. **Register width.** 8-bit registers (256 per function) and a 16-bit constant index. Generous for hand-written code; generated code might hit them.
2. **Iteration semantics.** Today `for x in lst` iterates a snapshot (mutation during the loop isn't seen). Python sees mutation. P-code can do either cheaply: an index walk over the live list, or a snapshot taken only if the loop body can mutate the list (the compiler knows). Proposal: keep snapshot semantics and copy only when needed.
3. ~~Signed standalone files in a pool.~~ **Decided:** a trusted pool runs a `.dcl` signed by a key it has imported (OpenPGP, §6), without source. Open: whether needs are still recomputed for trusted signers (§7). Recomputing is cheap, so the default is yes.
4. **Developer PCs.** Which keys a pool trusts, and whether unsigned work-in-progress may run on pool servers (LANGUAGE.md §9).
5. **Global replication (§8):** accept as the design, or keep shared counts and measure first?

## 10. Plan

| Step | Deliverable | Done when |
|---|---|---|
| R1 | Load-adaptive worker count and P-core default thread count (§8a) | Under the ten-run load, CPU per run falls at 8–16 threads with no loss when the machine is idle |
| P1 | Compiler (AST → p-code) and p-code interpreter behind `--engine=pcode`, tree-walker kept as `--engine=tree` | All golden tests, the CPython oracle and the METAR loads match at 1–8 threads, and TSan is clean |
| P2 | `.dcl` writer, loader, verifier; `dolphin -c file.dol` → `file.dcl`; `dolphin file.dcl`; `--disasm` | Round trip: compile, write, load, run gives identical output. Fuzzed files are rejected, never crash. |
| P3 | Global replication (§8), if measurements support it | Speedup at 8–16 threads improves on METAR |
| P4 | Remove the tree-walker | p-code is the only engine |
| — | Signing, pool cache | With M5 |
