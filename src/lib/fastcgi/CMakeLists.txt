pkg_check_modules(FCGI REQUIRED IMPORTED_TARGET fcgi)

add_library(fastcgi STATIC
        fcgi_request_cpp.hxx
        base64.cpp  fcgi_data.cpp  fcgi_listener.cpp
        fcgi_req_parser.cpp  fcgi_request.cpp
        fcgi_response.cpp  httpcodes.cpp  multipart.cpp
        urlencode.cpp  util.cpp
)
