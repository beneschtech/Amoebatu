/**
 * @file sharedprofile.hpp
 * @brief Per-user, per-machine scheduling profile in POSIX shared memory
 *        (PCODE.md §8b).
 *
 * Two jobs:
 *
 * 1. **Remember what earlier runs learned.** When the adaptive pool's
 *    throughput check fails (an added thread didn't help, on an uncontended
 *    machine), that limit is stored here. A later run starts no higher than
 *    it, instead of rediscovering it: no measurement at startup, and the figure
 *    survives until reboot ("cached and refreshed rarely", LANGUAGE.md open
 *    decision 2). It only ever lowers the topology's starting guess (useful
 *    where the guess is too high: NUMA, shared-unit modules, quotas); growth
 *    above the start happens within each run.
 * 2. **Let concurrent Dolphin programs see each other.** Each process holds a
 *    slot with its active thread count, refreshed at every control step. A new
 *    process starts at its fair share of the cores instead of at the full
 *    count, and a running one doesn't grow into cores the others are using.
 *    Contention (run-queue wait) still drives cutting; this only sets the
 *    starting point and a cap on growth.
 *
 * ### Safety
 * - The segment is `/dev/shm/dolphin-<uid>-v<version>`, mode 0600. `/dev/shm`
 *   is writable by everyone, so after opening, the segment is used only if it
 *   is owned by this user and accessible to no one else. Otherwise Dolphin
 *   runs without it.
 * - The version is in the name, so a different layout never reads this one.
 * - Everything read is a hint: values are clamped to the process's own limits
 *   before use. A corrupt or hostile value can at worst change how many
 *   threads run, never what the program computes.
 * - A slot whose process has died is reclaimed (`kill(pid, 0)` fails), and
 *   slots not refreshed for kStaleMs don't count toward others' threads.
 * - `DOLPHIN_SHM=0` in the environment disables it.
 */
#pragma once
#include <atomic>
#include <cstdint>

namespace dolphin {

/**
 * @brief This process's view of the shared machine profile. Not copyable.
 */
class SharedProfile {
public:
    static constexpr uint32_t kVersion = 1;    ///< Layout version (part of the segment name).
    static constexpr unsigned kSlots = 256;    ///< Concurrent Dolphin processes tracked per user.
    static constexpr int64_t kStaleMs = 2000;  ///< A slot not refreshed for this long doesn't count.

    /// @brief Open (creating if needed) the segment and claim a slot. Check ok() after.
    SharedProfile();
    ~SharedProfile();   ///< Releases the slot.
    SharedProfile(const SharedProfile&) = delete;
    SharedProfile& operator=(const SharedProfile&) = delete;

    /// @brief True if the segment is usable and this process holds a slot.
    bool ok() const { return slot_ != nullptr; }

    /// @brief Publish this process's active thread count (including main) and refresh its heartbeat.
    void publish(unsigned threads);
    /// @brief Live Dolphin processes other than this one, and their total active threads.
    struct Others { unsigned processes = 0, threads = 0; };
    Others others() const;

    /// @brief Most threads found useful on this machine by any earlier run, or 0 if unknown.
    unsigned learnedCeiling() const;
    /// @brief Record that @p threads is the most that helped (a throughput check failed above it
    ///        while the machine wasn't contended).
    void recordCeiling(unsigned threads);

private:
    struct Segment;
    struct Slot;
    Segment* seg_ = nullptr;
    Slot* slot_ = nullptr;
    int fd_ = -1;
};

} // namespace dolphin
