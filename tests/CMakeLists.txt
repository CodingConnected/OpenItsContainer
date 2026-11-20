cmake_minimum_required(VERSION 3.20)

# Fetch Google Test
include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)  # keep gtest out of the SDK install
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(googletest)

# Test helper library (header-only, so use INTERFACE)
add_library(tests_helpers INTERFACE)

target_include_directories(tests_helpers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/tcp  # Reuse test helpers from Boost UT tests
    ${CMAKE_SOURCE_DIR}/tests/tcp/unit  # TCP unit test helpers
    ${CMAKE_SOURCE_DIR}/tests/jsonrpc  # JSON-RPC test helpers
)

target_link_libraries(tests_helpers INTERFACE
    tlcfi_tcp
    tlcfi_jsonrpc
)

if (WIN32)
    target_link_libraries(tests_helpers INTERFACE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(tests_helpers INTERFACE Threads::Threads)
endif()

target_compile_definitions(tests_helpers INTERFACE ASIO_STANDALONE)
target_include_directories(tests_helpers INTERFACE ${asio_SOURCE_DIR}/asio/include)

# OpenSSL includes needed when ASIO SSL headers are included
if(OpenSSL_FOUND)
    target_link_libraries(tests_helpers INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()

if (MSVC)
    target_compile_options(tests_helpers INTERFACE /EHsc)
    target_compile_definitions(tests_helpers INTERFACE _WIN32_WINNT=0x0601)
endif()

# TcpServer tests (integration)
add_executable(test_tcp_server
    tcp/integration/test_tcp_server.cpp
)

target_link_libraries(test_tcp_server PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

# Add test to CTest with integration label
add_test(NAME Test_TcpServerTests COMMAND $<TARGET_FILE:test_tcp_server>)
set_tests_properties(Test_TcpServerTests PROPERTIES LABELS "integration")

# TcpServerTransport tests (integration)
add_executable(test_tcp_server_transport
    tcp/integration/test_tcp_server_transport.cpp
)

target_link_libraries(test_tcp_server_transport PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

# Add test to CTest with integration label
add_test(NAME Test_TcpServerTransportTests COMMAND $<TARGET_FILE:test_tcp_server_transport>)
set_tests_properties(Test_TcpServerTransportTests PROPERTIES LABELS "integration")

# TcpClientTransport tests (integration)
add_executable(test_tcp_client_transport
    tcp/integration/test_tcp_client_transport.cpp
)

target_link_libraries(test_tcp_client_transport PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

# Add test to CTest with integration label
add_test(NAME Test_TcpClientTransportTests COMMAND $<TARGET_FILE:test_tcp_client_transport>)
set_tests_properties(Test_TcpClientTransportTests PROPERTIES LABELS "integration")

# Integration tests
add_executable(test_integration
    tcp/integration/test_integration.cpp
)

target_link_libraries(test_integration PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

# Add test to CTest with integration label
add_test(NAME Test_IntegrationTests COMMAND $<TARGET_FILE:test_integration>)
set_tests_properties(Test_IntegrationTests PROPERTIES LABELS "integration")

# TCP Unit Tests (fast, mock-based)
# TcpClientTransport unit tests
add_executable(test_tcp_client_transport_unit
    tcp/unit/test_tcp_client_transport_unit.cpp
)

target_link_libraries(test_tcp_client_transport_unit PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_TcpClientTransportUnitTests COMMAND $<TARGET_FILE:test_tcp_client_transport_unit>)
set_tests_properties(Test_TcpClientTransportUnitTests PROPERTIES LABELS "unit")

# TcpServerTransport unit tests
add_executable(test_tcp_server_transport_unit
    tcp/unit/test_tcp_server_transport_unit.cpp
)

target_link_libraries(test_tcp_server_transport_unit PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_TcpServerTransportUnitTests COMMAND $<TARGET_FILE:test_tcp_server_transport_unit>)
set_tests_properties(Test_TcpServerTransportUnitTests PROPERTIES LABELS "unit")

# TcpServer unit tests
add_executable(test_tcp_server_unit
    tcp/unit/test_tcp_server_unit.cpp
)

target_link_libraries(test_tcp_server_unit PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_TcpServerUnitTests COMMAND $<TARGET_FILE:test_tcp_server_unit>)
set_tests_properties(Test_TcpServerUnitTests PROPERTIES LABELS "unit")

# BalancedJsonFramer tests
add_executable(test_balanced_json_framer
    jsonrpc/test_balanced_json_framer.cpp
)

target_link_libraries(test_balanced_json_framer PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

# Add test to CTest with unit label
add_test(NAME Test_BalancedJsonFramerTests COMMAND $<TARGET_FILE:test_balanced_json_framer>)
set_tests_properties(Test_BalancedJsonFramerTests PROPERTIES LABELS "unit")

# ContentLengthFramer tests (framer is unused by app code but kept as an alternative)
add_executable(test_content_length_framer
    jsonrpc/test_content_length_framer.cpp
)

target_link_libraries(test_content_length_framer PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_ContentLengthFramerTests COMMAND $<TARGET_FILE:test_content_length_framer>)
set_tests_properties(Test_ContentLengthFramerTests PROPERTIES LABELS "unit")

# NdjsonFramer tests (framer is unused by app code but kept as an alternative)
add_executable(test_ndjson_framer
    jsonrpc/test_ndjson_framer.cpp
)

target_link_libraries(test_ndjson_framer PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_NdjsonFramerTests COMMAND $<TARGET_FILE:test_ndjson_framer>)
set_tests_properties(Test_NdjsonFramerTests PROPERTIES LABELS "unit")

# JsonRpcDispatcher tests
add_executable(test_jsonrpc_dispatcher
    jsonrpc/test_jsonrpc_dispatcher.cpp
)

target_link_libraries(test_jsonrpc_dispatcher PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

# Add test to CTest with unit label
add_test(NAME Test_JsonRpcDispatcherTests COMMAND $<TARGET_FILE:test_jsonrpc_dispatcher>)
set_tests_properties(Test_JsonRpcDispatcherTests PROPERTIES LABELS "unit")

# PendingRequestManager tests
add_executable(test_pending_request_manager
    jsonrpc/test_pending_request_manager.cpp
)

target_link_libraries(test_pending_request_manager PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

# Add test to CTest with unit label
add_test(NAME Test_PendingRequestManagerTests COMMAND $<TARGET_FILE:test_pending_request_manager>)
set_tests_properties(Test_PendingRequestManagerTests PROPERTIES LABELS "unit")

# JsonRpcSession tests
add_executable(test_jsonrpc_session
    jsonrpc/test_jsonrpc_session.cpp
)

target_link_libraries(test_jsonrpc_session PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

# Add test to CTest with unit label
add_test(NAME Test_JsonRpcSessionTests COMMAND $<TARGET_FILE:test_jsonrpc_session>)
set_tests_properties(Test_JsonRpcSessionTests PROPERTIES LABELS "unit")

# TLS Tests
# Test certificate helper library
add_library(tls_test_certs STATIC
    tcp/tls/tls_test_certs.cpp
)

target_include_directories(tls_test_certs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(tls_test_certs PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
)

if (WIN32)
    target_link_libraries(tls_test_certs PRIVATE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(tls_test_certs PRIVATE Threads::Threads)
endif()

target_compile_definitions(tls_test_certs PRIVATE ASIO_STANDALONE)
target_include_directories(tls_test_certs PRIVATE ${asio_SOURCE_DIR}/asio/include)

# TlsConfig unit tests
add_executable(test_tls_config
    tcp/tls/test_tls_config.cpp
)

target_link_libraries(test_tls_config PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_TlsConfigTests COMMAND $<TARGET_FILE:test_tls_config>)
set_tests_properties(Test_TlsConfigTests PROPERTIES LABELS "unit")

# TcpServer TLS unit tests
add_executable(test_tcp_server_tls
    tcp/tls/test_tcp_server_tls.cpp
)

target_link_libraries(test_tcp_server_tls PRIVATE
    tests_helpers
    tls_test_certs
    GTest::gtest
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_test(NAME Test_TcpServerTlsTests COMMAND $<TARGET_FILE:test_tcp_server_tls>)
set_tests_properties(Test_TcpServerTlsTests PROPERTIES LABELS "unit")

# TcpServerTransport TLS unit tests
add_executable(test_tcp_server_transport_tls
    tcp/tls/test_tcp_server_transport_tls.cpp
)

target_link_libraries(test_tcp_server_transport_tls PRIVATE
    tests_helpers
    tls_test_certs
    GTest::gtest
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_test(NAME Test_TcpServerTransportTlsTests COMMAND $<TARGET_FILE:test_tcp_server_transport_tls>)
set_tests_properties(Test_TcpServerTransportTlsTests PROPERTIES LABELS "unit")

# TcpClientTransport TLS unit tests
add_executable(test_tcp_client_transport_tls
    tcp/tls/test_tcp_client_transport_tls.cpp
)

target_link_libraries(test_tcp_client_transport_tls PRIVATE
    tests_helpers
    tls_test_certs
    GTest::gtest
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_test(NAME Test_TcpClientTransportTlsTests COMMAND $<TARGET_FILE:test_tcp_client_transport_tls>)
set_tests_properties(Test_TcpClientTransportTlsTests PROPERTIES LABELS "unit")

# TLS integration tests
add_executable(test_tls_integration
    tcp/tls/test_tls_integration.cpp
)

target_link_libraries(test_tls_integration PRIVATE
    tests_helpers
    tls_test_certs
    GTest::gtest
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_test(NAME Test_TlsIntegrationTests COMMAND $<TARGET_FILE:test_tls_integration>)
set_tests_properties(Test_TlsIntegrationTests PROPERTIES LABELS "integration")

# TLS error scenario tests
add_executable(test_tls_errors
    tcp/tls/test_tls_errors.cpp
)

target_link_libraries(test_tls_errors PRIVATE
    tests_helpers
    tls_test_certs
    GTest::gtest
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_test(NAME Test_TlsErrorTests COMMAND $<TARGET_FILE:test_tls_errors>)
set_tests_properties(Test_TlsErrorTests PROPERTIES LABELS "integration")

# TLS negative tests (mutual-TLS / cert rejection)
add_executable(test_tls_mutual
    tcp/tls/test_tls_mutual.cpp
)

target_link_libraries(test_tls_mutual PRIVATE
    tests_helpers
    tls_test_certs
    GTest::gtest
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_test(NAME Test_TlsMutualTests COMMAND $<TARGET_FILE:test_tls_mutual>)
set_tests_properties(Test_TlsMutualTests PROPERTIES LABELS "integration")

# FI tests
add_executable(test_fi_session
    fi/FiSessionTests.cpp
)

target_link_libraries(test_fi_session PRIVATE
    tests_helpers
    tlcfi_fi
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_FiSession COMMAND $<TARGET_FILE:test_fi_session>)
set_tests_properties(Test_FiSession PROPERTIES LABELS "unit")

add_executable(test_fi_consumer_app
    fi/FiConsumerAppTests.cpp
)

target_link_libraries(test_fi_consumer_app PRIVATE
    tests_helpers
    tlcfi_fi
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_FiConsumerApp COMMAND $<TARGET_FILE:test_fi_consumer_app>)
set_tests_properties(Test_FiConsumerApp PROPERTIES LABELS "integration")

add_executable(test_fi_app_host
    fi/FiAppHostTests.cpp
)

target_link_libraries(test_fi_app_host PRIVATE
    tests_helpers
    tlcfi_fi
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_FiAppHost COMMAND $<TARGET_FILE:test_fi_app_host>)
set_tests_properties(Test_FiAppHost PROPERTIES LABELS "integration")

add_executable(test_fi_controller_app
    fi/FiControllerAppTests.cpp
)

target_link_libraries(test_fi_controller_app PRIVATE
    tests_helpers
    tlcfi_fi
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_FiControllerApp COMMAND $<TARGET_FILE:test_fi_controller_app>)
set_tests_properties(Test_FiControllerApp PROPERTIES LABELS "integration")

# Functional TLC-FI tests (NotifyEvent, protocol errors, watchdog, EndControl)
add_executable(test_fi_functional
    fi/test_fi_functional.cpp
)

target_link_libraries(test_fi_functional PRIVATE
    tests_helpers
    tlcfi_fi
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_FiFunctional COMMAND $<TARGET_FILE:test_fi_functional>)
set_tests_properties(Test_FiFunctional PROPERTIES LABELS "integration")

# Control-state logic tests (TLC-FI §4.7.2 decision-table client-side coverage)
add_executable(test_fi_control_state_logic
    fi/test_fi_control_state_logic.cpp
)

target_link_libraries(test_fi_control_state_logic PRIVATE
    tests_helpers
    tlcfi_fi
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_FiControlStateLogic COMMAND $<TARGET_FILE:test_fi_control_state_logic>)
set_tests_properties(Test_FiControlStateLogic PROPERTIES LABELS "integration")

# Model serializer round-trip tests (gen-FI / TLC-FI to_json/from_json)
add_executable(test_serializers
    models/test_serializers.cpp
)

target_link_libraries(test_serializers PRIVATE
    tests_helpers
    tlcfi_fi
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_Serializers COMMAND $<TARGET_FILE:test_serializers>)
set_tests_properties(Test_Serializers PROPERTIES LABELS "unit")

# CIF integration tests (actuated controller + CifBridge + CifHost)
add_executable(test_cif_integration
    cif/CifIntegrationTests.cpp
)

target_link_libraries(test_cif_integration PRIVATE
    tests_helpers
    tlcfi_fi
    tlcfi_cif
    actuated_controller
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_CifIntegration COMMAND $<TARGET_FILE:test_cif_integration>)
set_tests_properties(Test_CifIntegration PROPERTIES LABELS "integration")

# CifMessageServer tests (TCP bridge over CIF IBER/UBER ring buffers)
add_executable(test_cif_message_server
    cif/test_cif_message_server.cpp
)

target_link_libraries(test_cif_message_server PRIVATE
    tests_helpers
    tlcfi_cif
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_CifMessageServer COMMAND $<TARGET_FILE:test_cif_message_server>)
set_tests_properties(Test_CifMessageServer PROPERTIES LABELS "integration")

# Container runtime configuration tests
add_executable(test_runtime_config
    runtime/RuntimeConfigTests.cpp
)

target_link_libraries(test_runtime_config PRIVATE
    tlcfi_runtime
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_RuntimeConfig COMMAND $<TARGET_FILE:test_runtime_config>)
set_tests_properties(Test_RuntimeConfig PROPERTIES LABELS "unit")

# CIF bridge end-to-end tests (TLC-FI UpdateState -> CIF_WUS/CIF_IS)
add_executable(test_cif_bridge_e2e
    cif/test_cif_bridge_e2e.cpp
)

target_link_libraries(test_cif_bridge_e2e PRIVATE
    tests_helpers
    tlcfi_fi
    tlcfi_cif
    actuated_controller
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_CifBridgeE2E COMMAND $<TARGET_FILE:test_cif_bridge_e2e>)
set_tests_properties(Test_CifBridgeE2E PROPERTIES LABELS "unit")

# Param1Store unit tests (CIF_PARM1 persistence file: sparse JSON + writer thread)
add_executable(test_param1_store
    cif/Param1StoreTests.cpp
)

target_link_libraries(test_param1_store PRIVATE
    tests_helpers
    tlcfi_cif
    nlohmann_json::nlohmann_json
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_Param1Store COMMAND $<TARGET_FILE:test_param1_store>)
set_tests_properties(Test_Param1Store PROPERTIES LABELS "unit")

# Logging tests: makeTimestampedLogger level filtering and RollingFileStream rotation
add_executable(test_logging
    util/LoggingTests.cpp
)

target_link_libraries(test_logging PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_Logging COMMAND $<TARGET_FILE:test_logging>)
set_tests_properties(Test_Logging PROPERTIES LABELS "unit")

# PTP ring buffer unit tests (SPSC lock-free byte ring; header-only)
add_executable(test_ptp_ring
    cif/ptp/PtpRingTests.cpp
)

target_link_libraries(test_ptp_ring PRIVATE
    tests_helpers
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_PtpRing COMMAND $<TARGET_FILE:test_ptp_ring>)
set_tests_properties(Test_PtpRing PROPERTIES LABELS "unit")

# PTP link-set unit tests (per-portnr byte routing; pulls only PtpLinkSet/PtpLink
# from tlcfi_ptp — NOT PtpManager/PtpTransport — so no controller is needed).
add_executable(test_ptp_link_set
    cif/ptp/PtpLinkSetTests.cpp
)

target_link_libraries(test_ptp_link_set PRIVATE
    tests_helpers
    tlcfi_ptp
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_PtpLinkSet COMMAND $<TARGET_FILE:test_ptp_link_set>)
set_tests_properties(Test_PtpLinkSet PROPERTIES LABELS "unit")

# PTP C-ABI smoke test (extern "C" driver symbols + lazy PtpManager singleton).
# References the C symbols, which pull PtpTransport + PtpManager, which reference
# runtime::activeOptions() — hence the tlcfi_runtime link (its own isolated TU,
# so no controller is dragged in).
add_executable(test_ptp_transport
    cif/ptp/PtpTransportTests.cpp
)

target_link_libraries(test_ptp_transport PRIVATE
    tests_helpers
    tlcfi_ptp
    tlcfi_runtime
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_PtpTransport COMMAND $<TARGET_FILE:test_ptp_transport>)
set_tests_properties(Test_PtpTransport PROPERTIES LABELS "unit")

# PTP connect-mode integration test (real loopback socket + mock listener):
# dial, duplex byte transparency, and reconnect after the peer drops.
add_executable(test_ptp_connect
    cif/ptp/PtpConnectTests.cpp
)

target_link_libraries(test_ptp_connect PRIVATE
    tests_helpers
    tlcfi_ptp
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_PtpConnect COMMAND $<TARGET_FILE:test_ptp_connect>)
set_tests_properties(Test_PtpConnect PROPERTIES LABELS "integration" TIMEOUT 60)

# PTP listen-mode integration test (real acceptor + mock dialer): peer-IP demux,
# byte transparency, unknown-peer rejection, and re-adoption after reconnect.
add_executable(test_ptp_listen
    cif/ptp/PtpListenTests.cpp
)

target_link_libraries(test_ptp_listen PRIVATE
    tests_helpers
    tlcfi_ptp
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_PtpListen COMMAND $<TARGET_FILE:test_ptp_listen>)
set_tests_properties(Test_PtpListen PROPERTIES LABELS "integration" TIMEOUT 60)

# PTP transport soak/duplex (integration): content-agnostic proof of the byte
# pipe — concurrent full-duplex transfer of random streams (full 0..255 range,
# incl. control bytes) and a sustained bidirectional soak, over two real
# PtpLinkSets on a loopback socket. The transport never interprets PTP framing,
# so byte flow + transparency + volume is the complete test of our side.
add_executable(test_ptp_transport_soak
    cif/ptp/PtpTransportSoakTests.cpp
)

target_link_libraries(test_ptp_transport_soak PRIVATE
    tests_helpers
    tlcfi_ptp
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME Test_PtpTransportSoak COMMAND $<TARGET_FILE:test_ptp_transport_soak>)
set_tests_properties(Test_PtpTransportSoak PROPERTIES LABELS "integration" TIMEOUT 60)
