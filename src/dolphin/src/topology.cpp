/**
 * @file topology.cpp
 * @brief CPU topology detection. See topology.hpp.
 */
#include "topology.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include <sched.h>

namespace dolphin {

/// @brief First line of a small text file, or empty if it can't be read.
static std::string readLine(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    if (f) std::getline(f, s);
    return s;
}

std::vector<int> Topology::parseCpuList(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        try {
            auto dash = part.find('-');
            int lo = std::stoi(part.substr(0, dash));
            int hi = dash == std::string::npos ? lo : std::stoi(part.substr(dash + 1));
            for (int c = lo; c <= hi && c - lo < 4096; ++c) out.push_back(c);
        } catch (...) {   // stoi on garbage: skip that part
        }
    }
    return out;
}

/// @brief CPUs in this process's affinity mask (all online CPUs if it can't be read).
static std::vector<int> allowedCpus() {
    std::vector<int> cpus;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        for (int c = 0; c < CPU_SETSIZE; ++c)
            if (CPU_ISSET(c, &set)) cpus.push_back(c);
    }
    if (cpus.empty()) {
        unsigned n = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned c = 0; c < n; ++c) cpus.push_back(static_cast<int>(c));
    }
    return cpus;
}

/// @brief CPUs allowed by a cgroup v2 quota (`cpu.max` "quota period"), or 0 for no limit.
static unsigned cgroupCpuLimit() {
    // Our cgroup: the "0::/path" line of /proc/self/cgroup (cgroup v2).
    std::ifstream f("/proc/self/cgroup");
    std::string line, path;
    while (std::getline(f, line))
        if (line.rfind("0::", 0) == 0) path = line.substr(3);
    if (path.empty()) return 0;
    std::istringstream in(readLine("/sys/fs/cgroup" + path + "/cpu.max"));
    std::string quota;
    double period = 0;
    if (!(in >> quota >> period) || quota == "max" || period <= 0) return 0;
    try {
        return std::max(1u, static_cast<unsigned>(std::ceil(std::stod(quota) / period)));
    } catch (...) {
        return 0;
    }
}

Topology Topology::detect() {
    Topology t;
    std::vector<int> allowed = allowedCpus();
    std::set<int> allowedSet(allowed.begin(), allowed.end());
    t.hardLimit = static_cast<unsigned>(allowed.size());
    if (unsigned q = cgroupCpuLimit()) t.hardLimit = std::min(t.hardLimit, q);

    auto countAllowed = [&](const std::vector<int>& cpus) {
        return static_cast<unsigned>(std::count_if(cpus.begin(), cpus.end(), [&](int c) { return allowedSet.count(c); }));
    };

    // Intel hybrid: the kernel lists performance cores separately.
    std::vector<int> pcores = parseCpuList(readLine("/sys/devices/cpu_core/cpus"));
    if (unsigned n = countAllowed(pcores)) {
        t.start = n;
        t.basis = "P-core threads";
    } else {
        // Asymmetric ARM (big.LITTLE / DynamIQ): cpu_capacity differs.
        std::map<long, std::vector<int>> byCapacity;
        for (int c : allowed) {
            std::string cap = readLine("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpu_capacity");
            if (cap.empty()) { byCapacity.clear(); break; }
            try { byCapacity[std::stol(cap)].push_back(c); } catch (...) { byCapacity.clear(); break; }
        }
        if (byCapacity.size() > 1) {
            t.start = static_cast<unsigned>(byCapacity.rbegin()->second.size());
            t.basis = "fastest-core group";
        } else {
            // Symmetric: one per physical core (package, core id). SMT siblings,
            // and Bulldozer module pairs, which Linux reports the same way, are
            // left for the controller to add if they prove useful.
            std::set<std::pair<std::string, std::string>> cores;
            for (int c : allowed) {
                std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/topology/";
                cores.emplace(readLine(base + "physical_package_id"), readLine(base + "core_id"));
            }
            bool known = !cores.empty() && !(cores.size() == 1 && cores.begin()->second.empty());
            t.start = known ? static_cast<unsigned>(cores.size()) : t.hardLimit;
            t.basis = known ? "physical cores" : "allowed CPUs";
        }
    }
    t.start = std::clamp(t.start, 1u, t.hardLimit);
    return t;
}

} // namespace dolphin
