# Networking dependencies, pinned. Separate from Dependencies.cmake because the tests (and the
# servers) build sr::net without the client.
include(FetchContent)

# Asio (standalone, header-only, BSL-1.0): TCP and the TLS stream. The
# repository has no CMake build; the interface target is defined below.
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-38-2
    GIT_SHALLOW TRUE)

# OpenSSL 3 (TLS 1.3, certificates). Not fetched: its build needs Perl and NASM. On Windows the
# Shining Light installer (C:\Program Files\OpenSSL-Win64, its MSVC import libraries) is
# found below; on Linux the distribution's package.
if(WIN32 AND NOT OPENSSL_ROOT_DIR AND EXISTS "C:/Program Files/OpenSSL-Win64")
    set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
endif()
if(WIN32 AND NOT MSVC AND OPENSSL_ROOT_DIR)
    # clang++ (GNU front end, MSVC ABI): FindOpenSSL looks for MinGW names; point it at the
    # MSVC import libraries (the DLL runtime flavour; both run-time variants import the same DLLs).
    find_library(SSL_EAY NAMES libssl PATHS "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD" NO_DEFAULT_PATH)
    find_library(LIB_EAY NAMES libcrypto PATHS "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD" NO_DEFAULT_PATH)
endif()
find_package(OpenSSL 3.0 REQUIRED)

# cpp-httplib and nlohmann/json: the website's API (SroWeb) and the skill lab.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.59.0
    GIT_SHALLOW TRUE)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(asio httplib nlohmann_json)

add_library(asio INTERFACE)
target_include_directories(asio SYSTEM INTERFACE "${asio_SOURCE_DIR}/include")
target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
if(WIN32)
    target_compile_definitions(asio INTERFACE _WIN32_WINNT=0x0A00)
endif()

# Copies the OpenSSL DLLs next to an executable that links sr::net (Windows).
function(sr_copy_openssl_runtime target)
    if(NOT WIN32)
        return()
    endif()
    get_filename_component(openssl_root "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
    foreach(dll libssl-3-x64.dll libcrypto-3-x64.dll)
        find_file(SR_OPENSSL_DLL_${dll} NAMES ${dll} PATHS "${openssl_root}" "${openssl_root}/bin" NO_DEFAULT_PATH)
        if(NOT SR_OPENSSL_DLL_${dll})
            message(FATAL_ERROR "OpenSSL runtime ${dll} not found under ${openssl_root}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SR_OPENSSL_DLL_${dll}}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endforeach()
endfunction()
