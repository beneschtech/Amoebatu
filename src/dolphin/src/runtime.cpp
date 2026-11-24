/**
 * @file runtime.cpp
 * @brief Thread pool and futures. See runtime.hpp.
 */
#include "runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <chrono>

namespace {
constexpr size_t kBatchBytes = 64 * 1024;                           // write as soon as this much is queued
constexpr std::chrono::microseconds kLinger{2000};                  // otherwise wait this long for more
}

namespace dolphin {

uint64_t Future::tick() {
    static std::atomic<uint64_t> clock{0};
    return clock.fetch_add(1, std::memory_order_relaxed) + 1;
}

void Future::resolve(Value v) {
    {
        std::lock_guard<std::mutex> lk(m_);
        value_ = std::move(v);
        doneAt_.store(tick(), std::memory_order_relaxed);
        done_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
}

void Future::fail(DolphinError e) {
    {
        std::lock_guard<std::mutex> lk(m_);
        error_ = std::move(e);
        failed_ = true;
        error_.at = tick();
        doneAt_.store(error_.at, std::memory_order_relaxed);
        done_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
}

bool Future::waitFor(int us) {
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, std::chrono::microseconds(us), [&] { return done_.load(std::memory_order_acquire); });
}

ThreadPool::ThreadPool(unsigned workers, unsigned startActive, bool adaptive)
    : samples_(workers), adaptive_(adaptive && workers > 1) {
    unsigned start = adaptive_ ? std::clamp(startActive, 1u, workers) : workers;
    active_.store(start);
    stats_.activeMin.store(start);
    stats_.activeMax.store(start);
    lastControl_ = std::chrono::steady_clock::now();
    nextControlNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(lastControl_.time_since_epoch()).count() +
                         int64_t{kControlMs} * 1000000);
    threads_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) threads_.emplace_back([this, i] { workerLoop(i); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    parkCv_.notify_all();
    for (auto& t : threads_) t.join();
}

bool ThreadPool::submit(std::function<void()> task, const Future* key) {
    auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_);
        // Queue admission: a few tasks per active worker keeps every worker fed
        // without building a backlog that only adds latency. Beyond that,
        // inline is cheaper. Parked workers don't count.
        if (threads_.empty() || queue_.size() >= static_cast<size_t>(active_.load(std::memory_order_relaxed)) * 4) {
            stats_.refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        bool timed = idle_ > queue_.size();   // a sleeping worker will take it straight away
        queue_.push_back(Task{std::move(task), key, std::chrono::steady_clock::now(), timed});
    }
    stats_.submitted.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
    if (submitSink_)
        submitSink_(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
    return true;
}

// The queue is short (admission caps it at a few tasks per worker), so a
// linear scan for the key is cheap.
bool ThreadPool::runOwn(const Future* key) {
    std::function<void()> task;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->key == key) {
                task = std::move(it->fn);
                queue_.erase(it);
                break;
            }
        }
        if (!task) return false;
    }
    stats_.byHelpers.fetch_add(1, std::memory_order_relaxed);
    task();
    return true;
}

void ThreadPool::wait(Future& f) {
    while (!f.ready()) {
        if (runOwn(&f)) continue;
        f.waitFor(200);   // running on another thread: wait for it (wakes as soon as it completes)
    }
}

void ThreadPool::workerLoop(unsigned id) {
    auto parked = [&] { return id >= active_.load(std::memory_order_relaxed); };
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(m_);
            for (;;) {
                if (stop_ && (parked() || queue_.empty())) return;   // active workers drain the queue first
                if (parked()) {
                    parkCv_.wait(lk, [&] { return stop_ || !parked(); });
                    continue;
                }
                if (!queue_.empty()) break;
                ++idle_;   // idle_ counts only active workers, so wake-latency samples stay clean
                cv_.wait(lk, [&] { return stop_ || !queue_.empty() || parked(); });
                --idle_;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        if (task.timed && wakeSink_)
            wakeSink_(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - task.submitted).count()));
        stats_.byWorkers.fetch_add(1, std::memory_order_relaxed);
        task.fn();
        if (adaptive_) {
            sampleSchedstat(id);
            maybeControl();
        }
    }
}

// /proc/thread-self/schedstat: "<ns on cpu> <ns runnable but waiting> <timeslices>".
// Needs CONFIG_SCHED_INFO, which mainstream distro kernels (Ubuntu, RHEL/Alma)
// enable. Where it's missing, contention reads as zero: the pool can still
// grow (each step checked against throughput) but never cuts for load.
void ThreadPool::sampleSchedstat(unsigned id) {
    WorkerSample& s = samples_[id];
    auto now = std::chrono::steady_clock::now();
    if (!s.ok || now - s.last < std::chrono::milliseconds(kSampleMs)) return;
    s.last = now;
    unsigned long long run = 0, wait = 0;
    std::FILE* f = std::fopen("/proc/thread-self/schedstat", "r");
    if (!f || std::fscanf(f, "%llu %llu", &run, &wait) != 2) {
        s.ok = false;
        if (f) std::fclose(f);
        return;
    }
    std::fclose(f);
    if (s.run || s.wait) {   // first read only sets the baseline
        runNs_.fetch_add(run - s.run, std::memory_order_relaxed);
        waitNs_.fetch_add(wait - s.wait, std::memory_order_relaxed);
    }
    s.run = run;
    s.wait = wait;
}

void ThreadPool::setActive(unsigned n, const std::string& why) {
    unsigned old;
    {
        std::lock_guard<std::mutex> lk(m_);
        old = active_.load(std::memory_order_relaxed);
        active_.store(n, std::memory_order_relaxed);
    }
    cv_.notify_all();      // workers now beyond the count go park
    parkCv_.notify_all();  // workers now within it wake up
    unsigned lo = stats_.activeMin.load(std::memory_order_relaxed);
    while (n < lo && !stats_.activeMin.compare_exchange_weak(lo, n)) {}
    unsigned hi = stats_.activeMax.load(std::memory_order_relaxed);
    while (n > hi && !stats_.activeMax.compare_exchange_weak(hi, n)) {}
    if (traceSink_) traceSink_("workers " + std::to_string(old) + " -> " + std::to_string(n) + " (" + why + ")");
}

void ThreadPool::maybeControl() {
    auto now = std::chrono::steady_clock::now();
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    if (nowNs < nextControlNs_.load(std::memory_order_relaxed)) return;   // cheap check, no lock
    std::unique_lock<std::mutex> cl(controlM_, std::try_to_lock);
    if (!cl.owns_lock() || now - lastControl_ < std::chrono::milliseconds(kControlMs)) return;
    double dt = std::chrono::duration<double>(now - lastControl_).count();
    lastControl_ = now;
    nextControlNs_.store(nowNs + int64_t{kControlMs} * 1000000, std::memory_order_relaxed);

    uint64_t run = runNs_.load(), wait = waitNs_.load(), ops = opsDone_.load(), refused = stats_.refused.load();
    uint64_t dRun = run - lastRun_, dWait = wait - lastWait_, dOps = ops - lastOps_, dRefused = refused - lastRefused_;
    lastRun_ = run;
    lastWait_ = wait;
    lastOps_ = ops;
    lastRefused_ = refused;

    bool haveContention = dRun + dWait >= 1000000;   // at least 1 ms of samples
    double contention = haveContention ? static_cast<double>(dWait) / static_cast<double>(dRun + dWait) : 0.0;
    if (haveContention) stats_.contentionPermille.store(static_cast<unsigned>(contention * 1000));
    bool demand = dRefused > 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        demand = demand || !queue_.empty();
    }
    double throughput = static_cast<double>(dOps) / dt;
    unsigned w = active_.load(std::memory_order_relaxed);
    char pct[48];
    std::snprintf(pct, sizeof pct, "contention %.0f%%", contention * 100);

    // Did the last addition pay off? Only judge it while there was work to do.
    if (evaluating_) {
        evaluating_ = false;
        if (demand && thrBefore_ > 0 && throughput - thrBefore_ < kKeepGain * thrBefore_ / evalFrom_) {
            ceiling_ = evalFrom_;
            ceilingUntil_ = now + std::chrono::seconds(kCeilingS);
            stats_.reverted.fetch_add(1, std::memory_order_relaxed);
            setActive(evalFrom_, "added worker didn't add throughput; ceiling for " + std::to_string(kCeilingS) + " s");
            return;
        }
    }
    if (haveContention && contention > kCutAbove && w > 1) {
        stats_.cuts.fetch_add(1, std::memory_order_relaxed);
        setActive(std::max(1u, w - (w + 3) / 4), pct);
        return;
    }
    bool capped = ceiling_ && now < ceilingUntil_ && w >= ceiling_;
    if (contention < kRaiseBelow && demand && w < workers() && !capped) {
        evaluating_ = true;
        evalFrom_ = w;
        thrBefore_ = throughput;
        stats_.raises.fetch_add(1, std::memory_order_relaxed);
        setActive(w + 1, std::string(pct) + ", work waiting");
    }
}

// ============================================================ OutputChannel

OutputChannel::OutputChannel(std::FILE* out, size_t maxPendingBytes)
    : out_(out), maxBytes_(maxPendingBytes), thread_([this] { loop(); }) {}

OutputChannel::~OutputChannel() {
    drain();
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    hasData_.notify_one();
    thread_.join();
}

void OutputChannel::write(std::string text) {
    std::unique_lock<std::mutex> lk(m_);
    // Backpressure. An oversized single message is still accepted once the
    // queue is empty, so it can't wait forever.
    if (pendingBytes_ > 0 && pendingBytes_ + text.size() > maxBytes_) {
        ++stalls_;
        hasSpace_.wait(lk, [&] { return pendingBytes_ == 0 || pendingBytes_ + text.size() <= maxBytes_; });
    }
    size_t before = pendingBytes_;
    bool wasEmpty = pending_.empty();
    pendingBytes_ += text.size();
    pending_.push_back(std::move(text));
    ++enqueued_;
    bool crossed = before < kBatchBytes && pendingBytes_ >= kBatchBytes;
    lk.unlock();
    if (wasEmpty || crossed) hasData_.notify_one();   // otherwise the printer is already awake or lingering
}

void OutputChannel::drain() {
    std::unique_lock<std::mutex> lk(m_);
    uint64_t target = enqueued_;
    if (written_ >= target) return;
    ++drainWaiters_;
    hasData_.notify_one();   // cut the printer's linger short
    drained_.wait(lk, [&] { return written_ >= target; });
    --drainWaiters_;
}

void OutputChannel::loop() {
    std::vector<std::string> batch;
    std::string buffer;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m_);
            hasData_.wait(lk, [&] { return stop_ || !pending_.empty(); });
            if (pending_.empty()) return;   // stop_ and nothing left
            // Linger: let more output accumulate so it goes out in one write.
            hasData_.wait_for(lk, kLinger, [&] { return stop_ || drainWaiters_ > 0 || pendingBytes_ >= kBatchBytes; });
            batch.swap(pending_);            // take everything queued; producers continue at once
        }
        buffer.clear();
        size_t bytes = 0;
        for (auto& s : batch) bytes += s.size();
        buffer.reserve(bytes);
        for (auto& s : batch) buffer += s;
        std::fwrite(buffer.data(), 1, buffer.size(), out_);   // one syscall for the whole batch
        std::fflush(out_);
        {
            std::lock_guard<std::mutex> lk(m_);
            written_ += batch.size();
            pendingBytes_ -= bytes;
            ++batches_;
        }
        batch.clear();
        hasSpace_.notify_all();
        drained_.notify_all();
    }
}

} // namespace dolphin
