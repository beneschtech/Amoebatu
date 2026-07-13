//
// Created by cbenesch on 7/12/26.
//
#include <string>
#include <sys/types.h>
#include <transport_udp.h>
#include <jsonrpc.h>
#include <ifaddrs.h>
#include <iostream>
#include <netdb.h>
#include <thread>
#include <unistd.h>

extern bool keep_running;

static Json::Value get_addresses(const std::string &ifname) {
  struct ifaddrs *ifaddr, *ifa;
  int family, s;
  char host[NI_MAXHOST];
  Json::Value params;
  // Request the linked list of network interfaces
  if (getifaddrs(&ifaddr) == -1) {
    perror("getifaddrs failed");
    exit(EXIT_FAILURE);
  }

  // Walk through the linked list
  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    // Skip interfaces that do not have an assigned address
    if (ifa->ifa_addr == NULL) {
      continue;
    }

    family = ifa->ifa_addr->sa_family;

    // Filter for IPv4 (AF_INET) and IPv6 (AF_INET6)
    if (family == AF_INET || family == AF_INET6) {

      // Convert the binary address structure to a human-readable string
      s = getnameinfo(ifa->ifa_addr,
                      (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6),
                      host, NI_MAXHOST,
                      NULL, 0, NI_NUMERICHOST);

      if (s != 0) {
        std::cerr << "getnameinfo() failed: " << gai_strerror(s) << std::endl;
        exit(1);
      }
      // Exclude loopback interfaces
      if (strncmp(ifa->ifa_name, "lo", 2) == 0) {
        continue;
      }
      if (ifa->ifa_name == ifname || ifname.empty()) {
        params.append(host);
      }
    }
  }
  // Free the dynamically allocated memory
  freeifaddrs(ifaddr);
  return params;
}

void announcer_thread(const std::string& ip_address, u_int16_t port, const std::string &ifname,u_int32_t interval) {
  JsonRPC annmsg;
  annmsg.set_method("announce");
  annmsg.set_id("");
  Json::Value params;

  char hostbuf[256] = {0,};
  if (gethostname(hostbuf, sizeof(hostbuf)) != 0) {
    perror("gethostname failed");
    exit(EXIT_FAILURE);
  }

  u_int32_t count = 1;
  while (keep_running) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    count--;
    if (!count) {
      Json::Value annparams;
      annparams["hostname"] = hostbuf;
      annparams["addresses"] = get_addresses(ifname);
      annmsg.set_params(annparams);
      std::string json_str = annmsg.requestText();

      udp_broadcast::send(port, json_str,ip_address);
      count = interval;
    }
  }
}