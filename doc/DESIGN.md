# Distributed Adaptive Runtime — Design Session Notes
*Architecture inspired by Amoeba OS, evolved for modern parallel computing*

---

## Project Overview

A distributed computing OS layer built on LFS (Linux From Scratch) that enables true parallelism without strict determinism. Functions are first-class serializable objects dispatched across a cluster, with an adaptive scheduler that learns optimal placement over time through accumulated execution statistics.

**Core philosophy:**
- Fix Tanenbaum's Amoeba mistake — establish hardware capability/notification substrate *first*, then build RPC-like semantics on top
- Functions move to compute, not compute to functions
- The scheduler evolves rather than being hand-tuned
- Security through signed source, not encrypted bytecode

---

## Hardware Platform

- **Target:** Supermicro 4-pack (4U unit, 4 identical servers) — X11 generation (Skylake-SP / Purley platform)
- **NICs:** Intel X550 or X710 — drivers `ixgbe` or `i40e`
- **Storage:** NVMe per node (no RAID — individual NVMe is cleaner for distributed systems)
- **Network:** Flat same-subnet to start, cross-subnet edge cases deferred
- **IPMI:** Supermicro BMC — use `ipmitool` for headless node management

---

## OS Build

### Recommended Kernel
**Linux 6.12 LTS** — preferred over 7.1 for this project because:
- Well shaken out, patches flowing steadily
- Good LFS community documentation and known-good configs
- All required features fully mature: io_uring, eBPF, multicast, SMP
- When something goes wrong, the problem should be in *your* code, not an underdocumented kernel behavior
- Upgradeable later — the upgrade itself becomes a useful resilience test

### Critical Kernel Config Options
```
CONFIG_HZ_1000          # Fine-grained timer resolution, matters for distributed coordination latency
CONFIG_CGROUPS          # Required by systemd, useful for resource accounting later
CONFIG_BPF_SYSCALL      # eBPF for lightweight instrumentation without kernel modifications
CONFIG_NET_IPGRE        # GRE tunneling for future network flexibility
CONFIG_IP_MULTICAST     # Required for discovery broadcast
# Keep ip_tables intact — needed for isolating discovery traffic from data plane
```

### Init System
**systemd** — chosen for:
- `/etc/machine-id` provides a stable node identity anchor out of the box
- Unit dependency graph enables clean ordering of discovery and networking services at boot
- Well refined and standard

### Node Identity
Use `/etc/machine-id` (provided by systemd) as the stable identity anchor.
- Stable across reboots
- Generated on first boot
- Used as the base for `NodeId` in the protocol

### Build Strategy
Identical hardware across all 4 nodes — build once, image across all four.
Keep a per-node identity derived from `machine-id` so each node has a stable UUID before the network layer is up.

### Language / Toolchain Targets
- **Primary runtime:** C++
- **User-facing shell language:** Python-shaped syntax (see Execution Model below)
- **Build tooling / config layer:** TCL
- **Scripting:** Python

---

## Architecture Overview

### Execution Model — Distributed Dataflow with Implicit Futures

The user-facing language is a Python-syntax shell that is actually a distributed execution environment underneath. Key semantics:

```python
# Clean parallelism — both dispatch simultaneously, z blocks naturally at the point of use
x = compute_something_remote()   # dispatched to node 2
y = compute_something_else()     # dispatched to node 3
z = x + y                        # blocks here until both futures resolve

# Tight dependency — compiler/runtime learns to keep this local
# because stats will show remote dispatch + immediate branch = slow
x = compute_something_remote()
if x > 10:                       # needs x to resolve before branching
    do_thing_a()
else:
    do_thing_b()
```

**Key principle:** The performance signal teaches the scheduler. You don't need the compiler to reason about data flow dependencies explicitly — the scheduler learns patterns like "dispatch x then immediately branch on x = slow, keep local" through accumulated statistics. The runtime evolves away from bad patterns organically.

### Adaptive Scheduler

Each node maintains its own **local statistics** about where functions run fast:
- No central scheduler, no single point of failure
- Nodes accumulate evidence about which functions run faster on which nodes
- A **stochastic exploration term** (epsilon-greedy, like gradient descent with noise) prevents convergence on local optima / false bottoms
- **Decay term** on statistics so stale data doesn't permanently bias dispatch after hardware changes

```
dispatch_score(function, node) = weighted_avg(past_latencies) * recency_decay + exploration_noise
```

Two tuning knobs:
- **Decay rate** — how fast stale performance data loses influence
- **Exploration probability** — how often to try non-preferred nodes to find new pathways

Performance reputation for classes of work propagates via gossip — nodes develop institutional knowledge about what runs where over time.

### Global Variable Stratification

Three tiers, each with explicit cost:

```
Cluster Global    →  Expensive. Full consistency required. Visible everywhere.
                     Use sparingly. Ownership token model for burst writes.

Context Global    →  Node-local. Fast. No coordination cost.
                     Single machine access only. Similar to module-level globals.

Local             →  Function scope. Free. No coordination.
```

**Ownership tokens for cluster globals:** A node needing repeated writes can check out a cluster global, temporarily gaining exclusive write access (at local cost), then checks it back in. Similar to database row locking. Tokens are forcibly reclaimed on NODE_FAILED.

---

## Protocol — Message Types

Wire format: **Cap'n Proto** (zero-copy deserialization, schema enforced, excellent C++ support)

### Message 1 — Execute
Dispatch a function to a node for execution.
- Function referenced by UUID (not inline source — keeps messages small, cache does its job)
- Passes local argument values, or Future references if argument not yet resolved
- Hardware capability requirements
- References to any global variables needed
- Full return address chain for async routing without a central coordinator
- Process context (trust domain, capability scope)

### Message 2 — Function Fetch
Cache miss retrieval — worker doesn't have the function source yet.
- Pull model: worker asks when it gets a request it can't fulfill from cache
- First-time penalty is real but washes out quickly in statistics via decay
- Future optimization: speculative push (sender pre-loads function before dispatching Execute)
- Response carries full FunctionObject including source

### Message 3 — Global Variable (Fetch / Put / Modify)
Operations on cluster-scoped shared state.
- Fetch, Put, Modify
- AcquireLock / ReleaseLock (ownership token for burst writes)
- ForceRevoke (triggered by NODE_FAILED to reclaim orphaned tokens)

### Message 4 — Neighbor Discovery
Peer discovery via multicast on a known group, same subnet to start.
- Sender node ID and listen address
- Hardware capabilities and current load
- Known peer hints (gossip seed)
- **Epoch counter** for detecting stale neighbor tables without diffing full peer lists

### Message 5 — Capability / Load
Ongoing advertisement of what a node can do and how busy it is.

Two dispatch philosophies, both supported:
- **Caller-decides:** caller polls load/capability, routes locally. Fast, requires reasonably current data.
- **Callee-forwards:** node accepts and redirects if it can't handle due to load or capability mismatch.

Capability bitmap has two distinct sections:
```
hardware_capabilities  →  Absolute. Has GPU. Has display. Has specific I/O.
                           "Only the user's workstation can print to console."
current_load           →  Relative. CPU%, memory pressure, queue depth, I/O%.
                           Any node could run this — pick the least busy.
```

### Message 6 — Result
Return value (or exception) from completed execution. First-class message type.
- Correlation ID tying back to the originating Execute
- Status: OK, Exception, CapabilityMiss, Timeout, TrustViolation
- Return value or full exception traceback with origin node
- **Execution statistics** (wall time, CPU time, peak memory, cache hit) — feeds directly back into caller's scheduler weights, closing the adaptive feedback loop
- **Actual node that ran it** — may differ from dispatch target if callee-forwarded

### Message 7 — Cluster Event (OOB)
Out-of-band events processed by all nodes and forwarded to all known neighbors.
Flood propagation with deduplication (nodes track seen event IDs, drop duplicates).
On a LAN, full cluster convergence occurs in low milliseconds.

**Two priority tiers:**
```
SECURITY  (absolute first):
    KEY_REVOKED       →  flush all cache entries signed by this key immediately
    KEY_ADDED         →  new signing key registered
    TRUST_INVALIDATE  →  nuclear option, flush everything, recheck on demand

TOPOLOGY  (before normal traffic, after security):
    NODE_JOINED       →  new peer, update neighbor tables, increment epoch
    NODE_DEPARTED     →  clean self-reported shutdown
    NODE_FAILED       →  detected via missed heartbeats, triggers ForceRevoke
                         on any global variable ownership tokens held by that node
```

**Revocation propagation window:**
```
Worst case exposure = OOB propagation time + residual TTL
```
KEY_REVOKED bypasses TTL and triggers immediate local cache flush regardless.

---

## Function Object — The Core Primitive

Functions are **self-describing execution capsules** passed as objects between nodes.

**Source only — no precompiled bytecode.** Each node compiles locally using its own toolchain:
- Avoids precompiled bytecode as an attack vector
- Ensures each architecture compiles optimally for itself
- Simplifies the object format significantly

Binary resources (images etc.) carried as Base64 within the source representation.

```
FunctionObject {
    identity          // stable SHA256 hash of canonical source
                      // content-addressable — same function recognized across architectures
    canonicalSource   // source code, text only
    typeSignature     // inputs, outputs, side effect declarations
    capabilityManifest // what does this function need to run
    statsHint         // compact performance summary from origin node
    dependencies      // other FunctionObject identities this one needs
    signerKeyId       // which key signed this
    signature         // Ed25519 signature, 64 bytes
}
```

**Identity hashed from source, not bytecode** — two nodes with different architectures produce different bytecode but recognize they're talking about the same function. Nodes cache the compiled form after first compilation — Function Fetch penalty only paid once.

**Dependency graph:** Functions reference other functions by identity hash. A node missing a dependency requests it by hash. Essentially a git-like content-addressable object store for executable code.

---

## Security Model

### Core Approach
Sign source, don't encrypt it. Public keys are freely available by design — that's the point.

```
Developer writes function
  → Runtime hashes canonical source  →  FunctionId
  → Developer signs with Ed25519 private key
  → Signature + hash + version stored in Trust Repository (Postgres)
  → Any node receiving a FunctionObject verifies signature before caching or executing
  → Tampered or unsigned code never runs
  → Revoked key = anything signed by it stops working cluster-wide
```

### Why Ed25519
- Very fast to verify (microseconds, not milliseconds) — minimal overhead
- Small key and signature sizes — fits cleanly in Cap'n Proto schema
- Widely implemented, well audited

### Organic Code Distribution
Inject updated function at one node, it spreads at gossip speed:
1. Node receives FunctionObject with higher version number
2. Signature verifies against Trust Repository
3. Node replaces cached version, gossips new version advertisement to neighbors
4. Statistics carry forward with **version migration decay** — don't discard learned performance data, but discount it since behavior may have changed
5. During transition both versions may run simultaneously on different nodes — handled by version field in Result message

### Trust Repository — PostgreSQL

ACID compliant, query-heavy/write-light workload. Nodes cache trust lookups locally with a TTL. KEY_REVOKED OOB event triggers immediate cache flush regardless of TTL. Nodes that can't reach the Trust Repository **fail safe** — reject unverified functions rather than assuming valid.

```sql
CREATE TABLE signing_keys (
    key_id          UUID PRIMARY KEY,
    public_key      BYTEA NOT NULL,         -- Ed25519 public key
    owner_identity  TEXT NOT NULL,          -- human readable
    created_at      TIMESTAMPTZ NOT NULL,
    revoked_at      TIMESTAMPTZ,            -- NULL = still valid
    revoked_by      UUID REFERENCES signing_keys(key_id)
);

CREATE TABLE trust_records (
    function_identity   BYTEA PRIMARY KEY,  -- canonical source hash
    version             INTEGER NOT NULL,
    signer_key_id       UUID REFERENCES signing_keys(key_id),
    signature           BYTEA NOT NULL,
    created_at          TIMESTAMPTZ NOT NULL,
    superseded_by       BYTEA REFERENCES trust_records(function_identity),
    revoked             BOOLEAN DEFAULT FALSE
);

CREATE INDEX idx_trust_version
    ON trust_records(function_identity, version DESC);
CREATE INDEX idx_key_revocation
    ON signing_keys(revoked_at)
    WHERE revoked_at IS NOT NULL;
```

**Postgres features leveraged:**
- `LISTEN/NOTIFY` — push notification of revocations to connected nodes instantly, no polling
- Row-level security — only authorized service accounts can write trust records
- Trigger-based audit log on all trust table writes

**HA for production scale:**
- Postgres streaming replication with hot standby
- Patroni for automatic failover
- PgBouncer for connection pooling across the cluster

---

## Cap'n Proto Schema

```capnp
@0xd3a4b5c6e7f80912;

# ─── Core Identity Types ───────────────────────────────────────────

struct NodeId {
    machineId   @0 :Data;    # /etc/machine-id, 16 bytes
    epoch       @1 :UInt32;  # increments on each rejoin
}

struct FunctionId {
    sourceHash  @0 :Data;    # SHA256 of canonical source, 32 bytes
    version     @1 :UInt32;  # monotonically increasing
}

struct KeyId {
    id          @0 :Data;    # UUID, 16 bytes
}

# ─── Capability & Load ─────────────────────────────────────────────

struct HardwareCapabilities {
    hasGpu          @0 :Bool;
    hasDisplay      @1 :Bool;
    hasNvme         @2 :Bool;
    accelerators    @3 :List(Text);
    archFamily      @4 :ArchFamily;

    enum ArchFamily {
        x86_64      @0;
        aarch64     @1;
        riscv64     @2;
        unknown     @3;
    }
}

struct LoadVector {
    cpuPercent      @0 :UInt8;
    memoryPercent   @1 :UInt8;
    queueDepth      @2 :UInt32;
    ioPercent       @3 :UInt8;
}

# ─── Function Object ───────────────────────────────────────────────
# Source only — no precompiled bytecode.
# Each node compiles locally. Binary resources as Base64 in source.

struct FunctionObject {
    identity        @0 :FunctionId;
    canonicalSource @1 :Text;            # source code, text only
    capabilities    @2 :HardwareCapabilities;
    typeSignature   @3 :TypeSignature;
    statsHint       @4 :StatsHint;
    dependencies    @5 :List(FunctionId);
    signerKeyId     @6 :KeyId;
    signature       @7 :Data;            # Ed25519, 64 bytes

    struct TypeSignature {
        inputs      @0 :List(TypeDesc);
        outputs     @1 :List(TypeDesc);
        sideEffects @2 :SideEffectFlags;
    }

    struct TypeDesc {
        name        @0 :Text;
        typeTag     @1 :UInt32;
    }

    struct SideEffectFlags {
        readsGlobal     @0 :Bool;
        writesGlobal    @1 :Bool;
        hasIO           @2 :Bool;
        isIdempotent    @3 :Bool;
    }

    struct StatsHint {
        avgLatencyUs    @0 :UInt32;
        sampleCount     @1 :UInt32;
        preferredArch   @2 :HardwareCapabilities.ArchFamily;
    }
}

# ─── Global Variable ───────────────────────────────────────────────

struct GlobalVarRef {
    varId       @0 :Data;
    scope       @1 :Scope;
    ownerNode   @2 :NodeId;

    enum Scope {
        clusterGlobal   @0;   # expensive, full consistency
        contextGlobal   @1;   # node-local, fast
    }
}

# ─── Return Address Chain ──────────────────────────────────────────

struct ReturnAddress {
    node            @0 :NodeId;
    correlationId   @1 :Data;      # UUID
    callDepth       @2 :UInt32;    # cycle detection across nodes
}

# ─── Process Context ───────────────────────────────────────────────

struct ProcessContext {
    jobId           @0 :Data;
    submitterKeyId  @1 :KeyId;
    trustDomain     @2 :List(KeyId);    # valid signing keys for this job
    capabilityScope @3 :List(Text);     # what resources this job may touch
}

# ─── Top-Level Message Union ───────────────────────────────────────

struct Message {
    union {
        execute         @0 :ExecuteMsg;
        functionFetch   @1 :FunctionFetchMsg;
        globalVar       @2 :GlobalVarMsg;
        neighborDisco   @3 :NeighborDiscoMsg;
        capabilityLoad  @4 :CapabilityLoadMsg;
        result          @5 :ResultMsg;
        clusterEvent    @6 :ClusterEventMsg;
    }
}

# 1 ── Execute ──────────────────────────────────────────────────────

struct ExecuteMsg {
    functionId      @0 :FunctionId;
    arguments       @1 :List(Argument);
    globalRefs      @2 :List(GlobalVarRef);
    returnTo        @3 :ReturnAddress;
    processCtx      @4 :ProcessContext;
    requiredCaps    @5 :HardwareCapabilities;

    struct Argument {
        name        @0 :Text;
        value       @1 :Data;
        isFuture    @2 :Bool;   # true if value is a pending correlationId
    }
}

# 2 ── Function Fetch ───────────────────────────────────────────────

struct FunctionFetchMsg {
    union {
        request     @0 :Request;
        response    @1 :Response;
    }

    struct Request {
        functionId      @0 :FunctionId;
        requestingNode  @1 :NodeId;
        neededArch      @2 :HardwareCapabilities.ArchFamily;
    }

    struct Response {
        functionId  @0 :FunctionId;
        object      @1 :FunctionObject;
        notFound    @2 :Bool;
    }
}

# 3 ── Global Variable ──────────────────────────────────────────────

struct GlobalVarMsg {
    varRef          @0 :GlobalVarRef;
    operation       @1 :Operation;
    correlationId   @2 :Data;
    returnTo        @3 :ReturnAddress;
    payload         @4 :Data;           # serialized value for put/modify

    enum Operation {
        fetch       @0;
        put         @1;
        modify      @2;
        acquireLock @3;   # check out ownership token
        releaseLock @4;   # return ownership token
        forceRevoke @5;   # triggered on NODE_FAILED
    }
}

# 4 ── Neighbor Discovery ───────────────────────────────────────────

struct NeighborDiscoMsg {
    sender          @0 :NodeId;
    listenAddr      @1 :Text;
    capabilities    @2 :HardwareCapabilities;
    currentLoad     @3 :LoadVector;
    knownPeers      @4 :List(PeerHint);
    epoch           @5 :UInt32;

    struct PeerHint {
        node    @0 :NodeId;
        addr    @1 :Text;
    }
}

# 5 ── Capability / Load ────────────────────────────────────────────

struct CapabilityLoadMsg {
    sender          @0 :NodeId;
    capabilities    @1 :HardwareCapabilities;
    load            @2 :LoadVector;
    acceptingWork   @3 :Bool;
    epoch           @4 :UInt32;
}

# 6 ── Result ───────────────────────────────────────────────────────

struct ResultMsg {
    correlationId   @0 :Data;
    returnTo        @1 :ReturnAddress;
    status          @2 :Status;
    value           @3 :Data;
    exception       @4 :ExceptionInfo;
    executionStats  @5 :ExecutionStats;
    actualNode      @6 :NodeId;         # may differ if callee-forwarded
    functionVersion @7 :UInt32;         # which version actually ran

    enum Status {
        ok              @0;
        exception       @1;
        capabilityMiss  @2;   # no node could handle this
        timeout         @3;
        trustViolation  @4;   # signature check failed
    }

    struct ExceptionInfo {
        message     @0 :Text;
        traceback   @1 :List(Text);
        originNode  @2 :NodeId;
    }

    struct ExecutionStats {
        wallTimeUs      @0 :UInt32;
        cpuTimeUs       @1 :UInt32;
        memPeakKb       @2 :UInt32;
        cacheHit        @3 :Bool;     # was function already compiled/cached
    }
}

# 7 ── Cluster Event (OOB) ──────────────────────────────────────────

struct ClusterEventMsg {
    eventId     @0 :Data;           # UUID for deduplication
    eventType   @1 :EventType;
    priority    @2 :Priority;
    originator  @3 :NodeId;
    timestamp   @4 :Int64;          # unix micros
    epoch       @5 :UInt32;
    payload     @6 :EventPayload;
    signature   @7 :Data;           # all OOB events must be signed

    enum EventType {
        keyRevoked      @0;
        keyAdded        @1;
        nodeJoined      @2;
        nodeDeparted    @3;
        nodeFailed      @4;
        trustInvalidate @5;
    }

    enum Priority {
        security    @0;   # KEY_REVOKED, TRUST_INVALIDATE — absolute first
        topology    @1;   # NODE_* — before normal traffic
    }

    struct EventPayload {
        union {
            keyEvent    @0 :KeyEvent;
            nodeEvent   @1 :NodeEvent;
            invalidate  @2 :Void;     # TRUST_INVALIDATE needs no payload
        }
    }

    struct KeyEvent {
        keyId       @0 :KeyId;
        publicKey   @1 :Data;   # present on KEY_ADDED, empty on revoke
        ownerName   @2 :Text;
    }

    struct NodeEvent {
        node    @0 :NodeId;
        addr    @1 :Text;
        reason  @2 :Text;       # human readable, for logs
    }
}
```

---

## Open Design Questions (Parking Lot)

Came up during the session and deliberately deferred — good to revisit once the base layer is running:

1. **Future resolution on Execute arguments** — what does a receiving node do when `isFuture = true`? Block, subscribe, or return CapabilityMiss and let caller retry?
2. **GlobalVar conflict resolution** — last-write-wins on `modify`, or per-variable semantics?
3. **callDepth enforcement** — what is the max, and does hitting it return an exception or drop?
4. **Heterogeneous hardware** — deferred by starting with identical nodes. Capability negotiation needs design when mixing GPU nodes, workstations, Z16s etc.
5. **Cross-subnet discovery** — deferred, flat network to start. Will need seed/bootstrap node list when this grows.
6. **Speculative push** — nodes pre-loading functions before dispatching Execute to hide Function Fetch latency. Optimization for later.
7. **GlobalVar ownership token timeout** — how long before an unreturned token is considered abandoned without a NODE_FAILED event?

---

*Design session conducted June 2026. Hardware not yet in hand — design phase complete, LFS build starting.*
