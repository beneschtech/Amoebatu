# Prior Art & Divergence
## Amoeba OS → Amoebatu: What We Inherited, What We Fixed, What's New

*Based on primary source reading of the Tanenbaum et al. paper collection,
Vrije Universiteit Amsterdam, 1986–1996.*

---

## The Papers

The following papers were reviewed for this document. All are available in
`docs/papers/` in this repository.

| File | Title | Year |
|---|---|---|
| dcs86.pdf | Using Sparse Capabilities in a Distributed OS | 1986 |
| rds87.pdf | Reliability in Distributed Systems | 1987 |
| dcs87.pdf | Connecting RPC-Based Systems Using Wide-Area Networks | 1987 |
| euteco88.pdf | Problems with RPC | 1988 |
| osr88.pdf | Overview of the Amoeba Distributed OS | 1988 |
| spe89.pdf | The Performance of the Amoeba Distributed OS | 1989 |
| scm89.pdf | Amoeba Make (Amake) | 1989 |
| cacm90.pdf | Experiences with the Amoeba Distributed OS | 1990 |
| ieee90.pdf | Amoeba: A Distributed OS for the 1990s | 1990 |
| ukuug90.pdf | Amoeba Overview (UKUUG) | 1990 |
| cs91.pdf | Amoeba/Sprite Comparison | 1991 |
| comcom91.pdf | The Amoeba Distributed OS — A Status Report | 1991 |
| wssm91.pdf | Wide-Area Systems | 1991 |
| dse92.pdf | Distributed Operating Systems Anno 1992 | 1992 |
| tocs93.pdf | FLIP: Internetwork Protocol for Distributed Systems | 1993 |
| cpe94.pdf | Parallel Computing with Shared Objects (Orca) | 1994 |
| group94.pdf | Group Communication in Amoeba | 1994 |
| tromso94.pdf | The Amoeba Microkernel | 1994 |
| super95.pdf | Microkernels for Parallel Supercomputers | 1995 |
| dcs96.pdf | Group Communication Protocols — Lessons Learned | 1996 |

---

## Part 1: What Amoeba Got Right

These are the ideas from Amoeba that Amoebatu inherits, preserves, and builds upon.
Tanenbaum's team got these right and the field has largely vindicated them.

### 1.1 The Transparency Goal

Amoeba's stated goal from day one was that *"to the user, the complete system should
look like a single computer."* Users should not know which machines they are using,
how many, or where their files are stored. This is Amoebatu's goal too, stated almost
identically. The difference is in how we achieve it — Amoeba used RPC transparency,
Amoebatu uses function dispatch transparency.

### 1.2 The Processor Pool Model

Rather than giving each user a personal workstation for all their computing, Amoeba
centralized compute power in a **processor pool** — a rack of CPUs dynamically
allocated to jobs as needed and returned when finished. This is the right model.
It allows a single large job to temporarily commandeer many processors, provides
natural load balancing, and scales by adding hardware rather than reconfiguring software.

Tanenbaum's team chose this over the personal workstation model explicitly because
they correctly predicted that processor costs would fall faster than people's ability
to use dedicated machines. They were right. Amoebatu's cluster model is the
direct descendant of the processor pool — the Supermicro 4-pack is a processor pool.

### 1.3 Capability-Based Naming and Protection

Amoeba used 128-bit capabilities as the *single unified mechanism* for naming,
accessing, and protecting all objects — files, processes, memory segments, network
connections, everything. A capability has four fields:

```
Server Port (48 bits) | Object Number (24 bits) | Rights (8 bits) | Check Field (48 bits)
```

The check field uses a one-way function applied to `(random_seed XOR rights_bits)`
making capabilities cryptographically unforgeable without the server's secret random seed.
To restrict access, a capability is passed back to the server which creates a
restricted version with some rights bits zeroed — and a new check field computed over
the new rights, making it impossible to re-add rights you don't have.

**What Amoebatu takes from this:** The concept of capability-addressed function dispatch.
A function object in Amoebatu is analogous to a capability — it grants the holder the
right to invoke a computation, with signed proof of origin. The Ed25519 signature on
function objects is a modern, stronger equivalent to the capability check field.

**What Amoebatu changes:** Amoeba's capabilities required a central directory server
to map human-readable names to capabilities. Amoebatu uses content-addressable
identity (SHA256 of source) as the primary handle — no directory server needed for
function lookup, which removes a centralization point.

### 1.4 Microkernel Philosophy

Amoeba's kernel was intentionally minimal — managing only processes/threads, basic
memory management, communication, and low-level I/O. Everything else (file system,
directory service, process placement, even the window system) ran as user-space
server processes. Tanenbaum described this as *"the RISC approach to operating
systems: if a feature isn't essential, leave it out of the kernel."*

The advantages proved real: servers could be written and debugged without rebooting
the kernel, the system was highly portable (it ran on 68020, VAX, NS32016, 80386,
SPARC, and MIPS), and fault isolation meant a buggy server didn't crash the whole system.

**What Amoebatu takes from this:** The same philosophy — the LFS base system is
minimal by design, the distributed runtime runs in user space, and no new kernel
modifications are required. The runtime is a set of daemons, not kernel modules.

### 1.5 Objects and Servers

In Amoeba, every resource is an object managed by a server process. Clients don't
access resources directly — they ask the appropriate server to perform operations on
their behalf by presenting a capability. This gives location transparency: the client
doesn't know or care which machine the server is on.

**What Amoebatu takes from this:** Function objects as the core primitive — every
dispatchable unit of work is a self-describing object. The capability manifest inside
a FunctionObject determines which nodes can act as "server" for it.

### 1.6 Immutable File Design (Bullet Server)

The Bullet Server — Amoeba's primary file server — stored all files as immutable,
contiguous byte strings both on disk and in a 12-16MB RAM cache. Files couldn't be
modified; you read, edited locally, and wrote a new version with a new capability.

This design achieved extraordinary performance because:
- Files could be sent in a single RPC reply (no block-by-block protocol)
- Caching was trivially correct (immutable data never goes stale)
- Replication was trivial (all copies are identical by definition)
- Disk reads were single large DMA operations with no block table overhead

**What Amoebatu takes from this:** The FunctionObject's `canonicalSource` field is
immutable — identified by SHA256 of content, never modified in place, only superseded
by new versions. The content-addressable identity scheme is the Bullet Server's insight
applied to executable code. Same cache correctness properties follow naturally.

---

## Part 2: What Tanenbaum Got Wrong (His Own Admission)

The `cacm90.pdf` paper is remarkably candid. After eight years of building and using
Amoeba, Tanenbaum explicitly listed what they did wrong. Amoebatu is designed to not
repeat these mistakes.

### 2.1 Non-Preemptable Threads

**The mistake:** In Amoeba 4.0, threads ran to completion — no preemption. The idea
was that a thread using a critical table wouldn't be interrupted by another thread in
the same process. This seemed elegant and simple.

**What went wrong:** Programmers didn't have a clear mental model of when a thread
blocked. If a developer added a `printf` debug statement inside a critical region, that
`printf` did an RPC with a remote terminal server. While blocked waiting for the
acknowledgement, another thread could run and access the critical region, destroying
its invariants. The sanctity of the critical region could be broken by adding a print
statement — a property Tanenbaum called "very confusing to naive programmers."

Amoeba 5.0 made all threads preemptable and required mutexes for critical regions.

**Amoebatu's position:** Linux 6.12 LTS with `PREEMPT_RT` baked in. Full
preemptability everywhere, from the start. All shared state protected with
`std::mutex`, `std::atomic`, or `std::condition_variable`. The `keep_running`
variable in `neighbord` is `std::atomic<bool>` precisely because of this class of bug.

### 2.2 No Multicast at the Outset

**The mistake:** Amoeba was built on point-to-point RPC as the only communication
primitive. When parallel applications needed to notify many processes simultaneously
(e.g., the traveling salesman problem finding a better path and needing to tell all
workers immediately), there was no primitive for it. You had to simulate multicast with
N separate RPCs — expensive and non-atomic.

Tanenbaum wrote: *"This idea is obvious and we should have realized it earlier and put
it in from the start."* Amoeba 5.0 added group communication as a first-class primitive.

**Amoebatu's position:** Multicast is the *first* thing implemented. `neighbord`
discovers peers via UDP multicast broadcast before anything else exists.
The OOB cluster event system (Message 7) is essentially reliable multicast.
This is not an afterthought — it's the foundation.

### 2.3 Asynchronous RPC (Briefly, in Amoeba 2.0)

**The mistake:** Amoeba 2.0 had asynchronous RPC — the sender transmitted a
message and continued executing; a reply caused an interrupt. This allowed parallelism
but was, in Tanenbaum's words, *"impossible to program correctly."*

They reverted to synchronous RPC. Tanenbaum's advice: *"avoid asynchronous messages
like the plague."*

**Amoebatu's position:** We agree on the programmer experience side but take a
different technical approach. The user-facing language uses **implicit futures** —
the programmer writes apparently synchronous code, but the runtime dispatches
asynchronously and blocks only at actual dependency points. The asynchrony is
hidden from the programmer entirely. This is what Amoeba 2.0 was trying to achieve
but couldn't do cleanly without language-level support.

### 2.4 Building Their Own Window System

**The mistake:** Amoeba initially built a custom window system rather than porting X.
This consumed significant development effort on infrastructure that wasn't their
core research contribution, and the result was inferior to X.

They eventually ported X11 to Amoeba. Tanenbaum listed building the homebrew window
system as one of the explicit mistakes.

**Amoebatu's position:** We don't build any display system at all. The browser is
the display terminal. nginx + webc-fcgi + WebSocket delivers UI to whatever browser
the user has, which handles rendering, fonts, GPU acceleration, input, accessibility,
and cross-platform support for free. We spend zero effort on display infrastructure.

### 2.5 RPC as the Foundational Primitive

This is the central architectural mistake and deserves its own section.

**The mistake:** Amoeba made RPC the primitive on which everything else was built,
including device drivers. Every kernel service, every driver interaction, every
file operation went through RPC. The result was a system where the communication
layer's assumptions — synchronous, point-to-point, master-slave — were baked into
every abstraction above it.

The `euteco88.pdf` paper (a critique of RPC the Amoeba team themselves contributed to)
identified the core problems:

- RPC is *inherently master-slave* — not all distributed relationships have this structure
- RPC is *inherently point-to-point* — multicast requires N separate RPCs
- RPC conflates *communication mechanism* with *computational model*
- RPC is *synchronous by nature* — parallelism requires explicit workarounds
- RPC performance suffers from *marshaling/unmarshaling overhead* on every call

The consequence was that device drivers in Amoeba were essentially remote servers
with all the latency and ordering assumptions of RPC baked in before the hardware
layer was well understood in a distributed context.

**Amoebatu's position:** Establish the hardware capability substrate **first**.
Async event channels second. RPC-like dispatch semantics are a specialization on top,
not the foundation. The layering is:

```
Hardware capabilities (neighbord, capability advertisement)
    ↓
Async event channels (Cap'n Proto message layer, OOB events)
    ↓
Function dispatch semantics (Execute/Result message pair)
    ↓
Adaptive scheduler (emergent from statistics)
```

Device-like capabilities (has GPU, has display, has NVMe) are advertised as properties
of nodes — they inform dispatch decisions rather than being the communication mechanism
themselves. The `HardwareCapabilities` struct in the Cap'n Proto schema is the correct
abstraction level for hardware capabilities: it's a property, not a transport.

---

## Part 3: What the Papers Reveal That Amoeba Couldn't Solve

Beyond the acknowledged mistakes, careful reading reveals problems Amoeba struggled
with that Amoebatu's design addresses differently.

### 3.1 Static Scheduling — The Missing Adaptive Layer

Amoeba's `amake` (parallel make) allocated processors from the pool and assigned
work to them. The allocation strategy was essentially static — take N processors,
assign one compilation per processor, return when done. There was no mechanism for
the system to learn that certain compilations ran faster on certain machines, or to
adapt allocation based on observed performance.

The `cs91.pdf` Amoeba/Sprite comparison notes that Amoeba's processor pool model
made it *"more difficult to balance load effectively"* because pool processors were
allocated before the actual computational demands were known.

**Amoebatu's answer:** The adaptive scheduler with per-function, per-node performance
statistics. No static allocation. Functions accumulate performance history and the
dispatcher learns where each type of work runs best. The epsilon-greedy exploration
term prevents premature convergence. The system doesn't just allocate processors —
it learns which processors are best for which work.

### 3.2 No Trust Model for Code Distribution

Amoeba's capability system protected *data* objects effectively, but there was no
mechanism for asking *"did this executable come from a trusted source?"* Code was
loaded from the file system; if the file system was compromised, the code was
compromised. The capability check field protected the capability itself from forgery,
but not the content of the object the capability pointed to.

**Amoebatu's answer:** Ed25519 signatures on FunctionObjects. Every function object
carries a cryptographic proof of authorship. Nodes verify before caching or executing.
A fired employee's key can be revoked and their code stops running cluster-wide within
milliseconds via OOB cluster events. The trust repository (PostgreSQL) maintains the
authoritative record of valid signing keys and their revocation status.

### 3.3 RPC Under Load Degrades Non-Linearly

The `spe89.pdf` performance paper shows something revealing: Amoeba RPC performs
excellently on lightly loaded networks (1.1ms null RPC, 820 KB/s throughput at the
time — remarkable for 1989). But under load, with many clients and one server, the
performance degrades significantly because all communication serializes through the
RPC mechanism.

The stop-and-wait protocol for large messages (each packet individually acknowledged)
was a known bottleneck they planned to fix in 5.0.

**Amoebatu's answer:** Cap'n Proto with zero-copy deserialization. The message layer
is designed for throughput, not just round-trip time. The async dispatch model means
a node doesn't block waiting for RPC replies — it fires Execute messages and handles
Result messages as they arrive. The callee-forwards capability in Message 5 means
load can be redistributed without a central coordinator.

### 3.4 Group Communication Was an Afterthought

The `group94.pdf` and `dcs96.pdf` papers describe the group communication system
added in Amoeba 5.0 — sequencer-based reliable ordered multicast. It works, and
works well, but it was retrofitted onto a system built around point-to-point RPC.

The sequencer is a single node that assigns sequence numbers to all group messages.
As the `dcs96.pdf` lessons-learned paper notes, the sequencer became a bottleneck
in some applications, and a migrating sequencer would have been better — but adding
it would have required significant protocol redesign.

**Amoebatu's answer:** OOB cluster events (Message 7) use **flood with deduplication**
rather than a sequencer. Every node forwards every OOB event to all its neighbors;
nodes track event IDs they've seen and drop duplicates. This gives:
- No sequencer bottleneck
- No single point of failure
- Sub-millisecond propagation on LAN
- Convergence time proportional to network diameter, not cluster size

The tradeoff is weaker ordering guarantees — OOB events are not totally ordered
across the cluster. For security events (KEY_REVOKED) this is acceptable because
the desired behavior is *eventually consistent security*, not *strictly ordered security*.
Every node will process the revocation within milliseconds; the exact order relative
to other events doesn't matter as long as it happens fast.

### 3.5 Wide-Area Transparency Was Never Fully Solved

The `dcs87.pdf` and `comcom91.pdf` papers describe significant effort spent making
Amoeba work across wide-area networks via gateways. The gateway approach was
complex — requiring special link processes for each WAN protocol (X.25, TCP/IP, etc.)
and introducing latency discontinuities that broke the transparency illusion.

The FLIP protocol (`tocs93.pdf`) improved this but acknowledged: *"we have no
experience with large networks containing thousands of subnets."*

**Amoebatu's position:** Wide-area transparency is explicitly **deferred**. The design
starts with a flat same-subnet network of 4 identical nodes. Cross-subnet and WAN
operation are parking-lot items. When the time comes, the seed/bootstrap node list
in Message 4 (Neighbor Discovery) is the designed hook for cross-subnet extension.
This is honest about scope rather than over-promising transparency we haven't built.

---

## Part 4: What's Genuinely New in Amoebatu

These ideas have no clear direct ancestor in the Amoeba papers.

### 4.1 Adaptive Scheduling via Reinforcement Learning

Amoeba's processor pool allocated processors statically and had no mechanism for
learning from execution history. Amoebatu's scheduler is an **online learning system**:

- Each node maintains per-function, per-destination performance statistics
- Statistics decay over time (recent performance weighted more than historical)
- An epsilon-greedy exploration term periodically tries non-preferred nodes
- The scheduler converges toward optimal dispatch without being programmed to

This is epsilon-greedy reinforcement learning applied to task dispatch. The gradient
descent / false bottom analogy is apt: the exploration term prevents the system from
getting permanently stuck in a local optimum the same way noise helps gradient descent
escape saddle points.

No distributed OS paper in this collection describes anything like this. The closest
is Amoeba's `amake` parallel build tool, which adapted its work distribution based on
observed compilation times — but this was specific to the build system, not a general
scheduling primitive.

### 4.2 Functions as Self-Describing Execution Capsules

In Amoeba, an executable was a file — a collection of bytes identified by a capability,
loaded from the Bullet Server, and started by the process server. It carried no
self-description of its computational requirements, no performance history, no
dependency graph, no signing information.

Amoebatu's FunctionObject carries all of this:

```
FunctionObject {
    identity          // SHA256 of source — content-addressable
    canonicalSource   // portable source, text only
    typeSignature     // inputs, outputs, side effects
    capabilityManifest // what hardware this function needs
    statsHint         // performance summary from origin node
    dependencies      // other functions this one needs
    signerKeyId + signature  // cryptographic authorship proof
}
```

A function object can be received by a node that has never seen it before, and that
node immediately knows: what it does, what it needs to run, whether to trust it, how
it has performed elsewhere, and what other functions it depends on. This is the
difference between shipping a binary and shipping a self-describing agent.

### 4.3 Source-Only Distribution with Local Compilation

Amoeba distributed compiled binaries — architecture-specific executables built for
the 68020, SPARC, etc. This required maintaining separate binaries per architecture
and created the attack surface of receiving and executing pre-compiled foreign code.

Amoebatu distributes **source only**. Each receiving node compiles locally using its
own toolchain. This means:
- One source object works on x86_64, aarch64, riscv64, or any future architecture
- No pre-compiled bytecode as an attack surface
- Each compilation is optimized for the local CPU by the local compiler
- The identity hash (SHA256 of source) is stable across architectures

The first-time compilation penalty is real but washes out via the statistics decay —
after a function has been received and compiled once, subsequent dispatches are free.

### 4.4 Global Variable Stratification

Amoeba had a single namespace for shared objects, all protected by capabilities and
accessed via RPC. There was no semantic distinction between "this object is frequently
shared cluster-wide" and "this object is accessed only on one machine most of the time."
All shared state paid the same RPC cost.

Amoebatu introduces explicit stratification:

```
Cluster Global     → Expensive. Full consistency. Ownership token model.
Context Global     → Node-local. No coordination cost.
Local              → Function scope. Free.
```

The ownership token model (check out for burst writes, return when done) is
similar to database-style optimistic locking but applied to distributed variable access.
This makes the cost of coordination explicit and visible to the programmer —
expensive global globals should be used sparingly, and the language makes that cost
felt rather than hiding it.

### 4.5 Browser as Universal Display Terminal

Amoeba struggled with window systems for years, building a custom one and then
eventually porting X11. The papers don't consider what seems obvious in retrospect:
the browser is the universal display terminal.

Amoebatu's UI approach: nginx delivers a small static JavaScript bundle once.
After that, all UI communication is WebSocket — the server sends structured widget
descriptions (not HTML), the browser renders natively. The browser handles fonts,
GPU acceleration, input, accessibility, and cross-platform compatibility. The cluster
nodes never touch display hardware.

This maps naturally onto the capability model: a workstation with a browser is a node
with `hasDisplay = true`. Rendering to screen is a function dispatched to the only
node capable of it. No special-casing required.

---

## Part 5: Performance Context

The performance numbers from the Amoeba papers give useful context for what
Amoebatu should expect on modern hardware.

### Original Amoeba (1989, Motorola 68020 @ 16MHz, 10Mbit Ethernet)

| Operation | Time |
|---|---|
| Null RPC (user-to-user) | 1.1 ms |
| 8KB file read (remote) | 13.1 ms |
| 30KB message | 37.4 ms |
| Throughput (large messages) | ~800 KB/s |

These numbers were considered excellent in 1989 — 9x faster than Sun RPC at the time.

### Amoebatu Target (2025, Intel Skylake-SP, 10GbE)

The hardware improvement is roughly:
- CPU: ~200x faster (clock speed + IPC + cache improvements)
- Network: 1000x faster bandwidth (10Mbit → 10Gbit)
- Network latency: ~10x lower (10ms typical → ~100μs for 10GbE LAN)

Cap'n Proto zero-copy deserialization benchmarks suggest sub-microsecond message
parsing for typical message sizes. A null Execute/Result round trip on the Supermicro
cluster should be well under 100 microseconds — over 10x faster than Amoeba's
best null RPC, on hardware that costs a fraction of what an Ethernet-connected
68020 cluster cost in 1989.

The adaptive scheduler's statistical overhead (updating exponential moving averages)
is nanoseconds per dispatch — negligible relative to network latency.

---

## Summary: The Design Lineage

```
Amoeba (1986-1996)
│
│  Inherited by Amoebatu:
│  ├── Transparency goal (single system illusion)
│  ├── Processor pool model (dynamic allocation, shared compute)
│  ├── Capability-based identity (→ Ed25519 signed FunctionObjects)
│  ├── Microkernel philosophy (→ minimal LFS base, user-space runtime)
│  ├── Object model (→ FunctionObject as first-class primitive)
│  └── Immutable content identity (→ SHA256 content-addressing)
│
│  Fixed by Amoebatu:
│  ├── Non-preemptable threads → PREEMPT_RT kernel, std::atomic throughout
│  ├── No multicast at outset → multicast-first, neighbord is the foundation
│  ├── Custom window system → browser as universal display terminal
│  ├── RPC as foundation → hardware capability substrate first, RPC semantics on top
│  └── No trust model for code → Ed25519 signing, Postgres trust repository
│
│  Amoeba couldn't solve, Amoebatu addresses differently:
│  ├── Static scheduling → adaptive scheduler with online learning
│  ├── Group communication retrofitted → OOB flood propagation from the start
│  └── Wide-area transparency → explicitly deferred, designed hook in discovery layer
│
└── New in Amoebatu (no Amoeba precedent):
    ├── Adaptive scheduling via epsilon-greedy reinforcement learning
    ├── Self-describing execution capsules (FunctionObject)
    ├── Source-only distribution with local compilation
    ├── Global variable stratification (cluster/context/local)
    └── Browser as capability-addressed display terminal
```

---

*Document compiled July 2026 from primary source reading of the Tanenbaum et al.
Amoeba paper collection (1986–1996).*

*AI research assist: Claude (Anthropic)*
