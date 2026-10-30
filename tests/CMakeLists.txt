# Unit tests, one Catch2 executable per module (enable with
# -DC_SHARE_BUILD_TESTS=ON, which the linux/macos/windows presets do). Run them
# with `ctest --preset <platform>`, or each executable directly.
find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# Catch2's macros use __COUNTER__, which newer Clang's -Wpedantic reports as a
# C2y extension; older Clang does not know that warning, hence the first flag.
function(c_share_set_test_options target)
    c_share_set_compile_options(${target})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE -Wno-unknown-warning-option -Wno-c2y-extensions)
    endif()
endfunction()

add_executable(waker_tests waker/waker_test.cpp support/socket_pair.cpp)
target_include_directories(waker_tests PRIVATE support)
target_link_libraries(waker_tests PRIVATE c_share::waker Catch2::Catch2WithMain)
if(WIN32)
    target_link_libraries(waker_tests PRIVATE ws2_32)
endif()
c_share_set_test_options(waker_tests)
catch_discover_tests(waker_tests)

if(C_SHARE_BUILD_CRYPTO)
    add_executable(crypto_tests crypto/crypto_test.cpp)
    target_link_libraries(crypto_tests PRIVATE c_share::crypto Catch2::Catch2WithMain)
    c_share_set_test_options(crypto_tests)
    catch_discover_tests(crypto_tests)
endif()

if(C_SHARE_BUILD_HTTP)
    find_package(CURL REQUIRED)
    find_package(ZLIB REQUIRED)

    add_executable(http_tests
        http/client_c_test.cpp
        http/client_test.cpp
        http/ws_codec_test.cpp
        support/socket_pair.cpp
        support/test_server.cpp
    )
    # ws_codec.hpp is internal to the http module.
    target_include_directories(http_tests PRIVATE ${PROJECT_SOURCE_DIR}/modules/http/src support)
    # libcurl's header for curl_global_init (see client.hpp), zlib's for the
    # test server's side of compression. c_share::http already links both.
    target_include_directories(http_tests PRIVATE
        $<TARGET_PROPERTY:CURL::libcurl,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:ZLIB::ZLIB,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_link_libraries(http_tests PRIVATE c_share::http Catch2::Catch2WithMain)
    if(WIN32)
        target_link_libraries(http_tests PRIVATE ws2_32)
    endif()
    c_share_set_test_options(http_tests)
    catch_discover_tests(http_tests)
endif()

if(C_SHARE_BUILD_EVM)
    find_package(nlohmann_json CONFIG REQUIRED)

    add_executable(evm_tests
        evm/abi_test.cpp
        evm/client_c_test.cpp
        evm/client_test.cpp
        evm/networks_test.cpp
        evm/transaction_test.cpp
        support/socket_pair.cpp
        support/test_server.cpp
    )
    # abi.hpp, transaction.hpp, ... are internal to the evm module.
    target_include_directories(evm_tests PRIVATE ${PROJECT_SOURCE_DIR}/modules/evm/src support)
    # As for http_tests: libcurl's and zlib's headers only, for
    # curl_global_init and the test server.
    target_include_directories(evm_tests PRIVATE
        $<TARGET_PROPERTY:CURL::libcurl,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:ZLIB::ZLIB,INTERFACE_INCLUDE_DIRECTORIES>
    )
    # crypto.h for the transaction tests' signatures; c_share::evm already
    # links crypto.
    target_include_directories(evm_tests PRIVATE
        $<TARGET_PROPERTY:c_share::crypto,INTERFACE_INCLUDE_DIRECTORIES>)
    target_link_libraries(evm_tests PRIVATE c_share::evm nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
    if(WIN32)
        target_link_libraries(evm_tests PRIVATE ws2_32)
    endif()
    c_share_set_test_options(evm_tests)
    catch_discover_tests(evm_tests)
endif()
