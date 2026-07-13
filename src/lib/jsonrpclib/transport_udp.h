#pragma once

/**
 * udp_broadcast.h
 *
 * Simple UDP broadcast send / receive for Linux using POSIX sockets and STL.
 * No external dependencies beyond the standard C++ library and POSIX headers.
 *
 * Usage - send:
 *   auto result = udp_broadcast_send(8765, "hello world");
 *
 * Usage - receive (blocking, with optional timeout):
 *   auto result = udp_broadcast_receive(8765, 4096, 5000);
 *   if (result) {
 *       std::cout << result->from << " said: " << result->data << "\n";
 *   }
 */
#include <optional>
#include <cstdint>
#include <string>


struct ReceivedPacket {
  std::string data; // payload
  std::string from; // sender IP as dotted-decimal string
  uint16_t port; // sender port
};

namespace udp_broadcast {

  std::optional<ReceivedPacket> receive(
      uint16_t port,
      size_t   max_packet  = 4096,
      int      timeout_ms  = 0);

  void send(
    uint16_t           port,
    const std::string& message,
    const std::string& broadcast_ip = "255.255.255.255");

} // namespace udp_broadcast
