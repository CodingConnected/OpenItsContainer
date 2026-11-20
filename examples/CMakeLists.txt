# TLC-FI Examples

# Consumer Monitor Example
add_executable(consumer_app
    consumer_app.cpp
)

target_link_libraries(consumer_app PRIVATE
    tlcfi_fi
    nlohmann_json::nlohmann_json
)

# Define ASIO as standalone (not using Boost)
target_compile_definitions(consumer_app PRIVATE ASIO_STANDALONE)
target_include_directories(consumer_app PRIVATE ${asio_SOURCE_DIR}/asio/include)

# Platform-specific settings
if (MSVC)
    target_compile_options(consumer_app PRIVATE /EHsc)
    target_compile_definitions(consumer_app PRIVATE _WIN32_WINNT=0x0601)
endif()

if (WIN32)
    target_link_libraries(consumer_app PRIVATE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(consumer_app PRIVATE Threads::Threads)
endif()

# Control Application Example
add_executable(control_app
    control_app.cpp
)

target_link_libraries(control_app PRIVATE
    tlcfi_fi
    nlohmann_json::nlohmann_json
)

# Define ASIO as standalone (not using Boost)
target_compile_definitions(control_app PRIVATE ASIO_STANDALONE)
target_include_directories(control_app PRIVATE ${asio_SOURCE_DIR}/asio/include)

# Platform-specific settings
if (MSVC)
    target_compile_options(control_app PRIVATE /EHsc)
    target_compile_definitions(control_app PRIVATE _WIN32_WINNT=0x0601)
endif()

if (WIN32)
    target_link_libraries(control_app PRIVATE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(control_app PRIVATE Threads::Threads)
endif()

# Control Application with CIF Interface Example
add_executable(control_app_cif
    control_app_cif.cpp
)

target_link_libraries(control_app_cif PRIVATE
    tlcfi_runtime
    # PTP transport is on the link line but the actuated controller references
    # none of its C driver symbols, so static-lib linkage drops it entirely —
    # this is the Phase-1 "a non-PTP controller pulls in none of it" check.
    tlcfi_ptp
    actuated_controller
    nlohmann_json::nlohmann_json
)

# Define ASIO as standalone (not using Boost)
target_compile_definitions(control_app_cif PRIVATE ASIO_STANDALONE)
target_include_directories(control_app_cif PRIVATE ${asio_SOURCE_DIR}/asio/include)

# Platform-specific settings
if (MSVC)
    target_compile_options(control_app_cif PRIVATE /EHsc)
    target_compile_definitions(control_app_cif PRIVATE _WIN32_WINNT=0x0601)
endif()

if (WIN32)
    target_link_libraries(control_app_cif PRIVATE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(control_app_cif PRIVATE Threads::Threads)
endif()

# Keep the runtime config files alongside the binary so the source tree is the
# single source of truth (control_app_cif loads controller_config.json from its
# working directory by default).
foreach(cfg controller_config.json tlcfi.json)
    add_custom_command(TARGET control_app_cif POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${cfg}
            $<TARGET_FILE_DIR:control_app_cif>/${cfg}
        COMMENT "Copying ${cfg} to control_app_cif output directory")
endforeach()

