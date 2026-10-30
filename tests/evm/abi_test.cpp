#include "abi.hpp"
#include "hex.hpp"
#include "u256.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

using c_share::evm::abi_decode;
using c_share::evm::AbiError;
using c_share::evm::Bytes;
using c_share::evm::decode_revert_reason;
using c_share::evm::encode_call;
using c_share::evm::from_hex;
using c_share::evm::parse_address;
using c_share::evm::parse_function;
using c_share::evm::to_checksum_address;
using c_share::evm::to_hex;
using c_share::evm::U256;
using nlohmann::json;

namespace {

std::string call_hex(const std::string& function, const std::string& args) {
    return to_hex(encode_call(parse_function(function), json::parse(args)));
}

Bytes bytes(const std::string& hex) {
    const auto parsed = from_hex(hex);
    REQUIRE(parsed);
    return *parsed;
}

} // namespace

TEST_CASE("U256 parses, prints and refuses what does not fit", "[evm][u256]") {
    CHECK(U256::parse("0")->to_decimal() == "0");
    CHECK(U256::parse("1000000000000000000")->to_quantity() == "0xde0b6b3a7640000");
    CHECK(U256::parse("0xff")->to_decimal() == "255");
    const std::string max = "115792089237316195423570985008687907853269984665640564039457584007913129639935";
    CHECK(U256::parse(max)->to_quantity() == "0x" + std::string(64, 'f'));
    CHECK_FALSE(U256::parse("115792089237316195423570985008687907853269984665640564039457584007913129639936"));
    CHECK_FALSE(U256::parse("0x1" + std::string(64, '0')));
    CHECK_FALSE(U256::parse(""));
    CHECK_FALSE(U256::parse("12a"));
    CHECK_FALSE(U256::from_quantity("12"));
    CHECK(U256::from_quantity("0x0")->is_zero());
    CHECK(U256().to_quantity() == "0x0");
    CHECK(U256(1024).to_minimal_big_endian() == Bytes{0x04, 0x00});
}

// EIP-55's own examples.
TEST_CASE("addresses check and produce EIP-55 checksums", "[evm][address]") {
    const auto address = GENERATE(
        as<std::string>{}, "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359",
        "0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB", "0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb");
    CAPTURE(address);
    const auto parsed = parse_address(address);
    REQUIRE(parsed);
    CHECK(to_checksum_address(*parsed) == address);
    // All lowercase (or all uppercase) carries no checksum to check.
    std::string lower = address;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    CHECK(parse_address(lower));
    // One letter's case changed is a wrong checksum.
    std::string wrong = address;
    const auto letter = wrong.find_first_of("abcdef", 2);
    wrong[letter] = static_cast<char>(std::toupper(static_cast<unsigned char>(wrong[letter])));
    CHECK_FALSE(parse_address(wrong));
    CHECK_FALSE(parse_address(address.substr(0, 41)));
}

TEST_CASE("function signatures parse in their human-readable forms", "[evm][abi]") {
    const auto transfer = parse_function("function transfer(address to, uint256 amount) external returns (bool)");
    CHECK(transfer.signature() == "transfer(address,uint256)");
    CHECK(to_hex(transfer.selector().data(), 4) == "0xa9059cbb");
    CHECK(transfer.has_outputs);
    REQUIRE(transfer.outputs.size() == 1);
    CHECK(transfer.outputs[0].canonical() == "bool");

    const auto balance = parse_function("balanceOf(address)");
    CHECK(to_hex(balance.selector().data(), 4) == "0x70a08231");
    CHECK_FALSE(balance.has_outputs);

    const auto nested = parse_function(
        "function f(tuple(uint a, bytes32 b)[2] calldata items, (string,int)[] memory more, uint8[][3] grid) "
        "view returns ((address owner, uint256[] ids) result)");
    CHECK(nested.signature() == "f((uint256,bytes32)[2],(string,int256)[],uint8[][3])");
    CHECK(nested.outputs[0].canonical() == "(address,uint256[])");

    for (const char* bad : {"transfer(address", "f(uint7)", "f(bytes33)", "f(uint264)", "f(foo)", "f(uint256[0])",
                            "f() returns", "(uint256)", "f(uint256) extra (", "f(uint08)"}) {
        CAPTURE(bad);
        CHECK_THROWS_AS(parse_function(bad), AbiError);
    }
}

// The examples in the Solidity documentation's ABI specification.
TEST_CASE("calls encode as the Solidity ABI specification's examples", "[evm][abi]") {
    CHECK(call_hex("baz(uint32,bool)", "[69, true]") ==
          "0xcdcd77c0"
          "0000000000000000000000000000000000000000000000000000000000000045"
          "0000000000000000000000000000000000000000000000000000000000000001");
    CHECK(call_hex("bar(bytes3[2])", R"([["0x616263", "0x646566"]])") ==
          "0xfce353f6"
          "6162630000000000000000000000000000000000000000000000000000000000"
          "6465660000000000000000000000000000000000000000000000000000000000");
    CHECK(call_hex("sam(bytes,bool,uint256[])", R"(["0x64617665", true, [1, 2, 3]])") ==
          "0xa5643bf2"
          "0000000000000000000000000000000000000000000000000000000000000060"
          "0000000000000000000000000000000000000000000000000000000000000001"
          "00000000000000000000000000000000000000000000000000000000000000a0"
          "0000000000000000000000000000000000000000000000000000000000000004"
          "6461766500000000000000000000000000000000000000000000000000000000"
          "0000000000000000000000000000000000000000000000000000000000000003"
          "0000000000000000000000000000000000000000000000000000000000000001"
          "0000000000000000000000000000000000000000000000000000000000000002"
          "0000000000000000000000000000000000000000000000000000000000000003");
    CHECK(call_hex("f(uint256,uint32[],bytes10,bytes)",
                   R"(["0x123", ["0x456", "0x789"], "0x31323334353637383930", "0x48656c6c6f2c20776f726c6421"])") ==
          "0x8be65246"
          "0000000000000000000000000000000000000000000000000000000000000123"
          "0000000000000000000000000000000000000000000000000000000000000080"
          "3132333435363738393000000000000000000000000000000000000000000000"
          "00000000000000000000000000000000000000000000000000000000000000e0"
          "0000000000000000000000000000000000000000000000000000000000000002"
          "0000000000000000000000000000000000000000000000000000000000000456"
          "0000000000000000000000000000000000000000000000000000000000000789"
          "000000000000000000000000000000000000000000000000000000000000000d"
          "48656c6c6f2c20776f726c642100000000000000000000000000000000000000");
    CHECK(call_hex("g(uint256[][],string[])", R"([[[1, 2], [3]], ["one", "two", "three"]])") ==
          "0x2289b18c"
          "0000000000000000000000000000000000000000000000000000000000000040"
          "0000000000000000000000000000000000000000000000000000000000000140"
          "0000000000000000000000000000000000000000000000000000000000000002"
          "0000000000000000000000000000000000000000000000000000000000000040"
          "00000000000000000000000000000000000000000000000000000000000000a0"
          "0000000000000000000000000000000000000000000000000000000000000002"
          "0000000000000000000000000000000000000000000000000000000000000001"
          "0000000000000000000000000000000000000000000000000000000000000002"
          "0000000000000000000000000000000000000000000000000000000000000001"
          "0000000000000000000000000000000000000000000000000000000000000003"
          "0000000000000000000000000000000000000000000000000000000000000003"
          "0000000000000000000000000000000000000000000000000000000000000060"
          "00000000000000000000000000000000000000000000000000000000000000a0"
          "00000000000000000000000000000000000000000000000000000000000000e0"
          "0000000000000000000000000000000000000000000000000000000000000003"
          "6f6e650000000000000000000000000000000000000000000000000000000000"
          "0000000000000000000000000000000000000000000000000000000000000003"
          "74776f0000000000000000000000000000000000000000000000000000000000"
          "0000000000000000000000000000000000000000000000000000000000000005"
          "7468726565000000000000000000000000000000000000000000000000000000");
}

TEST_CASE("values decode back to what was encoded", "[evm][abi]") {
    const std::string function = "f(uint8 a, int16 b, int256 c, address d, bool e, bytes4 f, bytes g, string h, "
                                 "(uint256,string)[] i, int8[2] j)";
    const json args = json::parse(R"([
        "255", "-32768", "-1", "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", false, "0xdeadbeef", "0x",
        "héllo", [["1", "a"], ["2", ""]], ["-128", "127"]
    ])");
    const auto parsed = parse_function(function);
    const Bytes data = encode_call(parsed, args);
    CHECK(abi_decode(parsed.inputs, Bytes(data.begin() + 4, data.end())) == args);
}

TEST_CASE("values that do not fit their types are refused", "[evm][abi]") {
    const auto [function, args] = GENERATE(table<std::string, std::string>({
        {"f(uint8)", "[256]"},
        {"f(uint8)", R"(["-1"])"},
        {"f(int8)", "[128]"},
        {"f(int8)", "[-129]"},
        {"f(uint256)", "[1.5]"},
        {"f(uint256)", R"(["12a"])"},
        {"f(address)", R"(["0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAeD"])"},
        {"f(address)", R"(["0x1234"])"},
        {"f(bool)", "[1]"},
        {"f(bytes4)", R"(["0xdeadbe"])"},
        {"f(bytes)", R"(["dead"])"},
        {"f(string)", "[1]"},
        {"f(uint8[2])", "[[1]]"},
        {"f((uint8,bool))", "[[1]]"},
        {"f(uint8,uint8)", "[1]"},
        {"f(uint8)", R"({"a": 1})"},
    }));
    CAPTURE(function, args);
    CHECK_THROWS_AS(encode_call(parse_function(function), json::parse(args)), AbiError);
}

TEST_CASE("decoding refuses data that lies about its offsets and lengths", "[evm][abi]") {
    const auto types = parse_function("f() returns (bytes, uint256[])").outputs;
    // An offset far past the data.
    CHECK_THROWS_AS(abi_decode(types, bytes("0x" + std::string(62, 'f') + "00" + std::string(64, '0'))), AbiError);
    // A length past the data.
    CHECK_THROWS_AS(abi_decode(types, bytes("0x"
                                            "0000000000000000000000000000000000000000000000000000000000000040"
                                            "0000000000000000000000000000000000000000000000000000000000000060"
                                            "00000000000000000000000000000000000000000000000000000000ffffffff")),
                    AbiError);
    // Too short for its heads.
    CHECK_THROWS_AS(abi_decode(types, bytes("0x00")), AbiError);
    // Values out of their types' range.
    CHECK_THROWS_AS(
        abi_decode(parse_function("f() returns (uint8)").outputs, bytes("0x" + std::string(60, '0') + "0100")),
        AbiError);
    CHECK_THROWS_AS(abi_decode(parse_function("f() returns (bool)").outputs, bytes("0x" + std::string(63, '0') + "2")),
                    AbiError);
    CHECK_THROWS_AS(abi_decode(parse_function("f() returns (int8)").outputs, bytes("0x" + std::string(62, '0') + "80")),
                    AbiError);
}

TEST_CASE("revert data reads as its reason", "[evm][abi]") {
    // Error("Not enough Ether provided.")
    CHECK(decode_revert_reason(bytes("0x08c379a0"
                                     "0000000000000000000000000000000000000000000000000000000000000020"
                                     "000000000000000000000000000000000000000000000000000000000000001a"
                                     "4e6f7420656e6f7567682045746865722070726f76696465642e000000000000")) ==
          "Not enough Ether provided.");
    // Panic(0x11): an arithmetic overflow.
    CHECK(decode_revert_reason(bytes("0x4e487b71" + std::string(62, '0') + "11")) == "panic 0x11");
    CHECK_FALSE(decode_revert_reason(bytes("0x12345678")));
    CHECK_FALSE(decode_revert_reason(bytes("0x08c379a0")));
}
