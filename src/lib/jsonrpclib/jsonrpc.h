//
// Created by cbenesch on 7/6/26.
//

#ifndef AMEOBATU_JSONRPC_H
#define AMEOBATU_JSONRPC_H

#include <json/json.h>
#include <iostream>

#define JSONRPC_RANDOM_ID "RANDOMID"
#define JSONRPC_PARSE_ERROR (-32700)
#define JSONRPC_INVALID_REQUEST (-32600)
#define JSONRPC_METHOD_NOT_FOUND (-32601)
#define JSONRPC_INVALID_PARAMS (-32602)
#define JSONRPC_INTERNAL_ERROR (-32603)

class JsonRPC {
public:
  JsonRPC();
  ~JsonRPC() = default;
  bool parseRequest(const std::string& request);
  void error_response(int code,const std::string& errs = "");
  void error_response(int code,const Json::Value& errs);
  static void register_error_code(int code,const std::string& message);
  void printResponse() const { std::cout << m_response.toStyledString() << std::endl; }
  [[nodiscard]] std::string responseText() const;
  [[nodiscard]] Json::Value params() const { return m_params; }
  [[nodiscard]] std::string method() const { return m_method; }
  [[nodiscard]] std::string id() const { return m_id; }
  void setResponse(const Json::Value& response);

  void set_params(const Json::Value& params) { m_params = params; }
  void set_id(const std::string& id);
  void set_method(const std::string& method) { m_method = method; }
  [[nodiscard]] std::string requestText() const;

private:
  void reset();
  Json::Value m_request;
  Json::Value m_response;
  std::string m_method;
  std::string m_id;
  Json::Value m_params;
};


#endif //AMEOBATU_JSONRPC_H
