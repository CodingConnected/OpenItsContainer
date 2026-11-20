add_library(tlcfi_fi STATIC
    # Core FI implementation
    FiSession.cpp
    FiConsumerApp.cpp
    FiControllerApp.cpp
    FiFacilitiesServer.cpp
    FiAppHost.cpp
    # Model JSON serialization
    ../models/gen-fi/Json.cpp
    ../models/tlc-fi/Json.cpp
)

target_include_directories(tlcfi_fi PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(tlcfi_fi PUBLIC
    tlcfi_tcp
    tlcfi_jsonrpc
    nlohmann_json::nlohmann_json
)

# Define ASIO as standalone (not using Boost)
target_compile_definitions(tlcfi_fi PUBLIC ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_include_directories(tlcfi_fi PUBLIC ${ASIO_INCLUDE_DIR})

# Platform-specific settings
if (MSVC)
    target_compile_options(tlcfi_fi PRIVATE /EHsc)
    target_compile_definitions(tlcfi_fi PUBLIC _WIN32_WINNT=0x0601)
endif()

if (WIN32)
    target_link_libraries(tlcfi_fi PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(tlcfi_fi PUBLIC Threads::Threads)
endif()

