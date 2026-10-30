#pragma once

// EIP-1559 (type 2) transactions: RLP-encoded, signed, ready for
// eth_sendRawTransaction.

#include "hex.hpp"
#include "u256.hpp"

#include <array>
#include <optional>
#include <vector>

namespace c_share::evm {

// RLP, the encoding transactions are signed and sent in.
namespace rlp {

// A byte string item.
Bytes item(const std::uint8_t* data, std::size_t size);
inline Bytes item(const Bytes& bytes) {
    return item(bytes.data(), bytes.size());
}
// An integer item: big-endian without leading zeros (0 is empty).
inline Bytes item(const U256& value) {
    return item(value.to_minimal_big_endian());
}
// A list of already-encoded items.
Bytes list(const std::vector<Bytes>& items);

} // namespace rlp

struct Transaction {
    U256 chain_id;
    U256 nonce;
    U256 max_priority_fee_per_gas;
    U256 max_fee_per_gas;
    U256 gas_limit;
    std::optional<Address> to; // none: a contract deployment
    U256 value;
    Bytes data;

    // keccak256(0x02 || rlp([chain_id, nonce, ..., data, access_list])), the
    // hash a type 2 transaction's signature is over.
    [[nodiscard]] std::array<std::uint8_t, 32> signing_hash() const;
    // 0x02 || rlp([..., access_list, y_parity, r, s]) for a signature
    // r || s || y_parity, as the crypto module's recoverable ones are.
    [[nodiscard]] Bytes encode_signed(const std::array<std::uint8_t, 65>& signature) const;

private:
    [[nodiscard]] std::vector<Bytes> fields() const;
};

} // namespace c_share::evm
