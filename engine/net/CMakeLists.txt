# sr::net - connections between the client and our servers: frames and
# lanes (Framing.h) and TCP + TLS 1.3 on Asio and OpenSSL (Connection.h). Asio and OpenSSL stay
# private: nothing outside this module includes them.
add_library(sr_net STATIC
    src/Framing.cpp
    src/Connection.cpp
    src/Certificate.cpp
)
add_library(sr::net ALIAS sr_net)

target_include_directories(sr_net PUBLIC include)
target_link_libraries(sr_net PUBLIC sr::core PRIVATE asio OpenSSL::SSL OpenSSL::Crypto sr_compile_options)
if(WIN32)
    target_link_libraries(sr_net PRIVATE ws2_32 mswsock crypt32)
endif()

