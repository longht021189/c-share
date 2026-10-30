#include "u256.hpp"

#include "hex.hpp"

#include <algorithm>

namespace c_share::evm {

std::optional<U256> U256::parse(const std::string_view text) {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) return from_quantity(text);
    if (text.empty()) return std::nullopt;
    U256 value;
    for (const char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        if (!value.multiply(10) || !value.add(U256(static_cast<std::uint64_t>(c - '0')))) return std::nullopt;
    }
    return value;
}

std::optional<U256> U256::from_quantity(std::string_view text) {
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return std::nullopt;
    text.remove_prefix(2);
    while (text.size() > 1 && text.front() == '0')
        text.remove_prefix(1);
    if (text.size() > 64) return std::nullopt;
    U256 value;
    for (const char c : text) {
        const int digit = hex_digit(c);
        if (digit < 0) return std::nullopt;
        if (!value.multiply(16) || !value.add(U256(static_cast<std::uint64_t>(digit)))) return std::nullopt;
    }
    return value;
}

U256 U256::from_big_endian(const std::uint8_t* data, const std::size_t size) {
    U256 value;
    const std::size_t used = std::min<std::size_t>(size, 32);
    for (std::size_t i = 0; i < used; ++i) {
        const std::size_t byte_index = used - 1 - i; // 0 = least significant
        value.limbs_[byte_index / 4] |= static_cast<std::uint32_t>(data[size - used + i]) << (8 * (byte_index % 4));
    }
    return value;
}

std::array<std::uint8_t, 32> U256::to_big_endian() const {
    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < 32; ++i) {
        const std::size_t byte_index = 31 - i;
        out[i] = static_cast<std::uint8_t>(limbs_[byte_index / 4] >> (8 * (byte_index % 4)));
    }
    return out;
}

std::vector<std::uint8_t> U256::to_minimal_big_endian() const {
    const auto bytes = to_big_endian();
    std::size_t first = 0;
    while (first < bytes.size() && bytes[first] == 0)
        ++first;
    return {bytes.begin() + static_cast<std::ptrdiff_t>(first), bytes.end()};
}

std::string U256::to_decimal() const {
    if (is_zero()) return "0";
    U256 rest = *this;
    std::string digits;
    while (!rest.is_zero())
        digits.push_back(static_cast<char>('0' + rest.divide(10)));
    std::reverse(digits.begin(), digits.end());
    return digits;
}

std::string U256::to_quantity() const {
    const auto bytes = to_minimal_big_endian();
    if (bytes.empty()) return "0x0";
    std::string hex = to_hex(bytes.data(), bytes.size(), false);
    if (hex[0] == '0') hex.erase(0, 1);
    return "0x" + hex;
}

bool U256::is_zero() const {
    return std::all_of(limbs_.begin(), limbs_.end(), [](const std::uint32_t limb) { return limb == 0; });
}

unsigned U256::bit_length() const {
    for (std::size_t i = limbs_.size(); i-- > 0;) {
        if (limbs_[i] == 0) continue;
        unsigned bits = 0;
        for (std::uint32_t limb = limbs_[i]; limb != 0; limb >>= 1)
            ++bits;
        return static_cast<unsigned>(i) * 32 + bits;
    }
    return 0;
}

bool U256::bit(const unsigned index) const {
    return index < 256 && ((limbs_[index / 32] >> (index % 32)) & 1U) != 0;
}

std::uint64_t U256::low64() const {
    return (static_cast<std::uint64_t>(limbs_[1]) << 32) | limbs_[0];
}

bool U256::add(const U256& other) {
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < limbs_.size(); ++i) {
        const std::uint64_t sum = std::uint64_t{limbs_[i]} + other.limbs_[i] + carry;
        limbs_[i] = static_cast<std::uint32_t>(sum);
        carry = sum >> 32;
    }
    return carry == 0;
}

bool U256::multiply(const std::uint32_t factor) {
    std::uint64_t carry = 0;
    for (auto& limb : limbs_) {
        const std::uint64_t product = std::uint64_t{limb} * factor + carry;
        limb = static_cast<std::uint32_t>(product);
        carry = product >> 32;
    }
    return carry == 0;
}

U256 U256::negated() const {
    U256 result = inverted();
    (void)result.add(U256(1)); // wraps for 0, as two's complement does
    return result;
}

U256 U256::inverted() const {
    U256 result;
    for (std::size_t i = 0; i < limbs_.size(); ++i)
        result.limbs_[i] = ~limbs_[i];
    return result;
}

U256 U256::mask(const unsigned bits) {
    U256 result;
    for (unsigned i = 0; i < 256 && i < bits; ++i)
        result.limbs_[i / 32] |= 1U << (i % 32);
    return result;
}

U256 U256::bitwise_and(const U256& other) const {
    U256 result;
    for (std::size_t i = 0; i < limbs_.size(); ++i)
        result.limbs_[i] = limbs_[i] & other.limbs_[i];
    return result;
}

U256 U256::bitwise_or(const U256& other) const {
    U256 result;
    for (std::size_t i = 0; i < limbs_.size(); ++i)
        result.limbs_[i] = limbs_[i] | other.limbs_[i];
    return result;
}

bool operator<(const U256& a, const U256& b) {
    for (std::size_t i = a.limbs_.size(); i-- > 0;) {
        if (a.limbs_[i] != b.limbs_[i]) return a.limbs_[i] < b.limbs_[i];
    }
    return false;
}

std::uint32_t U256::divide(const std::uint32_t divisor) {
    std::uint64_t remainder = 0;
    for (std::size_t i = limbs_.size(); i-- > 0;) {
        const std::uint64_t current = (remainder << 32) | limbs_[i];
        limbs_[i] = static_cast<std::uint32_t>(current / divisor);
        remainder = current % divisor;
    }
    return static_cast<std::uint32_t>(remainder);
}

} // namespace c_share::evm
