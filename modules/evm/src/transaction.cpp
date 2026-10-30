#include "transaction.hpp"

namespace c_share::evm {

namespace rlp {

namespace {

// A string's or list's length prefix: `offset` (0x80 strings, 0xc0 lists)
// plus the length up to 55, else plus 55 and the length of the length.
Bytes prefix(const std::uint8_t offset, const std::size_t length) {
    if (length <= 55) return {static_cast<std::uint8_t>(offset + length)};
    const auto length_bytes = U256(length).to_minimal_big_endian();
    Bytes out{static_cast<std::uint8_t>(offset + 55 + length_bytes.size())};
    out.insert(out.end(), length_bytes.begin(), length_bytes.end());
    return out;
}

} // namespace

Bytes item(const std::uint8_t* data, const std::size_t size) {
    // A single byte below 0x80 is its own encoding.
    if (size == 1 && data[0] < 0x80) return {data[0]};
    Bytes out = prefix(0x80, size);
    out.insert(out.end(), data, data + size);
    return out;
}

Bytes list(const std::vector<Bytes>& items) {
    std::size_t length = 0;
    for (const auto& encoded : items)
        length += encoded.size();
    Bytes out = prefix(0xc0, length);
    for (const auto& encoded : items)
        out.insert(out.end(), encoded.begin(), encoded.end());
    return out;
}

} // namespace rlp

namespace {

constexpr std::uint8_t kEip1559Type = 0x02;

Bytes typed(const Bytes& payload) {
    Bytes out{kEip1559Type};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

} // namespace

std::vector<Bytes> Transaction::fields() const {
    return {
        rlp::item(chain_id),
        rlp::item(nonce),
        rlp::item(max_priority_fee_per_gas),
        rlp::item(max_fee_per_gas),
        rlp::item(gas_limit),
        to ? rlp::item(to->data(), to->size()) : rlp::item(nullptr, 0),
        rlp::item(value),
        rlp::item(data),
        rlp::list({}), // access list: none
    };
}

std::array<std::uint8_t, 32> Transaction::signing_hash() const {
    return keccak256(typed(rlp::list(fields())));
}

Bytes Transaction::encode_signed(const std::array<std::uint8_t, 65>& signature) const {
    std::vector<Bytes> items = fields();
    items.push_back(rlp::item(U256(signature[64])));
    items.push_back(rlp::item(U256::from_big_endian(signature.data(), 32)));
    items.push_back(rlp::item(U256::from_big_endian(signature.data() + 32, 32)));
    return typed(rlp::list(items));
}

} // namespace c_share::evm
