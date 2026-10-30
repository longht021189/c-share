#include "hex.hpp"

#include "c_share/crypto/crypto.h"

#include <stdexcept>

namespace c_share::evm {

int hex_digit(const char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string to_hex(const std::uint8_t* data, const std::size_t size, const bool prefixed) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = prefixed ? "0x" : "";
    out.reserve(out.size() + size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0f]);
    }
    return out;
}

std::optional<Bytes> from_hex(std::string_view text) {
    if (text.size() < 2 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return std::nullopt;
    text.remove_prefix(2);
    if (text.size() % 2 != 0) return std::nullopt;
    Bytes out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = hex_digit(text[2 * i]);
        const int low = hex_digit(text[2 * i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out[i] = static_cast<std::uint8_t>(high << 4 | low);
    }
    return out;
}

std::optional<Address> parse_address(const std::string_view text) {
    if (text.size() != 42) return std::nullopt;
    const auto bytes = from_hex(text);
    if (!bytes) return std::nullopt;
    Address address{};
    std::copy(bytes->begin(), bytes->end(), address.begin());

    const std::string_view digits = text.substr(2);
    const bool has_upper = digits.find_first_of("ABCDEF") != std::string_view::npos;
    if (const bool has_lower = digits.find_first_of("abcdef") != std::string_view::npos;
        has_upper && has_lower && to_checksum_address(address).substr(2) != digits)
        return std::nullopt;
    return address;
}

std::string to_checksum_address(const Address& address) {
    std::string hex = to_hex(address.data(), address.size(), false);
    // EIP-55: a letter is uppercase where the hash of the lowercase hex has
    // its matching nibble at 8 or above.
    const auto hash = keccak256(reinterpret_cast<const std::uint8_t*>(hex.data()), hex.size());
    for (std::size_t i = 0; i < hex.size(); ++i) {
        if (const int nibble = i % 2 == 0 ? hash[i / 2] >> 4 : hash[i / 2] & 0x0f; hex[i] >= 'a' && nibble >= 8)
            hex[i] = static_cast<char>(hex[i] - 'a' + 'A');
    }
    return "0x" + hex;
}

std::array<std::uint8_t, 32> keccak256(const std::uint8_t* data, const std::size_t size) {
    std::array<std::uint8_t, 32> hash{};
    if (c_share_crypto_keccak256(data, size, hash.data()) != 0) throw std::runtime_error("keccak256 failed");
    return hash;
}

} // namespace c_share::evm
