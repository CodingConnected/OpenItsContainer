add_library(tlcfi_jsonrpc STATIC
    RpcTypes.hpp
    IMessageFramer.hpp
    ContentLengthFramer.cpp
    ContentLengthFramer.hpp
    NdjsonFramer.cpp
    NdjsonFramer.hpp
    BalancedJsonFramer.cpp
    BalancedJsonFramer.hpp
    PendingRequestManager.hpp
    PendingRequestManager.cpp
    JsonRpcDispatcher.hpp
    JsonRpcDispatcher.cpp
    JsonRpcSession.hpp
    JsonRpcSession.cpp
)

target_include_directories(tlcfi_jsonrpc PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../tcp
)

target_link_libraries(tlcfi_jsonrpc
    PUBLIC
        nlohmann_json::nlohmann_json
        tlcfi_tcp
)   
if (WIN32)
    target_link_libraries(tlcfi_jsonrpc PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(tlcfi_jsonrpc PRIVATE Threads::Threads)
endif()

target_compile_definitions(tlcfi_jsonrpc PUBLIC ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_include_directories(tlcfi_jsonrpc PUBLIC ${ASIO_INCLUDE_DIR})

if (MSVC)
    target_compile_options(tlcfi_jsonrpc PUBLIC /EHsc)
    target_compile_definitions(tlcfi_jsonrpc PUBLIC _WIN32_WINNT=0x0601)
endif()


