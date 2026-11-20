add_library(tlcfi_cif STATIC
    CifBridge.cpp
    CifHost.cpp
    CifMessageServer.cpp
    Param1Store.cpp
)

target_include_directories(tlcfi_cif PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/fi
)

target_link_libraries(tlcfi_cif PUBLIC
    tlcfi_fi
    nlohmann_json::nlohmann_json
)

# Define ASIO as standalone (not using Boost)
target_compile_definitions(tlcfi_cif PUBLIC ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_include_directories(tlcfi_cif PUBLIC ${ASIO_INCLUDE_DIR})

# Platform-specific settings
if (MSVC)
    target_compile_options(tlcfi_cif PRIVATE /EHsc)
    target_compile_definitions(tlcfi_cif PUBLIC _WIN32_WINNT=0x0601)
endif()

if (WIN32)
    target_link_libraries(tlcfi_cif PUBLIC ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(tlcfi_cif PUBLIC Threads::Threads)
endif()

# PTP (Point-to-Point) transport: the optional serial/TCP koppelsignaal driver.
#
# Kept in its own static library so it is OPTIONAL at zero cost. A controller
# built WITH PTP references the C driver symbols (lees_char_COM_IBER etc. in
# PtpTransport.cpp), which pulls this whole library into the final binary; a
# controller built WITHOUT PTP references none of them and static-lib linkage
# drops it entirely. The library references tlcfi::runtime::activeOptions()
# (resolved when the final executable links tlcfi_runtime) but NEVER the other
# way round, so the runtime never force-links PTP.
add_library(tlcfi_ptp STATIC
    ptp/PtpLink.cpp
    ptp/PtpLinkSet.cpp
    ptp/PtpListener.cpp
    ptp/PtpManager.cpp
    ptp/PtpTransport.cpp
)

target_include_directories(tlcfi_ptp PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

# tlcfi_cif carries asio (ASIO_STANDALONE + include dir), the platform sockets
# libs and the MSVC flags, all PUBLIC, so PTP needs nothing more here.
target_link_libraries(tlcfi_ptp PUBLIC
    tlcfi_cif
)

if (MSVC)
    target_compile_options(tlcfi_ptp PRIVATE /EHsc)
    target_compile_definitions(tlcfi_ptp PUBLIC _WIN32_WINNT=0x0601)
endif()

# Actuated traffic controller (defines CIF buffers + applicatieprogramma)
add_library(actuated_controller STATIC
    ActuatedController.cpp
)

target_include_directories(actuated_controller PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(actuated_controller PUBLIC
    nlohmann_json::nlohmann_json
)

if (MSVC)
    target_compile_options(actuated_controller PRIVATE /EHsc)
endif()

