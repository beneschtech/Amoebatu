/**
 * @file poller.cpp
 * @brief Poller backends: see poller.hpp.
 */
#include "poller.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(__linux__)
#include <sys/epoll.h>
#elif defined(__FreeBSD__) || defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#include <sys/time.h>
#define DOLPHIN_KQUEUE 1
#else
#error "Poller: no backend for this platform yet (Windows: add a WSAPoll or IOCP backend)"
#endif

namespace dolphin {

namespace {
constexpr int kBatch = 64;   ///< Events taken per wait() call.
}

#if defined(__linux__)

Poller::Poller() : fd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (fd_ < 0) throw std::runtime_error(std::string("epoll_create1: ") + std::strerror(errno));
}
Poller::~Poller() {
    if (fd_ >= 0) ::close(fd_);
}

static uint32_t epollEvents(unsigned ev) {
    return (ev & Poller::In ? EPOLLIN : 0u) | (ev & Poller::Out ? EPOLLOUT : 0u);
}

bool Poller::add(int fd, IoHandler* h, unsigned events) {
    epoll_event e{};
    e.events = epollEvents(events);
    e.data.ptr = h;
    return ::epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &e) == 0;
}

bool Poller::modify(int fd, IoHandler* h, unsigned events) {
    epoll_event e{};
    e.events = epollEvents(events);
    e.data.ptr = h;
    return ::epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &e) == 0;
}

void Poller::remove(int fd) { ::epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr); }

int Poller::wait(int timeoutMs) {
    epoll_event evs[kBatch];
    int n = ::epoll_wait(fd_, evs, kBatch, timeoutMs);
    if (n < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; ++i) {
        unsigned ev = (evs[i].events & EPOLLIN ? In : 0u) | (evs[i].events & EPOLLOUT ? Out : 0u) |
                      (evs[i].events & (EPOLLHUP | EPOLLERR) ? Hup : 0u);
        static_cast<IoHandler*>(evs[i].data.ptr)->onReady(ev);
    }
    return n;
}

#elif defined(DOLPHIN_KQUEUE)

// kqueue keeps one filter per direction, so modify() adds or deletes the
// EVFILT_READ / EVFILT_WRITE entries to match. Untested on real BSD/macOS.

Poller::Poller() : fd_(::kqueue()) {
    if (fd_ < 0) throw std::runtime_error(std::string("kqueue: ") + std::strerror(errno));
}
Poller::~Poller() {
    if (fd_ >= 0) ::close(fd_);
}

bool Poller::add(int fd, IoHandler* h, unsigned events) { return modify(fd, h, events); }

bool Poller::modify(int fd, IoHandler* h, unsigned events) {
    struct kevent ch[2];
    EV_SET(&ch[0], fd, EVFILT_READ, events & In ? EV_ADD | EV_ENABLE : EV_ADD | EV_DISABLE, 0, 0, h);
    EV_SET(&ch[1], fd, EVFILT_WRITE, events & Out ? EV_ADD | EV_ENABLE : EV_ADD | EV_DISABLE, 0, 0, h);
    return ::kevent(fd_, ch, 2, nullptr, 0, nullptr) == 0;
}

void Poller::remove(int fd) {
    struct kevent ch[2];
    EV_SET(&ch[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&ch[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(fd_, ch, 2, nullptr, 0, nullptr);   // errors (filter never added) don't matter
}

int Poller::wait(int timeoutMs) {
    struct kevent evs[kBatch];
    timespec ts{timeoutMs / 1000, (timeoutMs % 1000) * 1000000L};
    int n = ::kevent(fd_, nullptr, 0, evs, kBatch, timeoutMs < 0 ? nullptr : &ts);
    if (n < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; ++i) {
        unsigned ev = (evs[i].filter == EVFILT_READ ? In : 0u) | (evs[i].filter == EVFILT_WRITE ? Out : 0u) |
                      (evs[i].flags & (EV_EOF | EV_ERROR) ? Hup : 0u);
        static_cast<IoHandler*>(evs[i].udata)->onReady(ev);
    }
    return n;
}

#endif

} // namespace dolphin
