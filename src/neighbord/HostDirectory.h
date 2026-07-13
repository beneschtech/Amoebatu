//
// Created by cbenesch on 7/13/26.
//

#ifndef AMEOBATU_HOSTDIRECTORY_H
#define AMEOBATU_HOSTDIRECTORY_H

#include <mutex>
#include <vector>
#include "../lib/jsonrpclib/rpc_processor.h"
class HostDirectory: public RPCProcessor {
public:
  HostDirectory();
  bool virtual registerMethod(const std::string &method, std::function<int(HostDirectory *,const Json::Value&, Json::Value&)> func);
  int processRequest(const JsonRPC& request) override;

  struct Host {
    std::string hostname;
    std::string ip;
  };
  int announceCB(const Json::Value& params,Json::Value& result);
  int stopCB(const Json::Value& params,Json::Value& result);

protected:
  std::map<std::string, std::function<int(HostDirectory *,const Json::Value&, Json::Value&)>> m_methods;

private:
  std::vector<Host> m_hosts;
  std::mutex m_mutex;
};


#endif //AMEOBATU_HOSTDIRECTORY_H
