# Amoebatu 🦠

> *What if your office full of PCs was one computer?*

Amoebatu is a distributed adaptive computing layer for Linux clusters. Inspired
by Tanenbaum's Amoeba OS — and determined to fix its mistakes — it turns a
network of ordinary machines into a single transparent computing environment
where functions run wherever they run fastest, and the system gets smarter
about that every day.

No shared storage. No central scheduler. No single point of failure.
Just nodes that find each other, learn from each other, and get faster together.

From the real developer:

I know this all screams of AI vibe coding, but trust me, theres a real developer behind this. One who resists change
sometimes a little too much, and has been doing this long enough that an obscure project from the 90s is the first
thing that came to mind as the biggest problem to be solved in computer science today. Claude has been wonderful with
documentation, code review, and pooling a vast store of knowledge than any one developer could ever hope to have at
their disposal. Despite my best efforts to downplay AI's usefulness, Claude has been a great help in making this
project seem possible.

Current Dev Environment:

Ubuntu 26

CLion (Jetbrains)

Target system running LFS build (13.0-systemd) with a growing but limited amount of packages built. Supermicro node
circa 2017 just before NVME native support and 64 bit PCIE space addressing. 32x Opteron cores, 128G RAM, NVME main OS 
drive via PCIE adapter. Dont worry, I can afford this, the RAM is DDR3. If the new cluster is
DDR4 I will apply for a second mortgage.

Final pre-release test cluster. SuperMicro "4-pack" 1st NVME generation (VROC)

---

## The Core Idea

In most distributed systems you move data to code. Amoebatu moves **code to data**.

Functions are self-describing objects that travel across the network and execute
wherever the cluster decides is best — based on hardware capability, current load,
and accumulated performance history. A function that runs faster on node 3 will
increasingly be sent to node 3. A function that needs a GPU will only ever go to
nodes that have one. A function that needs a display will go to the workstation.

The programmer writes something that looks like Python. The runtime handles the rest.

```python
# These two run in parallel on whatever nodes are best suited
x = process_radar_sweep(raw_data)
y = fetch_model_output(timestamp)

# This waits naturally — no locks, no callbacks, no async/await ceremony
result = correlate(x, y)
```

Add more machines to the network and the whole cluster gets faster.
The system figures out how to use them.

---

## What Makes This Different

**Tanenbaum got Amoeba mostly right, but put RPC before device drivers.**
That baked implicit determinism into every layer that followed. Amoebatu
fixes the layering:

```
Hardware capability substrate       ← establish this first
      ↓
Async event channels                ← build on top
      ↓
RPC-like dispatch semantics         ← specialization, not foundation
      ↓
Adaptive scheduling                 ← emerges from statistics, not configuration
```

**The scheduler learns.** Every function execution generates performance data
that flows back to the dispatching node. Over time, the cluster develops
institutional knowledge about what runs where. New nodes are explored
periodically so the system never gets permanently stuck on a local optimum —
the same principle that keeps gradient descent from false bottoms.

**Security without overhead.** Function objects carry Ed25519 signatures.
Nodes verify before executing. Revocations propagate in milliseconds via
out-of-band gossip. You don't encrypt your code — you sign it, the same
way you sign a git commit.

**Heterogeneous by design.** The same framework that runs on four identical
Supermicro servers scales to a mixed environment of workstations, GPU nodes,
and data center hardware. Capability-addressed dispatch means the right work
always finds the right hardware.

---

## Current Status

🟢 **Working:** Neighbor discovery daemon (`neighbord`) — nodes find each other
via UDP multicast and maintain a live peer table.

🔨 **In progress:** Node identity, SQLite peer persistence, merge-on-discovery
gossip, FastCGI API for peer list queries.

📋 **Designed, not yet built:** Function object dispatch, adaptive scheduler,
global variable stratification, trust repository, web UI.

This is early stage but architecturally serious. The design has been thought
through carefully. See [DESIGN.md](docs/DESIGN.md) for the full specification.

---

## Architecture at a Glance

```
┌─────────────────────────────────────────────────────────┐
│                    User Code (Python-shaped)             │
├─────────────────────────────────────────────────────────┤
│              Distributed Runtime (C++)                   │
│   ┌──────────────┐  ┌──────────────┐  ┌─────────────┐  │
│   │   Adaptive   │  │   Function   │  │   Global    │  │
│   │  Scheduler   │  │Object Cache  │  │  Var Store  │  │
│   └──────────────┘  └──────────────┘  └─────────────┘  │
├─────────────────────────────────────────────────────────┤
│              Cap'n Proto Message Layer                   │
│  Execute │ Fetch │ GlobalVar │ Discovery │ Result │ OOB  │
├─────────────────────────────────────────────────────────┤
│           neighbord — Cluster Membership                 │
│      UDP Multicast Discovery + Gossip Propagation        │
└─────────────────────────────────────────────────────────┘
```

Each node runs the same stack. There is no master node.

---

## Getting Started

### Prerequisites

- Linux (LFS, Debian, Ubuntu, Fedora — anything with a modern kernel)
- Kernel 6.6+ recommended; 6.12 LTS preferred
- C++17 compiler (GCC 12+ or Clang 15+)
- CMake 3.20+
- systemd (for `/etc/machine-id`) or equivalent

### Build

```bash
git clone https://github.com/beneschtech/Amoebatu.git
cd Amoebatu
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Run the discovery daemon

```bash
# On each node
sudo ./neighbord -p 26781 -i eth0

# Watch the cluster form
tail -f /var/log/amoebatu/neighbord.log
```

Nodes on the same subnet will find each other within one announce interval (15s default).

---

## Project Structure

```
src/
  neighbord/          # Discovery daemon — the first piece
  lib/
    fastcgi/          # HTTP/FastCGI layer (webc-fcgi, hardened)
    jsonrpclib/       # JSON-RPC over UDP for inter-node messaging
docs/
  DESIGN.md           # Full architecture specification
  papers/             # Tanenbaum's original Amoeba papers (the inspiration)
```

---

## The Bigger Picture

The near-term goal is a 4-node Supermicro cluster running a working function
dispatch prototype. The longer-term goal is something you can drop into an
office network and have it transparently absorb idle compute — every PC a
participant, every idle cycle useful, the whole thing self-organizing.

The design is intentionally conservative about what requires consensus
(almost nothing) and aggressive about what can be eventually consistent
(almost everything). This keeps the system fast and the failure modes simple.

---

## Contributing

This project is looking for people who are interested in:

- **C++ runtime development** — the scheduler, function object serialization,
  Cap'n Proto message layer
- **Systems programming** — kernel interfaces, network programming, memory management
- **Distributed systems** — consensus, gossip protocols, consistency models
- **Language design** — the Python-shaped frontend that compiles to distributed IR
- **Security** — Ed25519 signing infrastructure, trust repository, key management
- **Testing** — distributed systems are hard to test well; good ideas welcome

If any of that sounds interesting, open an issue and say hello. Early stage
means your ideas can actually shape the architecture.

Please read [DESIGN.md](docs/DESIGN.md) before diving in — the design is
detailed enough that understanding it first will save everyone time.

---

## Prior Art & Inspiration

- **Amoeba OS** (Tanenbaum et al., 1990s) — the inspiration and the cautionary tale
- **Plan 9** — everything is a file, taken seriously
- **Inferno / Limbo** — distributed execution done differently
- **Erlang/OTP** — actor model, let it crash, the good parts
- **Cilk** — implicit parallelism done right for C
- **Kademlia DHT** — bootstrap and gossip ideas
- **Cap'n Proto** — wire format (Kenton Varda, ex-Protocol Buffers author)

---

## License

MIT — see [LICENSE](LICENSE)

---

*Named for Amoeba OS, with the 'tu' suffix because it's what Amoeba
was trying to become.*

*First AI contributor: Claude (Anthropic) — architecture, code review,
and security hardening, June–July 2026.*
