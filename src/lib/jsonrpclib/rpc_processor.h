//
// Created by cbenesch on 7/8/26.
//

#ifndef AMEOBATU_RPC_PROCESSOR_H
#define AMEOBATU_RPC_PROCESSOR_H
#include <functional>
#include <string>
#include <map>
#include <atomic>
#include "jsonrpc.h"

class RPCProcessor {
public:
  virtual ~RPCProcessor() = default;
  RPCProcessor();

  /**
   * The default methods for these will likely be enough but can be overridden in derived classes.
   */
  /**
   *
   * @param method the string of the method to check for existence
   * @return true if the method exists, false otherwise
   */
  [[nodiscard]] virtual bool hasMethod(const std::string& method) const;

  /**
   * Processes an incoming JSON-RPC request and executes the corresponding RPC method.
   *
   * @param request a constant reference to the JsonRPC object representing the request to process
   * @return an integer indicating the result of the request processing, where 0 typically denotes success,
   *         or an error code specific to the implementation
   */
  [[nodiscard]] virtual int processRequest(const JsonRPC& request);

  /**
   * Registers a new RPC method with the processor.
   *
   * @param method the string identifier of the method to register
   * @param func a function pointer to the implementation of the method
   * @return true if the method was successfully registered, false if the method already exists
   */
  bool virtual registerMethod(const std::string &method, std::function<int(RPCProcessor *,const Json::Value&, Json::Value&)> func);

protected:
  /**
   * This ping method is used to check if the RPC processor is alive. It also is a template
   * for all other method functions in derived classes.
   * @param params a const reference to the JSON request object
   * @param response a reference to the JSON response object to populate
   * @return 0 for success or the jsonrpc error code to set. If an error is returned,
   * the response object will be discarded except for ["error"]["data"] if it exists.
   */
  int pingMethod(const Json::Value& params, Json::Value& response) const;
  std::atomic_uint64_t m_transCnt;
  mutable std::map<std::string, std::function<int(RPCProcessor *,const Json::Value&, Json::Value&)>> m_methods;
  
};

typedef std::function<int(RPCProcessor *,const Json::Value&, Json::Value&)> RPCCallback;

#endif //AMEOBATU_RPC_PROCESSOR_H
