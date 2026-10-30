#pragma once

// Hex text, as JSON-RPC and ABI values spell bytes, and EVM addresses.

// What this file uses, even where CLion finds it through other headers.
// ReSharper disable CppUnusedIncludeDirective
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// ReSharper restore CppUnusedIncludeDirective

namespace c_share::evm {

using Bytes = std::vector<std::uint8_t>;
using Address = std::array<std::uint8_t, 20>;

// 0-15, or -1 if `c` is not a hex digit.
int hex_digit(char c);

// Lowercase hex, with a "0x" prefix if `prefixed`.
std::string to_hex(const std::uint8_t* data, std::size_t size, bool prefixed = true);
inline std::string to_hex(const Bytes& bytes, const bool prefixed = true) {
    return to_hex(bytes.data(), bytes.size(), prefixed);
}

// "0x" and an even number of hex digits (either case); nullopt otherwise.
std::optional<Bytes> from_hex(std::string_view text);

// "0x" and 40 hex digits. Mixed case must be the EIP-55 checksum, as other
// clients require: a wrong one is a mistyped address. nullopt otherwise.
std::optional<Address> parse_address(std::string_view text);
// The EIP-55 checksummed form.
std::string to_checksum_address(const Address& address);

// Keccak-256, through the crypto module.
std::array<std::uint8_t, 32> keccak256(const std::uint8_t* data, std::size_t size);
inline std::array<std::uint8_t, 32> keccak256(const Bytes& bytes) {
    return keccak256(bytes.data(), bytes.size());
}

} // namespace c_share::evm
