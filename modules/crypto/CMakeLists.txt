find_package(wolfssl CONFIG REQUIRED)

add_library(crypto STATIC
    src/crypto.cpp
)
add_library(c_share::crypto ALIAS crypto)

target_include_directories(crypto
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# wolfSSL is strictly an implementation detail behind crypto.h's plain C
# API -- not exposed to consumers, so PRIVATE.
target_link_libraries(crypto
    PRIVATE
        wolfssl::wolfssl
)

# wolfssl::wolfssl's CMake package config doesn't propagate this (it's
# "heuristically generated" per vcpkg's own install output), so without it
# every wolfcrypt header silently compiles with none of its feature macros
# defined (HAVE_CHACHA, HAVE_ED25519, ...) -- everything this module calls
# would fail to even compile, not just fail to link. (The overlay wolfssl
# port additionally patches the installed options.h so that
# WOLFSSL_CUSTOM_CURVES/HAVE_ECC_KOBLITZ -- needed for the secp256k1 calls
# in crypto.cpp -- show up as defined here too, matching how libwolfssl.a
# itself was actually built; see that port's portfile.cmake for why that
# patch is necessary at all.)
target_compile_definitions(crypto PRIVATE WOLFSSL_USE_OPTIONS_H)

c_share_set_compile_options(crypto)
