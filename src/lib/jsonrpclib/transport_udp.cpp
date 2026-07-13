/**
 * udp_example.cpp
 *
 * Compile:
 *   g++ -std=c++17 -o udp_example udp_example.cpp
 *
 * Run as sender:
 *   ./udp_example send "hello from sender"
 *
 * Run as receiver (single packet, 5s timeout):
 *   ./udp_example receive
 *
 * Run as receiver loop (ctrl-c to stop):
 *   ./udp_example loop
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include "transport_udp.h"
#include "jsonrpc.h"


// ---- RAII socket handle ----------------------------------------------------

struct Socket {
    int fd = -1;
    Socket() : fd(::socket(AF_INET, SOCK_DGRAM, 0)) {
        if (fd < 0)
            throw std::runtime_error(std::string("socket(): ") + strerror(errno));
    }
    ~Socket() { if (fd >= 0) ::close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    void close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

// ---- Helpers ----------------------------------------------------------------

inline void set_broadcast(int fd) {
    int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0)
        throw std::runtime_error(std::string("setsockopt SO_BROADCAST: ") + strerror(errno));
}

inline void set_reuseaddr(int fd) {
    int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
        throw std::runtime_error(std::string("setsockopt SO_REUSEADDR: ") + strerror(errno));
}

namespace udp_broadcast {
    /**
     * Broadcast `message` on the given `port`.
     *
     * @param port        UDP port to broadcast on
     * @param message     payload to send
     * @param broadcast_ip broadcast address (default 255.255.255.255)
     *
     * @throws std::runtime_error on socket error
     */
    void send(
        uint16_t           port,
        const std::string& message,
        const std::string& broadcast_ip)
    {
        Socket sock;
        set_broadcast(sock.fd);

        // Allow rapid re-use of the port on the send side too (useful in tests)
        set_reuseaddr(sock.fd);

        sockaddr_in dest{};
        dest.sin_family      = AF_INET;
        dest.sin_port        = htons(port);
        dest.sin_addr.s_addr = ::inet_addr(broadcast_ip.c_str());

        ssize_t sent = ::sendto(
            sock.fd,
            message.data(), message.size(),
            0,
            reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

        if (sent < 0)
            throw std::runtime_error(std::string("sendto(): ") + strerror(errno));
        sock.close();
    }


    /**
     * Listen on `port` for a single UDP broadcast packet.
     *
     * @param port          UDP port to listen on
     * @param max_packet    maximum payload size in bytes (default 4096)
     * @param timeout_ms    milliseconds to wait before giving up;
     *                      0 = block forever, negative = same as 0
     *
     * @returns ReceivedPacket if a packet arrived within the timeout,
     *          std::nullopt on timeout.
     * @throws  std::runtime_error on socket error.
     */
    std::optional<ReceivedPacket> receive(
        uint16_t port,
        size_t   max_packet,
        int      timeout_ms)
    {
        Socket sock;
        set_broadcast(sock.fd);
        set_reuseaddr(sock.fd);

        sockaddr_in bind_addr{};
        bind_addr.sin_family      = AF_INET;
        bind_addr.sin_port        = htons(port);
        bind_addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(sock.fd,
                   reinterpret_cast<sockaddr*>(&bind_addr),
                   sizeof(bind_addr)) < 0)
            throw std::runtime_error(std::string("bind(): ") + strerror(errno));

        // Optional timeout via select()
        if (timeout_ms > 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock.fd, &rfds);

            timeval tv{};
            tv.tv_sec  = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;

            int ready = ::select(sock.fd + 1, &rfds, nullptr, nullptr, &tv);
            if (ready < 0)
                throw std::runtime_error(std::string("select(): ") + strerror(errno));
            if (ready == 0)
                return std::nullopt;  // timed out
        }

        std::vector<char> buf(max_packet);
        sockaddr_in sender{};
        socklen_t   sender_len = sizeof(sender);

        ssize_t n = ::recvfrom(
            sock.fd,
            buf.data(), buf.size(),
            0,
            reinterpret_cast<sockaddr*>(&sender), &sender_len);

        if (n < 0)
            throw std::runtime_error(std::string("recvfrom(): ") + strerror(errno));

        ReceivedPacket pkt;
        pkt.data = std::string(buf.data(), static_cast<size_t>(n));
        pkt.from = ::inet_ntoa(sender.sin_addr);
        pkt.port = ntohs(sender.sin_port);
        return pkt;
    }
}