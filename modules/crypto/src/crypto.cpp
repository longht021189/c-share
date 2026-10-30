#include "c_share/crypto/crypto.h"

#include <wolfssl/wolfcrypt/chacha20_poly1305.h>
#include <wolfssl/wolfcrypt/curve25519.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/sha3.h>

#include <cstdint>
#include <cstring>
#include <memory>

namespace {

// RAII wrapper so every early return still frees the RNG.
class ScopedRng {
public:
    ScopedRng() : ok_(wc_InitRng(&rng_) == 0) {}
    ~ScopedRng() {
        if (ok_) {
            wc_FreeRng(&rng_);
        }
    }
    ScopedRng(const ScopedRng&) = delete;
    ScopedRng& operator=(const ScopedRng&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    WC_RNG* get() { return &rng_; }

private:
    WC_RNG rng_{};
    bool ok_;
};

// RFC 7748 section 5 clamping, applied to a local copy so callers never
// need to pre-clamp: wolfSSL's raw curve25519 helpers require the input
// to already be clamped and reject it otherwise (unlike e.g.
// x25519-dalek, which always clamps internally), so without this, any
// private key not produced by generate_keypair here would be rejected.
void clamp_x25519_private_key(uint8_t key[C_SHARE_CRYPTO_X25519_KEY_LEN]) {
    key[0] &= 248;
    key[31] &= 127;
    key[31] |= 64;
}

constexpr int kChaCha20Rounds = 20;

uint32_t rotl32(const uint32_t v, const int n) {
    return (v << n) | (v >> (32 - n));
}

void chacha_quarter_round(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b;
    d ^= a;
    d = rotl32(d, 16);
    c += d;
    b ^= c;
    b = rotl32(b, 12);
    a += b;
    d ^= a;
    d = rotl32(d, 8);
    c += d;
    b ^= c;
    b = rotl32(b, 7);
}

uint32_t load_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void store_le32(uint8_t* p, const uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// HChaCha20: the ChaCha20 block permutation run over a 16-byte nonce
// instead of a 12-byte one, without the final feedforward addition. This
// derives the subkey XChaCha20-Poly1305 uses to extend ChaCha20-Poly1305's
// 12-byte nonce to 24 bytes.
//
// wolfSSL's own CMake build doesn't wire up its XChaCha20-Poly1305
// implementation yet (wolfssl/CMakeLists.txt has a standing
// "TODO: - XCHACHA" and nothing ever defines HAVE_XCHACHA, so
// wc_XChaCha20Poly1305_Encrypt/Decrypt -- declared in
// chacha20_poly1305.h -- are never actually compiled into the library),
// so this implements the same construction directly on top of wolfSSL's
// plain ChaCha20-Poly1305 (12-byte nonce) instead of depending on it. See
// this module's README for the wire format this produces.
void hchacha20(const uint8_t key[32], const uint8_t nonce16[16], uint8_t out_subkey[32]) {
    uint32_t state[16] = {
        0x61707865u,
        0x3320646eu,
        0x79622d32u,
        0x6b206574u,
        load_le32(key + 0),
        load_le32(key + 4),
        load_le32(key + 8),
        load_le32(key + 12),
        load_le32(key + 16),
        load_le32(key + 20),
        load_le32(key + 24),
        load_le32(key + 28),
        load_le32(nonce16 + 0),
        load_le32(nonce16 + 4),
        load_le32(nonce16 + 8),
        load_le32(nonce16 + 12),
    };

    for (int i = 0; i < kChaCha20Rounds / 2; ++i) {
        chacha_quarter_round(state[0], state[4], state[8], state[12]);
        chacha_quarter_round(state[1], state[5], state[9], state[13]);
        chacha_quarter_round(state[2], state[6], state[10], state[14]);
        chacha_quarter_round(state[3], state[7], state[11], state[15]);
        chacha_quarter_round(state[0], state[5], state[10], state[15]);
        chacha_quarter_round(state[1], state[6], state[11], state[12]);
        chacha_quarter_round(state[2], state[7], state[8], state[13]);
        chacha_quarter_round(state[3], state[4], state[9], state[14]);
    }

    store_le32(out_subkey + 0, state[0]);
    store_le32(out_subkey + 4, state[1]);
    store_le32(out_subkey + 8, state[2]);
    store_le32(out_subkey + 12, state[3]);
    store_le32(out_subkey + 16, state[12]);
    store_le32(out_subkey + 20, state[13]);
    store_le32(out_subkey + 24, state[14]);
    store_le32(out_subkey + 28, state[15]);
}

// Builds the 12-byte IETF ChaCha20-Poly1305 nonce XChaCha20-Poly1305 uses
// once the 24-byte nonce has been reduced via HChaCha20: four zero bytes
// followed by the last 8 bytes of the original nonce.
void xchacha_iv(const uint8_t nonce24[C_SHARE_CRYPTO_XCHACHA20POLY1305_NONCE_LEN],
                uint8_t out_iv[CHACHA20_POLY1305_AEAD_IV_SIZE]) {
    std::memset(out_iv, 0, 4);
    std::memcpy(out_iv + 4, nonce24 + 16, 8);
}

// secp256k1's order n, big-endian: private keys are in [1, n - 1].
constexpr uint8_t kSecp256k1Order[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
    0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b, 0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41,
};

// Whether 0 < key < n, in time that does not depend on the key: the borrow
// out of key - n is 1 exactly when key < n.
bool secp256k1_private_key_in_range(const uint8_t key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN]) {
    unsigned borrow = 0;
    unsigned any_bit = 0;
    for (int i = C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN - 1; i >= 0; --i) {
        const unsigned difference = unsigned{key[i]} - unsigned{kSecp256k1Order[i]} - borrow;
        borrow = (difference >> 8) & 1U;
        any_bit |= key[i];
    }
    return (borrow & static_cast<unsigned>(any_bit != 0)) != 0;
}

// Initializes `key` from a raw secp256k1 private scalar. On success (0),
// the caller owns `key` and must wc_ecc_free it; on failure, this has
// already cleaned up after itself, so the caller must not.
int import_secp256k1_private_key(const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN], ecc_key* key) {
    // Checked here, not left to wolfSSL: deriving a public key refuses a
    // key out of range, but signing with one succeeds, and produces a
    // signature nothing verifies.
    if (!secp256k1_private_key_in_range(private_key)) {
        return ECC_PRIV_KEY_E;
    }
    int ret = wc_ecc_init(key);
    if (ret != 0) {
        return ret;
    }
    ret = wc_ecc_import_private_key_ex(private_key, C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN, nullptr, 0, key,
                                       ECC_SECP256K1);
    if (ret != 0) {
        wc_ecc_free(key);
    }
    return ret;
}

// An mp_int that is cleared, and its memory zeroed, however the scope ends.
class MpInt {
public:
    MpInt() : ok_(mp_init(&value_) == MP_OKAY) {}
    ~MpInt() {
        if (ok_) {
            mp_forcezero(&value_);
            mp_clear(&value_);
        }
    }
    MpInt(const MpInt&) = delete;
    MpInt& operator=(const MpInt&) = delete;
    MpInt(MpInt&&) = delete;
    MpInt& operator=(MpInt&&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    mp_int* get() { return &value_; }

private:
    mp_int value_{};
    bool ok_;
};

struct PointDeleter {
    void operator()(ecc_point* point) const { wc_ecc_del_point(point); }
};
using Point = std::unique_ptr<ecc_point, PointDeleter>;

// The recovery id of the signature (r, s) wolfSSL's RFC 6979 signing just
// made over `hash`: the parity of y of R = k * G, k being the nonce, which
// is regenerated here (it is a function of the key and the hash alone).
// R's x is also checked against r, so a k that differs from the one signing
// used fails rather than giving a wrong id.
int secp256k1_recovery_id(const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN], const uint8_t hash[32],
                          const mp_int* r, int* out_recovery_id) {
    const int curve_idx = wc_ecc_get_curve_idx(ECC_SECP256K1);
    const ecc_set_type* curve = wc_ecc_get_curve_params(curve_idx);
    if (curve == nullptr) {
        return ECC_CURVE_OID_E;
    }
    MpInt d;
    MpInt k;
    MpInt order;
    MpInt prime;
    MpInt a;
    MpInt x_mod_order;
    if (!d.ok() || !k.ok() || !order.ok() || !prime.ok() || !a.ok() || !x_mod_order.ok()) {
        return MEMORY_E;
    }
    const Point generator(wc_ecc_new_point());
    const Point nonce_point(wc_ecc_new_point());
    if (!generator || !nonce_point) {
        return MEMORY_E;
    }
    int ret = mp_read_radix(order.get(), curve->order, MP_RADIX_HEX);
    if (ret == 0) {
        ret = mp_read_radix(prime.get(), curve->prime, MP_RADIX_HEX);
    }
    if (ret == 0) {
        ret = mp_read_radix(a.get(), curve->Af, MP_RADIX_HEX);
    }
    if (ret == 0) {
        ret = mp_read_unsigned_bin(d.get(), private_key, C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN);
    }
    if (ret == 0) {
        ret = wc_ecc_gen_deterministic_k(hash, 32, WC_HASH_TYPE_SHA256, d.get(), k.get(), order.get(), nullptr);
    }
    if (ret == 0) {
        ret = wc_ecc_get_generator(generator.get(), curve_idx);
    }
    if (ret == 0) {
        ret = wc_ecc_mulmod(k.get(), generator.get(), nonce_point.get(), a.get(), prime.get(), 1);
    }
    if (ret == 0) {
        ret = mp_mod(nonce_point->x, order.get(), x_mod_order.get());
    }
    if (ret != 0) {
        return ret;
    }
    // R's x at or above the order (about 1 in 2^128) would need ids 2 and 3,
    // which an Ethereum signature has no room for.
    if (mp_cmp(x_mod_order.get(), r) != MP_EQ || mp_cmp(nonce_point->x, order.get()) != MP_LT) {
        return ECC_BAD_ARG_E;
    }
    *out_recovery_id = mp_isodd(nonce_point->y) ? 1 : 0;
    return 0;
}

// wolfSSL refuses a null buffer even with a length of 0, which an empty
// message may well come with (e.g. an empty std::vector's data()).
const uint8_t* or_empty(const uint8_t* buffer) {
    static constexpr uint8_t kEmpty = 0;
    return buffer ? buffer : &kEmpty;
}

uint8_t* or_empty(uint8_t* buffer) {
    static uint8_t empty = 0; // never written: only ever passed with a length of 0
    return buffer ? buffer : &empty;
}

} // namespace

int c_share_crypto_keccak256(const uint8_t* data, const size_t data_len,
                             uint8_t out_hash[C_SHARE_CRYPTO_KECCAK256_LEN]) {
    // wolfSSL's SHA3-256 defaults to NIST's finalized padding (0x06).
    // Ethereum's "Keccak256" is the older, pre-finalization Keccak
    // padding (0x01) -- the WC_HASH_SHA3_KECCAK256 flag (only settable
    // when compiled with WOLFSSL_HASH_FLAGS; see the wolfssl overlay
    // port) switches wc_Sha3_256_Final to that padding instead, with
    // everything else about the algorithm unchanged.
    wc_Sha3 sha3;
    int ret = wc_InitSha3_256(&sha3, nullptr, INVALID_DEVID);
    if (ret != 0) {
        return ret;
    }
    ret = wc_Sha3_SetFlags(&sha3, WC_HASH_SHA3_KECCAK256);
    if (ret == 0) {
        ret = wc_Sha3_256_Update(&sha3, or_empty(data), static_cast<word32>(data_len));
    }
    if (ret == 0) {
        ret = wc_Sha3_256_Final(&sha3, out_hash);
    }
    wc_Sha3_256_Free(&sha3);
    return ret;
}

int c_share_crypto_random_bytes(uint8_t* out, const size_t out_len) {
    ScopedRng rng;
    if (!rng.ok()) {
        return -1;
    }
    return wc_RNG_GenerateBlock(rng.get(), out, static_cast<word32>(out_len));
}

int c_share_crypto_x25519_generate_keypair(uint8_t out_private_key[C_SHARE_CRYPTO_X25519_KEY_LEN],
                                           uint8_t out_public_key[C_SHARE_CRYPTO_X25519_KEY_LEN]) {
    ScopedRng rng;
    if (!rng.ok()) {
        return -1;
    }
    if (const int ret = wc_curve25519_make_priv(rng.get(), C_SHARE_CRYPTO_X25519_KEY_LEN, out_private_key); ret != 0) {
        return ret;
    }
    return wc_curve25519_make_pub(C_SHARE_CRYPTO_X25519_KEY_LEN, out_public_key, C_SHARE_CRYPTO_X25519_KEY_LEN,
                                  out_private_key);
}

int c_share_crypto_x25519_public_from_private(const uint8_t private_key[C_SHARE_CRYPTO_X25519_KEY_LEN],
                                              uint8_t out_public_key[C_SHARE_CRYPTO_X25519_KEY_LEN]) {
    uint8_t clamped[C_SHARE_CRYPTO_X25519_KEY_LEN];
    std::memcpy(clamped, private_key, sizeof(clamped));
    clamp_x25519_private_key(clamped);
    return wc_curve25519_make_pub(C_SHARE_CRYPTO_X25519_KEY_LEN, out_public_key, C_SHARE_CRYPTO_X25519_KEY_LEN,
                                  clamped);
}

int c_share_crypto_x25519_diffie_hellman(const uint8_t private_key[C_SHARE_CRYPTO_X25519_KEY_LEN],
                                         const uint8_t their_public_key[C_SHARE_CRYPTO_X25519_KEY_LEN],
                                         uint8_t out_shared_secret[C_SHARE_CRYPTO_X25519_KEY_LEN]) {
    uint8_t clamped[C_SHARE_CRYPTO_X25519_KEY_LEN];
    std::memcpy(clamped, private_key, sizeof(clamped));
    clamp_x25519_private_key(clamped);
    return wc_curve25519_generic(C_SHARE_CRYPTO_X25519_KEY_LEN, out_shared_secret, C_SHARE_CRYPTO_X25519_KEY_LEN,
                                 clamped, C_SHARE_CRYPTO_X25519_KEY_LEN, their_public_key);
}

int c_share_crypto_ed25519_generate_keypair(uint8_t out_private_key[C_SHARE_CRYPTO_ED25519_PRIVATE_KEY_LEN],
                                            uint8_t out_public_key[C_SHARE_CRYPTO_ED25519_PUBLIC_KEY_LEN]) {
    ScopedRng rng;
    if (!rng.ok()) {
        return -1;
    }
    ed25519_key key;
    int ret = wc_ed25519_init(&key);
    if (ret != 0) {
        return ret;
    }
    ret = wc_ed25519_make_key(rng.get(), C_SHARE_CRYPTO_ED25519_PRIVATE_KEY_LEN, &key);
    if (ret == 0) {
        word32 priv_len = C_SHARE_CRYPTO_ED25519_PRIVATE_KEY_LEN;
        ret = wc_ed25519_export_private_only(&key, out_private_key, &priv_len);
    }
    if (ret == 0) {
        word32 pub_len = C_SHARE_CRYPTO_ED25519_PUBLIC_KEY_LEN;
        ret = wc_ed25519_export_public(&key, out_public_key, &pub_len);
    }
    wc_ed25519_free(&key);
    return ret;
}

int c_share_crypto_ed25519_public_from_private(const uint8_t private_key[C_SHARE_CRYPTO_ED25519_PRIVATE_KEY_LEN],
                                               uint8_t out_public_key[C_SHARE_CRYPTO_ED25519_PUBLIC_KEY_LEN]) {
    ed25519_key key;
    int ret = wc_ed25519_init(&key);
    if (ret != 0) {
        return ret;
    }
    ret = wc_ed25519_import_private_only(private_key, C_SHARE_CRYPTO_ED25519_PRIVATE_KEY_LEN, &key);
    if (ret == 0) {
        // Output straight into key.p: wc_ed25519_export_public/
        // wc_ed25519_sign_msg below read the public key from there, and
        // wc_ed25519_make_public only sets key->pubKeySet when its output
        // buffer *is* key->p (see wolfSSL's own test.h for this exact
        // pattern).
        ret = wc_ed25519_make_public(&key, key.p, ED25519_PUB_KEY_SIZE);
    }
    if (ret == 0) {
        word32 pub_len = C_SHARE_CRYPTO_ED25519_PUBLIC_KEY_LEN;
        ret = wc_ed25519_export_public(&key, out_public_key, &pub_len);
    }
    wc_ed25519_free(&key);
    return ret;
}

int c_share_crypto_ed25519_sign(const uint8_t private_key[C_SHARE_CRYPTO_ED25519_PRIVATE_KEY_LEN],
                                const uint8_t* message, const size_t message_len,
                                uint8_t out_signature[C_SHARE_CRYPTO_ED25519_SIGNATURE_LEN]) {
    ed25519_key key;
    int ret = wc_ed25519_init(&key);
    if (ret != 0) {
        return ret;
    }
    ret = wc_ed25519_import_private_only(private_key, C_SHARE_CRYPTO_ED25519_PRIVATE_KEY_LEN, &key);
    if (ret == 0) {
        ret = wc_ed25519_make_public(&key, key.p, ED25519_PUB_KEY_SIZE);
    }
    if (ret == 0) {
        word32 sig_len = C_SHARE_CRYPTO_ED25519_SIGNATURE_LEN;
        ret = wc_ed25519_sign_msg(or_empty(message), static_cast<word32>(message_len), out_signature, &sig_len, &key);
    }
    wc_ed25519_free(&key);
    return ret;
}

int c_share_crypto_ed25519_verify(const uint8_t public_key[C_SHARE_CRYPTO_ED25519_PUBLIC_KEY_LEN],
                                  const uint8_t* message, const size_t message_len,
                                  const uint8_t signature[C_SHARE_CRYPTO_ED25519_SIGNATURE_LEN]) {
    ed25519_key key;
    int ret = wc_ed25519_init(&key);
    if (ret != 0) {
        return ret;
    }
    ret = wc_ed25519_import_public(public_key, C_SHARE_CRYPTO_ED25519_PUBLIC_KEY_LEN, &key);
    int verified = 0;
    if (ret == 0) {
        ret = wc_ed25519_verify_msg(signature, C_SHARE_CRYPTO_ED25519_SIGNATURE_LEN, or_empty(message),
                                    static_cast<word32>(message_len), &verified, &key);
    }
    wc_ed25519_free(&key);
    if (ret != 0) {
        return ret;
    }
    return verified == 1 ? 0 : -1;
}

int c_share_crypto_hkdf_sha256(const uint8_t* ikm, const size_t ikm_len, const uint8_t* info, const size_t info_len,
                               uint8_t* out, const size_t out_len) {
    return wc_HKDF(WC_SHA256, ikm, static_cast<word32>(ikm_len), nullptr, 0, info, static_cast<word32>(info_len), out,
                   static_cast<word32>(out_len));
}

int c_share_crypto_xchacha20poly1305_encrypt(const uint8_t key[C_SHARE_CRYPTO_XCHACHA20POLY1305_KEY_LEN],
                                             const uint8_t nonce[C_SHARE_CRYPTO_XCHACHA20POLY1305_NONCE_LEN],
                                             const uint8_t* plaintext, const size_t plaintext_len,
                                             uint8_t* out_ciphertext) {
    uint8_t subkey[C_SHARE_CRYPTO_XCHACHA20POLY1305_KEY_LEN];
    hchacha20(key, nonce, subkey);

    uint8_t iv[CHACHA20_POLY1305_AEAD_IV_SIZE];
    xchacha_iv(nonce, iv);

    return wc_ChaCha20Poly1305_Encrypt(subkey, iv, nullptr, 0, or_empty(plaintext), static_cast<word32>(plaintext_len),
                                       out_ciphertext, out_ciphertext + plaintext_len);
}

int c_share_crypto_xchacha20poly1305_decrypt(const uint8_t key[C_SHARE_CRYPTO_XCHACHA20POLY1305_KEY_LEN],
                                             const uint8_t nonce[C_SHARE_CRYPTO_XCHACHA20POLY1305_NONCE_LEN],
                                             const uint8_t* ciphertext, const size_t ciphertext_len,
                                             uint8_t* out_plaintext) {
    if (ciphertext_len < C_SHARE_CRYPTO_XCHACHA20POLY1305_TAG_LEN) {
        return -1;
    }
    const size_t plaintext_len = ciphertext_len - C_SHARE_CRYPTO_XCHACHA20POLY1305_TAG_LEN;

    uint8_t subkey[C_SHARE_CRYPTO_XCHACHA20POLY1305_KEY_LEN];
    hchacha20(key, nonce, subkey);

    uint8_t iv[CHACHA20_POLY1305_AEAD_IV_SIZE];
    xchacha_iv(nonce, iv);

    return wc_ChaCha20Poly1305_Decrypt(subkey, iv, nullptr, 0, ciphertext, static_cast<word32>(plaintext_len),
                                       ciphertext + plaintext_len, or_empty(out_plaintext));
}

int c_share_crypto_secp256k1_generate_keypair(uint8_t out_private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN],
                                              uint8_t out_public_key[C_SHARE_CRYPTO_SECP256K1_PUBLIC_KEY_LEN]) {
    ScopedRng rng;
    if (!rng.ok()) {
        return -1;
    }
    // Draws again until the key is in range, which keeps it uniform over
    // [1, n - 1] (reducing modulo n would not, by a little). A second draw
    // happens about once in 2^128 keys; running out of attempts means the
    // random source is broken.
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (const int ret = wc_RNG_GenerateBlock(rng.get(), out_private_key, C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN);
            ret != 0) {
            return ret;
        }
        if (secp256k1_private_key_in_range(out_private_key)) {
            return c_share_crypto_secp256k1_public_from_private(out_private_key, out_public_key);
        }
    }
    return -1;
}

int c_share_crypto_secp256k1_validate_private_key(const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN]) {
    return secp256k1_private_key_in_range(private_key) ? 0 : ECC_PRIV_KEY_E;
}

int c_share_crypto_secp256k1_public_from_private(const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN],
                                                 uint8_t out_public_key[C_SHARE_CRYPTO_SECP256K1_PUBLIC_KEY_LEN]) {
    ecc_key key;
    int ret = import_secp256k1_private_key(private_key, &key);
    if (ret != 0) {
        return ret;
    }

    ret = wc_ecc_make_pub(&key, nullptr);
    if (ret == 0) {
        word32 x_len = 32;
        word32 y_len = 32;
        ret = wc_ecc_export_public_raw(&key, out_public_key, &x_len, out_public_key + 32, &y_len);
    }
    wc_ecc_free(&key);
    return ret;
}

int c_share_crypto_evm_address_from_private_key(const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN],
                                                uint8_t out_address[C_SHARE_CRYPTO_EVM_ADDRESS_LEN]) {
    // The uncompressed public key, x and y coordinates back-to-back with
    // no leading 0x04 tag byte -- what Keccak-256 hashes to derive an
    // EVM address.
    uint8_t pub[C_SHARE_CRYPTO_SECP256K1_PUBLIC_KEY_LEN];
    int ret = c_share_crypto_secp256k1_public_from_private(private_key, pub);
    if (ret != 0) {
        return ret;
    }

    uint8_t hash[C_SHARE_CRYPTO_KECCAK256_LEN];
    ret = c_share_crypto_keccak256(pub, sizeof(pub), hash);
    if (ret != 0) {
        return ret;
    }

    std::memcpy(out_address, hash + sizeof(hash) - C_SHARE_CRYPTO_EVM_ADDRESS_LEN, C_SHARE_CRYPTO_EVM_ADDRESS_LEN);
    return 0;
}

int c_share_crypto_secp256k1_sign(const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN],
                                  const uint8_t hash[32],
                                  uint8_t out_signature[C_SHARE_CRYPTO_SECP256K1_SIGNATURE_LEN]) {
    ScopedRng rng;
    if (!rng.ok()) {
        return -1;
    }

    ecc_key key;
    int ret = import_secp256k1_private_key(private_key, &key);
    if (ret != 0) {
        return ret;
    }

    mp_int r;
    mp_int s;
    const bool r_ok = mp_init(&r) == 0;
    const bool s_ok = mp_init(&s) == 0;
    ret = r_ok && s_ok ? 0 : -1;
    if (ret == 0) {
        ret = wc_ecc_sign_hash_ex(hash, 32, rng.get(), &key, &r, &s);
    }
    if (ret == 0) {
        ret = mp_to_unsigned_bin_len(&r, out_signature, 32);
    }
    if (ret == 0) {
        ret = mp_to_unsigned_bin_len(&s, out_signature + 32, 32);
    }
    if (r_ok) {
        mp_clear(&r);
    }
    if (s_ok) {
        mp_clear(&s);
    }
    wc_ecc_free(&key);
    return ret;
}

int c_share_crypto_secp256k1_sign_recoverable(
    const uint8_t private_key[C_SHARE_CRYPTO_SECP256K1_PRIVATE_KEY_LEN], const uint8_t hash[32],
    uint8_t out_signature[C_SHARE_CRYPTO_SECP256K1_RECOVERABLE_SIGNATURE_LEN]) {
    ScopedRng rng;
    if (!rng.ok()) {
        return -1;
    }
    ecc_key key;
    int ret = import_secp256k1_private_key(private_key, &key);
    if (ret != 0) {
        return ret;
    }
    MpInt r;
    MpInt s;
    MpInt order;
    MpInt twice_s;
    ret = r.ok() && s.ok() && order.ok() && twice_s.ok() ? 0 : MEMORY_E;
    if (ret == 0) {
        ret = wc_ecc_set_deterministic_ex(&key, 1, WC_HASH_TYPE_SHA256);
    }
    if (ret == 0) {
        ret = wc_ecc_sign_hash_ex(hash, 32, rng.get(), &key, r.get(), s.get());
    }
    wc_ecc_free(&key);
    int recovery_id = 0;
    if (ret == 0) {
        ret = secp256k1_recovery_id(private_key, hash, r.get(), &recovery_id);
    }
    if (ret == 0) {
        ret = mp_read_radix(order.get(), wc_ecc_get_curve_params(wc_ecc_get_curve_idx(ECC_SECP256K1))->order,
                            MP_RADIX_HEX);
    }
    // EIP-2: s above half the order becomes order - s, which negates R, so
    // the recovery id flips with it.
    if (ret == 0) {
        ret = mp_add(s.get(), s.get(), twice_s.get());
    }
    if (ret == 0 && mp_cmp(twice_s.get(), order.get()) == MP_GT) {
        ret = mp_sub(order.get(), s.get(), s.get());
        recovery_id ^= 1;
    }
    if (ret == 0) {
        ret = mp_to_unsigned_bin_len(r.get(), out_signature, 32);
    }
    if (ret == 0) {
        ret = mp_to_unsigned_bin_len(s.get(), out_signature + 32, 32);
    }
    if (ret == 0) {
        out_signature[64] = static_cast<uint8_t>(recovery_id);
    }
    return ret;
}

int c_share_crypto_secp256k1_verify(const uint8_t public_key[C_SHARE_CRYPTO_SECP256K1_PUBLIC_KEY_LEN],
                                    const uint8_t hash[32],
                                    const uint8_t signature[C_SHARE_CRYPTO_SECP256K1_SIGNATURE_LEN]) {
    ecc_key key;
    int ret = wc_ecc_init(&key);
    if (ret != 0) {
        return ret;
    }
    ret = wc_ecc_import_unsigned(&key, public_key, public_key + 32, nullptr, ECC_SECP256K1);

    mp_int r;
    mp_int s;
    bool r_ok = false;
    bool s_ok = false;
    if (ret == 0) {
        r_ok = mp_init(&r) == 0;
        s_ok = mp_init(&s) == 0;
        ret = r_ok && s_ok ? 0 : -1;
    }
    if (ret == 0) {
        ret = mp_read_unsigned_bin(&r, signature, 32);
    }
    if (ret == 0) {
        ret = mp_read_unsigned_bin(&s, signature + 32, 32);
    }
    int verified = 0;
    if (ret == 0) {
        ret = wc_ecc_verify_hash_ex(&r, &s, hash, 32, &verified, &key);
    }
    if (r_ok) {
        mp_clear(&r);
    }
    if (s_ok) {
        mp_clear(&s);
    }
    wc_ecc_free(&key);
    if (ret != 0) {
        return ret;
    }
    return verified == 1 ? 0 : -1;
}
