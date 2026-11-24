/**
 * @file costmodel.hpp
 * @brief How expensive is a call, and is it worth a thread? (the local half
 *        of the adaptive scheduler)
 *
 * Four rolling measurements, all taken from ordinary execution (no
 * calibration runs, so no wasted cycles on a busy machine):
 *
 * | Measurement | What it is | Depends on |
 * |---|---|---|
 * | **Work** per function | operations executed per call (statements and expressions evaluated, including work it dispatched) | the code and its input only: same on every machine, under any load |
 * | **Rate** | operations per nanosecond, from recent calls | the machine *and* its current load |
 * | **Submit cost** | CPU time the submitting thread spends handing a task to the pool (queue lock, push, waking a worker) | the machine, lock contention |
 * | **Wake latency** | time from submit until an idle worker starts the task | the OS scheduler, sleep depth, load |
 *
 * Submit cost and wake latency are different kinds of cost. Submit cost is
 * overhead: the submitter pays it and does nothing useful meanwhile. Wake
 * latency is only delay: the submitter keeps working while the worker wakes,
 * so it matters only when the submitter will soon wait for the result.
 *
 * - **A single call** (`x = f()` overlapped with later statements) is dispatched
 *   if its predicted time (work ÷ rate) is at least kCallFactor × (submit +
 *   wake). The caller may reach the use of `x` and have to wait, then be woken
 *   itself, so both count.
 * - **A parallel map** splits into about kBatchesPerThread batches per thread,
 *   for load balance (items' cost varies). Each batch must be at least
 *   kBatchFactor × submit, so overhead stays small, and at least one wake
 *   latency, so a worker isn't woken for less work than its wakeup. The
 *   caller runs the last batch itself, so it never just waits for wakeups.
 *
 * Wake latency is bimodal: a worker that just went idle starts a task in a few
 * µs, one asleep in the kernel takes tens of µs. A median flips between the two
 * modes. A trimmed mean (largest sample dropped) over 32 samples reflects how
 * often each happens, and moves smoothly as the mix changes.
 *
 * Work counts are portable. They're what to cache across program runs, and
 * what to send to another node, which can predict its own time from its own
 * advertised rate (LANGUAGE.md §2.2, capacity placement).
 *
 * All windows are small fixed-size rings (8 samples per function, as
 * LANGUAGE.md open decision 2 suggests 5–10), guarded by a mutex. Samples are
 * taken once per call or submit, so the lock is never hot.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dolphin {

struct Def;

/// @brief Fixed-size ring of (a, b) samples with running sums. Thread-safe.
class RollingWindow {
public:
    explicit RollingWindow(size_t capacity) : buf_(capacity) {}
    /// @brief Add a sample, evicting the oldest once full.
    void add(double a, double b);
    /// @brief Sums of a and b over the samples currently held, and their count.
    struct Totals { double a = 0, b = 0; size_t n = 0; };
    Totals totals() const;
    /// @brief Mean of the a values with the largest one dropped (if there are
    ///        at least 4), or @p empty if there are none. Robust to one stall.
    double trimmedMeanA(double empty) const;

private:
    mutable std::mutex m_;
    std::vector<std::pair<double, double>> buf_;
    size_t next_ = 0, count_ = 0;
    double sumA_ = 0, sumB_ = 0;
};

/**
 * @brief Rolling work/rate/submit/wake measurements and the dispatch thresholds derived from them.
 */
class CostModel {
public:
    static constexpr size_t kFnWindow = 8;              ///< Samples kept per function.
    static constexpr size_t kRateWindow = 32;           ///< Samples kept for the rate (shared by all functions).
    static constexpr size_t kPoolWindow = 32;           ///< Samples kept for submit cost and wake latency.
    static constexpr double kCallFactor = 4.0;          ///< A single call must cost this many (submit + wake).
    static constexpr double kBatchFactor = 16.0;        ///< A batch must cost at least this many submits.
    static constexpr unsigned kBatchesPerThread = 4;    ///< Parallel-map batches per thread, for load balance.
    static constexpr double kDefaultSubmitNs = 2e3;     ///< Before any submit has been measured.
    static constexpr double kDefaultWakeNs = 12.5e3;    ///< Before any wakeup has been measured.
    static constexpr uint64_t kProbeEvery = 64;         ///< Inline decisions without a fresh wake sample before probing.
    static constexpr double kFixedCallNs = 50e3;        ///< `--fixed-thresholds`: single-call threshold.
    static constexpr double kFixedBatchNs = 250e3;      ///< `--fixed-thresholds`: batch target.

    /**
     * @brief Use the old constant thresholds instead of measured ones (`--fixed-thresholds`).
     *
     * For A/B comparison: predictions become each function's average measured
     * time (no ops ÷ rate), calls are dispatched at ≥ kFixedCallNs, batches
     * target kFixedBatchNs, and there are no probes. Exactly the behaviour
     * before the cost model. Set before execution starts.
     */
    void setFixed(bool on) { fixed_ = on; }
    bool fixed() const { return fixed_; }

    /// @brief Start tracking @p d (parallel-safe functions only). Call before execution starts.
    void track(const Def* d);
    /// @brief Is @p d measured (i.e. parallel-safe)?
    bool tracks(const Def* d) const { return fn_.count(d) != 0; }

    /**
     * @brief Record one completed call of @p d.
     * @param d      The function (ignored unless tracked).
     * @param ops    Operations it executed, including work it dispatched and joined.
     * @param ns     Wall time.
     * @param clean  True if the call never blocked waiting for another thread.
     *               Only clean calls are rate samples: a call that sat waiting
     *               would make the machine look slower than it is.
     */
    void recordCall(const Def* d, uint64_t ops, uint64_t ns, bool clean);
    /// @brief Record the submitter's time for one accepted ThreadPool::submit().
    void recordSubmit(uint64_t ns);
    /// @brief Record how long a task waited before an idle worker started it.
    void recordWake(uint64_t ns);

    /// @brief Average operations per call of @p d, or -1 if never measured.
    double avgOps(const Def* d) const;
    /// @brief Current execution rate in operations per nanosecond, or -1 if unknown.
    double rate() const;
    /// @brief Submit cost (trimmed mean), or the default if none measured yet.
    double submitNs() const;
    /// @brief Wake latency (trimmed mean), or the default if none measured yet.
    double wakeNs() const;
    /// @brief Predicted wall time of one call of @p d: avgOps ÷ rate (or its measured
    ///        average time if no rate sample exists yet). -1 if never measured.
    double predictNs(const Def* d) const;
    /// @brief A single call is worth a thread if predicted to take at least this long.
    double callThresholdNs() const { return fixed_ ? kFixedCallNs : kCallFactor * (submitNs() + wakeNs()); }
    /// @brief Smallest worthwhile parallel-map batch.
    double minBatchNs() const;
    /**
     * @brief Should this call, which the threshold says to run inline, be dispatched anyway?
     *
     * Wake latency is only measured when something is dispatched. If a busy
     * spell pushed the threshold above every call, nothing would be dispatched,
     * so it would never be re-measured and the threshold would stay high after
     * the load went away. A call worth at least one wake latency is sent as a
     * probe once kProbeEvery inline decisions have gone by without a new
     * sample. Costs at most one mediocre dispatch per 64 decisions.
     */
    bool shouldProbe(double predictedNs);

private:
    std::unordered_map<const Def*, std::unique_ptr<RollingWindow>> fn_;   ///< (ops, ns) per function; map fixed after setup.
    RollingWindow rate_{kRateWindow};                                     ///< (ops, ns) from clean calls.
    RollingWindow submit_{kPoolWindow};                                   ///< (ns, unused).
    RollingWindow wake_{kPoolWindow};                                     ///< (ns, unused).
    std::atomic<uint64_t> sinceWake_{0};                                  ///< Inline decisions since the last wake sample.
    bool fixed_ = false;                                                  ///< `--fixed-thresholds`.
};

} // namespace dolphin
