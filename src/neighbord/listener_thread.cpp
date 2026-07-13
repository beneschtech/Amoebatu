//
// Created by cbenesch on 7/13/26.
//
#include "HostDirectory.h"
#include "../jsonrpclib/transport_udp.h"
#include "../jsonrpclib/jsonrpc.h"

extern std::atomic_bool keep_running;
extern HostDirectory host_directory;

void listener_thread(u_int16_t port) {
  while (keep_running) {
    auto msg = udp_broadcast::receive(port,8192,1000);
    if (msg == std::nullopt)
      continue;
    JsonRPC msgjson;
    if (!msgjson.parseRequest(msg->data)) {
      continue;
    }
    host_directory.processRequest(msgjson);
  }
}