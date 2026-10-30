#ifndef C_SHARE_CRYPTO_H
#define C_SHARE_CRYPTO_H

/*
 * Plain C API (deliberately not the C++ style used by the other modules)
 * so it can be consumed directly by bindgen from Rust, or by any other
 * language with a C FFI, without a C++ ABI in the way. wolfSSL is the
 * implementation behind every function here and never appears in this
 * header.
 *
 * Every function returns 0 on success and a non-zero value on failure.
 * Buffers are fixed-size (see the *_LEN constants) unless documented
 * otherwise.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C_SHARE_CRYPTO_X25519_KEY_LEN 32

#define C_SHARE_CRYPTO_ED25519_PRIVATE_KEY_LEN 32
#define C_SHARE_CRYPTO_ED25519_PUBLIC_KEY_LEN 32
#define C_SHARE_CRYPTO_ED25519_SIGNATURE_LEN 64

#define C_SHARE_CRYPTO_XCHACHA20POLY1305_KEY_LEN 32
#define C_SHARE_CRYPTO_XCHACHA20POLY1305_NONCE_LEN 24
#define C_SHARE_CRYPTO_XCHACHA20POLY1305_TAG_LEN 16

#define C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN 32
#define C_SHARE_CRYPTO_SECP256K1_PUBLIC_KEY_LEN 64
#define C_SHARE_CRYPTO_SECP256K1_SIGNATURE_LEN 64
#define C_SHARE_CRYPTO_SECP256K1_RECOVERABLE_SIGNATURE_LEN 65
#define C_SHARE_CRYPTO_EVM_ADDRESS_LEN 20
#define C_SHARE_CRYPTO_KECCAK256_LEN 32

/* Fills `out_len` bytes of `out` with cryptographically secure random
 * output (wolfSSL's default OS entropy source). Returns 0 on success. */
int c_share_crypto_random_bytes(uint8_t* out, size_t out_len);

/* -- X25519 (Diffie-Hellman key exchange) -- */

/* Generates a new random X25519 keypair. */
int c_share_crypto_x25519_generate_keypair(uint8_t out_private_key[C_SHARE_CRYPTO_X25519_KEY_LEN],
                                           uint8_t out_public_key[C_SHARE_CRYPTO_X25519_KEY_LEN]);

/* Derives the public key matching `private_key`. `private_key` does not
 * need to already be clamped per RFC 7748 -- this clamps a local copy
 * first, same as every other function here that takes an X25519 private
 * key, so any 32 bytes can be used as a private key. */
int c_share_crypto_x25519_public_from_private(const uint8_t private_key[C_SHARE_CRYPTO_X25519_KEY_LEN],
                                              uint8_t out_public_key[C_SHARE_CRYPTO_X25519_KEY_LEN]);

/* Computes the X25519 Diffie-Hellman shared secret between `private_key`
 * and `their_public_key`. The raw output must never be used directly as
 * a symmetric key -- run it through a KDF (c_share_crypto_hkdf_sha256)
 * first. */
int c_share_crypto_x25519_diffie_hellman(const uint8_t private_key[C_SHARE_CRYPTO_X25519_KEY_LEN],
                                         const uint8_t their_public_key[C_SHARE_CRYPTO_X25519_KEY_LEN],
                                         uint8_t out_shared_secret[C_SHARE_CRYPTO_X25519_KEY_LEN]);

/* -- Ed25519 (signatures) -- */

/* Generates a new random Ed25519 keypair. */
int c_share_crypto_ed25519_generate_keypair(uint8_t out_private_key[C_SHARE_CRYPTO_ED25519_PRIVATE_KEY_LEN],
                                            uint8_t out_public_key[C_SHARE_CRYPTO_ED25519_PUBLIC_KEY_LEN]);

/* Derives the public key matching a 32-byte Ed25519 private key (seed). */
int c_share_crypto_ed25519_public_from_private(const uint8_t private_key[C_SHARE_CRYPTO_ED25519_PRIVATE_KEY_LEN],
                                               uint8_t out_public_key[C_SHARE_CRYPTO_ED25519_PUBLIC_KEY_LEN]);

/* Signs `message` with `private_key` (a 32-byte seed). */
int c_share_crypto_ed25519_sign(const uint8_t private_key[C_SHARE_CRYPTO_ED25519_PRIVATE_KEY_LEN],
                                const uint8_t* message, size_t message_len,
                                uint8_t out_signature[C_SHARE_CRYPTO_ED25519_SIGNATURE_LEN]);

/* Verifies `signature` over `message` against `public_key`. Returns 0 if
 * valid, non-zero otherwise -- including malformed input, so a non-zero
 * return must always be treated as "not verified", never as an error to
 * handle separately. */
int c_share_crypto_ed25519_verify(const uint8_t public_key[C_SHARE_CRYPTO_ED25519_PUBLIC_KEY_LEN],
                                  const uint8_t* message, size_t message_len,
                                  const uint8_t signature[C_SHARE_CRYPTO_ED25519_SIGNATURE_LEN]);

/* -- HKDF-SHA256 (RFC 5869) -- */

/* Derives `out_len` bytes of key material from `ikm` via HKDF-SHA256,
 * with no salt (a zero-filled salt of the hash's output length, per the
 * RFC) and `info` as the context string. */
int c_share_crypto_hkdf_sha256(const uint8_t* ikm, size_t ikm_len, const uint8_t* info, size_t info_len, uint8_t* out,
                               size_t out_len);

/* -- XChaCha20-Poly1305 (AEAD, no additional authenticated data) -- */

/* Encrypts `plaintext`. `out_ciphertext` must be `plaintext_len +
 * C_SHARE_CRYPTO_XCHACHA20POLY1305_TAG_LEN` bytes: ciphertext followed by
 * the authentication tag. */
int c_share_crypto_xchacha20poly1305_encrypt(const uint8_t key[C_SHARE_CRYPTO_XCHACHA20POLY1305_KEY_LEN],
                                             const uint8_t nonce[C_SHARE_CRYPTO_XCHACHA20POLY1305_NONCE_LEN],
                                             const uint8_t* plaintext, size_t plaintext_len, uint8_t* out_ciphertext);

/* Decrypts a bundle produced by c_share_crypto_xchacha20poly1305_encrypt.
 * `ciphertext_len` includes the trailing tag; `out_plaintext` must be
 * `ciphertext_len - C_SHARE_CRYPTO_XCHACHA20POLY1305_TAG_LEN` bytes.
 * Returns 0 on success (tag verified), non-zero if authentication fails
 * or `ciphertext_len` is too short -- never read `out_plaintext` unless
 * this returns 0. */
int c_share_crypto_xchacha20poly1305_decrypt(const uint8_t key[C_SHARE_CRYPTO_XCHACHA20POLY1305_KEY_LEN],
                                             const uint8_t nonce[C_SHARE_CRYPTO_XCHACHA20POLY1305_NONCE_LEN],
                                             const uint8_t* ciphertext, size_t ciphertext_len, uint8_t* out_plaintext);

/* -- Keccak-256 --
 *
 * The original Keccak padding (0x01) Ethereum uses everywhere it says
 * "Keccak256" or "SHA3": not NIST's final SHA3-256 (0x06), which hashes to
 * different bytes. `data` may be null when `data_len` is 0. */
int c_share_crypto_keccak256(const uint8_t* data, size_t data_len, uint8_t out_hash[C_SHARE_CRYPTO_KECCAK256_LEN]);

/* -- secp256k1 (ECDSA signatures, EVM-style keys/addresses) --
 *
 * Public keys are the 64-byte uncompressed point `x || y` (each a
 * 32-byte big-endian curve coordinate), with no leading 0x04 tag byte.
 *
 * A private key is a 32-byte big-endian number in [1, n - 1], n being the
 * curve's order:
 *   n = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE BAAEDCE6 AF48A03B BFD25E8C D0364141
 * Every function here taking a private key refuses one outside that range
 * (0, or n and above), as go-ethereum and RustCrypto's k256 do.
 *
 * So do not use 32 random bytes as a key: about 1 in 2^128 of them is out
 * of range. Unlikely as that is, a key that could be invalid needs checking
 * wherever it is made; c_share_crypto_secp256k1_generate_keypair draws again
 * until the key is in range, so its keys never do. A key from elsewhere (an
 * import, a seed phrase's derivation) can be checked with
 * c_share_crypto_secp256k1_validate_private_key.
 */

/* Generates a new random keypair: a private key uniformly distributed over
 * [1, n - 1], and its public key. */
int c_share_crypto_secp256k1_generate_keypair(uint8_t out_private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN],
                                              uint8_t out_public_key[C_SHARE_CRYPTO_SECP256K1_PUBLIC_KEY_LEN]);

/* Returns 0 if `private_key` is a valid private key (in [1, n - 1]),
 * non-zero otherwise. */
int c_share_crypto_secp256k1_validate_private_key(const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN]);

/* Derives the public key matching `private_key`. */
int c_share_crypto_secp256k1_public_from_private(const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN],
                                                 uint8_t out_public_key[C_SHARE_CRYPTO_SECP256K1_PUBLIC_KEY_LEN]);

/* Derives the 20-byte EVM address for a secp256k1 private key:
 * address = the last 20 bytes of Keccak-256(public_key), where
 * public_key is `c_share_crypto_secp256k1_public_from_private`'s
 * output. */
int c_share_crypto_evm_address_from_private_key(const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN],
                                                uint8_t out_address[C_SHARE_CRYPTO_EVM_ADDRESS_LEN]);

/* Signs a 32-byte digest (e.g. a Keccak-256 hash of the real message --
 * this does not hash anything itself) with `private_key`, producing a
 * raw ECDSA signature `out_signature = r || s` (each a 32-byte
 * big-endian curve scalar). This is plain ECDSA over secp256k1: unlike
 * an Ethereum transaction/personal_sign signature, there is no recovery
 * id byte and `s` is not normalized to the lower half of the curve
 * order, so it round-trips with `c_share_crypto_secp256k1_verify` but is
 * not directly an Ethereum wire-format signature. */
int c_share_crypto_secp256k1_sign(const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN],
                                  const uint8_t hash[32],
                                  uint8_t out_signature[C_SHARE_CRYPTO_SECP256K1_SIGNATURE_LEN]);

/* Signs a 32-byte digest the way Ethereum transactions and messages are
 * signed: `out_signature = r || s || v`, 65 bytes, where `v` is the
 * recovery id (0 or 1, the parity of the nonce point's y; add 27 for
 * personal_sign's v, or use it as-is as an EIP-1559 transaction's
 * yParity) and `s` is in the lower half of the curve order (EIP-2). The
 * nonce is RFC 6979's (HMAC-SHA256), so the same key and digest always give
 * the same signature, as with go-ethereum and ethers. */
int c_share_crypto_secp256k1_sign_recoverable(
    const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN], const uint8_t hash[32],
    uint8_t out_signature[C_SHARE_CRYPTO_SECP256K1_RECOVERABLE_SIGNATURE_LEN]);

/* Verifies `signature` (r || s, as produced by
 * c_share_crypto_secp256k1_sign) over `hash` against `public_key`.
 * Returns 0 if valid, non-zero otherwise -- including malformed input,
 * so a non-zero return must always be treated as "not verified", never
 * as an error to handle separately. */
int c_share_crypto_secp256k1_verify(const uint8_t public_key[C_SHARE_CRYPTO_SECP256K1_PUBLIC_KEY_LEN],
                                    const uint8_t hash[32],
                                    const uint8_t signature[C_SHARE_CRYPTO_SECP256K1_SIGNATURE_LEN]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* C_SHARE_CRYPTO_H */
