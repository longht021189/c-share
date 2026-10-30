#include "c_share/crypto/crypto.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes from_hex(const std::string& hex) {
    Bytes out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

template <std::size_t N> std::array<std::uint8_t, N> array_from_hex(const std::string& hex) {
    const Bytes bytes = from_hex(hex);
    std::array<std::uint8_t, N> out{};
    REQUIRE(bytes.size() == N);
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

template <typename Container> std::string to_hex(const Container& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}

Bytes bytes_of(const std::string& text) {
    return {text.begin(), text.end()};
}

} // namespace

TEST_CASE("random_bytes fills the buffer with different bytes each call", "[crypto]") {
    std::array<std::uint8_t, 32> a{};
    std::array<std::uint8_t, 32> b{};
    REQUIRE(c_share_crypto_random_bytes(a.data(), a.size()) == 0);
    REQUIRE(c_share_crypto_random_bytes(b.data(), b.size()) == 0);
    CHECK(a != b);
    CHECK(a != std::array<std::uint8_t, 32>{});
}

// RFC 7748 section 6.1.
TEST_CASE("X25519 matches RFC 7748's Diffie-Hellman example", "[crypto][x25519]") {
    const auto alice_private = array_from_hex<32>("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto bob_private = array_from_hex<32>("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");

    std::array<std::uint8_t, 32> alice_public{};
    std::array<std::uint8_t, 32> bob_public{};
    REQUIRE(c_share_crypto_x25519_public_from_private(alice_private.data(), alice_public.data()) == 0);
    REQUIRE(c_share_crypto_x25519_public_from_private(bob_private.data(), bob_public.data()) == 0);
    CHECK(to_hex(alice_public) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    CHECK(to_hex(bob_public) == "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");

    std::array<std::uint8_t, 32> alice_shared{};
    std::array<std::uint8_t, 32> bob_shared{};
    REQUIRE(c_share_crypto_x25519_diffie_hellman(alice_private.data(), bob_public.data(), alice_shared.data()) == 0);
    REQUIRE(c_share_crypto_x25519_diffie_hellman(bob_private.data(), alice_public.data(), bob_shared.data()) == 0);
    CHECK(to_hex(alice_shared) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    CHECK(alice_shared == bob_shared);
}

TEST_CASE("X25519 generated keypairs agree on a shared secret", "[crypto][x25519]") {
    std::array<std::uint8_t, 32> a_private{};
    std::array<std::uint8_t, 32> a_public{};
    std::array<std::uint8_t, 32> b_private{};
    std::array<std::uint8_t, 32> b_public{};
    REQUIRE(c_share_crypto_x25519_generate_keypair(a_private.data(), a_public.data()) == 0);
    REQUIRE(c_share_crypto_x25519_generate_keypair(b_private.data(), b_public.data()) == 0);

    std::array<std::uint8_t, 32> derived{};
    REQUIRE(c_share_crypto_x25519_public_from_private(a_private.data(), derived.data()) == 0);
    CHECK(derived == a_public);

    std::array<std::uint8_t, 32> a_shared{};
    std::array<std::uint8_t, 32> b_shared{};
    REQUIRE(c_share_crypto_x25519_diffie_hellman(a_private.data(), b_public.data(), a_shared.data()) == 0);
    REQUIRE(c_share_crypto_x25519_diffie_hellman(b_private.data(), a_public.data(), b_shared.data()) == 0);
    CHECK(a_shared == b_shared);
}

// RFC 8032 section 7.1, TEST 1 and TEST 2.
TEST_CASE("Ed25519 matches RFC 8032's test vectors", "[crypto][ed25519]") {
    struct Vector {
        const char* private_key;
        const char* public_key;
        const char* message;
        const char* signature;
    };
    const auto [private_key_hex, public_key_hex, message_hex, signature_hex] = GENERATE(
        Vector{
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f05"
            "95bbe24655141438e7a100b"},
        Vector{
            "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb"
            "4302aeeb00d291612bb0c00"});

    const auto private_key = array_from_hex<32>(private_key_hex);
    const Bytes message = from_hex(message_hex);

    std::array<std::uint8_t, 32> public_key{};
    REQUIRE(c_share_crypto_ed25519_public_from_private(private_key.data(), public_key.data()) == 0);
    CHECK(to_hex(public_key) == public_key_hex);

    std::array<std::uint8_t, 64> signature{};
    REQUIRE(c_share_crypto_ed25519_sign(private_key.data(), message.data(), message.size(), signature.data()) == 0);
    CHECK(to_hex(signature) == signature_hex);
    CHECK(c_share_crypto_ed25519_verify(public_key.data(), message.data(), message.size(), signature.data()) == 0);
}

TEST_CASE("Ed25519 rejects a signature that does not match", "[crypto][ed25519]") {
    std::array<std::uint8_t, 32> private_key{};
    std::array<std::uint8_t, 32> public_key{};
    REQUIRE(c_share_crypto_ed25519_generate_keypair(private_key.data(), public_key.data()) == 0);
    const Bytes message = bytes_of("a message");
    std::array<std::uint8_t, 64> signature{};
    REQUIRE(c_share_crypto_ed25519_sign(private_key.data(), message.data(), message.size(), signature.data()) == 0);
    REQUIRE(c_share_crypto_ed25519_verify(public_key.data(), message.data(), message.size(), signature.data()) == 0);

    SECTION("another message") {
        const Bytes other = bytes_of("another message");
        CHECK(c_share_crypto_ed25519_verify(public_key.data(), other.data(), other.size(), signature.data()) != 0);
    }
    SECTION("a changed signature") {
        signature[10] ^= 0x01;
        CHECK(c_share_crypto_ed25519_verify(public_key.data(), message.data(), message.size(), signature.data()) != 0);
    }
    SECTION("another key") {
        std::array<std::uint8_t, 32> other_private{};
        std::array<std::uint8_t, 32> other_public{};
        REQUIRE(c_share_crypto_ed25519_generate_keypair(other_private.data(), other_public.data()) == 0);
        CHECK(c_share_crypto_ed25519_verify(other_public.data(), message.data(), message.size(), signature.data()) !=
              0);
    }
}

// RFC 5869 appendix A.3: no salt, no info.
TEST_CASE("HKDF-SHA256 matches RFC 5869's test case 3", "[crypto][hkdf]") {
    const Bytes ikm(22, 0x0b);
    std::array<std::uint8_t, 42> okm{};
    REQUIRE(c_share_crypto_hkdf_sha256(ikm.data(), ikm.size(), nullptr, 0, okm.data(), okm.size()) == 0);
    CHECK(to_hex(okm) == "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");
}

TEST_CASE("HKDF-SHA256 output depends on info", "[crypto][hkdf]") {
    const Bytes ikm = bytes_of("input key material");
    const Bytes info_a = bytes_of("a");
    const Bytes info_b = bytes_of("b");
    std::array<std::uint8_t, 32> a{};
    std::array<std::uint8_t, 32> b{};
    REQUIRE(c_share_crypto_hkdf_sha256(ikm.data(), ikm.size(), info_a.data(), info_a.size(), a.data(), a.size()) == 0);
    REQUIRE(c_share_crypto_hkdf_sha256(ikm.data(), ikm.size(), info_b.data(), info_b.size(), b.data(), b.size()) == 0);
    CHECK(a != b);
}

TEST_CASE("XChaCha20-Poly1305 round-trips and authenticates", "[crypto][xchacha]") {
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, 24> nonce{};
    REQUIRE(c_share_crypto_random_bytes(key.data(), key.size()) == 0);
    REQUIRE(c_share_crypto_random_bytes(nonce.data(), nonce.size()) == 0);
    const Bytes plaintext = bytes_of("Ladies and Gentlemen of the class of '99");

    Bytes ciphertext(plaintext.size() + C_SHARE_CRYPTO_XCHACHA20POLY1305_TAG_LEN);
    REQUIRE(c_share_crypto_xchacha20poly1305_encrypt(key.data(), nonce.data(), plaintext.data(), plaintext.size(),
                                                     ciphertext.data()) == 0);
    CHECK(Bytes(ciphertext.begin(), ciphertext.begin() + static_cast<std::ptrdiff_t>(plaintext.size())) != plaintext);

    Bytes decrypted(plaintext.size());
    SECTION("decrypts with the same key and nonce") {
        REQUIRE(c_share_crypto_xchacha20poly1305_decrypt(key.data(), nonce.data(), ciphertext.data(), ciphertext.size(),
                                                         decrypted.data()) == 0);
        CHECK(decrypted == plaintext);
    }
    SECTION("fails on a changed ciphertext") {
        ciphertext[3] ^= 0x01;
        CHECK(c_share_crypto_xchacha20poly1305_decrypt(key.data(), nonce.data(), ciphertext.data(), ciphertext.size(),
                                                       decrypted.data()) != 0);
    }
    SECTION("fails on a changed tag") {
        ciphertext.back() ^= 0x01;
        CHECK(c_share_crypto_xchacha20poly1305_decrypt(key.data(), nonce.data(), ciphertext.data(), ciphertext.size(),
                                                       decrypted.data()) != 0);
    }
    SECTION("fails with another nonce") {
        nonce[23] ^= 0x01;
        CHECK(c_share_crypto_xchacha20poly1305_decrypt(key.data(), nonce.data(), ciphertext.data(), ciphertext.size(),
                                                       decrypted.data()) != 0);
    }
    SECTION("fails on input shorter than a tag") {
        CHECK(c_share_crypto_xchacha20poly1305_decrypt(key.data(), nonce.data(), ciphertext.data(),
                                                       C_SHARE_CRYPTO_XCHACHA20POLY1305_TAG_LEN - 1,
                                                       decrypted.data()) != 0);
    }
}

TEST_CASE("XChaCha20-Poly1305 encrypts an empty message to a bare tag", "[crypto][xchacha]") {
    constexpr std::array<std::uint8_t, 32> key{};
    constexpr std::array<std::uint8_t, 24> nonce{};
    std::array<std::uint8_t, C_SHARE_CRYPTO_XCHACHA20POLY1305_TAG_LEN> tag{};
    REQUIRE(c_share_crypto_xchacha20poly1305_encrypt(key.data(), nonce.data(), nullptr, 0, tag.data()) == 0);
    CHECK(c_share_crypto_xchacha20poly1305_decrypt(key.data(), nonce.data(), tag.data(), tag.size(), nullptr) == 0);
}

// The private key 1: its public key is the curve's generator G.
TEST_CASE("secp256k1 and EVM addresses match the well-known key 1", "[crypto][secp256k1]") {
    std::array<std::uint8_t, 32> private_key{};
    private_key[31] = 1;

    std::array<std::uint8_t, 64> public_key{};
    REQUIRE(c_share_crypto_secp256k1_public_from_private(private_key.data(), public_key.data()) == 0);
    CHECK(to_hex(public_key) == "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
                                "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8");

    std::array<std::uint8_t, 20> address{};
    REQUIRE(c_share_crypto_evm_address_from_private_key(private_key.data(), address.data()) == 0);
    CHECK(to_hex(address) == "7e5f4552091a69125d5dfcb7b8c2659029395bdf");
}

// n - 1 is -1: its public key is -G, G's x with p - y.
TEST_CASE("secp256k1 takes the largest private key, n - 1", "[crypto][secp256k1]") {
    const auto private_key = array_from_hex<32>("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364140");
    CHECK(c_share_crypto_secp256k1_validate_private_key(private_key.data()) == 0);
    std::array<std::uint8_t, 64> public_key{};
    REQUIRE(c_share_crypto_secp256k1_public_from_private(private_key.data(), public_key.data()) == 0);
    CHECK(to_hex(public_key) == "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
                                "b7c52588d95c3b9aa25b0403f1eef75702e84bb7597aabe663b82f6f04ef2777");
}

TEST_CASE("secp256k1 refuses private keys out of range", "[crypto][secp256k1]") {
    const auto [name, hex] = GENERATE(table<const char*, const char*>({
        {"zero", "0000000000000000000000000000000000000000000000000000000000000000"},
        {"n", "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141"},
        {"n + 1", "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364142"},
        {"2^256 - 1", "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},
    }));
    CAPTURE(name);
    const auto private_key = array_from_hex<32>(hex);
    CHECK(c_share_crypto_secp256k1_validate_private_key(private_key.data()) != 0);
    std::array<std::uint8_t, 64> public_key{};
    CHECK(c_share_crypto_secp256k1_public_from_private(private_key.data(), public_key.data()) != 0);
    std::array<std::uint8_t, 20> address{};
    CHECK(c_share_crypto_evm_address_from_private_key(private_key.data(), address.data()) != 0);
    // wolfSSL alone would sign with these, producing what nothing verifies.
    constexpr std::array<std::uint8_t, 32> hash{1};
    std::array<std::uint8_t, 64> signature{};
    CHECK(c_share_crypto_secp256k1_sign(private_key.data(), hash.data(), signature.data()) != 0);
}

TEST_CASE("secp256k1 generates valid, distinct keypairs", "[crypto][secp256k1]") {
    std::array<std::uint8_t, 32> first{};
    std::array<std::uint8_t, 64> first_public{};
    REQUIRE(c_share_crypto_secp256k1_generate_keypair(first.data(), first_public.data()) == 0);
    CHECK(c_share_crypto_secp256k1_validate_private_key(first.data()) == 0);
    std::array<std::uint8_t, 64> derived{};
    REQUIRE(c_share_crypto_secp256k1_public_from_private(first.data(), derived.data()) == 0);
    CHECK(derived == first_public);

    std::array<std::uint8_t, 32> second{};
    std::array<std::uint8_t, 64> second_public{};
    REQUIRE(c_share_crypto_secp256k1_generate_keypair(second.data(), second_public.data()) == 0);
    CHECK(second != first);
    CHECK(second_public != first_public);
}

TEST_CASE("secp256k1 signatures verify, and only for their hash and key", "[crypto][secp256k1]") {
    std::array<std::uint8_t, 32> private_key{};
    std::array<std::uint8_t, 64> public_key{};
    REQUIRE(c_share_crypto_secp256k1_generate_keypair(private_key.data(), public_key.data()) == 0);

    std::array<std::uint8_t, 32> hash{};
    REQUIRE(c_share_crypto_random_bytes(hash.data(), hash.size()) == 0);
    std::array<std::uint8_t, 64> signature{};
    REQUIRE(c_share_crypto_secp256k1_sign(private_key.data(), hash.data(), signature.data()) == 0);
    CHECK(c_share_crypto_secp256k1_verify(public_key.data(), hash.data(), signature.data()) == 0);

    SECTION("another hash") {
        hash[0] ^= 0x01;
        CHECK(c_share_crypto_secp256k1_verify(public_key.data(), hash.data(), signature.data()) != 0);
    }
    SECTION("a changed signature") {
        signature[40] ^= 0x01;
        CHECK(c_share_crypto_secp256k1_verify(public_key.data(), hash.data(), signature.data()) != 0);
    }
    SECTION("another key") {
        std::array<std::uint8_t, 32> other_private{};
        std::array<std::uint8_t, 64> other_public{};
        REQUIRE(c_share_crypto_secp256k1_generate_keypair(other_private.data(), other_public.data()) == 0);
        CHECK(c_share_crypto_secp256k1_verify(other_public.data(), hash.data(), signature.data()) != 0);
    }
}

TEST_CASE("Keccak-256 is Ethereum's, not NIST SHA3-256", "[crypto][keccak]") {
    std::array<std::uint8_t, C_SHARE_CRYPTO_KECCAK256_LEN> hash{};
    REQUIRE(c_share_crypto_keccak256(nullptr, 0, hash.data()) == 0);
    CHECK(to_hex(hash) == "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
    const Bytes abc = bytes_of("abc");
    REQUIRE(c_share_crypto_keccak256(abc.data(), abc.size(), hash.data()) == 0);
    CHECK(to_hex(hash) == "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
}

// EIP-155's example transaction: its signing hash, private key and signature
// (v = 37 on chain 1, so the recovery id is 0).
TEST_CASE("secp256k1 recoverable signatures match EIP-155's example", "[crypto][secp256k1]") {
    const auto private_key = array_from_hex<32>("4646464646464646464646464646464646464646464646464646464646464646");
    const auto hash = array_from_hex<32>("daf5a779ae972f972197303d7b574746c7ef83eadac0f2791ad23db92e4c8e53");
    std::array<std::uint8_t, C_SHARE_CRYPTO_SECP256K1_RECOVERABLE_SIGNATURE_LEN> signature{};
    REQUIRE(c_share_crypto_secp256k1_sign_recoverable(private_key.data(), hash.data(), signature.data()) == 0);
    CHECK(to_hex(signature) == "28ef61340bd939bc2195fe537567866003e1a15d3c71ff63e1590620aa636276"
                               "67cbe9d8997f761aecb703304b3800ccf555c9f3dc64214b297fb1966a3b6d83"
                               "00");
}

TEST_CASE("secp256k1 recoverable signatures verify, with s in the lower half", "[crypto][secp256k1]") {
    std::array<std::uint8_t, 32> private_key{};
    std::array<std::uint8_t, 64> public_key{};
    REQUIRE(c_share_crypto_secp256k1_generate_keypair(private_key.data(), public_key.data()) == 0);
    // Half the order, rounded down: s must not be above it.
    const auto half_order = array_from_hex<32>("7fffffffffffffffffffffffffffffff5d576e7357a4501ddfe92f46681b20a0");
    for (int i = 0; i < 32; ++i) {
        std::array<std::uint8_t, 32> hash{};
        REQUIRE(c_share_crypto_random_bytes(hash.data(), hash.size()) == 0);
        std::array<std::uint8_t, C_SHARE_CRYPTO_SECP256K1_RECOVERABLE_SIGNATURE_LEN> signature{};
        REQUIRE(c_share_crypto_secp256k1_sign_recoverable(private_key.data(), hash.data(), signature.data()) == 0);
        CHECK(c_share_crypto_secp256k1_verify(public_key.data(), hash.data(), signature.data()) == 0);
        CHECK(std::lexicographical_compare(half_order.begin(), half_order.end(), signature.begin() + 32,
                                           signature.begin() + 64) == false);
        CHECK(signature[64] <= 1);
        // Deterministic: the same key and hash sign the same.
        std::array<std::uint8_t, C_SHARE_CRYPTO_SECP256K1_RECOVERABLE_SIGNATURE_LEN> again{};
        REQUIRE(c_share_crypto_secp256k1_sign_recoverable(private_key.data(), hash.data(), again.data()) == 0);
        CHECK(again == signature);
    }
}
