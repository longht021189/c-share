# c-share overlay of vcpkg's upstream wolfssl port
# (https://github.com/microsoft/vcpkg/blob/master/ports/wolfssl/portfile.cmake).
#
# Changes from upstream:
#
# 1. Adds an "ed25519" feature that maps to wolfSSL's own WOLFSSL_ED25519
#    CMake option. Upstream exposes a toggle for WOLFSSL_CURVE25519
#    (X25519 / ECDH) but not for WOLFSSL_ED25519 (EdDSA signatures), even
#    though wolfSSL's own CMakeLists.txt supports both (both default to
#    "disabled" upstream). See modules/http/README.md for why c-share
#    needs this.
#
# 2. Adds -DHAVE_ECC_KOBLITZ, -DWOLFSSL_CUSTOM_CURVES, and
#    -DWOLFSSL_HASH_FLAGS to the unconditional extra CFLAGS below
#    (WOLFSSL_ECC itself is already on by default upstream, so no feature
#    toggle is needed for it), plus a post-install patch of the installed
#    options.h -- see the comment right before that patch, further down
#    in this file, for why the CFLAGS alone are not enough. HAVE_ECC_KOBLITZ
#    turns on the secp256k1 curve, which upstream's WOLFSSL_ECCCUSTCURVES
#    option only ever enables bundled with three unrelated curves
#    (secp{224,256}r2/brainpool/CDH -- see wolfSSL's CMakeLists.txt); this
#    defines just the one flag it actually needs instead, plus
#    WOLFSSL_CUSTOM_CURVES, which wolfSSL's ecc.c `#error`s out without
#    regardless of which of the two it's paired with. WOLFSSL_HASH_FLAGS
#    turns on wc_Sha3_SetFlags/wc_Sha3_256_Final's WC_HASH_SHA3_KECCAK256
#    handling, which switches SHA3-256's finalization padding from
#    NIST SHA3's 0x06 to the original, pre-NIST Keccak's 0x01 -- the
#    padding Ethereum (and every other pre-NIST-finalization "Keccak256")
#    actually uses. `c_share_crypto_evm_address_from_private_key`
#    (modules/crypto/src/crypto.cpp) needs all three of these.
#
# 3. Adds -DWOLFSSL_ECDSA_DETERMINISTIC_K, with the same options.h patch:
#    RFC 6979 deterministic ECDSA (wc_ecc_set_deterministic_ex,
#    wc_ecc_gen_deterministic_k). c_share_crypto_secp256k1_sign_recoverable
#    regenerates the nonce k a signature used, to find the recovery id
#    Ethereum signatures carry; it also makes those signatures reproducible
#    (and testable against published vectors). It adds fields to ecc_key, so
#    options.h must agree with the build here too.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wolfssl/wolfssl
    REF "v${VERSION}-stable"
    SHA512 ed0f6bff353b1a835e1f1c15e1863eda9e04030962211be64d7c764f8373b016778e04ea9538ca6cf2f58d55d0f855c2af1739d776f9697b2f4a13a5e489d85f
    HEAD_REF master
)

if ("asio" IN_LIST FEATURES)
    set(ENABLE_ASIO yes)
else()
    set(ENABLE_ASIO no)
endif()

if ("dtls" IN_LIST FEATURES)
    set(ENABLE_DTLS yes)
else()
    set(ENABLE_DTLS no)
endif()

if ("quic" IN_LIST FEATURES)
    set(ENABLE_QUIC yes)
else()
    set(ENABLE_QUIC no)
endif()

if ("curve25519" IN_LIST FEATURES)
    set(ENABLE_CURVE25519 yes)
else()
    set(ENABLE_CURVE25519 no)
endif()

# c-share addition: see file header comment.
if ("ed25519" IN_LIST FEATURES)
    set(ENABLE_ED25519 yes)
else()
    set(ENABLE_ED25519 no)
endif()

vcpkg_cmake_get_vars(cmake_vars_file)
include("${cmake_vars_file}")

foreach(config RELEASE DEBUG)
  string(APPEND VCPKG_COMBINED_C_FLAGS_${config} " -DHAVE_EX_DATA -DNO_WOLFSSL_STUB -DWOLFSSL_ALT_CERT_CHAINS -DWOLFSSL_DES_ECB -DWOLFSSL_CUSTOM_OID -DHAVE_OID_ENCODING -DWOLFSSL_CERT_GEN -DWOLFSSL_ASN_TEMPLATE -DWOLFSSL_KEY_GEN -DHAVE_PKCS7 -DHAVE_AES_KEYWRAP -DHAVE_AES_ECB -DWOLFSSL_AES_DIRECT -DHAVE_X963_KDF -DWOLFSSL_PUBLIC_MP -DHAVE_ECC_KOBLITZ -DWOLFSSL_CUSTOM_CURVES -DWOLFSSL_HASH_FLAGS -DWOLFSSL_ECDSA_DETERMINISTIC_K")
  if ("secret-callback" IN_LIST FEATURES)
      string(APPEND VCPKG_COMBINED_C_FLAGS_${config} " -DHAVE_SECRET_CALLBACK")
  endif()
endforeach()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
      -DWOLFSSL_BUILD_OUT_OF_TREE=yes
      -DWOLFSSL_EXAMPLES=no
      -DWOLFSSL_CRYPT_TESTS=no
      -DWOLFSSL_OPENSSLEXTRA=yes
      -DWOLFSSL_TPM=yes
      -DWOLFSSL_TLSX=yes
      -DWOLFSSL_OCSP=yes
      -DWOLFSSL_OCSPSTAPLING=yes
      -DWOLFSSL_OCSPSTAPLING_V2=yes
      -DWOLFSSL_CRL=yes
      -DWOLFSSL_DES3=yes
      -DWOLFSSL_HPKE=yes
      -DWOLFSSL_SNI=yes
      -DWOLFSSL_ASIO=${ENABLE_ASIO}
      -DWOLFSSL_DTLS=${ENABLE_DTLS}
      -DWOLFSSL_DTLS13=${ENABLE_DTLS}
      -DWOLFSSL_DTLS_CID=${ENABLE_DTLS}
      -DWOLFSSL_QUIC=${ENABLE_QUIC}
      -DWOLFSSL_SESSION_TICKET=${ENABLE_QUIC}
      -DWOLFSSL_CURVE25519=${ENABLE_CURVE25519}
      -DWOLFSSL_ED25519=${ENABLE_ED25519}
    OPTIONS_RELEASE
      -DCMAKE_C_FLAGS=${VCPKG_COMBINED_C_FLAGS_RELEASE}
    OPTIONS_DEBUG
      -DCMAKE_C_FLAGS=${VCPKG_COMBINED_C_FLAGS_DEBUG}
      -DWOLFSSL_DEBUG=yes)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/wolfssl)

# wolfSSL's generated wolfssl/options.h -- the header WOLFSSL_USE_OPTIONS_H
# tells any *consumer* to include, so it sees the same feature set the
# library itself was built with -- is produced by configure_file()'ing
# cmake/options.h.in against real CMake variables (one #cmakedefine per
# macro). But WOLFSSL_CUSTOM_CURVES and HAVE_ECC_KOBLITZ are never set()
# as CMake variables anywhere in wolfSSL's CMakeLists.txt -- they only
# ever get appended, as plain "-DWOLFSSL_CUSTOM_CURVES" strings, to a
# WOLFSSL_DEFINITIONS list that's applied straight to the library's own
# compile flags. So options.h.in's #cmakedefine for each always resolves
# to false, no matter what -- meaning our -DHAVE_ECC_KOBLITZ
# -DWOLFSSL_CUSTOM_CURVES above (needed, and sufficient, to actually build
# secp256k1 support into libwolfssl.a) leaves the *installed* options.h
# permanently claiming both are off. Since HAVE_ECC_KOBLITZ/
# WOLFSSL_CUSTOM_CURVES change ecc_key's layout (ecc.h adds a
# WOLFSSL_CUSTOM_CURVES-only `int deallocSet` field), any WOLFSSL_USE_OPTIONS_H
# consumer -- like this repo's crypto module -- would silently compile
# against a smaller ecc_key than the one wolfSSL's own ECC calls actually
# write into, corrupting the stack on every call. Patching the two
# relevant lines post-install (each is a `#cmakedefine`-generated,
# currently-commented-out `#define`) is the only way to get options.h to
# agree with how the library was actually built.
vcpkg_replace_string(
    "${CURRENT_PACKAGES_DIR}/include/wolfssl/options.h"
    "/* #undef WOLFSSL_CUSTOM_CURVES */"
    "#define WOLFSSL_CUSTOM_CURVES"
)
vcpkg_replace_string(
    "${CURRENT_PACKAGES_DIR}/include/wolfssl/options.h"
    "/* #undef HAVE_ECC_KOBLITZ */"
    "#define HAVE_ECC_KOBLITZ"
)
# WOLFSSL_ECDSA_DETERMINISTIC_K has no line in options.h.in at all, so it is
# added next to HAVE_ECC_KOBLITZ (change 3 in the header comment).
vcpkg_replace_string(
    "${CURRENT_PACKAGES_DIR}/include/wolfssl/options.h"
    "#define HAVE_ECC_KOBLITZ"
    "#define HAVE_ECC_KOBLITZ\n\n#undef WOLFSSL_ECDSA_DETERMINISTIC_K\n#define WOLFSSL_ECDSA_DETERMINISTIC_K"
)

if(VCPKG_TARGET_IS_IOS OR VCPKG_TARGET_IS_OSX)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/wolfssl.pc" "Libs.private: " "Libs.private: -framework CoreFoundation -framework Security ")
    if(NOT VCPKG_BUILD_TYPE)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/wolfssl.pc" "Libs.private: " "Libs.private: -framework CoreFoundation -framework Security ")
    endif()
endif()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
