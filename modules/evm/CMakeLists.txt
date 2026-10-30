find_package(nlohmann_json CONFIG REQUIRED)

add_library(evm STATIC
    src/abi.cpp
    src/client.cpp
    src/client_c.cpp
    src/hex.cpp
    src/networks.cpp
    src/transaction.cpp
    src/u256.cpp
)
add_library(c_share::evm ALIAS evm)

target_include_directories(evm
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# client.hpp takes an http::Client, so http is part of this module's
# interface; crypto (keys, signatures, Keccak-256) and nlohmann_json (the
# JSON-RPC and ABI values) stay behind it.
target_link_libraries(evm
    PUBLIC
        c_share::http
    PRIVATE
        c_share::crypto
        nlohmann_json::nlohmann_json
)

c_share_set_compile_options(evm)
