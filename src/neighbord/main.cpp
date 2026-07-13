//
// Created by cbenesch on 7/12/26.
//
#include <iostream>
#include <string>
#include <arpa/inet.h>
#include <thread>
#include "HostDirectory.h"
#include <getopt.h>

static std::string s_bcast_addr("255.255.255.255");
static u_int16_t s_bcast_port = 26781;
static u_int32_t s_interval = 15;
static std::string s_ifname;

bool keep_running = true;
HostDirectory host_directory;

bool parse_args(int argc, char* argv[]);
void show_help();
void show_version();
void announcer_thread(const std::string& ip_address, u_int16_t port, const std::string &ifname,u_int32_t interval);
void listener_thread(u_int16_t port);

int main(int argc, char* argv[]) {
  parse_args(argc, argv);
  show_version();
  std::cout << "Broadcast address: " << s_bcast_addr << std::endl;
  std::cout << "Broadcast port: " << s_bcast_port << std::endl;
  std::cout << "Interface name: " << s_ifname << std::endl;
  std::cout << "Interval: " << s_interval << std::endl;
  std::cout << std::endl;
  std::thread ann_thread(announcer_thread, s_bcast_addr, s_bcast_port, s_ifname,s_interval);
  std::thread lis_thread(listener_thread, s_bcast_port);

  while (keep_running) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  ann_thread.join();
  lis_thread.join();
  return 0;
}

bool parse_args(int argc, char* argv[]) {
  int opt;
  while ((opt = getopt(argc,argv,"hvp:a:i:t:")) != -1) {
    switch (opt) {
      case 'h':
        show_help();
        exit(0);
      case 'v':
        show_version();
        exit(0);
      case 'a':
        s_bcast_addr = optarg;
        break;
      case 'i':
        s_ifname = optarg;
        break;
      case 'p':
        s_bcast_port = atoi(optarg);
        break;
      case 't':
        s_interval = atoi(optarg);
        break;
      default:
        show_help();
        exit(1);
    }
  }
  return true;
}

void show_help() {
  show_version();
  std::cout << "Usage: neighbord [-h] [-v] [-a <address>] [-p <port>] [-t <interval>] [-i <interface>]" << std::endl;
  std::cout << "  -h, Show this help message and exit" << std::endl;
  std::cout << "  -v, Show version information and exit" << std::endl;
  std::cout << "  -a, Set the broadcast address (default: 255.255.255.255)" << std::endl;
  std::cout << "  -p, Set the broadcast port (default: 26781)" << std::endl;
  std::cout << "  -t, Set the broadcast interval in seconds (default: 15)" << std::endl;
  std::cout << "  -i, Set the interface name addresses to announce (default: none - all addresses)" << std::endl;
}

void show_version() {
  std::cout << "neighbord version 0.1" << std::endl;
}
