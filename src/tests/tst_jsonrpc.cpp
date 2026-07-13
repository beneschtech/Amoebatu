//
// Created by cbenesch on 7/7/26.
//
#include <getopt.h>
#include <cppunit/TestFixture.h>
#include <cppunit/TestSuite.h>
#include <cppunit/TestCaller.h>
#include <cppunit/TestResult.h>
#include <cppunit/XmlOutputter.h>
#include <cppunit/ui/text/TestRunner.h>
#include "../lib/jsonrpclib/jsonrpc.h"
#include "../lib/jsonrpclib/rpc_processor.h"

class JsonRpcTest : public CppUnit::TestFixture {
  public:
    JsonRpcTest() = default;
    static CppUnit::Test *suite() {
      auto *m_suite = new CppUnit::TestSuite( "JsonRPCTests" );
      m_suite->addTest( new CppUnit::TestCaller<JsonRpcTest>(
                                   "Valid Message",
                                   &JsonRpcTest::testJsonRpc ) );
      m_suite->addTest( new CppUnit::TestCaller<JsonRpcTest>(
                                   "Not Json",
                                   &JsonRpcTest::testNotJson ) );
      m_suite->addTest( new CppUnit::TestCaller<JsonRpcTest>(
                                   "Invalid JSONRPC Messages",
                                   &JsonRpcTest::testJsonInv ) );
      m_suite->addTest( new CppUnit::TestCaller<JsonRpcTest>(
                                   "JSONRpc Processing (ping)",
                                   &JsonRpcTest::testPing ) );
      m_suite->addTest( new CppUnit::TestCaller<JsonRpcTest>(
                                   "JSONRpc Request Text",
                                   &JsonRpcTest::testRequestText ) );
      m_suite->addTest( new CppUnit::TestCaller<JsonRpcTest>(
                                   "End to End",
                                   &JsonRpcTest::endToEnd ) );
      return m_suite;
    }
  protected:
    void testJsonRpc();
    void testNotJson();
    void testJsonInv();
    void testPing();
    void testRequestText();
    void endToEnd();
};

static bool verbose_flag = false;
static bool xml_flag = false;

bool parse_args(const int argc, char* argv[]) {
  int opt;
  while ((opt = getopt(argc,argv,"xv")) != -1) {
    switch (opt) {
      case 'x':
        xml_flag = true;
        break;
      case 'v':
        verbose_flag = true;
        break;
      default:
        std::cerr << "Option " << static_cast<char>(opt) << " not recognized" << std::endl;
        return false;
    }
  }
  return true;
}

void JsonRpcTest::testNotJson() {
  JsonRPC rpc;
  CPPUNIT_ASSERT(!rpc.parseRequest("Not json"));
  CPPUNIT_ASSERT(!rpc.parseRequest("{}}"));
}

void JsonRpcTest::testJsonInv() {
  JsonRPC rpc;
  CPPUNIT_ASSERT(!rpc.parseRequest(R"({"method":"add","params":[1,2],"id":1})"));
  CPPUNIT_ASSERT(!rpc.parseRequest(R"({"jsonrpc":"2.0","params":[1,2],"id":1})"));
}

void JsonRpcTest::testJsonRpc() {
  JsonRPC rpc;
  CPPUNIT_ASSERT(rpc.parseRequest(R"({"jsonrpc":"2.0","method":"add","params":[1,2],"id":1})"));
  CPPUNIT_ASSERT(rpc.method() == "add");
  CPPUNIT_ASSERT(rpc.id() == "1");
  CPPUNIT_ASSERT(rpc.params().size() == 2);
}

void JsonRpcTest::testPing() {
  JsonRPC rpc;
  CPPUNIT_ASSERT(rpc.parseRequest(R"({"jsonrpc":"2.0","method":"ping","id":1})"));
  CPPUNIT_ASSERT(rpc.method() == "ping");
  CPPUNIT_ASSERT(rpc.id() == "1");
  CPPUNIT_ASSERT(rpc.params().size() == 0);
  RPCProcessor prc;
  CPPUNIT_ASSERT(prc.hasMethod("ping"));
  int rv = prc.processRequest(rpc);
  CPPUNIT_ASSERT(rv == 0);
  if (verbose_flag)
    std::cout << "\n" << rpc.responseText() << "\n" << std::endl;
  CPPUNIT_ASSERT(rpc.parseRequest(R"({"jsonrpc":"2.0","method":"pong","id":1})"));
  rv = prc.processRequest(rpc);
  CPPUNIT_ASSERT_EQUAL(rv,-32601);
  if (verbose_flag)
    std::cout << "\n" << rv << ": " <<rpc.responseText() << "\n" << std::endl;
}

void JsonRpcTest::testRequestText() {
  JsonRPC rpc;
  rpc.set_method("ping");
  rpc.set_id(JSONRPC_RANDOM_ID);
  std::string req = rpc.requestText();
  if (verbose_flag)
    std::cout << req << std::endl;
  CPPUNIT_ASSERT(req.length());
}

void JsonRpcTest::endToEnd() {
  JsonRPC rpc_send;
  rpc_send.set_method("ping");
  rpc_send.set_id(JSONRPC_RANDOM_ID);
  std::string req = rpc_send.requestText();
  if (verbose_flag)
    std::cout << req << std::endl;
  CPPUNIT_ASSERT(req.length());
  RPCProcessor rpc_proc;
  JsonRPC rpc_recv;
  CPPUNIT_ASSERT(rpc_recv.parseRequest(req));
  int rv = rpc_proc.processRequest(rpc_recv);
  CPPUNIT_ASSERT(rv == 0);
  if (verbose_flag)
    std::cout << "\n" << rpc_recv.responseText() << "\n" << std::endl;
}

int main(int argc,char *argv[])
{
  if (!parse_args(argc,argv))
    return 1;
  CppUnit::TextUi::TestRunner runner;
  if (xml_flag) {
    runner.setOutputter (new CppUnit::XmlOutputter (&runner.result(),std::cout));
  }
  runner.addTest(JsonRpcTest::suite());
  const bool wasSuccessful = runner.run();
  return wasSuccessful ? 0 : 1;
}

