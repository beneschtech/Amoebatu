/**
 * @file topology.hpp
 * @brief What CPUs this process may use, and how many threads to start with.
 *
 * Read once, when the pool is created: a handful of small sysfs/procfs reads
 * (microseconds), no measurement runs. Everything learned about how many
 * workers actually help comes later, from running tasks (ThreadPool's
 * adaptive controller). Topology only sets the hard limit and the starting
 * point.
 *
 * | Machine | Starting threads |
 * |---|---|
 * | Intel hybrid (`/sys/devices/cpu_core` exists) | P-core hardware threads |
 * | Asymmetric ARM (`cpu_capacity` differs between CPUs) | CPUs in the fastest group |
 * | Everything else (Xeon, EPYC, Opteron, ...) | one per physical core |
 *
 * The hard limit is the CPUs in our affinity mask, further capped by a cgroup
 * v2 CPU quota (`cpu.max`), so Docker, systemd and `taskset` limits are
 * respected. Whether SMT siblings, E-cores or a second NUMA node actually add
 * throughput is not guessed here: the controller measures it.
 */
#pragma once
#include <string>
#include <vector>

namespace dolphin {

/// @brief CPU topology facts that matter for sizing the thread pool.
struct Topology {
    unsigned hardLimit = 1;   ///< Threads we may run at once (affinity ∩ cgroup quota), ≥ 1.
    unsigned start = 1;       ///< Suggested starting thread count, including the main thread.
    std::string basis;        ///< How @c start was chosen, for `--stats` ("P-core threads", ...).

    /// @brief Read the topology of the machine and this process's limits.
    static Topology detect();
    /// @brief Parse a Linux CPU list such as `0-11,14`. Malformed parts are skipped.
    static std::vector<int> parseCpuList(const std::string& s);
};

} // namespace dolphin
