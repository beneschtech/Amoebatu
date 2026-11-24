/**
 * @file costmodel.cpp
 * @brief Rolling cost measurements. See costmodel.hpp.
 */
#include "costmodel.hpp"

#include <algorithm>

namespace dolphin {

void RollingWindow::add(double a, double b) {
    std::lock_guard<std::mutex> lk(m_);
    if (count_ == buf_.size()) {   // full: evict the oldest
        sumA_ -= buf_[next_].first;
        sumB_ -= buf_[next_].second;
    } else {
        ++count_;
    }
    buf_[next_] = {a, b};
    sumA_ += a;
    sumB_ += b;
    next_ = (next_ + 1) % buf_.size();
}

RollingWindow::Totals RollingWindow::totals() const {
    std::lock_guard<std::mutex> lk(m_);
    return Totals{sumA_, sumB_, count_};
}

double RollingWindow::trimmedMeanA(double empty) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!count_) return empty;
    if (count_ < 4) return sumA_ / static_cast<double>(count_);
    double mx = buf_[0].first;
    for (size_t i = 1; i < count_; ++i) mx = std::max(mx, buf_[i].first);
    return (sumA_ - mx) / static_cast<double>(count_ - 1);
}

void CostModel::track(const Def* d) {
    if (!fn_.count(d)) fn_.emplace(d, std::make_unique<RollingWindow>(kFnWindow));
}

void CostModel::recordCall(const Def* d, uint64_t ops, uint64_t ns, bool clean) {
    auto it = fn_.find(d);
    if (it == fn_.end()) return;
    it->second->add(static_cast<double>(ops), static_cast<double>(ns));
    if (clean && ops > 0 && ns > 0) rate_.add(static_cast<double>(ops), static_cast<double>(ns));
}

void CostModel::recordSubmit(uint64_t ns) { submit_.add(static_cast<double>(ns), 0); }

void CostModel::recordWake(uint64_t ns) {
    wake_.add(static_cast<double>(ns), 0);
    sinceWake_.store(0, std::memory_order_relaxed);
}

bool CostModel::shouldProbe(double predictedNs) {
    if (fixed_) return false;
    if (predictedNs < wakeNs()) return false;   // too small to ever be worth it: no probe needed
    if (sinceWake_.fetch_add(1, std::memory_order_relaxed) + 1 < kProbeEvery) return false;
    sinceWake_.store(0, std::memory_order_relaxed);
    return true;
}

double CostModel::submitNs() const { return submit_.trimmedMeanA(kDefaultSubmitNs); }
double CostModel::wakeNs() const { return wake_.trimmedMeanA(kDefaultWakeNs); }
double CostModel::minBatchNs() const {
    if (fixed_) return kFixedBatchNs;
    return std::max(kBatchFactor * submitNs(), wakeNs());
}

double CostModel::avgOps(const Def* d) const {
    auto it = fn_.find(d);
    if (it == fn_.end()) return -1;
    auto t = it->second->totals();
    return t.n ? t.a / static_cast<double>(t.n) : -1;
}

// Sum of ops over sum of time across the window (not an average of ratios),
// so long calls weigh in proportion to the time they represent.
double CostModel::rate() const {
    auto t = rate_.totals();
    return t.n >= kMinRateSamples && t.b > 0 ? t.a / t.b : -1;
}


double CostModel::predictNs(const Def* d) const {
    auto it = fn_.find(d);
    if (it == fn_.end()) return -1;
    auto t = it->second->totals();
    if (!t.n) return -1;
    double r = fixed_ ? -1 : rate();
    if (r > 0) return (t.a / static_cast<double>(t.n)) / r;   // work ÷ current rate
    return t.b / static_cast<double>(t.n);                    // no rate yet: its own measured time
}

} // namespace dolphin
