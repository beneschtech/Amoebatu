/**
 * @file poller.hpp
 * @brief Readiness notification for sockets, dispatching to object pointers.
 *
 * Each registered socket carries an IoHandler pointer: epoll keeps it in
 * `epoll_event.data.ptr`, kqueue in `kevent.udata`. wait() calls the handler
 * of every socket that's ready, so no lookup table is needed.
 *
 * Backends: epoll (Linux), kqueue (FreeBSD, macOS). The kqueue backend is
 * written but untested. Windows needs a third one: WSAPoll keeps this same
 * readiness model; IOCP would be faster but reports completions instead.
 */
#pragma once

namespace dolphin {

/// @brief Something that wants to hear when a socket is ready.
struct IoHandler {
    virtual ~IoHandler() = default;
    /// @brief Called from Poller::wait() on the polling thread. @p events: Poller::In / Out / Hup.
    virtual void onReady(unsigned events) = 0;
};

class Poller {
public:
    enum : unsigned { In = 1, Out = 2, Hup = 4 };

    Poller();    ///< Throws std::runtime_error if the kernel object can't be created.
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    /// @brief Watch @p fd for @p events (In / Out), calling @p h when ready. False on failure (errno set).
    bool add(int fd, IoHandler* h, unsigned events);
    /// @brief Change what @p fd is watched for; 0 pauses it without removing it.
    bool modify(int fd, IoHandler* h, unsigned events);
    /// @brief Stop watching @p fd. Call before closing it.
    void remove(int fd);
    /// @brief Wait up to @p timeoutMs (-1 = forever) and dispatch. Returns how many handlers ran, -1 on error.
    int wait(int timeoutMs);

private:
    int fd_ = -1;   ///< The epoll or kqueue descriptor.
};

} // namespace dolphin
