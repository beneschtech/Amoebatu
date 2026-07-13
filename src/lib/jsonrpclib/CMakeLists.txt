pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)

add_library(jsonrpc SHARED
        jsonrpc.cpp jsonrpc.h
        rpc_processor.cpp rpc_processor.h
        transport_udp.h transport_udp.cpp
)
set_target_properties(jsonrpc PROPERTIES
    VERSION 0.1.0
    SOVERSION 0.1
)
target_link_libraries(jsonrpc PRIVATE PkgConfig::JSONCPP)