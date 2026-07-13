pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
pkg_check_modules(FCGI REQUIRED IMPORTED_TARGET fcgi)

add_executable(neighbord main.cpp announcer_thread.cpp
        listener_thread.cpp
        HostDirectory.cpp
        HostDirectory.h)

target_link_libraries(neighbord PRIVATE jsonrpc fastcgi PkgConfig::JSONCPP PkgConfig::FCGI)
target_include_directories(neighbord PRIVATE ../lib/jsonrpclib ../lib/fastcgi)