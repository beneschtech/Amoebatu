/**
 * @file sharedprofile.cpp
 * @brief Shared machine profile. See sharedprofile.hpp.
 */
#include "sharedprofile.hpp"

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dolphin {

// Everything in the segment is a lock-free atomic, so processes coordinate
// without locks that a crashed process could leave held.
static_assert(std::atomic<uint64_t>::is_always_lock_free, "shared profile needs lock-free 64-bit atomics");
static_assert(std::atomic<int64_t>::is_always_lock_free, "shared profile needs lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "shared profile needs lock-free 32-bit atomics");

/// One process's entry; a cache line each, so processes don't share lines.
struct alignas(64) SharedProfile::Slot {
    std::atomic<uint64_t> pid;          ///< Owner, 0 = free.
    std::atomic<uint32_t> threads;      ///< Owner's active threads, including main.
    std::atomic<int64_t> heartbeatMs;   ///< steady_clock ms of the owner's last publish().
};

struct SharedProfile::Segment {
    static constexpr uint64_t kMagic = 0x444F4C5048494E31ull;   // "DOLPHIN1"
    std::atomic<uint64_t> magic;            ///< kMagic once initialized (zero-filled before).
    std::atomic<uint32_t> learnedCeiling;   ///< Most useful threads found so far, 0 = unknown.
    std::atomic<uint32_t> reserved;
    Slot slots[kSlots];
};

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// @brief Is process @p pid gone? (A pid we can't signal for permission reasons still exists.)
static bool processGone(uint64_t pid) {
    return pid == 0 || (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH);
}

SharedProfile::SharedProfile() {
    if (const char* e = std::getenv("DOLPHIN_SHM"); e && std::strcmp(e, "0") == 0) return;
    std::string name = "/dolphin-" + std::to_string(geteuid()) + "-v" + std::to_string(kVersion);
    fd_ = shm_open(name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) return;
    // /dev/shm is world-writable: someone else could have created this name
    // first. Use it only if it's ours and private.
    struct stat st{};
    if (fstat(fd_, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
        close(fd_);
        fd_ = -1;
        return;
    }
    // Growing it zero-fills; never shrink a segment another process maps.
    if (static_cast<size_t>(st.st_size) < sizeof(Segment) && ftruncate(fd_, sizeof(Segment)) != 0) {
        close(fd_);
        fd_ = -1;
        return;
    }
    void* p = mmap(nullptr, sizeof(Segment), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
        close(fd_);
        fd_ = -1;
        return;
    }
    seg_ = static_cast<Segment*>(p);   // zero-filled memory is a valid all-free segment
    uint64_t expect = 0;
    seg_->magic.compare_exchange_strong(expect, Segment::kMagic);
    if (seg_->magic.load() != Segment::kMagic) return;   // not ours after all: stay unusable

    // Claim a free slot, or one whose owner has died.
    uint64_t me = static_cast<uint64_t>(getpid());
    for (unsigned i = 0; i < kSlots && !slot_; ++i) {
        Slot& s = seg_->slots[i];
        uint64_t owner = s.pid.load();
        if (owner == me || !processGone(owner)) continue;
        if (s.pid.compare_exchange_strong(owner, me)) slot_ = &s;
    }
    if (slot_) publish(1);
}

SharedProfile::~SharedProfile() {
    if (slot_) {
        slot_->threads.store(0);
        slot_->pid.store(0);
    }
    if (seg_) munmap(seg_, sizeof(Segment));
    if (fd_ >= 0) close(fd_);
}

void SharedProfile::publish(unsigned threads) {
    if (!slot_) return;
    slot_->threads.store(threads, std::memory_order_relaxed);
    slot_->heartbeatMs.store(nowMs(), std::memory_order_relaxed);
}

SharedProfile::Others SharedProfile::others() const {
    Others o;
    if (!slot_) return o;
    int64_t now = nowMs();
    for (const Slot& s : seg_->slots) {
        if (&s == slot_ || s.pid.load(std::memory_order_relaxed) == 0) continue;
        if (now - s.heartbeatMs.load(std::memory_order_relaxed) > kStaleMs) continue;
        ++o.processes;
        o.threads += std::min<uint32_t>(s.threads.load(std::memory_order_relaxed), 4096);   // hint: clamp
    }
    return o;
}

unsigned SharedProfile::learnedCeiling() const {
    return slot_ ? std::min<uint32_t>(seg_->learnedCeiling.load(std::memory_order_relaxed), 4096) : 0;
}

void SharedProfile::recordCeiling(unsigned threads) {
    if (slot_) seg_->learnedCeiling.store(threads, std::memory_order_relaxed);
}

} // namespace dolphin
