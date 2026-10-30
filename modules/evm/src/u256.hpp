#pragma once

// A 256-bit unsigned integer: EVM words, wei amounts, quantities. Just the
// operations this module needs, on 32-bit limbs so that it builds on every
// supported compiler (MSVC has no 128-bit integer type).

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

class U256 {
public:
    constexpr U256() = default;
    constexpr explicit U256(const std::uint64_t value)
        : limbs_{static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32)} {}

    // Decimal digits, or "0x" and hex digits (either case). nullopt if
    // empty, malformed or at least 2^256.
    static std::optional<U256> parse(std::string_view text);
    // A JSON-RPC quantity: "0x" and hex digits.
    static std::optional<U256> from_quantity(std::string_view text);
    // Big-endian, at most 32 bytes.
    static U256 from_big_endian(const std::uint8_t* data, std::size_t size);

    [[nodiscard]] std::array<std::uint8_t, 32> to_big_endian() const;
    // Big-endian without leading zero bytes; empty for 0 (RLP's integers).
    [[nodiscard]] std::vector<std::uint8_t> to_minimal_big_endian() const;
    [[nodiscard]] std::string to_decimal() const;
    // "0x" and hex digits without leading zeros, "0x0" for 0.
    [[nodiscard]] std::string to_quantity() const;

    [[nodiscard]] bool is_zero() const;
    // The number of bits up to the highest set one; 0 for 0.
    [[nodiscard]] unsigned bit_length() const;
    [[nodiscard]] bool bit(unsigned index) const;
    // The low 64 bits.
    [[nodiscard]] std::uint64_t low64() const;

    // Each returns false, leaving *this unspecified, on overflow.
    [[nodiscard]] bool add(const U256& other);
    [[nodiscard]] bool multiply(std::uint32_t factor);

    // Two's complement: ~x + 1, modulo 2^256.
    [[nodiscard]] U256 negated() const;
    // ~x
    [[nodiscard]] U256 inverted() const;
    // 2^bits - 1, for bits in [0, 256].
    static U256 mask(unsigned bits);
    [[nodiscard]] U256 bitwise_and(const U256& other) const;
    [[nodiscard]] U256 bitwise_or(const U256& other) const;

    friend bool operator==(const U256& a, const U256& b) { return a.limbs_ == b.limbs_; }
    friend bool operator!=(const U256& a, const U256& b) { return !(a == b); }
    friend bool operator<(const U256& a, const U256& b);
    friend bool operator>(const U256& a, const U256& b) { return b < a; }
    friend bool operator<=(const U256& a, const U256& b) { return !(b < a); }

private:
    // Divides by `divisor`, returning the remainder.
    std::uint32_t divide(std::uint32_t divisor);

    std::array<std::uint32_t, 8> limbs_{}; // least significant first
};

} // namespace c_share::evm
