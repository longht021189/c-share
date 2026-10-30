#include "abi.hpp"

#include "u256.hpp"

// What this file uses, even where CLion finds it through other headers.
// ReSharper disable CppUnusedIncludeDirective
#include <algorithm>
#include <cctype>
// ReSharper restore CppUnusedIncludeDirective

namespace c_share::evm {

namespace {

using nlohmann::json;

constexpr std::size_t kWord = 32;

bool is_identifier_char(const char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
}

// A recursive-descent parser over a function signature or type list.
class Parser {
public:
    explicit Parser(const std::string_view text) : text_(text) {}

    AbiFunction function() {
        AbiFunction function;
        std::string name = identifier();
        if (name == "function") name = identifier();
        if (name.empty()) fail("a function name");
        function.name = name;
        function.inputs = parameters();
        // State mutability, visibility: skipped up to "returns".
        while (true) {
            const std::string word = identifier();
            if (word.empty()) break;
            if (word == "returns") {
                function.outputs = parameters();
                function.has_outputs = true;
                break;
            }
        }
        end();
        return function;
    }

    std::vector<AbiType> types() {
        auto types = parameters();
        end();
        return types;
    }

private:
    // "(" [parameter {"," parameter}] ")", a parameter being a type and
    // words after it (a name, "indexed", a data location), all ignored.
    std::vector<AbiType> parameters() {
        expect('(');
        std::vector<AbiType> types;
        if (accept(')')) return types;
        do {
            types.push_back(type());
            while (!identifier().empty()) {
            }
        } while (accept(','));
        expect(')');
        return types;
    }

    AbiType type() {
        AbiType type;
        skip_spaces();
        if (peek() == '(') {
            type.kind = AbiType::Kind::Tuple;
            type.components = parameters();
        } else {
            if (const std::string name = identifier(); name == "tuple") {
                type.kind = AbiType::Kind::Tuple;
                type.components = parameters();
            } else {
                type = elementary(name);
            }
        }
        // Array suffixes, innermost first: "uint8[2][]" is a dynamic array
        // of uint8[2].
        while (accept('[')) {
            AbiType array;
            skip_spaces();
            std::string digits;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0)
                digits.push_back(text_[pos_++]);
            expect(']');
            if (digits.empty()) {
                array.kind = AbiType::Kind::Array;
            } else {
                array.kind = AbiType::Kind::FixedArray;
                const auto length = U256::parse(digits);
                if (!length || length->bit_length() > 20 || length->is_zero()) fail("an array length from 1 to 2^20");
                array.size = static_cast<unsigned>(length->low64());
            }
            array.components.push_back(std::move(type));
            type = std::move(array);
        }
        return type;
    }

    [[nodiscard]] static AbiType elementary(const std::string& name) {
        AbiType type;
        const auto sized = [&](const std::string& prefix, const AbiType::Kind kind, const unsigned min,
                               const unsigned max, const unsigned step, const unsigned bare) {
            if (name.compare(0, prefix.size(), prefix) != 0) return false;
            const std::string rest = name.substr(prefix.size());
            unsigned size = bare;
            if (!rest.empty()) {
                if (rest.size() > 3 ||
                    !std::all_of(rest.begin(), rest.end(),
                                 [](const char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }) ||
                    rest[0] == '0')
                    return false;
                size = static_cast<unsigned>(std::stoul(rest));
            }
            if (size < min || size > max || size % step != 0) return false;
            type.kind = kind;
            type.size = size;
            return true;
        };
        if (name == "address") {
            type.kind = AbiType::Kind::Address;
        } else if (name == "bool") {
            type.kind = AbiType::Kind::Bool;
        } else if (name == "string") {
            type.kind = AbiType::Kind::String;
        } else if (name == "bytes") {
            type.kind = AbiType::Kind::Bytes;
        } else if (name == "byte") {
            type.kind = AbiType::Kind::FixedBytes;
            type.size = 1;
        } else if (!sized("uint", AbiType::Kind::Uint, 8, 256, 8, 256) &&
                   !sized("int", AbiType::Kind::Int, 8, 256, 8, 256) &&
                   !sized("bytes", AbiType::Kind::FixedBytes, 1, 32, 1, 0)) {
            throw AbiError("unknown ABI type \"" + name + "\"");
        }
        return type;
    }

    std::string identifier() {
        skip_spaces();
        std::string out;
        while (pos_ < text_.size() && is_identifier_char(text_[pos_]))
            out.push_back(text_[pos_++]);
        return out;
    }

    void skip_spaces() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0)
            ++pos_;
    }

    [[nodiscard]] char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    bool accept(const char c) {
        skip_spaces();
        if (peek() != c) return false;
        ++pos_;
        return true;
    }

    void expect(const char c) {
        if (!accept(c)) fail(std::string("'") + c + "'");
    }

    void end() {
        skip_spaces();
        if (pos_ != text_.size()) fail("the end");
    }

    [[noreturn]] void fail(const std::string& expected) const {
        throw AbiError("expected " + expected + " at position " + std::to_string(pos_) + " of \"" + std::string(text_) +
                       "\"");
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

std::string describe(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// An unsigned value of a uintN, or the magnitude and sign of an intN's.
struct Integer {
    U256 magnitude;
    bool negative = false;
};

Integer integer_value(const AbiType& type, const json& value) {
    Integer out;
    if (value.is_number_unsigned()) {
        out.magnitude = U256(value.get<std::uint64_t>());
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        out.negative = signed_value < 0;
        out.magnitude = U256(signed_value < 0 ? 0 - static_cast<std::uint64_t>(signed_value)
                                              : static_cast<std::uint64_t>(signed_value));
    } else if (value.is_string()) {
        std::string_view text = value.get_ref<const std::string&>();
        if (!text.empty() && text[0] == '-') {
            out.negative = true;
            text.remove_prefix(1);
        }
        const auto parsed = U256::parse(text);
        if (!parsed) throw AbiError("not a number: " + describe(value));
        out.magnitude = *parsed;
    } else {
        throw AbiError(type.canonical() + " takes an integer (a number or a string), not " + describe(value));
    }
    if (out.negative && out.magnitude.is_zero()) out.negative = false;
    return out;
}

std::array<std::uint8_t, kWord> integer_word(const AbiType& type, const json& value) {
    const auto [magnitude, negative] = integer_value(type, value);
    if (type.kind == AbiType::Kind::Uint) {
        if (negative || magnitude.bit_length() > type.size)
            throw AbiError(describe(value) + " does not fit " + type.canonical());
        return magnitude.to_big_endian();
    }
    // intN holds [-2^(N-1), 2^(N-1) - 1].
    const U256 limit = U256::mask(type.size - 1); // 2^(N-1) - 1
    U256 negative_limit = limit;
    (void)negative_limit.add(U256(1));
    if (negative ? negative_limit < magnitude : limit < magnitude)
        throw AbiError(describe(value) + " does not fit " + type.canonical());
    return (negative ? magnitude.negated() : magnitude).to_big_endian();
}

Bytes hex_value(const AbiType& type, const json& value) {
    if (!value.is_string()) throw AbiError(type.canonical() + " takes a hex string, not " + describe(value));
    auto bytes = from_hex(value.get_ref<const std::string&>());
    if (!bytes) throw AbiError(type.canonical() + " takes \"0x\" and hex digits, not " + describe(value));
    return std::move(*bytes);
}

void append(Bytes& out, const std::uint8_t* data, const std::size_t size) {
    out.insert(out.end(), data, data + size);
}

void append_word(Bytes& out, const U256& value) {
    const auto word = value.to_big_endian();
    append(out, word.data(), word.size());
}

// `data`, then zeros up to a multiple of 32 bytes.
void append_padded(Bytes& out, const std::uint8_t* data, const std::size_t size) {
    append(out, data, size);
    out.resize(out.size() + (kWord - size % kWord) % kWord, 0);
}

Bytes encode_tuple(const std::vector<AbiType>& types, const json& values);

Bytes encode_value(const AbiType& type, const json& value) {
    Bytes out;
    switch (type.kind) {
        case AbiType::Kind::Uint:
        case AbiType::Kind::Int: {
            const auto word = integer_word(type, value);
            append(out, word.data(), word.size());
            break;
        }
        case AbiType::Kind::Address: {
            if (!value.is_string()) throw AbiError("address takes a string, not " + describe(value));
            const auto address = parse_address(value.get_ref<const std::string&>());
            if (!address) throw AbiError("not an address (or a wrong EIP-55 checksum): " + describe(value));
            out.resize(kWord - address->size(), 0);
            append(out, address->data(), address->size());
            break;
        }
        case AbiType::Kind::Bool:
            if (!value.is_boolean()) throw AbiError("bool takes true or false, not " + describe(value));
            append_word(out, U256(value.get<bool>() ? 1 : 0));
            break;
        case AbiType::Kind::FixedBytes: {
            const Bytes bytes = hex_value(type, value);
            if (bytes.size() != type.size)
                throw AbiError(type.canonical() + " takes exactly " + std::to_string(type.size) + " bytes, not " +
                               describe(value));
            append_padded(out, bytes.data(), bytes.size());
            break;
        }
        case AbiType::Kind::Bytes: {
            const Bytes bytes = hex_value(type, value);
            append_word(out, U256(bytes.size()));
            append_padded(out, bytes.data(), bytes.size());
            break;
        }
        case AbiType::Kind::String: {
            if (!value.is_string()) throw AbiError("string takes a string, not " + describe(value));
            const auto& text = value.get_ref<const std::string&>();
            append_word(out, U256(text.size()));
            append_padded(out, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
            break;
        }
        case AbiType::Kind::Array:
        case AbiType::Kind::FixedArray: {
            if (!value.is_array()) throw AbiError(type.canonical() + " takes an array, not " + describe(value));
            if (type.kind == AbiType::Kind::FixedArray && value.size() != type.size)
                throw AbiError(type.canonical() + " takes " + std::to_string(type.size) + " values, not " +
                               std::to_string(value.size()));
            if (type.kind == AbiType::Kind::Array) append_word(out, U256(value.size()));
            const std::vector elements(value.size(), type.components[0]);
            const Bytes encoded = encode_tuple(elements, value);
            append(out, encoded.data(), encoded.size());
            break;
        }
        case AbiType::Kind::Tuple: {
            const Bytes encoded = encode_tuple(type.components, value);
            append(out, encoded.data(), encoded.size());
            break;
        }
    }
    return out;
}

// The heads of every value in order, then the tails of the dynamic ones,
// each head of a dynamic value being its tail's offset from the start.
Bytes encode_tuple(const std::vector<AbiType>& types, const json& values) {
    if (!values.is_array()) throw AbiError("values come as a JSON array, not " + describe(values));
    if (values.size() != types.size())
        throw AbiError("expected " + std::to_string(types.size()) + " values, not " + std::to_string(values.size()));
    std::size_t heads_size = 0;
    for (const auto& type : types)
        heads_size += type.head_size();
    Bytes heads;
    Bytes tails;
    for (std::size_t i = 0; i < types.size(); ++i) {
        Bytes encoded = encode_value(types[i], values[i]);
        if (types[i].dynamic()) {
            append_word(heads, U256(heads_size + tails.size()));
            append(tails, encoded.data(), encoded.size());
        } else {
            append(heads, encoded.data(), encoded.size());
        }
    }
    append(heads, tails.data(), tails.size());
    return heads;
}

// Reads `data` with bounds checks: a decoder must not trust offsets and
// lengths it is given.
class Reader {
public:
    explicit Reader(const Bytes& data) : data_(data) {}

    [[nodiscard]] U256 word(const std::size_t at) const {
        need(at, kWord);
        return U256::from_big_endian(data_.data() + at, kWord);
    }

    // A word that is an offset or a length: it must fit the data.
    [[nodiscard]] std::size_t size_word(const std::size_t at) const {
        const U256 value = word(at);
        if (value.bit_length() > 32 || value.low64() > data_.size())
            throw AbiError("an offset or length past the data");
        // Narrows on 32-bit targets.
        // ReSharper disable once CppRedundantCastExpression
        return static_cast<std::size_t>(value.low64());
    }

    [[nodiscard]] Bytes bytes(const std::size_t at, const std::size_t size) const {
        need(at, size);
        return {data_.begin() + static_cast<std::ptrdiff_t>(at),
                data_.begin() + static_cast<std::ptrdiff_t>(at + size)};
    }

    [[nodiscard]] std::size_t size() const { return data_.size(); }

private:
    void need(const std::size_t at, const std::size_t size) const {
        if (at > data_.size() || size > data_.size() - at) throw AbiError("the data ends early");
    }

    const Bytes& data_;
};

json decode_tuple(const std::vector<AbiType>& types, const Reader& reader, std::size_t base);

json decode_value(const AbiType& type, const Reader& reader, const std::size_t at) {
    switch (type.kind) {
        case AbiType::Kind::Uint: {
            const U256 value = reader.word(at);
            if (value.bit_length() > type.size) throw AbiError("a value too large for " + type.canonical());
            return value.to_decimal();
        }
        case AbiType::Kind::Int: {
            const U256 value = reader.word(at);
            // Bits N-1 to 255 must all repeat the sign bit (bit N-1), so the
            // word is the value sign-extended to 256 bits.
            const bool negative = value.bit(type.size - 1);
            if (const U256 sign_bits = U256::mask(type.size - 1).inverted();
                value.bitwise_and(sign_bits) != (negative ? sign_bits : U256()))
                throw AbiError("a value too large for " + type.canonical());
            return negative ? "-" + value.negated().to_decimal() : value.to_decimal();
        }
        case AbiType::Kind::Address: {
            const Bytes word = reader.bytes(at, kWord);
            if (std::any_of(word.begin(), word.begin() + 12, [](const std::uint8_t b) { return b != 0; }))
                throw AbiError("an address with bits above its 160");
            Address address{};
            std::copy(word.begin() + 12, word.end(), address.begin());
            return to_checksum_address(address);
        }
        case AbiType::Kind::Bool: {
            const U256 value = reader.word(at);
            if (value.bit_length() > 1) throw AbiError("a bool that is neither 0 nor 1");
            return !value.is_zero();
        }
        case AbiType::Kind::FixedBytes: return to_hex(reader.bytes(at, type.size));
        case AbiType::Kind::Bytes:
        case AbiType::Kind::String: {
            const std::size_t length = reader.size_word(at);
            const Bytes bytes = reader.bytes(at + kWord, length);
            if (type.kind == AbiType::Kind::Bytes) return to_hex(bytes);
            return std::string(bytes.begin(), bytes.end());
        }
        case AbiType::Kind::Array: {
            const std::size_t length = reader.size_word(at);
            // Every element takes at least one word: a longer claim is false.
            if (length > (reader.size() - at - kWord) / kWord) throw AbiError("an array longer than the data");
            return decode_tuple(std::vector(length, type.components[0]), reader, at + kWord);
        }
        case AbiType::Kind::FixedArray:
            return decode_tuple(std::vector(std::size_t{type.size}, type.components[0]), reader, at);
        case AbiType::Kind::Tuple: return decode_tuple(type.components, reader, at);
    }
    throw AbiError("unknown type");
}

// `base` is 0 only for the outermost tuple: arrays and tuples within it
// recurse with their own.
// ReSharper disable once CppDFAConstantParameter
json decode_tuple(const std::vector<AbiType>& types, const Reader& reader, const std::size_t base) {
    json values = json::array();
    std::size_t at = base;
    for (const auto& type : types) {
        if (type.dynamic()) {
            const std::size_t offset = reader.size_word(at);
            values.push_back(decode_value(type, reader, base + offset));
        } else {
            values.push_back(decode_value(type, reader, at));
        }
        at += type.head_size();
    }
    return values;
}

} // namespace

bool AbiType::dynamic() const {
    switch (kind) {
        case Kind::Bytes:
        case Kind::String:
        case Kind::Array: return true;
        case Kind::FixedArray: return components[0].dynamic();
        case Kind::Tuple:
            return std::any_of(components.begin(), components.end(), [](const AbiType& c) { return c.dynamic(); });
        default: return false;
    }
}

std::size_t AbiType::head_size() const {
    if (dynamic()) return kWord;
    if (kind == Kind::FixedArray) return size * components[0].head_size();
    if (kind == Kind::Tuple) {
        std::size_t total = 0;
        for (const auto& component : components)
            total += component.head_size();
        return total;
    }
    return kWord;
}

std::string AbiType::canonical() const {
    switch (kind) {
        case Kind::Uint: return "uint" + std::to_string(size);
        case Kind::Int: return "int" + std::to_string(size);
        case Kind::Address: return "address";
        case Kind::Bool: return "bool";
        case Kind::FixedBytes: return "bytes" + std::to_string(size);
        case Kind::Bytes: return "bytes";
        case Kind::String: return "string";
        case Kind::Array: return components[0].canonical() + "[]";
        case Kind::FixedArray: return components[0].canonical() + "[" + std::to_string(size) + "]";
        case Kind::Tuple: {
            std::string out = "(";
            for (std::size_t i = 0; i < components.size(); ++i) {
                if (i > 0) out += ',';
                out += components[i].canonical();
            }
            return out + ")";
        }
    }
    return {};
}

std::string AbiFunction::signature() const {
    AbiType tuple;
    tuple.kind = AbiType::Kind::Tuple;
    tuple.components = inputs;
    return name + tuple.canonical();
}

std::array<std::uint8_t, 4> AbiFunction::selector() const {
    const std::string text = signature();
    const auto hash = keccak256(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return {hash[0], hash[1], hash[2], hash[3]};
}

AbiFunction parse_function(const std::string_view text) {
    return Parser(text).function();
}

std::vector<AbiType> parse_types(const std::string_view text) {
    return Parser(text).types();
}

Bytes abi_encode(const std::vector<AbiType>& types, const json& values) {
    return encode_tuple(types, values);
}

json abi_decode(const std::vector<AbiType>& types, const Bytes& data) {
    return decode_tuple(types, Reader(data), 0);
}

Bytes encode_call(const AbiFunction& function, const json& args) {
    const auto selector = function.selector();
    Bytes out(selector.begin(), selector.end());
    const Bytes encoded = abi_encode(function.inputs, args);
    out.insert(out.end(), encoded.begin(), encoded.end());
    return out;
}

std::optional<std::string> decode_revert_reason(const Bytes& data) {
    if (data.size() < 4) return std::nullopt;
    const Bytes body(data.begin() + 4, data.end());
    try {
        if (data[0] == 0x08 && data[1] == 0xc3 && data[2] == 0x79 && data[3] == 0xa0) {
            return abi_decode(parse_types("(string)"), body)[0].get<std::string>();
        }
        if (data[0] == 0x4e && data[1] == 0x48 && data[2] == 0x7b && data[3] == 0x71) {
            const auto code = U256::parse(abi_decode(parse_types("(uint256)"), body)[0].get<std::string>());
            return "panic " + (code ? code->to_quantity() : std::string("?"));
        }
    } catch (const AbiError&) {
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace c_share::evm
