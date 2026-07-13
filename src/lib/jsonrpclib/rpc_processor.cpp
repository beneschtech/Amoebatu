//
// Created by cbenesch on 7/8/26.
//

#include "rpc_processor.h"

#include <utility>

RPCProcessor::RPCProcessor() {
  m_transCnt = 0;
  m_methods.clear();
  m_methods["ping"] = &RPCProcessor::pingMethod;
}

bool RPCProcessor::hasMethod(const std::string &method) const {
  const auto it = m_methods.find(method);
  return it != m_methods.end();
}

bool RPCProcessor::registerMethod(const std::string &method, std::function<int(RPCProcessor *,const Json::Value&, Json::Value&)> func) {
  if (const auto it = m_methods.find(method); it != m_methods.end()) {
    return false;
  }
  m_methods[method] = std::move(func);
  return true;
}

int RPCProcessor::processRequest(const JsonRPC& request) {
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

int RPCProcessor::pingMethod(const Json::Value& params, Json::Value& response) const {
  response["transaction_count"] = m_transCnt.load();
  if (!params.empty()) {
    response["extra_params"] = params;
  }
  return 0;
}
