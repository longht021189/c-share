# Each module is a self-contained static library with its own CMakeLists.txt.
# Add new modules here as they are created.
add_subdirectory(waker)

if(C_SHARE_BUILD_HTTP)
    add_subdirectory(http)
endif()

if(C_SHARE_BUILD_CRYPTO)
    add_subdirectory(crypto)
endif()

# Builds on http (its JSON-RPC) and crypto (keys, signatures).
if(C_SHARE_BUILD_EVM)
    add_subdirectory(evm)
endif()
