/**
 * @file stream.hpp
 * @brief The `Stream` builtin object: files, HTTP(S) URLs, TCP and Unix sockets.
 *
 * One object for every kind of outbound I/O, so the analyzer can tell from a
 * single constructor whether a function needs IO or Network
 * (Stream::needsFor()). Listening (servers) is deliberately not here.
 *
 * | Target                     | Kind  | Need    | Opens as                           |
 * |----------------------------|-------|---------|------------------------------------|
 * | `path` or `file://path`    | File  | IO      | fopen(path, "rb")                  |
 * | `http://…`, `https://…`    | Http  | Network | nothing yet; GET on the first read |
 * | `tcp://host:port`          | Tcp   | Network | connected socket                   |
 * | `unix://path`              | Unix  | Network | connected AF_UNIX stream socket    |
 *
 * Opening never raises: a failure clears `valid` and sets `error`. Operations
 * on a stream that isn't valid, and operations that fail, report through
 * their bool result and `error`; the builtins turn that into IOError.
 *
 * File mode is a thin wrapper over the stdio functions (fopen, fread, fwrite,
 * fseek, ftell). Socket reads and writes, the HTTP GET and every POST honour
 * `timeout` (seconds, default kDefaultTimeout).
 *
 * Not thread-safe: a Stream is only used by functions that have needs, which
 * run in program order on one thread (the barrier before every effect).
 * A Response, by contrast, is completed by its own thread and may be read
 * from any thread.
 */
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <string>

namespace dolphin {

/**
 * @brief The promise returned by writing to an HTTP stream (a POST).
 *
 * The request runs on its own thread. Reading `code`, `body`, `valid` or
 * `error` waits for it; `ready` never waits.
 */
struct Response {
    /// @brief What the request produced.
    struct Result {
        bool ok = false;        ///< The request completed (any HTTP status).
        int64_t code = 0;       ///< HTTP status, 0 if the request failed.
        std::string body;       ///< Response body.
        std::string error;      ///< Why it failed; empty if ok.
    };
    std::shared_future<Result> fut;   ///< Completed by the request thread.

    /// @brief Wait for the request and return its result.
    const Result& get() const { return fut.get(); }
    /// @brief Has the request finished? Never blocks.
    bool ready() const { return fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
};

/// @brief An open (or failed-to-open) file, URL or socket. See the file comment.
class Stream {
public:
    enum class Kind { File, Http, Tcp, Unix };
    static constexpr double kDefaultTimeout = 30.0;   ///< Seconds.

    /// @brief The kind of stream @p target names (by its scheme).
    static Kind kindOf(const std::string& target);
    /// @brief NeedIO for a file, NeedNetwork for anything else.
    static unsigned needsFor(const std::string& target);

    /// @brief Open @p target: a file read-only, a socket connected, a URL not yet fetched.
    explicit Stream(std::string target);
    /// @brief Adopt a connected socket accepted by a Listener; @p peer becomes `name`.
    Stream(int fd, Kind kind, std::string peer);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    /**
     * @brief Reopen: for a file, @p mode is `r` (read), `rw` (read/write,
     *        must exist), `wc` (write, create or truncate) or `a` (append).
     *        Sockets reconnect and a URL forgets its fetched response; their
     *        mode must be `r`, `rw` or `wc` and changes nothing else.
     * @return The new `valid`. False with `error` set also for an unknown mode.
     */
    bool open(const std::string& mode);
    /// @brief Close. `valid` becomes false; `error` is cleared.
    void close();

    /// @brief Read up to @p n bytes (@p n < 0: everything to end of file /
    ///        connection close). Empty at end. False (error set) on failure.
    bool read(int64_t n, std::string& out);
    /// @brief Read through the next `\n` (included), or to the end.
    bool readLine(std::string& out);
    /// @brief Write all of @p data (file or socket). Not for HTTP: see post().
    bool write(const std::string& data);
    /// @brief POST @p body to an HTTP stream; the request runs on its own thread.
    std::shared_ptr<Response> post(std::string body, std::string contentType);
    /// @brief fseek(); @p pos receives the new position. Files only.
    bool seek(int64_t off, int whence, int64_t& pos);
    /// @brief ftell(). Files only.
    bool tell(int64_t& pos);

    Kind kind() const { return kind_; }
    const std::string& target() const { return target_; }
    bool valid() const { return valid_; }
    const std::string& error() const { return error_; }
    int64_t code() const { return code_; }   ///< HTTP status of the last GET; 0 for files and sockets.
    bool seekable() const { return seekable_; }
    double timeout() const { return timeout_; }
    void setTimeout(double s) { timeout_ = s; }
    /// @brief Record @p msg as the error and return false (for callers' one-line failures).
    bool fail(std::string msg);

private:
    bool openFile(const char* mode);
    bool connectSocket();
    bool fetch();                    ///< HTTP GET into buf_ (once per open).
    bool fill(bool& eof);            ///< Socket: append whatever arrives within the timeout to buf_.
    int timeoutMs() const;

    Kind kind_;
    std::string target_;             ///< As given.
    std::string path_;               ///< File path, socket path, or URL.
    std::string host_;               ///< Tcp.
    std::string port_;               ///< Tcp.
    bool valid_ = false;
    std::string error_;
    int64_t code_ = 0;
    bool seekable_ = false;
    double timeout_ = kDefaultTimeout;

    FILE* f_ = nullptr;              ///< File.
    int fd_ = -1;                    ///< Tcp / Unix.
    bool fetched_ = false;           ///< Http: buf_ holds the response.
    bool accepted_ = false;          ///< Adopted from a Listener: open() can't reconnect it.
    std::string buf_;                ///< Http response, or socket read-ahead.
    size_t pos_ = 0;                 ///< Read position in buf_.
};

} // namespace dolphin
