/**
 * @file listener.cpp
 * @brief Listener: see listener.hpp.
 *
 * Threads:
 *  - the **poll thread** runs Poller::wait(): accepts on every listening
 *    socket (Acceptor::onReady) and wakes up for the Waker pipe (stop,
 *    resume accepting) and the signal pipe (SIGINT / SIGTERM);
 *  - **workers** (a pool, or one detached thread per connection) wrap each
 *    connection in a Stream and call the handler;
 *  - whoever calls stop() or wait() runs shutdown(): join the poll thread,
 *    wait for in-flight connections (cut off after the drain timeout), join
 *    the pool.
 */
#include "listener.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>

namespace dolphin {

thread_local Listener* Listener::tlWorkerOf_ = nullptr;

namespace {

std::string sysError(const std::string& what) { return what + ": " + std::strerror(errno); }

bool setNonBlocking(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    return fl >= 0 && ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

/// A non-blocking, close-on-exec pipe.
bool makePipe(int fds[2]) {
#if defined(__linux__)
    return ::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0;
#else
    return ::pipe(fds) == 0 && setNonBlocking(fds[0]) && setNonBlocking(fds[1]);
#endif
}

/// "tcp://1.2.3.4:5678" / "tcp://[::1]:5678" for an accepted peer.
std::string peerName(const sockaddr_storage& ss) {
    char host[INET6_ADDRSTRLEN] = "?";
    int port = 0;
    if (ss.ss_family == AF_INET) {
        auto& a = reinterpret_cast<const sockaddr_in&>(ss);
        ::inet_ntop(AF_INET, &a.sin_addr, host, sizeof host);
        port = ntohs(a.sin_port);
        return std::string("tcp://") + host + ":" + std::to_string(port);
    }
    auto& a = reinterpret_cast<const sockaddr_in6&>(ss);
    if (IN6_IS_ADDR_V4MAPPED(&a.sin6_addr)) {   // a v4 client on a dual-stack socket: show it as v4
        in_addr v4;
        std::memcpy(&v4, &a.sin6_addr.s6_addr[12], 4);
        ::inet_ntop(AF_INET, &v4, host, sizeof host);
        return std::string("tcp://") + host + ":" + std::to_string(ntohs(a.sin6_port));
    }
    ::inet_ntop(AF_INET6, &a.sin6_addr, host, sizeof host);
    return std::string("tcp://[") + host + "]:" + std::to_string(ntohs(a.sin6_port));
}

// ---- registry and signals
//
// SIGINT / SIGTERM write a byte to one process-wide pipe, which every running
// listener's poller watches; whichever poll thread sees it first stops them
// all. The previous handlers come back when the last listener stops.

std::mutex gRegM;
std::set<Listener*> gRunning;
int gSigPipe[2] = {-1, -1};
struct sigaction gOldInt, gOldTerm;

void onSignal(int) {
    int saved = errno;
    char c = 1;
    [[maybe_unused]] ssize_t n = ::write(gSigPipe[1], &c, 1);
    errno = saved;
}

struct SignalWatcher final : IoHandler {
    void onReady(unsigned) override {
        char buf[64];
        bool got = false;
        while (::read(gSigPipe[0], buf, sizeof buf) > 0) got = true;
        if (!got) return;   // another listener's poll thread got there first
        std::vector<Listener*> all;
        {
            std::lock_guard<std::mutex> lk(gRegM);
            all.assign(gRunning.begin(), gRunning.end());
        }
        for (Listener* l : all) l->requestStop();   // on a poll thread: never wait here; wait() / exit shut down
    }
};
SignalWatcher gSignalWatcher;

/// Register @p l; the first running listener installs the signal handlers.
bool registerRunning(Listener* l, std::string& err) {
    std::lock_guard<std::mutex> lk(gRegM);
    if (gSigPipe[0] < 0 && !makePipe(gSigPipe)) {
        err = sysError("signal pipe");
        return false;
    }
    if (gRunning.empty()) {
        struct sigaction sa{};
        sa.sa_handler = onSignal;
        sigemptyset(&sa.sa_mask);
        ::sigaction(SIGINT, &sa, &gOldInt);
        ::sigaction(SIGTERM, &sa, &gOldTerm);
        ::signal(SIGPIPE, SIG_IGN);   // a client hanging up must not kill the server
    }
    gRunning.insert(l);
    return true;
}

void unregisterRunning(Listener* l) {
    std::lock_guard<std::mutex> lk(gRegM);
    if (gRunning.erase(l) && gRunning.empty()) {
        ::sigaction(SIGINT, &gOldInt, nullptr);
        ::sigaction(SIGTERM, &gOldTerm, nullptr);
    }
}

} // namespace

// ------------------------------------------------------------------ waker

void Listener::Waker::onReady(unsigned) {
    char buf[64];
    while (::read(rd, buf, sizeof buf) > 0) {
    }
}

void Listener::Waker::poke() const {
    char c = 1;
    [[maybe_unused]] ssize_t n = ::write(wr, &c, 1);   // full pipe = already poked
}

// ------------------------------------------------------------------ configuration

Listener::Listener(int port) : defaultPort_(port) {}

Listener::Listener(const std::string& target) {
    Endpoint ep;
    if (parseEndpoint(target, ep)) endpoints_.push_back(ep);   // else error_ is set; start() reports it
    else endpoints_.push_back(Endpoint{Stream::Kind::File, "", 0, ""});   // marker: start() fails with error_
}

Listener::~Listener() {
    if (started_) {
        stopping_ = true;
        if (tlWorkerOf_ != this) shutdown();
    }
}

bool Listener::configurable() {
    if (!started_) return true;
    error_ = "a Listener can only be configured before start()";
    return false;
}

bool Listener::parseEndpoint(const std::string& addr, Endpoint& ep) {
    if (addr.compare(0, 7, "unix://") == 0) {
        ep.kind = Stream::Kind::Unix;
        ep.path = addr.substr(7);
        if (ep.path.empty()) return error_ = "unix:// needs a path", false;
        return true;
    }
    ep.kind = Stream::Kind::Tcp;
    std::string hp = addr.compare(0, 6, "tcp://") == 0 ? addr.substr(6) : addr;
    if (addr.compare(0, 6, "tcp://") != 0) {   // a bare host or IP: the constructor's port
        ep.host = hp;
        ep.port = defaultPort_;
        if (hp.size() > 2 && hp.front() == '[' && hp.back() == ']') ep.host = hp.substr(1, hp.size() - 2);
        return true;
    }
    size_t colon = hp.rfind(':');
    if (colon == std::string::npos) return error_ = "tcp address needs host:port: " + addr, false;
    ep.host = hp.substr(0, colon);
    if (ep.host.size() > 2 && ep.host.front() == '[' && ep.host.back() == ']') ep.host = ep.host.substr(1, ep.host.size() - 2);
    if (ep.host == "*" || ep.host == "0.0.0.0" || ep.host == "::") ep.host.clear();
    char* end = nullptr;
    long p = std::strtol(hp.c_str() + colon + 1, &end, 10);
    if (!end || *end || p < 0 || p > 65535) return error_ = "bad port in " + addr, false;
    ep.port = static_cast<int>(p);
    return true;
}

bool Listener::bind(const std::string& addr) {
    if (!configurable()) return false;
    Endpoint ep;
    if (!parseEndpoint(addr, ep)) return false;
    endpoints_.push_back(ep);
    return true;
}

bool Listener::setThreads(unsigned n) {
    if (!configurable()) return false;
    threads_ = n;
    perConnection_ = false;
    return true;
}

bool Listener::setPerConnection() {
    if (!configurable()) return false;
    perConnection_ = true;
    return true;
}

bool Listener::setLimit(unsigned n) {
    if (!configurable()) return false;
    limit_ = n;
    return true;
}

bool Listener::setTimeout(double s) {
    if (!configurable()) return false;
    timeout_ = s;
    return true;
}

bool Listener::setDrain(double s) {
    if (!configurable()) return false;
    drain_ = s;
    return true;
}

std::string Listener::describe() const {
    std::string o;
    for (auto& a : acceptors_) {
        if (!o.empty()) o += ", ";
        if (a->kind == Stream::Kind::Unix) {
            o += "unix://" + a->path;
            continue;
        }
        sockaddr_storage ss{};
        socklen_t len = sizeof ss;
        if (::getsockname(a->fd, reinterpret_cast<sockaddr*>(&ss), &len) == 0) o += peerName(ss);
    }
    return o;
}

// ------------------------------------------------------------------ starting

bool Listener::openEndpoint(const Endpoint& ep) {
    int fd = -1;
    if (ep.kind == Stream::Kind::Unix) {
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (ep.path.size() >= sizeof sa.sun_path) return error_ = "unix socket path too long: " + ep.path, false;
        std::memcpy(sa.sun_path, ep.path.c_str(), ep.path.size() + 1);
        // A socket file left by a server that died: remove it, but only if nobody answers on it.
        struct stat st;
        if (::stat(ep.path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
            int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
            bool live = probe >= 0 && ::connect(probe, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0;
            if (probe >= 0) ::close(probe);
            if (live) return error_ = "unix://" + ep.path + ": another server is listening there", false;
            ::unlink(ep.path.c_str());
        }
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return error_ = sysError("socket"), false;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
            error_ = sysError("unix://" + ep.path);
            ::close(fd);
            return false;
        }
    } else {
        auto bindTcp = [&](const sockaddr* sa, socklen_t len, int family) -> bool {
            fd = ::socket(family, SOCK_STREAM, 0);
            if (fd < 0) return false;
            int one = 1, zero = 0;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (family == AF_INET6 && ep.host.empty()) ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
            if (::bind(fd, sa, len) == 0) return true;
            int e = errno;
            ::close(fd);
            fd = -1;
            errno = e;
            return false;
        };
        bool ok = false;
        std::string where = (ep.host.empty() ? std::string("*") : ep.host) + ":" + std::to_string(ep.port);
        if (ep.host.empty()) {   // all addresses: dual-stack IPv6, or IPv4 where there's no IPv6
            sockaddr_in6 s6{};
            s6.sin6_family = AF_INET6;
            s6.sin6_addr = in6addr_any;
            s6.sin6_port = htons(static_cast<uint16_t>(ep.port));
            ok = bindTcp(reinterpret_cast<sockaddr*>(&s6), sizeof s6, AF_INET6);
            if (!ok && (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT || errno == EADDRNOTAVAIL)) {
                sockaddr_in s4{};
                s4.sin_family = AF_INET;
                s4.sin_addr.s_addr = htonl(INADDR_ANY);
                s4.sin_port = htons(static_cast<uint16_t>(ep.port));
                ok = bindTcp(reinterpret_cast<sockaddr*>(&s4), sizeof s4, AF_INET);
            }
        } else {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
            addrinfo* res = nullptr;
            std::string port = std::to_string(ep.port);
            if (int rc = ::getaddrinfo(ep.host.c_str(), port.c_str(), &hints, &res); rc != 0)
                return error_ = ep.host + ": " + gai_strerror(rc), false;
            for (addrinfo* a = res; a && !ok; a = a->ai_next) ok = bindTcp(a->ai_addr, a->ai_addrlen, a->ai_family);
            ::freeaddrinfo(res);
        }
        if (!ok) return error_ = sysError("tcp://" + where), false;
    }
    if (::listen(fd, SOMAXCONN) != 0 || !setNonBlocking(fd)) {
        error_ = sysError("listen");
        ::close(fd);
        return false;
    }
    acceptors_.push_back(std::make_unique<Acceptor>(this, fd, ep.kind, ep.kind == Stream::Kind::Unix ? ep.path : ""));
    if (ep.kind == Stream::Kind::Tcp && port_ == 0) {
        sockaddr_storage ss{};
        socklen_t len = sizeof ss;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) == 0)
            port_ = ntohs(ss.ss_family == AF_INET ? reinterpret_cast<sockaddr_in&>(ss).sin_port
                                                  : reinterpret_cast<sockaddr_in6&>(ss).sin6_port);
    }
    return true;
}

bool Listener::start(Handler h) {
    if (!configurable()) return false;
    if (!error_.empty()) return false;   // a bad constructor target
    auto fail = [&] {
        for (auto& a : acceptors_) {
            ::close(a->fd);
            if (!a->path.empty()) ::unlink(a->path.c_str());
        }
        acceptors_.clear();
        if (waker_.rd >= 0) ::close(waker_.rd), ::close(waker_.wr);
        waker_.rd = waker_.wr = -1;
        poller_.reset();
        port_ = 0;
        return false;
    };
    std::vector<Endpoint> eps = endpoints_;
    if (eps.empty()) eps.push_back(Endpoint{Stream::Kind::Tcp, "", defaultPort_, ""});
    for (auto& ep : eps)
        if (!openEndpoint(ep)) return fail();
    try {
        poller_ = std::make_unique<Poller>();
    } catch (const std::exception& e) {
        error_ = e.what();
        return fail();
    }
    int p[2];
    if (!makePipe(p)) {
        error_ = sysError("pipe");
        return fail();
    }
    waker_.rd = p[0];
    waker_.wr = p[1];
    if (!registerRunning(this, error_)) return fail();
    bool ok = poller_->add(waker_.rd, &waker_, Poller::In) && poller_->add(gSigPipe[0], &gSignalWatcher, Poller::In);
    for (auto& a : acceptors_) ok = ok && poller_->add(a->fd, a.get(), Poller::In);
    if (!ok) {
        error_ = sysError("poller");
        unregisterRunning(this);
        return fail();
    }

    handler_ = std::move(h);
    started_ = true;
    if (!perConnection_) {
        for (unsigned i = 0; i < threads_; ++i)
            workers_.emplace_back([this] {
                tlWorkerOf_ = this;
                for (;;) {
                    Conn c;
                    {
                        std::unique_lock<std::mutex> lk(m_);
                        work_.wait(lk, [&] { return !queue_.empty() || stopping_; });
                        if (queue_.empty()) return;   // stopping, and everything accepted has been served
                        c = queue_.front();
                        queue_.pop_front();
                    }
                    serve(c);
                }
            });
    }
    pollThread_ = std::thread([this] { pollLoop(); });
    return true;
}

// ------------------------------------------------------------------ running

void Listener::pollLoop() {
    while (!stopping_) {
        // Paused for running out of descriptors: retry every 100 ms, in case nothing finishes.
        if (poller_->wait(accepting_ ? -1 : 100) < 0) break;
        if (!accepting_ && active_ < limit_ && !stopping_) setAccepting(true);
    }
    for (auto& a : acceptors_) {   // stop accepting: new clients now get "connection refused"
        poller_->remove(a->fd);
        ::close(a->fd);
        if (!a->path.empty()) ::unlink(a->path.c_str());
    }
}

void Listener::setAccepting(bool on) {
    accepting_ = on;
    for (auto& a : acceptors_) poller_->modify(a->fd, a.get(), on ? unsigned{Poller::In} : 0u);
}

void Listener::acceptFrom(Acceptor& a) {
    while (!stopping_) {
        if (active_ >= limit_) {   // full: leave the rest in the backlog until a connection finishes
            setAccepting(false);
            return;
        }
        sockaddr_storage ss{};
        socklen_t len = sizeof ss;
#if defined(__linux__)
        int fd = ::accept4(a.fd, reinterpret_cast<sockaddr*>(&ss), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        int fd = ::accept(a.fd, reinterpret_cast<sockaddr*>(&ss), &len);
        if (fd >= 0) setNonBlocking(fd);
#endif
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                std::cerr << "Listener: " << std::strerror(errno) << "; pausing accept\n";
                setAccepting(false);   // pollLoop retries every 100 ms
            }
            return;   // EAGAIN: nothing more waiting
        }
#if defined(__APPLE__)
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        std::string peer = a.kind == Stream::Kind::Unix ? "unix://" + a.path : peerName(ss);
        ++active_;
        dispatch(Conn{fd, std::move(peer), a.kind});
    }
}

void Listener::dispatch(Conn c) {
    {
        std::lock_guard<std::mutex> lk(m_);
        inFlight_.insert(c.fd);
    }
    if (perConnection_) {
        try {
            std::thread([this, c] {
                tlWorkerOf_ = this;
                serve(c);
            }).detach();
        } catch (const std::system_error& e) {   // out of threads: drop this one, keep serving
            std::cerr << "Listener: cannot start a thread: " << e.what() << "\n";
            {
                std::lock_guard<std::mutex> lk(m_);
                inFlight_.erase(c.fd);
            }
            ::close(c.fd);
            finished();
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        queue_.push_back(std::move(c));
    }
    work_.notify_one();
}

void Listener::serve(Conn c) {
    {   // every Dolphin value made for this connection dies in this scope, before finished()
        auto st = std::make_shared<Stream>(c.fd, c.kind, c.peer);
        st->setTimeout(timeout_);
        try {
            handler_(st);
        } catch (const std::exception& e) {   // the handler binding reports Dolphin errors itself
            std::cerr << "Listener: handler for " << c.peer << " failed: " << e.what() << "\n";
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            inFlight_.erase(c.fd);   // before closing: after this the number may be reused
        }
        st->close();
    }
    finished();
}

void Listener::finished() {
    std::lock_guard<std::mutex> lk(m_);
    unsigned left = --active_;
    ++served_;
    if (left + 1 == limit_ && !stopping_) waker_.poke();   // was full: let the poll thread resume accepting
    idle_.notify_all();
}

// ------------------------------------------------------------------ stopping

void Listener::requestStop() {
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        stopping_ = true;
    }
    waker_.poke();
    work_.notify_all();
    idle_.notify_all();
}

void Listener::stop() {
    requestStop();
    if (tlWorkerOf_ == this) return;   // a handler can't wait for itself; wait() or exit finishes the job
    if (pollThread_.joinable() && pollThread_.get_id() == std::this_thread::get_id()) return;   // signal watcher
    shutdown();
}

void Listener::wait() {
    if (!started_) return;
    {
        std::unique_lock<std::mutex> lk(m_);
        idle_.wait(lk, [&] { return stopping_.load(); });
    }
    shutdown();
}

void Listener::shutdown() {
    if (!started_) return;
    {
        std::unique_lock<std::mutex> lk(m_);
        if (shutDown_) return;
        shutDown_ = true;
    }
    requestStop();
    if (pollThread_.joinable()) pollThread_.join();   // no more accepts; listening sockets closed

    std::unique_lock<std::mutex> lk(m_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(drain_);
    if (!idle_.wait_until(lk, deadline, [&] { return active_ == 0; })) {
        std::cerr << "Listener: " << active_ << " connection(s) still open after " << drain_ << " s; closing them\n";
        // Their reads and writes now fail, so handlers return; queued ones fail at once.
        // A handler that ignores those errors and loops forever still blocks shutdown.
        for (int fd : inFlight_) ::shutdown(fd, SHUT_RDWR);
        idle_.wait(lk, [&] { return active_ == 0; });
    }
    lk.unlock();
    work_.notify_all();
    for (auto& t : workers_) t.join();
    workers_.clear();
    unregisterRunning(this);
    poller_.reset();
    ::close(waker_.rd);
    ::close(waker_.wr);
    waker_.rd = waker_.wr = -1;
}

void Listener::stopAll() {
    std::vector<Listener*> all;
    {
        std::lock_guard<std::mutex> lk(gRegM);
        all.assign(gRunning.begin(), gRunning.end());
    }
    for (Listener* l : all) l->requestStop();
    for (Listener* l : all) l->shutdown();
}

} // namespace dolphin
