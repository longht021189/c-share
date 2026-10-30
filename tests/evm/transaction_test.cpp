#include "transaction.hpp"

#include "c_share/crypto/crypto.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using c_share::evm::Address;
using c_share::evm::Bytes;
using c_share::evm::from_hex;
using c_share::evm::to_hex;
using c_share::evm::Transaction;
using c_share::evm::U256;
namespace rlp = c_share::evm::rlp;

namespace {

Bytes text(const std::string& value) {
    return {value.begin(), value.end()};
}

Address address_35() {
    Address address{};
    address.fill(0x35);
    return address;
}

// Signs `tx` with EIP-155's example key, 0x46...46.
std::string sign(const Transaction& tx) {
    std::array<std::uint8_t, 32> key{};
    key.fill(0x46);
    const auto hash = tx.signing_hash();
    std::array<std::uint8_t, 65> signature{};
    REQUIRE(c_share_crypto_secp256k1_sign_recoverable(key.data(), hash.data(), signature.data()) == 0);
    return to_hex(tx.encode_signed(signature), false);
}

} // namespace

// The examples on ethereum.org's RLP page.
TEST_CASE("RLP encodes as its specification's examples", "[evm][rlp]") {
    CHECK(to_hex(rlp::item(text("dog"))) == "0x83646f67");
    CHECK(to_hex(rlp::list({rlp::item(text("cat")), rlp::item(text("dog"))})) == "0xc88363617483646f67");
    CHECK(to_hex(rlp::item(Bytes{})) == "0x80");
    CHECK(to_hex(rlp::list({})) == "0xc0");
    CHECK(to_hex(rlp::item(U256(0))) == "0x80");
    CHECK(to_hex(rlp::item(Bytes{0x00})) == "0x00");
    CHECK(to_hex(rlp::item(U256(15))) == "0x0f");
    CHECK(to_hex(rlp::item(U256(1024))) == "0x820400");
    // [ [], [[]], [ [], [[]] ] ]
    const Bytes empty = rlp::list({});
    CHECK(to_hex(rlp::list({empty, rlp::list({empty}), rlp::list({empty, rlp::list({empty})})})) ==
          "0xc7c0c1c0c3c0c1c0");
    const std::string lorem = "Lorem ipsum dolor sit amet, consectetur adipisicing elit";
    CHECK(to_hex(rlp::item(text(lorem))) == "0xb838" + to_hex(text(lorem), false));
}

// Expected transactions from eth-account 0.13.7's sign_transaction, for the
// same fields and key: both sign with RFC 6979, so the bytes must match.
TEST_CASE("EIP-1559 transactions sign as eth-account signs them", "[evm][transaction]") {
    SECTION("an ERC-20 transfer on mainnet") {
        Transaction tx;
        tx.chain_id = U256(1);
        tx.nonce = U256(9);
        tx.max_priority_fee_per_gas = U256(1'000'000'000);
        tx.max_fee_per_gas = U256(21'000'000'000);
        tx.gas_limit = U256(60000);
        tx.to = address_35();
        tx.data = *from_hex("0xa9059cbb0000000000000000000000003535353535353535353535353535353535353535"
                            "00000000000000000000000000000000000000000000000000000000000003e8");
        CHECK(sign(tx) == "02f8b00109843b9aca008504e3b2920082ea609435353535353535353535353535353535353535358"
                          "0b844a9059cbb000000000000000000000000353535353535353535353535353535353535353500"
                          "000000000000000000000000000000000000000000000000000000000003e8c080a07b8d08ceceb7"
                          "8b5865f18a1fc8c06663a62563349fffb904d998d0a958d7f06da06818a580272d0e5463d3548a5d"
                          "950adfa4020670d4b0428de41360bfabb543a7");
    }
    SECTION("1 ETH on Sepolia") {
        Transaction tx;
        tx.chain_id = U256(11155111);
        tx.max_priority_fee_per_gas = U256(2);
        tx.max_fee_per_gas = U256(30'000'000'000);
        tx.gas_limit = U256(21000);
        tx.to = address_35();
        tx.value = *U256::parse("1000000000000000000");
        CHECK(sign(tx) == "02f87283aa36a780028506fc23ac00825208943535353535353535353535353535353535353535880d"
                          "e0b6b3a764000080c080a0605a369793d65fd87a1ea9f56f07c5a2ec7be8d225369014d58e120a6a8b"
                          "9b95a03fb44456d0272d612097fef8b42ad007a881188545b18cfa5b003efce25063d4");
    }
}
