pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
pkg_check_modules(CPPUNIT REQUIRED IMPORTED_TARGET cppunit)

add_executable(test_jsonrpc tst_jsonrpc.cpp)
target_link_libraries(test_jsonrpc PRIVATE jsonrpc PkgConfig::JSONCPP PkgConfig::CPPUNIT)