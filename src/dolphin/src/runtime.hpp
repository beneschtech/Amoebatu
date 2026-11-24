/**
 * @file runtime.hpp
 * @brief Local parallel execution: a thread pool and futures (milestone M2).
 *
 * Design points:
 *
 * - **Admission by load.** submit() refuses work when the queue is already
 *   deeper than the pool can drain soon. The interpreter then runs the call
 *   inline. Under heavy load (many programs, busy cores) this degrades to
 *   plain sequential execution instead of piling up tasks and context
 *   switches. That matches the scheduling priority: run inline if busy, use a
 *   local thread if one is free, and go remote only as a last resort (M5).
 *
 * - **Help only with your own work.** A thread waiting on a future that's
 *   still queued takes that task out of the queue and runs it itself
 *   (runOwn()). If the task is already running on another thread, the waiter
 *   just waits. It never picks up unrelated work: a thread waiting for a 1 ms
 *   task that helps by running someone else's 50 ms task delays its own
 *   result by 50 ms. That's priority inversion, and a first-to-return failure
 *   rule makes it visible (the slow call "wins"). Still deadlock-free: a
 *   waiter only ever blocks on a task that's actively running somewhere, so
 *   every chain of waits ends in progress.
 *
 * - **No Dolphin knowledge.** The pool runs `std::function<void()>`; the
 *   interpreter wraps calls. A Future stores either a Value or a DolphinError.
 */
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "value.hpp"

namespace dolphin {

/**
 * @brief The eventual result of a call running on another thread.
 *
 * Written once by the task (resolve() or fail()), read by whoever waits.
 * The mutex/condition_variable pair also provides the happens-before edge
 * that makes the result's contents visible to the reader.
 */
struct Future {
    uint64_t seq = 0;   ///< Dispatch order within the dispatching task; lower = earlier in program order.
    int line = 0;       ///< Source line of the call, for error messages.

    /// @brief A tick of a process-wide clock, taken when a result or error is
    ///        stored. Lower = returned earlier. Decides "first failure to
    ///        return" (LANGUAGE.md decision 17).
    static uint64_t tick();
    /// @brief The tick at which this future completed (0 while not ready).
    uint64_t doneAt() const { return doneAt_.load(std::memory_order_acquire); }

    /// @brief Ask the task not to start. A task that hasn't started yet sees
    ///        this and fails immediately with a Cancelled error, which nobody
    ///        reports. A task already running finishes normally.
    void cancel() { cancelled_.store(true, std::memory_order_release); }
    bool cancelled() const { return cancelled_.load(std::memory_order_acquire); }

    /// Operations the task executed (for the cost model). Set by the task before
    /// it completes; taken (and zeroed) by whoever joins it, so it's counted once.
    std::atomic<uint64_t> ops{0};

    /// @brief Store the result and wake waiters.
    void resolve(Value v);
    /// @brief Store an exception and wake waiters.
    void fail(DolphinError e);
    /// @brief Has resolve() or fail() been called? Never blocks.
    bool ready() const { return done_.load(std::memory_order_acquire); }
    /// @brief Did the task fail? Only meaningful once ready().
    bool failed() const { return failed_; }
    /// @brief The result. Only valid once ready() and !failed().
    const Value& value() const { return value_; }
    /// @brief The exception. Only valid once ready() and failed().
    const DolphinError& error() const { return error_; }

    /// @brief Block until ready() for at most @p us microseconds. True if ready.
    bool waitFor(int us);

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> done_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<uint64_t> doneAt_{0};
    bool failed_ = false;
    Value value_;
    DolphinError error_;
};
using FutureP = std::shared_ptr<Future>;

/// @brief Counters for `dolphin --stats`.
struct PoolStats {
    std::atomic<uint64_t> submitted{0};   ///< Tasks accepted by submit().
    std::atomic<uint64_t> refused{0};     ///< submit() calls refused because the queue was full (caller ran inline).
    std::atomic<uint64_t> byWorkers{0};   ///< Tasks run by pool workers.
    std::atomic<uint64_t> byHelpers{0};   ///< Tasks run by the thread waiting for them (still queued when it needed them).
};

/**
 * @brief Fixed-size pool of worker threads with one shared FIFO queue.
 *
 * One queue and one mutex is deliberately simple. Per-thread deques with
 * work stealing can replace it if profiling shows queue contention.
 */
class ThreadPool {
public:
    /// @param workers  Number of worker threads (0 = never run anything in parallel).
    explicit ThreadPool(unsigned workers);
    ~ThreadPool();   ///< Drains nothing: finishes running tasks, then joins.

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief Queue a task if there's capacity.
     * @param task  The work.
     * @param key   The future the task completes, so a waiter can find it (runOwn()).
     * @return False if the pool has no workers or the queue is full; the
     *         caller should then do the work inline.
     */
    bool submit(std::function<void()> task, const Future* key);

    /// @brief If the task that completes @p key is still queued, remove it and
    ///        run it on the calling thread. True if it ran.
    bool runOwn(const Future* key);

    /// @brief Wait for @p f: run its task here if it's still queued, otherwise wait for it.
    void wait(Future& f);

    unsigned workers() const { return static_cast<unsigned>(threads_.size()); }

    /**
     * @brief Report pool timings (nanoseconds) for the cost model. Set before submitting anything.
     * @param submit  Called with the submitter's time for each accepted submit():
     *                queue lock, push and waking a worker.
     * @param wake    Called with the time from submit() until an idle worker
     *                starts the task. Only tasks submitted while a worker was
     *                idle and nothing was queued ahead of them are timed: a task
     *                that waited behind others measures queueing, not wakeup.
     */
    void setTimingSinks(std::function<void(uint64_t)> submit, std::function<void(uint64_t)> wake) {
        submitSink_ = std::move(submit);
        wakeSink_ = std::move(wake);
    }
    PoolStats& stats() { return stats_; }

private:
    void workerLoop();

    std::vector<std::thread> threads_;
    struct Task {
        std::function<void()> fn;
        const Future* key;   ///< The future this task completes.
        std::chrono::steady_clock::time_point submitted;
        bool timed;          ///< An idle worker was free for it: a clean wake-latency sample.
    };
    unsigned idle_ = 0;      ///< Workers waiting for work (guarded by m_).
    std::function<void(uint64_t)> submitSink_, wakeSink_;
    std::deque<Task> queue_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    size_t maxQueue_;   ///< Admission limit: beyond this depth, submit() refuses.
    PoolStats stats_;
};

/**
 * @brief Asynchronous, ordered output: a printer thread drains a queue.
 *
 * `print()` appends its line to a mutex-protected vector and returns; it never
 * waits on the output device. The printer thread swaps the whole vector out,
 * joins the lines into one buffer and writes it with a single fwrite +
 * fflush. So a burst of prints costs one syscall instead of one each, and
 * the program keeps computing while the terminal or pipe catches up.
 *
 * Ordering: messages are written in the order write() was called. Dolphin
 * only calls it from the main thread, in program order (print is an IO
 * effect), so output order is exactly the sequential order.
 *
 * Coalescing: once woken, the printer lingers up to kLinger (2 ms) or until
 * kBatchBytes (64 KB) are queued before writing, and the producer only
 * signals when the queue goes from empty to non-empty or crosses
 * kBatchBytes. Without this, a printer faster than the program wakes for
 * every line, so each print costs a context switch plus its own write(),
 * which is slower than writing synchronously. drain() ends the wait at once.
 *
 * Backpressure: if more than @c maxPendingBytes are queued (the program is
 * far ahead of a slow consumer), write() blocks until the printer catches up.
 * Memory stays bounded.
 *
 * drain() waits until everything queued so far has been written and flushed.
 * The interpreter calls it before anything whose order relative to stdout
 * matters: error messages on stderr, statistics, and exit.
 */
class OutputChannel {
public:
    /// @param out              Destination stream (normally stdout).
    /// @param maxPendingBytes  Backpressure threshold.
    explicit OutputChannel(std::FILE* out, size_t maxPendingBytes = 8u << 20);
    ~OutputChannel();   ///< Drains, then stops and joins the printer thread.

    OutputChannel(const OutputChannel&) = delete;
    OutputChannel& operator=(const OutputChannel&) = delete;

    /// @brief Queue @p text for output. Blocks only when the queue is over its byte limit.
    void write(std::string text);
    /// @brief Block until everything queued so far is written and flushed.
    void drain();

    uint64_t messages() const { std::lock_guard<std::mutex> lk(m_); return enqueued_; }   ///< Messages queued so far.
    uint64_t batches() const { std::lock_guard<std::mutex> lk(m_); return batches_; }      ///< fwrite calls made (one per batch).
    uint64_t stalls() const { std::lock_guard<std::mutex> lk(m_); return stalls_; }        ///< write() calls that waited for space.

private:
    void loop();

    std::FILE* out_;
    size_t maxBytes_;
    mutable std::mutex m_;
    std::condition_variable hasData_, hasSpace_, drained_;
    std::vector<std::string> pending_;   ///< The thread-safe vector: guarded by m_.
    size_t pendingBytes_ = 0;
    int drainWaiters_ = 0;    ///< Threads in drain(): the printer stops lingering when > 0.
    uint64_t enqueued_ = 0;   ///< Messages accepted (guarded by m_).
    uint64_t written_ = 0;    ///< Messages written and flushed (guarded by m_).
    uint64_t batches_ = 0;
    uint64_t stalls_ = 0;
    bool stop_ = false;
    std::thread thread_;      ///< Declared last: started after every member above exists.
};

} // namespace dolphin
