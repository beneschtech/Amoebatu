//
// Created by cbenesch on 7/13/26.
//

#include "HostDirectory.h"

extern bool keep_running;
HostDirectory::HostDirectory()
{
  m_hosts.push_back(Host{"localhost","127.0.0.1"});
  m_hosts.push_back(Host{"localhost","::1"});
  HostDirectory::registerMethod("announce",&HostDirectory::announceCB);
  HostDirectory::registerMethod("stop",&HostDirectory::stopCB);
}

int HostDirectory::processRequest(const JsonRPC& request) {
  Json::Value result;
  const auto it = m_methods.find(request.method());
  int rv = -1;
  m_transCnt.fetch_add(1);
  if (it == m_methods.end()) {
    result["error"]["data"] = request.method();
    rv = -32601;
  } else {
    rv = it->second(this, request.params(), result);
  }
  if (rv >= 0) {
    (const_cast<JsonRPC&>(request)).setResponse(result);
  } else {

    (const_cast<JsonRPC&>(request)).error_response(rv,result);
  }
  return rv;
}

bool HostDirectory::registerMethod(const std::string &method,
  std::function<int(HostDirectory *, const Json::Value &, Json::Value &)> func) {
  if (const auto it = m_methods.find(method); it != m_methods.end()) {
    return false;
  }
  m_methods[method] = std::move(func);
  return true;
}

int HostDirectory::announceCB(const Json::Value& params,Json::Value& result) {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::string hostname = params["hostname"].asString();
  size_t nAddr = params["addresses"].size();
  for (size_t i = 0; i < nAddr; ++i) {
    Json::Value::ArrayIndex idx = i;
    std::string addr = params["addresses"][idx].asString();
    bool found = false;
    for (const auto &he : m_hosts) {
      if (he.hostname == hostname && he.ip == addr) {
        found = true;
        break;
      }
    }
    if (!found) {
      m_hosts.push_back(Host{hostname,addr});
    }
  }
  result.clear();
  return 0;
}

int HostDirectory::stopCB(const Json::Value &, Json::Value &result) {
  keep_running = false;
  result.clear();
  return 0;
}
