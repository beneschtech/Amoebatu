
#include <exception>
#include <iostream>
#include <memory>
#include <thread>
#include <errno.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "fcgi_request_cpp.hxx"
#include <fcgiapp.h>

/**
 * @brief FCGIListener::FCGIListener default constructor
 * There should only be one of these objects in an
 * application and runs its event loop in a separate
 * thread.
 */
FCGIListener::FCGIListener()
{
    p_listenerSocketPath.clear();
    p_fcgiHandle = -1;
    p_errorString.clear();
    p_stopFlag = false;
    p_state = INVALID;
    p_isUnixSocket = false;
}

/**
 * @brief FCGIListener::FCGIListener c-tor which sets up
 * the passed in path
 * @param listenerPath the path to listen to as a unix
 * socket filename, or TCP listen port
 */
FCGIListener::FCGIListener(std::string listenerPath)
    :FCGIListener()
{
    p_listenerSocketPath = listenerPath;
}

/**
 * @brief FCGIListener::~FCGIListener d-tor for the listening
 * object. Only unlinks the socket file if the path was
 * actually a Unix socket — not a TCP port string.
 */
FCGIListener::~FCGIListener()
{
    if (p_isUnixSocket)
        ::unlink(p_listenerSocketPath.c_str());
}

/**
 * @brief FCGIListener::open Attempts to open and create the UNIX socket.
 * If successful sets the internal status and the socket descriptor,
 * otherwise sets the error flags and message.
 *
 * Socket backlog raised to 128 (was 10) to avoid dropped connections
 * under normal load.
 *
 * @return true if success, false if not and sets the error string/flags
 */
bool FCGIListener::open()
{
    p_errorString.clear();
    if (p_listenerSocketPath.empty())
    {
        p_errorString = "Blank listener path";
        return false;
    }

    // Determine whether we're binding a Unix socket path or a TCP port.
    // A TCP address contains ':' (e.g. ":9000" or "127.0.0.1:9000").
    // A Unix socket path does not.
    p_isUnixSocket = (p_listenerSocketPath.find(':') == std::string::npos);

    FCGX_Init();

    if (p_isUnixSocket)
    {
        // Remove stale socket file if present. ENOENT is fine — it just
        // means there was no leftover file to clean up.
        if (::unlink(p_listenerSocketPath.c_str()) != 0 && errno != ENOENT)
        {
            char errbuf[256];
            p_errorString = p_listenerSocketPath;
            p_errorString.append(" (unlink): ");
            p_errorString.append(strerror_r(errno, errbuf, sizeof(errbuf)));
            return false;
        }

        // Probe write access to the directory before handing off to
        // FCGX_OpenSocket, which gives a less useful error on failure.
        FILE *f = fopen(p_listenerSocketPath.c_str(), "w");
        if (!f)
        {
            char errbuf[256];
            p_errorString = p_listenerSocketPath;
            p_errorString.append(": ");
            p_errorString.append(strerror_r(errno, errbuf, sizeof(errbuf)));
            return false;
        }
        fclose(f);
        ::unlink(p_listenerSocketPath.c_str());
    }

    // Backlog of 128 instead of original 10 — under real load 10 causes
    // the OS to start silently dropping connections.
    p_fcgiHandle = FCGX_OpenSocket(p_listenerSocketPath.c_str(), 128);
    if (p_fcgiHandle >= 0)
        p_state = ATTACHED;
    return (p_fcgiHandle != -1);
}

/**
 * @brief FCGIListener::start checks that the socket has been open()ed
 * then starts the accept thread. The accept thread will listen for
 * connections and push parsed requests onto the queue, waking any
 * thread blocked in nextRequest().
 * @return true if started, false if there was a problem
 */
bool FCGIListener::start()
{
    if (p_fcgiHandle <= 0)
    {
        p_errorString = "Invalid cgi handle (call open() first)";
        return false;
    }
    if (p_state == RUNNING)
    {
        p_errorString = "Listener already running";
        return false;
    }
    std::thread t1(&FCGIListener::thr_listen, this);
    t1.detach();
    return true;
}

/**
 * @brief thr_listen — internal accept thread.
 *
 * Replaced the original two-mutex scheme (which had a race between
 * try_lock() in the producer and lock_guard in the consumer, and could
 * crash on nextRequest() when the queue was empty) with a standard
 * mutex + condition_variable pattern:
 *
 *   - Producer (this thread): locks, pushes, notifies.
 *   - Consumer (nextRequest): waits on the condition variable until
 *     the queue is non-empty, then pops under the same lock.
 *
 * This eliminates the race and the crash with zero extra complexity.
 * The deque replaces the vector for O(1) front-pop instead of O(n).
 */
void FCGIListener::thr_listen()
{
    p_state = RUNNING;
    p_stopFlag = false;
    FCGI::SetThreadName("FCGI Listen");

    while (!p_stopFlag)
    {
        std::shared_ptr<FCGX_Request> req = std::make_shared<FCGX_Request>();
        FCGX_InitRequest(req.get(), p_fcgiHandle, 0);

        if (FCGX_Accept_r(req.get()) == 0)
        {
            FCGIRequest reqst(req);
            if (reqst.parse())
            {
                req.reset();
                {
                    std::lock_guard<std::mutex> lk(p_mutex);
                    p_reqQueue.push_back(reqst);
                }
                p_cv.notify_one();
            }
            // If parse() fails (e.g. oversized POST, bad Content-Length)
            // we simply discard the request. The caller that dispatched
            // to this FCGI process will time out and nginx will return
            // a 502; the cluster UI can handle that gracefully.
        }
        else
        {
            char errbuf[256];
            if (!p_stopFlag)
            {
                p_errorString = strerror_r(errno, errbuf, sizeof(errbuf));
            }
            p_stopFlag = true;
        }
    }
    p_state = STOPPED;
    // Wake any thread blocked in nextRequest() so it can see STOPPED
    // and return cleanly rather than blocking forever.
    p_cv.notify_all();
}

/**
 * @brief FCGIListener::stop
 * Signals the accept thread to exit and closes the socket.
 */
void FCGIListener::stop()
{
    p_stopFlag = true;
    ::shutdown(p_fcgiHandle, SHUT_RDWR);
    ::close(p_fcgiHandle);
}

/**
 * @brief FCGIListener::nextRequest
 * Blocks until a parsed request is available, then returns it.
 * Returns an invalid (nullptr) FCGIRequest if the listener has stopped.
 *
 * Original version could crash: it called p_reqQueue.front() without
 * checking whether the queue was empty, and used try_lock() in a context
 * that required a guaranteed lock. Both issues are eliminated here by
 * waiting on the condition variable under the mutex.
 */
FCGIRequest FCGIListener::nextRequest()
{
    std::unique_lock<std::mutex> lk(p_mutex);
    p_cv.wait(lk, [this] {
        return !p_reqQueue.empty() || p_state == STOPPED;
    });

    if (p_reqQueue.empty())
    {
        // Listener stopped with nothing queued — return invalid request.
        // Callers should check FCGIRequest::isValid() before using it.
        return FCGIRequest(nullptr);
    }

    FCGIRequest rv = p_reqQueue.front();
    p_reqQueue.pop_front();
    return rv;
}
