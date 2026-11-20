# add_cvnc_controller(<target>
#     C_SOURCES        <files...>   # the developer's controller C glue (defines
#                                   #   the CIF_* buffers, applicatieprogramma,
#                                   #   getCifItemName)
#     CXX_SOURCES      <files...>   # optional additional C++ sources
#     CONTROLLER_LIBS  <libs...>    # the vendor's prebuilt static .libs
# )
#
# Produces an executable that runs the developer's CVN-C controller inside the
# OpenItsContainer TLC-FI container. The SDK supplies main() (via
# OpenItsContainer::tlcfi_main); the developer supplies only their controller.
#
# ABI: the target is pinned to x64 + the dynamic CRT (/MD, /MDd in Debug) to
# match the prebuilt container libs and the typical MSVC-built vendor .libs.
# Mixing CRTs (/MT) or architectures will not link — see the SDK README.
function(add_cvnc_controller TARGET)
    cmake_parse_arguments(ARG "" "" "C_SOURCES;CXX_SOURCES;CONTROLLER_LIBS" ${ARGN})

    if(NOT ARG_C_SOURCES AND NOT ARG_CXX_SOURCES)
        message(FATAL_ERROR "add_cvnc_controller(${TARGET}): no C_SOURCES or CXX_SOURCES given")
    endif()

    add_executable(${TARGET} ${ARG_C_SOURCES} ${ARG_CXX_SOURCES})

    target_link_libraries(${TARGET} PRIVATE
        OpenItsContainer::tlcfi_main
        ${ARG_CONTROLLER_LIBS})

    # Always /MD (release CRT), even in Debug builds: the container ships a
    # single /MD lib set, and matching it lets the same prebuilt libs (and the
    # vendor's /MD libs) link in any configuration. Debug builds still debug the
    # developer's own sources (/Zi /Od); they just use the release CRT.
    set_target_properties(${TARGET} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

    if(MSVC)
        # Delay-load OpenSSL so a plain-TCP deployment runs without the OpenSSL
        # DLLs present; they are only required at runtime if TLS is used.
        # Override the names if your OpenSSL build uses different DLL names.
        if(NOT DEFINED OPENITS_OPENSSL_DLLS)
            set(OPENITS_OPENSSL_DLLS libssl-3-x64.dll libcrypto-3-x64.dll)
        endif()
        foreach(_dll IN LISTS OPENITS_OPENSSL_DLLS)
            target_link_options(${TARGET} PRIVATE "/DELAYLOAD:${_dll}")
        endforeach()
        target_link_libraries(${TARGET} PRIVATE delayimp)
    endif()
endfunction()
