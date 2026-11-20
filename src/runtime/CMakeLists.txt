# Container runtime: the generic, controller-agnostic entry point (runContainer).
# Hosts whatever CVN-C controller is linked into the final binary.
add_library(tlcfi_runtime STATIC
    RuntimeApp.cpp
    RuntimeActiveOptions.cpp
    RuntimeConfig.cpp
    RuntimeApp.hpp
)

target_include_directories(tlcfi_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

# tlcfi_cif brings in tlcfi_fi, ASIO_STANDALONE, the asio include dir,
# /EHsc and _WIN32_WINNT (all PUBLIC), so the runtime needs nothing more.
target_link_libraries(tlcfi_runtime PUBLIC
    tlcfi_cif
)

# Used by loadTlcfiConfig() to parse the tlcfi.json connection file.
target_link_libraries(tlcfi_runtime PRIVATE
    nlohmann_json::nlohmann_json
)

if (MSVC)
    target_compile_options(tlcfi_runtime PRIVATE /EHsc)
endif()

# SDK entry point: a static lib carrying main(). The CRT pulls main() from the
# archive, so a consumer links this and supplies only their controller.
add_library(tlcfi_main STATIC
    main.cpp
)

target_link_libraries(tlcfi_main PUBLIC
    tlcfi_runtime
)

if (MSVC)
    target_compile_options(tlcfi_main PRIVATE /EHsc)
endif()
