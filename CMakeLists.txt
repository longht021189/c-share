cmake_minimum_required(VERSION 3.21)

# Declared before project(): vcpkg installs the manifest's dependencies there,
# and the tests' (Catch2) only when they are built. A consumer building
# c-share as a dependency (e.g. r-share's build.rs) leaves them off.
option(C_SHARE_BUILD_TESTS "Build c-share tests" OFF)
if(C_SHARE_BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES tests)
endif()

project(c-share
    VERSION 0.1.0
    LANGUAGES C CXX
)

# Android/NDK is a target platform, so we cap the language standard at C++17.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# This project only ever produces static libraries.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

# Static libs get linked into shared consumers (e.g. an Android .so), so build
# every object as position-independent regardless of platform default.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CompilerOptions)

option(C_SHARE_BUILD_EXAMPLES "Build c-share example executables" ON)
option(C_SHARE_BUILD_HTTP "Build the http module (requires libcurl/wolfSSL via vcpkg)" ON)
option(C_SHARE_BUILD_CRYPTO "Build the crypto module (requires wolfSSL via vcpkg)" ON)
# On by default, but only with both modules it builds on: a consumer that
# turns http or crypto off (e.g. r-share's build.rs) gets no evm either.
include(CMakeDependentOption)
cmake_dependent_option(C_SHARE_BUILD_EVM "Build the evm module (requires the http and crypto modules)" ON
    "C_SHARE_BUILD_HTTP;C_SHARE_BUILD_CRYPTO" OFF)
option(C_SHARE_COVERAGE "Instrument c-share's targets for Clang source-based coverage" OFF)

if(C_SHARE_COVERAGE AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "C_SHARE_COVERAGE needs Clang or AppleClang, not ${CMAKE_CXX_COMPILER_ID}")
endif()
set(C_SHARE_SANITIZE "" CACHE STRING
    "Clang/GCC sanitizers for c-share's targets, as -fsanitize= takes them (e.g. address,undefined)")

if(C_SHARE_SANITIZE AND MSVC)
    message(FATAL_ERROR "C_SHARE_SANITIZE is for Clang and GCC")
endif()

add_subdirectory(modules)

if(C_SHARE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(C_SHARE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
