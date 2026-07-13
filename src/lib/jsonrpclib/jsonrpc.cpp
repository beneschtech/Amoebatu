//
// Created by cbenesch on 7/6/26.
//

#include "jsonrpc.h"

#include <algorithm>
#include <random>

JsonRPC::JsonRPC() {
  reset();
}

void JsonRPC::reset() {
  m_request.clear();
  m_response.clear();
  m_id.clear();
  m_params.clear();
  m_method.clear();
}

bool JsonRPC::parseRequest(const std::string& request) {
  reset();
  const Json::CharReaderBuilder builder;
  std::string errs;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;

  if (!reader->parse(request.c_str(), request.c_str() + request.size(), &root, &errs)) {
    std::ranges::replace(errs,'\n','-');
    error_response(-32700,errs);
    return false;
  }
  m_request = root;
  if (!m_request.isMember("jsonrpc")) {
    error_response(-32600,std::string("jsonrpc field missing"));
    return false;
  }
  if (strncmp(m_request["jsonrpc"].asCString(),"2.0",3) != 0) {
    error_response(-32600,std::string("jsonrpc version must be 2.0"));
    return false;
  }
  if (!m_request.isMember("method")) {
    error_response(-32600,std::string("method field missing"));
    return false;
  }
  m_method = m_request["method"].asString();
  if (m_request.isMember("id")) {
    m_id = m_request["id"].asString();
  }
  if (m_request.isMember("params"))
    m_params = m_request["params"];
  return true;
}

static std::map<int,std::string> s_codeToMessage {
  { -32700, "Parse error" },
  { -32600, "Invalid request" },
  { -32601, "Method not found" },
  { -32602, "Invalid params" },
  { -32603, "Internal error" }
};

void JsonRPC::error_response(int code,const std::string& errs) {
  if (m_id.empty() && code != -32700) {
    m_response.clear();
    return;
  }
  m_response["jsonrpc"] = "2.0";
  m_response["error"]["code"] = code;
  auto it = s_codeToMessage.find(code);
  if (it != s_codeToMessage.end()) {
    m_response["error"]["message"] = it->second;
  } else {
    m_response["error"]["message"] = "Unknown error";
  }
  if (!m_response["error"].isMember("data"))
    m_response["error"]["data"] = errs;
  m_response["id"] = m_id;
}

void JsonRPC::error_response(int code,const Json::Value& errs) {
  if (m_id.empty() && code != -32700) {
    m_response.clear();
    return;
  }
  m_response = errs;
  m_response["jsonrpc"] = "2.0";
  m_response["error"]["code"] = code;
  auto it = s_codeToMessage.find(code);
  if (it != s_codeToMessage.end()) {
    m_response["error"]["message"] = it->second;
  } else {
    m_response["error"]["message"] = "Unknown error";
  }
  if (!m_response["error"].isMember("data"))
    m_response["error"]["data"] = errs;
  m_response["id"] = m_id;
}

void JsonRPC::register_error_code(int code, const std::string &message) {
  if (auto it = s_codeToMessage.find(code); it != s_codeToMessage.end()) {
    // dont allow overwriting of default error messages
    if (code != -32700 && code != -32600 && code != -32603 && code != -32602 && code != -32601) {
      it->second = message;
    }
  } else {
    s_codeToMessage[code] = message;
  }
}

void JsonRPC::setResponse(const Json::Value &response) {
  if (m_id.empty()) {
    m_response.clear();
    return;
  }
  m_response["jsonrpc"] = "2.0";
  m_response["id"] = m_id;
  m_response["result"] = response;
}

void JsonRPC::set_id(const std::string &id) {
  if (id == JSONRPC_RANDOM_ID) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> distrib(0, INT32_MAX);
    const int random_num = distrib(gen);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", random_num);
    m_id = buf;
  } else {
    m_id = id;
  }
}

std::string JsonRPC::responseText() const {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  const std::string rv = Json::writeString(builder, m_response);
  return rv;
}

std::string JsonRPC::requestText() const {
  Json::Value request;
  request["jsonrpc"] = "2.0";
  if (!m_id.empty())
    request["id"] = m_id;
  request["method"] = m_method;
  if (!m_params.isNull())
    request["params"] = m_params;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  const std::string rv = Json::writeString(builder, request);
  return rv;
}
