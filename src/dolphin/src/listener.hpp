/**
 * @file listener.hpp
 * @brief The `Listener` builtin object: a TCP / Unix-socket server.
 *
 * Configure, then start(handler). A polling thread accepts connections and
 * hands each one, as a connected Stream, to a worker thread that calls the
 * handler. stop() stops accepting, lets every accepted connection finish
 * (up to the drain timeout), then shuts down.
 *
 * | Setting             | Default                                    |
 * |---------------------|--------------------------------------------|
 * | bind addresses      | all (IPv6 `::` dual-stack, else `0.0.0.0`) |
 * | workers             | pool of kDefaultThreads threads            |
 * | connection limit    | kDefaultLimit open connections at once     |
 * | connection timeout  | Stream::kDefaultTimeout seconds            |
 * | drain timeout       | kDefaultDrain seconds                      |
 *
 * Every setting must be made before start(). Accepting is paused, not
 * refused, while the limit is reached: new clients wait in the kernel's
 * backlog.
 *
 * SIGINT and SIGTERM stop every running listener gracefully (wait() returns).
 * The C++ class knows nothing about the interpreter: the handler is a
 * std::function, which builtins.cpp binds to a Dolphin function.
 *
 * POSIX only so far: accept4/pipe2 on Linux, accept/fcntl elsewhere. Windows
 * needs a Poller backend and winsock equivalents (see poller.hpp).
 */
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "poller.hpp"
#include "stream.hpp"

namespace dolphin {

class Listener {
public:
    static constexpr unsigned kDefaultThreads = 16;    ///< Handlers mostly wait on sockets, so more than the core count.
    static constexpr unsigned kDefaultLimit = 1024;
    static constexpr double kDefaultDrain = 30.0;      ///< Seconds stop() waits for in-flight connections.

    /// @brief Called on a worker thread with each accepted connection.
    using Handler = std::function<void(std::shared_ptr<Stream>)>;

    /// @brief Listen on TCP port @p port (0 = any free port; see port()).
    explicit Listener(int port);
    /// @brief Listen on @p target: `tcp://host:port` or `unix:///path` (more can be added with bind()).
    explicit Listener(const std::string& target);
    ~Listener();   ///< stop() if still running.
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    /// @name Configuration (before start(); each returns false with error() set otherwise)
    /// @{
    /// @brief Add an address: a host/IP (with the constructor's port), `tcp://host:port`
    ///        or `unix:///path`. The first call replaces the default "all addresses".
    bool bind(const std::string& addr);
    bool setThreads(unsigned n);       ///< Pool of @p n workers (the default mode).
    bool setPerConnection();           ///< A new thread for every connection instead.
    bool setLimit(unsigned n);         ///< Most connections open at once.
    bool setTimeout(double s);         ///< Read/write timeout given to each connection's Stream.
    bool setDrain(double s);           ///< How long stop() waits before cutting connections off.
    /// @}

    /// @brief Bind, listen and start accepting. False (error() set) if any address fails.
    bool start(Handler h);
    /// @brief Stop accepting, finish in-flight connections, shut down. Idempotent.
    ///        Called from one of this listener's own handlers, it only starts the
    ///        shutdown (the handler can't wait for itself); wait() finishes it.
    void stop();
    /// @brief Block until stop() is called (by a handler, another thread or a signal),
    ///        then finish the shutdown.
    void wait();

    bool running() const { return started_ && !stopping_.load(); }
    bool started() const { return started_; }                           ///< start() succeeded once (stopped or not).
    int port() const { return port_; }                                  ///< Bound TCP port (useful after port 0).
    unsigned active() const { return active_.load(); }                  ///< Connections accepted and not yet finished.
    uint64_t served() const { return served_.load(); }                  ///< Connections finished so far.
    const std::string& error() const { return error_; }
    std::string describe() const;                                       ///< "tcp://[::]:8080, unix:///x" for messages.

    /// @brief Start stopping without waiting: wait() wakes up and finishes the shutdown.
    void requestStop();
    /// @brief Stop every running listener (program exit: Interp::run()).
    static void stopAll();

private:
    struct Conn { int fd; std::string peer; Stream::Kind kind; };
    struct Endpoint {
        Stream::Kind kind;
        std::string host;    ///< Tcp: empty = all addresses.
        int port = 0;
        std::string path;    ///< Unix.
    };
    struct Acceptor final : IoHandler {
        Listener* l;
        int fd;
        Stream::Kind kind;
        std::string path;    ///< Unix: socket file, removed at shutdown.
        Acceptor(Listener* l, int fd, Stream::Kind k, std::string p) : l(l), fd(fd), kind(k), path(std::move(p)) {}
        void onReady(unsigned) override { l->acceptFrom(*this); }
    };
    struct Waker final : IoHandler {
        int rd = -1, wr = -1;
        void onReady(unsigned) override;   ///< Drains the pipe; the poll loop then checks its flags.
        void poke() const;
    };

    bool configurable();
    bool parseEndpoint(const std::string& addr, Endpoint& ep);
    bool openEndpoint(const Endpoint& ep);
    void acceptFrom(Acceptor& a);
    void setAccepting(bool on);
    void dispatch(Conn c);
    void serve(Conn c);
    void finished();
    void pollLoop();
    void shutdown();

    // configuration
    int defaultPort_ = 0;
    std::vector<Endpoint> endpoints_;   ///< Empty = all addresses on defaultPort_.
    bool perConnection_ = false;
    unsigned threads_ = kDefaultThreads;
    unsigned limit_ = kDefaultLimit;
    double timeout_ = Stream::kDefaultTimeout;
    double drain_ = kDefaultDrain;
    std::string error_;

    // running state
    Handler handler_;
    bool started_ = false;
    int port_ = 0;
    std::unique_ptr<Poller> poller_;
    std::vector<std::unique_ptr<Acceptor>> acceptors_;
    Waker waker_;
    std::thread pollThread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> resume_{false};   ///< A connection finished below the limit: re-enable accepting.
    bool accepting_ = true;             ///< Poll thread only.
    std::atomic<unsigned> active_{0};
    std::atomic<uint64_t> served_{0};

    std::mutex m_;                      ///< Guards everything below.
    std::condition_variable work_;      ///< Pool workers: a connection is queued, or stopping.
    std::condition_variable idle_;      ///< active_ dropped / stop requested (wait(), shutdown()).
    std::deque<Conn> queue_;
    std::vector<std::thread> workers_;
    std::set<int> inFlight_;            ///< Sockets being served, to cut off after the drain timeout.
    // Per-connection threads are detached: each one's last act is finished(),
    // under m_, after every Dolphin value it held is gone; shutdown() waits for
    // active_ == 0, so none outlives the listener in any way that matters.
    bool shutDown_ = false;

    static thread_local Listener* tlWorkerOf_;   ///< The listener whose worker this thread is.
};

} // namespace dolphin
