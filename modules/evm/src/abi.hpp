#pragma once

// The Solidity contract ABI: function signatures in their human-readable
// form, and values encoded to and decoded from calldata, as JSON.
//
// Values, both ways:
//   uintN, intN   a JSON number, or a string: decimal (intN may start with
//                 '-') or "0x" hex. Decoded: always a decimal string, since
//                 JSON numbers lose precision past 2^53.
//   address       "0x" and 40 hex digits (a mixed-case one must be its
//                 EIP-55 checksum). Decoded: checksummed.
//   bool          true or false.
//   bytesN, bytes "0x" and hex digits (bytesN: exactly N bytes).
//   string        a JSON string.
//   T[], T[k]     a JSON array (T[k]: of k values).
//   tuples        a JSON array of the components' values.

#include "hex.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace c_share::evm {

// A signature that does not parse, or a value that does not fit its type.
class AbiError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct AbiType {
    enum class Kind {
        Uint,
        Int,
        Address,
        Bool,
        FixedBytes,
        Bytes,
        String,
        Array,
        FixedArray,
        Tuple,
    };

    Kind kind = Kind::Uint;
    // Bits for Uint/Int, bytes for FixedBytes, the length for FixedArray.
    unsigned size = 0;
    // A tuple's components, or an array's one element type.
    std::vector<AbiType> components;

    [[nodiscard]] bool dynamic() const;
    // The bytes it takes in its enclosing tuple's head.
    [[nodiscard]] std::size_t head_size() const;
    // As in a function signature: "uint256", "(address,bool)[]".
    [[nodiscard]] std::string canonical() const;
};

struct AbiFunction {
    std::string name;
    std::vector<AbiType> inputs;
    std::vector<AbiType> outputs;
    // Whether the signature said what it returns ("returns (...)").
    bool has_outputs = false;

    // "transfer(address,uint256)"
    [[nodiscard]] std::string signature() const;
    [[nodiscard]] std::array<std::uint8_t, 4> selector() const;
};

// A function as Solidity, ethers and viem write it, e.g.
//   "function balanceOf(address owner) view returns (uint256)"
//   "transfer(address,uint256)"
// The "function" keyword, parameter names, data locations and state
// mutability are optional and ignored.
AbiFunction parse_function(std::string_view text);
// A parenthesized list of types, e.g. "(uint256,bool)", names allowed.
std::vector<AbiType> parse_types(std::string_view text);

// The values of `types` (a JSON array, one value each), ABI-encoded.
Bytes abi_encode(const std::vector<AbiType>& types, const nlohmann::json& values);
// `data` decoded as `types`: a JSON array, one value each.
nlohmann::json abi_decode(const std::vector<AbiType>& types, const Bytes& data);

// The calldata calling `function` with `args` (a JSON array).
Bytes encode_call(const AbiFunction& function, const nlohmann::json& args);

// What a revert's data says: "reason" for Error(string), "panic 0x11" for
// Panic(uint256). nullopt for anything else (e.g. a custom error).
std::optional<std::string> decode_revert_reason(const Bytes& data);

} // namespace c_share::evm
