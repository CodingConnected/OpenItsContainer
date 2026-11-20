add_library(tlcfi_tcp STATIC
    ITransport.hpp
    TcpClientTransport.cpp
    TcpClientTransport.hpp
    TcpServer.hpp
    TcpServer.cpp
    TcpServerTransport.hpp
    TcpServerTransport.cpp
    TlsConfig.hpp
)

target_include_directories(tlcfi_tcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if (WIN32)
    target_link_libraries(tlcfi_tcp PRIVATE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(tlcfi_tcp PRIVATE Threads::Threads)
endif()

target_compile_definitions(tlcfi_tcp PUBLIC ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_include_directories(tlcfi_tcp PUBLIC ${ASIO_INCLUDE_DIR})

# OpenSSL for TLS support
# OpenSSL is found and imported targets are created in the root CMakeLists.txt
# Link as PUBLIC so dependent targets get OpenSSL includes (needed for ASIO SSL headers)
target_link_libraries(tlcfi_tcp PUBLIC OpenSSL::SSL OpenSSL::Crypto)

if (MSVC)
    target_compile_options(tlcfi_tcp PUBLIC /EHsc)
    target_compile_definitions(tlcfi_tcp PUBLIC _WIN32_WINNT=0x0601)
endif()


