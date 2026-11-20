cmake_minimum_required(VERSION 3.20)
project(OpenItsContainer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(TLCFI_BUILD_TESTS "Build tlcfi tests" OFF)
set(TLCFI_PROTOCOL_VERSION "2.0.1" CACHE STRING "TLC-FI protocol version (1.1.0 or 2.0.1)")
set_property(CACHE TLCFI_PROTOCOL_VERSION PROPERTY STRINGS "1.1.0" "2.0.1")

# Set version-specific compile definitions
if(TLCFI_PROTOCOL_VERSION STREQUAL "1.1.0")
    add_compile_definitions(TLCFI_VERSION_1_1_0)
    message(STATUS "Building with TLC-FI protocol version 1.1.0")
elseif(TLCFI_PROTOCOL_VERSION STREQUAL "2.0.1")
    add_compile_definitions(TLCFI_VERSION_2_0_1)
    message(STATUS "Building with TLC-FI protocol version 2.0.1")
else()
    message(FATAL_ERROR "Invalid TLCFI_PROTOCOL_VERSION: ${TLCFI_PROTOCOL_VERSION}. Must be '1.1.0' or '2.0.1'")
endif()

# Prefer system packages first (Linux/WSL), fall back to FetchContent.
find_package(asio QUIET CONFIG)
find_package(nlohmann_json QUIET CONFIG)
find_path(ASIO_INCLUDE_DIR NAMES asio.hpp)
find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)

if (TARGET asio::asio)
    message(STATUS "Using system Asio package")
    get_target_property(ASIO_INCLUDE_DIR asio::asio INTERFACE_INCLUDE_DIRECTORIES)
elseif (ASIO_INCLUDE_DIR)
    message(STATUS "Using system Asio headers at: ${ASIO_INCLUDE_DIR}")
else()
    include(FetchContent)
    message(STATUS "System Asio not found, fetching from GitHub")
    FetchContent_Declare(
      asio
      GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
      GIT_TAG asio-1-36-0
      GIT_SHALLOW TRUE
      GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(asio)

    add_library(asio INTERFACE)
    add_library(asio::asio ALIAS asio)
    target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
    set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include")
endif()

if (TARGET nlohmann_json::nlohmann_json)
    message(STATUS "Using system nlohmann_json package")
elseif (NLOHMANN_JSON_INCLUDE_DIR)
    message(STATUS "Using system nlohmann_json headers at: ${NLOHMANN_JSON_INCLUDE_DIR}")
    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json INTERFACE ${NLOHMANN_JSON_INCLUDE_DIR})
else()
    if (NOT COMMAND FetchContent_Declare)
        include(FetchContent)
    endif()
    message(STATUS "System nlohmann_json not found, fetching from GitHub")
    set(JSON_Install OFF CACHE BOOL "" FORCE)  # keep nlohmann out of the SDK install
    FetchContent_Declare(
      nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG v3.12.0
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# OpenSSL for TLS support
# When using vcpkg, OpenSSL is found automatically via find_package.
# For manual installations, set OPENSSL_ROOT_DIR CMake variable.
find_package(OpenSSL REQUIRED)

add_subdirectory(src)

add_subdirectory(examples)

# Tests
if(TLCFI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- SDK install / packaging ---
# Installs the container as prebuilt static libs + the minimal public headers,
# plus a CMake package (find_package(OpenItsContainer) + add_cvnc_controller()).
# A consumer links these with their own CVN-C controller; they never compile
# our sources, so no asio/nlohmann/OpenSSL headers are shipped.
option(OPENITS_INSTALL_SDK "Install the OpenItsContainer SDK (libs, headers, cmake package)" ON)
if(OPENITS_INSTALL_SDK)
    include(CMakePackageConfigHelpers)

    # A single CRT-consistent (/MD) lib set serves both Debug and Release
    # consumer builds (consumers always build /MD; they debug only their own
    # sources, not the container). Build+install the Release config.
    install(TARGETS
            tlcfi_main tlcfi_runtime tlcfi_cif tlcfi_ptp tlcfi_fi tlcfi_tcp tlcfi_jsonrpc
        ARCHIVE DESTINATION lib/x64)

    install(FILES sdk/README.md DESTINATION .)

    install(FILES src/runtime/RuntimeApp.hpp DESTINATION include/runtime)
    install(FILES src/cif/CifController.h src/cif/CIF_INC.h DESTINATION include/cif)
    # RuntimeApp.hpp (public) references ptp::PtpLinkConfig by value, so the POD
    # config header must ship too. (The tlcfi_ptp lib is installed above; PTP is
    # a standalone, force-link-free lib that a consumer's controller pulls in
    # only when it references the PTP driver symbols.)
    install(FILES src/cif/ptp/PtpConfig.hpp DESTINATION include/cif/ptp)

    # cominter.h declares those same driver symbols for the CCOL PTP module.
    # ptpcom.c pulls it in with a BARE `#include "cominter.h"` (when AUTOMAAT is
    # defined), so it must sit on the default include root (<sdk>/include, the
    # only dir the SDK's cmake package and MSVC .props expose) to resolve with
    # no extra include-path setup by the consumer.
    install(FILES src/cif/ptp/cominter.h DESTINATION include)

    install(FILES
            sdk/template/controller.c
            sdk/template/tlcfi.json
        DESTINATION template)

    # Plain-MSVC consumption path: property sheet + sample .sln/.vcxproj.
    install(FILES sdk/msvc/OpenItsContainer.props DESTINATION msvc)
    install(FILES
            sdk/msvc/sample/my_controller.vcxproj
            sdk/msvc/sample/my_controller.sln
        DESTINATION msvc/sample)

    configure_package_config_file(
        cmake/OpenItsContainerConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/OpenItsContainerConfig.cmake
        INSTALL_DESTINATION cmake)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/OpenItsContainerConfigVersion.cmake
        VERSION 0.1.0
        COMPATIBILITY SameMajorVersion)

    install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/OpenItsContainerConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/OpenItsContainerConfigVersion.cmake
            cmake/AddCvncController.cmake
        DESTINATION cmake)
endif()