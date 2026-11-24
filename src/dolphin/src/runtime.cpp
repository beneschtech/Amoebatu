/**
 * @file runtime.cpp
 * @brief Thread pool and futures. See runtime.hpp.
 */
#include "runtime.hpp"

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

// Queue admission: a few tasks per worker keeps every worker fed without
// building a backlog that only adds latency. Beyond that, inline is cheaper.
ThreadPool::ThreadPool(unsigned workers) : maxQueue_(static_cast<size_t>(workers) * 4) {
    threads_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) threads_.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

bool ThreadPool::submit(std::function<void()> task, const Future* key) {
    auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_);
        if (threads_.empty() || queue_.size() >= maxQueue_) {
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

void ThreadPool::workerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(m_);
            ++idle_;
            cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            --idle_;
            if (stop_ && queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        if (task.timed && wakeSink_)
            wakeSink_(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - task.submitted).count()));
        stats_.byWorkers.fetch_add(1, std::memory_order_relaxed);
        task.fn();
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
