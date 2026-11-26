/**
 * @file stream.cpp
 * @brief Stream and Response: see stream.hpp.
 */
#include "stream.hpp"

#include <curl/curl.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>

#include "ast.hpp"

namespace dolphin {

namespace {

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

std::string sysError(const std::string& what) { return what + ": " + std::strerror(errno); }

/// curl_global_init isn't thread-safe; do it once, before any request thread starts.
void curlInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t curlSink(char* p, size_t sz, size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * n);
    return sz * n;
}

/// One blocking HTTP request (GET when @p post is null).
Response::Result httpRequest(const std::string& url, const std::string* post, const std::string& contentType,
                             long timeoutMs) {
    Response::Result r;
    CURL* c = curl_easy_init();
    if (!c) {
        r.error = "curl_easy_init failed";
        return r;
    }
    char err[CURL_ERROR_SIZE] = {0};
    curl_slist* hdrs = nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlSink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);   // timeouts without SIGALRM: we run on many threads
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeoutMs);
    if (post) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post->data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(post->size()));
        hdrs = curl_slist_append(hdrs, ("Content-Type: " + contentType).c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        r.ok = true;
        r.code = code;
    } else {
        r.error = err[0] ? err : curl_easy_strerror(rc);
        r.body.clear();
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

} // namespace

// ------------------------------------------------------------------ kinds

Stream::Kind Stream::kindOf(const std::string& t) {
    if (startsWith(t, "http://") || startsWith(t, "https://")) return Kind::Http;
    if (startsWith(t, "tcp://")) return Kind::Tcp;
    if (startsWith(t, "unix://")) return Kind::Unix;
    return Kind::File;
}

unsigned Stream::needsFor(const std::string& t) { return kindOf(t) == Kind::File ? NeedIO : NeedNetwork; }

Stream::Stream(std::string target) : kind_(kindOf(target)), target_(std::move(target)) {
    switch (kind_) {
    case Kind::File:
        path_ = startsWith(target_, "file://") ? target_.substr(7) : target_;
        break;
    case Kind::Http:
        path_ = target_;
        curlInit();
        break;
    case Kind::Unix:
        path_ = target_.substr(7);
        break;
    case Kind::Tcp: {
        std::string hp = target_.substr(6);
        if (!hp.empty() && hp.back() == '/') hp.pop_back();
        size_t colon = hp.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == hp.size()) {
            fail("tcp URL needs host:port: " + target_);
            return;
        }
        host_ = hp.substr(0, colon);
        port_ = hp.substr(colon + 1);
        if (host_.size() > 2 && host_.front() == '[' && host_.back() == ']') host_ = host_.substr(1, host_.size() - 2);
        break;
    }
    }
    open("r");
}

Stream::Stream(int fd, Kind kind, std::string peer) : kind_(kind), target_(std::move(peer)), accepted_(true) {
    fd_ = fd;
    valid_ = true;
}

Stream::~Stream() { close(); }

bool Stream::fail(std::string msg) {
    error_ = std::move(msg);
    return false;
}

int Stream::timeoutMs() const {
    double ms = timeout_ * 1000.0;
    return ms < 0 ? -1 : ms > 2e9 ? 2000000000 : static_cast<int>(ms);
}

// ------------------------------------------------------------------ open / close

bool Stream::open(const std::string& mode) {
    if (accepted_) return fail("an accepted connection can't be reopened");
    if (kind_ == Kind::Tcp && host_.empty()) return false;   // malformed URL: keep the constructor's error
    close();
    error_.clear();
    if (kind_ == Kind::File) {
        const char* m = mode == "r" ? "rb" : mode == "rw" ? "r+b" : mode == "wc" ? "wb" : mode == "a" ? "ab" : nullptr;
        if (!m) return fail("unknown mode '" + mode + "' (use r, rw, wc or a)");
        return valid_ = openFile(m);
    }
    if (mode != "r" && mode != "rw" && mode != "wc") return fail("unknown mode '" + mode + "' (use r, rw or wc)");
    if (kind_ == Kind::Http) return valid_ = true;   // nothing to connect until the first request
    return valid_ = connectSocket();
}

void Stream::close() {
    if (f_) std::fclose(f_);
    if (fd_ >= 0) ::close(fd_);
    f_ = nullptr;
    fd_ = -1;
    valid_ = false;
    seekable_ = false;
    fetched_ = false;
    buf_.clear();
    pos_ = 0;
    error_.clear();
}

bool Stream::openFile(const char* mode) {
    f_ = std::fopen(path_.c_str(), mode);
    if (!f_) return fail(sysError(path_));
    seekable_ = std::fseek(f_, 0, SEEK_CUR) == 0;   // false for pipes and character devices
    return true;
}

bool Stream::connectSocket() {
    // Each candidate address: non-blocking connect, then wait for it within the timeout.
    auto attempt = [&](int family, const sockaddr* sa, socklen_t len) -> bool {
        int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return fail(sysError("socket"));
        if (::connect(fd, sa, len) != 0) {
            if (errno != EINPROGRESS) {
                fail(sysError(target_));
                ::close(fd);
                return false;
            }
            pollfd p{fd, POLLOUT, 0};
            int rc = ::poll(&p, 1, timeoutMs());
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            if (rc == 0) {
                ::close(fd);
                return fail(target_ + ": connect timed out");
            }
            if (rc < 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr) {
                if (soerr) errno = soerr;
                fail(sysError(target_));
                ::close(fd);
                return false;
            }
        }
        fd_ = fd;
        return true;
    };
    if (kind_ == Kind::Unix) {
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (path_.size() >= sizeof sa.sun_path) return fail("unix socket path too long: " + path_);
        std::memcpy(sa.sun_path, path_.c_str(), path_.size() + 1);
        return attempt(AF_UNIX, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (int rc = ::getaddrinfo(host_.c_str(), port_.c_str(), &hints, &res); rc != 0)
        return fail(host_ + ": " + gai_strerror(rc));
    bool ok = false;
    for (addrinfo* a = res; a && !ok; a = a->ai_next) ok = attempt(a->ai_family, a->ai_addr, a->ai_addrlen);
    ::freeaddrinfo(res);
    if (ok) error_.clear();
    return ok;
}

// ------------------------------------------------------------------ reading

bool Stream::fetch() {
    Response::Result r = httpRequest(path_, nullptr, "", timeoutMs());
    if (!r.ok) {
        valid_ = false;
        return fail(target_ + ": " + r.error);
    }
    code_ = r.code;
    buf_ = std::move(r.body);
    pos_ = 0;
    fetched_ = true;
    return true;
}

bool Stream::fill(bool& eof) {
    eof = false;
    pollfd p{fd_, POLLIN, 0};
    int rc = ::poll(&p, 1, timeoutMs());
    if (rc == 0) return fail(target_ + ": read timed out");
    if (rc < 0) return fail(sysError(target_));
    char tmp[65536];
    ssize_t got = ::recv(fd_, tmp, sizeof tmp, 0);
    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
        return fail(sysError(target_));
    }
    if (got == 0) eof = true;
    buf_.append(tmp, static_cast<size_t>(got));
    return true;
}

bool Stream::read(int64_t n, std::string& out) {
    out.clear();
    if (!valid_) return fail(error_.empty() ? target_ + ": not open" : error_);
    switch (kind_) {
    case Kind::File: {
        if (n < 0) {
            char tmp[65536];
            size_t got;
            while ((got = std::fread(tmp, 1, sizeof tmp, f_)) > 0) out.append(tmp, got);
        } else {
            out.resize(static_cast<size_t>(n));
            out.resize(std::fread(out.data(), 1, out.size(), f_));
        }
        if (std::ferror(f_)) {
            std::clearerr(f_);
            return fail(sysError(path_));
        }
        return true;
    }
    case Kind::Http: {
        if (!fetched_ && !fetch()) return false;
        size_t take = n < 0 ? buf_.size() - pos_ : std::min(static_cast<size_t>(n), buf_.size() - pos_);
        out.assign(buf_, pos_, take);
        pos_ += take;
        return true;
    }
    case Kind::Tcp:
    case Kind::Unix: {
        // Sized: return what's buffered or the first data to arrive (like recv).
        // Unsized: everything until the peer closes.
        bool eof = false;
        while (pos_ == buf_.size() || n < 0) {
            if (pos_ == buf_.size()) {
                buf_.clear();
                pos_ = 0;
            }
            if (!fill(eof)) return false;
            if (eof) break;
        }
        size_t avail = buf_.size() - pos_;
        size_t take = n < 0 ? avail : std::min(static_cast<size_t>(n), avail);
        out.assign(buf_, pos_, take);
        pos_ += take;
        return true;
    }
    }
    return false;
}

bool Stream::readLine(std::string& out) {
    out.clear();
    if (!valid_) return fail(error_.empty() ? target_ + ": not open" : error_);
    if (kind_ == Kind::File) {
        int c;
        while ((c = std::fgetc(f_)) != EOF) {
            out += static_cast<char>(c);
            if (c == '\n') break;
        }
        if (std::ferror(f_)) {
            std::clearerr(f_);
            return fail(sysError(path_));
        }
        return true;
    }
    if (kind_ == Kind::Http && !fetched_ && !fetch()) return false;
    for (;;) {
        size_t nl = buf_.find('\n', pos_);
        if (nl != std::string::npos || kind_ == Kind::Http) {
            size_t end = nl == std::string::npos ? buf_.size() : nl + 1;
            out.assign(buf_, pos_, end - pos_);
            pos_ = end;
            return true;
        }
        bool eof = false;
        if (!fill(eof)) return false;
        if (eof) {
            out.assign(buf_, pos_, std::string::npos);
            pos_ = buf_.size();
            return true;
        }
    }
}

// ------------------------------------------------------------------ writing

bool Stream::write(const std::string& data) {
    if (!valid_) return fail(error_.empty() ? target_ + ": not open" : error_);
    if (kind_ == Kind::Http) return fail("write to an http stream takes a dict, str or bytes to POST");
    if (kind_ == Kind::File) {
        if (std::fwrite(data.data(), 1, data.size(), f_) != data.size() || std::fflush(f_) != 0) {
            std::clearerr(f_);
            return fail(sysError(path_));
        }
        return true;
    }
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n >= 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return fail(sysError(target_));
        pollfd p{fd_, POLLOUT, 0};
        int rc = ::poll(&p, 1, timeoutMs());
        if (rc == 0) return fail(target_ + ": write timed out");
        if (rc < 0) return fail(sysError(target_));
    }
    return true;
}

std::shared_ptr<Response> Stream::post(std::string body, std::string contentType) {
    auto r = std::make_shared<Response>();
    std::string url = path_;
    long ms = timeoutMs();
    r->fut = std::async(std::launch::async, [url, body = std::move(body), ct = std::move(contentType), ms] {
                 return httpRequest(url, &body, ct, ms);
             }).share();
    return r;
}

// ------------------------------------------------------------------ seeking

bool Stream::seek(int64_t off, int whence, int64_t& pos) {
    if (!valid_) return fail(error_.empty() ? target_ + ": not open" : error_);
    if (!seekable_) return fail(target_ + ": not seekable");
    if (std::fseek(f_, static_cast<long>(off), whence) != 0) return fail(sysError(path_));
    return tell(pos);
}

bool Stream::tell(int64_t& pos) {
    if (!valid_) return fail(error_.empty() ? target_ + ": not open" : error_);
    if (!seekable_) return fail(target_ + ": not seekable");
    long p = std::ftell(f_);
    if (p < 0) return fail(sysError(path_));
    pos = p;
    return true;
}

} // namespace dolphin
