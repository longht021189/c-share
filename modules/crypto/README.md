# c_share::crypto

A plain C API (`crypto.h`) wrapping wolfSSL's X25519, Ed25519, secp256k1,
HKDF-SHA256, ChaCha20-Poly1305 and Keccak-256 primitives, plus a secure
random byte generator. Unlike the other modules, this one is deliberately **not** C++:
it exists to be consumed by bindgen (or any other language's C FFI) without
a C++ ABI in the way, so its header uses only fixed-size buffers and plain
functions, no classes or namespaces. wolfSSL itself never appears in
`crypto.h`.

Every function returns `0` on success and non-zero on failure -- including
`*_verify`/`*_decrypt`, where non-zero always means "not verified"/"not
authenticated", never a separate error to unwrap.

## Usage

```c
#include "c_share/crypto/crypto.h"

uint8_t private_key[32], public_key[32];
c_share_crypto_x25519_generate_keypair(private_key, public_key);

uint8_t shared_secret[32];
c_share_crypto_x25519_diffie_hellman(private_key, their_public_key, shared_secret);

uint8_t key[32];
c_share_crypto_hkdf_sha256(shared_secret, 32, info, info_len, key, 32);

uint8_t evm_private_key[32], evm_public_key[64];
c_share_crypto_secp256k1_generate_keypair(evm_private_key, evm_public_key); // not random_bytes: see below

uint8_t address[20];
c_share_crypto_evm_address_from_private_key(evm_private_key, address);

// A key from elsewhere (imported, derived from a seed phrase): check it first.
if (c_share_crypto_secp256k1_validate_private_key(imported_key) != 0) { /* not a secp256k1 key */ }
c_share_crypto_secp256k1_public_from_private(imported_key, public_key);

uint8_t signature[64];
c_share_crypto_secp256k1_sign(evm_private_key, hash /* 32 bytes, e.g. Keccak-256 of a message */, signature);
c_share_crypto_secp256k1_verify(evm_public_key, hash, signature); // 0 == valid

// Ethereum's form: r || s || v (v = recovery id 0/1), s in the lower half,
// RFC 6979 nonce -- what a transaction or personal_sign signature needs.
uint8_t eth_signature[65];
c_share_crypto_secp256k1_sign_recoverable(evm_private_key, hash, eth_signature);

uint8_t digest[32];
c_share_crypto_keccak256(data, data_len, digest); // Ethereum's Keccak-256, not NIST SHA3-256
```

## Why a secp256k1 private key is not just 32 random bytes

A secp256k1 private key is a number in `[1, n - 1]`, where `n` is the
curve's order:

```
n = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE BAAEDCE6 AF48A03B BFD25E8C D0364141
```

32 random bytes read big-endian are a number in `[0, 2^256 - 1]`. `n` is
just below `2^256`, so about 1 in `2^128` of them is `n` or above, and one
in `2^256` is 0. Either is no key at all: go-ethereum (`crypto.ToECDSA`) and
RustCrypto's k256 (`SecretKey::from_slice`) refuse them, and so does every
function here.

That will practically never happen to one random draw, but:

- Code that makes a key must handle it anyway, or it is wrong in a way no
  test would ever catch. `c_share_crypto_secp256k1_generate_keypair` draws
  again until the key is in range (rejection sampling, which keeps keys
  uniform over `[1, n - 1]`; reducing modulo `n` would favor the smallest
  keys, very slightly).
- Keys also come from outside: imports, and derivation from a seed phrase
  (BIP-32 says to skip to the next index when a derived key is out of
  range). `c_share_crypto_secp256k1_validate_private_key` checks one.
- Before this check, wolfSSL refused such a key when deriving its public
  key or address, but **signed with it**, returning success and a signature
  that nothing verifies. `crypto.cpp` now checks the range itself, in time
  that does not depend on the key, before any secp256k1 operation.

X25519 is different: any 32 bytes are a valid X25519 private key, since the
key is clamped (RFC 7748 section 5) before use. Ed25519's 32-byte private key is
a seed that is hashed, so any 32 bytes work there too.

## Why XChaCha20-Poly1305 doesn't call into wolfSSL for it

wolfSSL's own header (`wolfssl/wolfcrypt/chacha20_poly1305.h`) declares
`wc_XChaCha20Poly1305_Encrypt`/`Decrypt` behind `#ifdef HAVE_XCHACHA` -- but
its CMake build never defines `HAVE_XCHACHA`: `wolfssl/CMakeLists.txt` has a
standing `# TODO: - XCHACHA`, and `WOLFSSL_XCHACHA`'s only reference in
`cmake/functions.cmake` sets an internal `BUILD_XCHACHA` variable that
nothing else in the CMake build consumes. So even building c-share's wolfSSL
overlay port with a hypothetical `-DWOLFSSL_XCHACHA=yes` would not actually
compile `wc_XChaCha20Poly1305_*` into the library -- confirmed by checking
`nm` output on a built `libwolfssl.a`, which has no `XChaCha20Poly1305`
symbols at all despite the header declaring them.

Rather than patch wolfSSL's incomplete CMake wiring, `crypto.cpp` implements
HChaCha20 itself: the small, standard, well-specified permutation XChaCha's
extended-nonce construction is built from (see the XChaCha draft RFC or
libsodium's `crypto_core_hchacha20`). It derives a per-message subkey with
it. Then it calls wolfSSL's plain, fully-supported `wc_ChaCha20Poly1305_*`
(12-byte nonce) with that subkey, and a nonce built from the last 8 bytes of
the original 24-byte nonce. This produces the exact same
`AEAD_XChaCha20_Poly1305` construction any other implementation (libsodium,
RustCrypto's `chacha20poly1305` crate, ...) does -- verified bit-for-bit
against the `chacha20poly1305` crate's `XChaCha20Poly1305` for the same
key/nonce/plaintext during development.

## Why the secp256k1 functions need wolfSSL overlay port changes

Every secp256k1 function here (`c_share_crypto_secp256k1_public_from_private`,
`c_share_crypto_evm_address_from_private_key`, `c_share_crypto_secp256k1_sign`,
`c_share_crypto_secp256k1_verify`) needs the first thing below, which the
vcpkg-share `wolfssl` overlay port (`ports/wolfssl/portfile.cmake` there)
doesn't enable by default; `c_share_crypto_evm_address_from_private_key`
additionally needs the second, Keccak-256-specific one:

- **secp256k1.** wolfSSL's ECC support (`HAVE_ECC`) is already on by
  default, but the secp256k1 curve specifically is gated behind
  `HAVE_ECC_KOBLITZ` (which itself `#error`s out unless
  `WOLFSSL_CUSTOM_CURVES` is also defined -- see `ecc.c`), and upstream
  only ever turns `HAVE_ECC_KOBLITZ` on bundled with three unrelated
  curves via `WOLFSSL_ECCCUSTCURVES=all` (secp224r2/256r2, Brainpool, and
  CDH -- see wolfSSL's `CMakeLists.txt`). The overlay port instead defines
  `-DHAVE_ECC_KOBLITZ -DWOLFSSL_CUSTOM_CURVES` directly.
- **Keccak-256 vs. SHA3-256.** Ethereum's "Keccak256" is the original,
  pre-NIST-finalization Keccak padding (a trailing `0x01` bit pattern);
  NIST's finalized SHA3-256 (what `wc_Sha3_256_Final` uses by default)
  changed that padding to `0x06`, producing a completely different digest
  for the same input. wolfSSL's `sha3.c` actually already implements both
  -- `wc_Sha3_SetFlags(sha3, WC_HASH_SHA3_KECCAK256)` switches a `wc_Sha3`
  context to the old padding before `wc_Sha3_256_Final` -- but that
  setter only exists when compiled with `WOLFSSL_HASH_FLAGS`, which the
  overlay port also now defines.
- **Deterministic ECDSA.** `c_share_crypto_secp256k1_sign_recoverable`
  needs RFC 6979 nonces (`WOLFSSL_ECDSA_DETERMINISTIC_K`), which add fields
  to `ecc_key`: the overlay port defines the macro for the build and, since
  wolfSSL's `options.h.in` has no line for it, adds it to the installed
  `options.h` as well, for the same reason as the two macros above.

`c_share_crypto_secp256k1_public_from_private` uses `wc_ecc_import_private_key_ex`
+ `wc_ecc_make_pub` to compute the public key point (`private_key * G`) on
secp256k1, then `wc_ecc_export_public_raw` to get its raw 32-byte x/y
coordinates. `c_share_crypto_evm_address_from_private_key` builds on that
and Keccak-256 (via the flag above) over `x || y` (the uncompressed public
key without its leading `0x04` tag byte) -- the last 20 bytes of that hash
are the address, exactly as every other EVM chain implementation derives
it. `c_share_crypto_secp256k1_sign`/`_verify` are plain ECDSA over the same
curve (`wc_ecc_sign_hash_ex`/`wc_ecc_verify_hash_ex`, operating on raw `r`/`s`
`mp_int`s rather than a DER-encoded signature) and don't hash anything
themselves, so they only need the curve enabled, not Keccak-256.

## Other wolfSSL API quirks worth knowing if you touch `crypto.cpp`

- `wc_curve25519_export_public`/`wc_curve25519_import_private` (no `_ex`
  suffix) default to **big-endian**, not the little-endian RFC 7748 wire
  format X25519 uses everywhere else (TLS, SSH, WireGuard, dalek, ...).
  `crypto.cpp` avoids the whole `curve25519_key`/endianness question by
  using wolfSSL's raw byte-array helpers instead
  (`wc_curve25519_make_priv`/`make_pub`/`generic`), which always operate in
  the standard little-endian format.
- Those raw helpers also *require* the private scalar to already be
  clamped per RFC 7748 §5 and reject it otherwise (`ECC_BAD_ARG_E`) --
  unlike e.g. x25519-dalek, which clamps internally on every use regardless
  of input. `crypto.cpp` clamps a local copy before calling them, so (like
  dalek) any 32 bytes can be used as an X25519 private key here.
- Signing with an Ed25519 key built from `wc_ed25519_import_private_only`
  fails (`BAD_FUNC_ARG`, since `key->pubKeySet` is never set) unless you
  additionally call `wc_ed25519_make_public(&key, key.p, ED25519_PUB_KEY_SIZE)`
  -- passing `key.p` itself as the output buffer, exactly as wolfSSL's own
  `wolfssl/test.h` does. Passing a separate buffer computes the right bytes
  but leaves the key unable to sign or export its public half.
- `wc_ecc_sign_hash_ex`/`wc_ecc_verify_hash_ex` (the `_ex` suffix here, unlike
  curve25519's, is the one to use) take/produce `r`/`s` as raw `mp_int`s
  instead of a DER-encoded ECDSA signature, which is what the plain
  `wc_ecc_sign_hash`/`wc_ecc_verify_hash` (no `_ex`) produce/expect.
  `crypto.cpp` uses the `_ex` forms specifically to avoid a DER
  encode/decode round trip, converting each `mp_int` to/from a fixed
  32-byte big-endian buffer with `mp_to_unsigned_bin_len`/
  `mp_read_unsigned_bin` instead. Note that `c_share_crypto_secp256k1_sign`'s
  output is *not* an Ethereum transaction/`personal_sign` signature as-is:
  Ethereum signatures additionally carry a recovery id byte (`v`) and
  normalize `s` to the lower half of the curve order to prevent signature
  malleability (EIP-2); plain ECDSA verification doesn't need
  them. `c_share_crypto_secp256k1_sign_recoverable` adds both: it signs
  with RFC 6979's deterministic nonce (wolfSSL's
  `wc_ecc_set_deterministic_ex`), regenerates that nonce
  (`wc_ecc_gen_deterministic_k`) to compute R = k * G, whose y parity is the
  recovery id, and negates `s` (flipping the id) when it is in the upper half.
- `wolfssl::wolfssl`'s vcpkg-generated CMake package config is
  "heuristically generated" (vcpkg says so in its own installation output) and
  doesn't propagate `WOLFSSL_USE_OPTIONS_H`, so this module's
  `CMakeLists.txt` defines it explicitly. Without it, every wolfcrypt
  header silently compiles with none of its feature macros defined
  (`HAVE_CHACHA`, `HAVE_ED25519`, ...) and everything here fails to
  compile, not just to link.
