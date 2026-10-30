#include "ws_codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <string>
#include <vector>

using c_share::http::detail::parse_ws_extensions;
using c_share::http::detail::WsCodec;
using c_share::http::detail::WsDeflateParams;
using c_share::http::detail::WsIncoming;
using c_share::http::detail::WsOpcode;

namespace {

std::string bytes(const std::vector<int>& values) {
    std::string out;
    for (const int value : values)
        out.push_back(static_cast<char>(value));
    return out;
}

WsDeflateParams deflate_params() {
    WsDeflateParams params;
    params.enabled = true;
    return params;
}

// Feeds `data` to `codec`, requiring it to be valid.
std::vector<WsIncoming> decode(WsCodec& codec, const std::string& data) {
    std::vector<WsIncoming> out;
    std::string error;
    REQUIRE(codec.decode(data.data(), data.size(), out, error));
    CHECK(error.empty());
    return out;
}

// Feeds `data` to `codec`, requiring it to fail; returns the error.
std::string decode_error(WsCodec& codec, const std::string& data) {
    std::vector<WsIncoming> out;
    std::string error;
    REQUIRE_FALSE(codec.decode(data.data(), data.size(), out, error));
    CHECK_FALSE(error.empty());
    return error;
}

// A frame `encode` produced, unmasked: {first byte, payload}.
struct Sent {
    std::uint8_t first;
    std::string payload;
};

Sent unmask(const std::string& frame) {
    REQUIRE(frame.size() >= 6);
    const auto byte = [&frame](const std::size_t i) {
        return static_cast<std::uint8_t>(frame[i]);
    };
    REQUIRE((byte(1) & 0x80) != 0); // a client masks every frame
    std::uint64_t length = byte(1) & 0x7f;
    std::size_t header = 2;
    if (length == 126) {
        length = (std::uint64_t{byte(2)} << 8) | byte(3);
        header = 4;
    } else if (length == 127) {
        length = 0;
        for (std::size_t i = 2; i < 10; ++i)
            length = (length << 8) | byte(i);
        header = 10;
    }
    REQUIRE(frame.size() == header + 4 + length);
    std::string payload = frame.substr(header + 4);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>(payload[i] ^ frame[header + i % 4]);
    return {byte(0), payload};
}

} // namespace

TEST_CASE("parse_ws_extensions accepts what we offer", "[ws_codec]") {
    SECTION("no extension") {
        const auto params = parse_ws_extensions({});
        REQUIRE(params);
        CHECK_FALSE(params->enabled);
    }
    SECTION("plain permessage-deflate, in any case") {
        const auto params = parse_ws_extensions({"Permessage-Deflate"});
        REQUIRE(params);
        CHECK(params->enabled);
        CHECK_FALSE(params->server_no_context_takeover);
        CHECK_FALSE(params->client_no_context_takeover);
    }
    SECTION("with its parameters") {
        const auto params = parse_ws_extensions(
            {"permessage-deflate; server_no_context_takeover; client_no_context_takeover; server_max_window_bits=10"});
        REQUIRE(params);
        CHECK(params->enabled);
        CHECK(params->server_no_context_takeover);
        CHECK(params->client_no_context_takeover);
    }
    SECTION("a quoted window size") {
        CHECK(parse_ws_extensions({"permessage-deflate; server_max_window_bits=\"15\""}));
    }
}

TEST_CASE("parse_ws_extensions refuses what we did not offer", "[ws_codec]") {
    const std::string value =
        GENERATE(as<std::string>{}, "x-webkit-deflate-frame", "permessage-deflate, permessage-deflate",
                 "permessage-deflate; client_max_window_bits=10", "permessage-deflate; server_max_window_bits=7",
                 "permessage-deflate; server_max_window_bits=16", "permessage-deflate; server_max_window_bits=abc",
                 "permessage-deflate; server_no_context_takeover=1", "permessage-deflate; unknown");
    CAPTURE(value);
    CHECK_FALSE(parse_ws_extensions({value}));
}

TEST_CASE("encode masks a frame and sizes its length field", "[ws_codec]") {
    WsCodec codec(WsDeflateParams{});
    const std::size_t size = GENERATE(0, 5, 125, 126, 65535, 65536);
    CAPTURE(size);
    const std::string payload(size, 'x');
    const std::string frame = codec.encode(WsOpcode::Binary, payload);

    const auto [first, sent_payload] = unmask(frame);
    CHECK(first == 0x82); // FIN, no RSV1, binary
    CHECK(sent_payload == payload);
    const std::size_t length_byte = static_cast<std::uint8_t>(frame[1]) & 0x7f;
    CHECK(length_byte == (size < 126 ? size : size <= 0xffff ? std::size_t{126} : std::size_t{127}));
}

TEST_CASE("encode uses a fresh mask each frame", "[ws_codec]") {
    WsCodec codec(WsDeflateParams{});
    const std::string a = codec.encode(WsOpcode::Text, "same");
    const std::string b = codec.encode(WsOpcode::Text, "same");
    CHECK(a.substr(2, 4) != b.substr(2, 4));
}

TEST_CASE("decode reads RFC 6455's example frames", "[ws_codec]") {
    WsCodec codec(WsDeflateParams{});

    SECTION("an unmasked text message") {
        const auto out = decode(codec, bytes({0x81, 0x05, 'H', 'e', 'l', 'l', 'o'}));
        REQUIRE(out.size() == 1);
        CHECK(out[0].opcode == WsOpcode::Text);
        CHECK(out[0].payload == "Hello");
    }
    SECTION("a masked one") {
        const auto out = decode(codec, bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58}));
        REQUIRE(out.size() == 1);
        CHECK(out[0].payload == "Hello");
    }
    SECTION("a message in two fragments") {
        const auto out = decode(codec, bytes({0x01, 0x03, 'H', 'e', 'l', 0x80, 0x02, 'l', 'o'}));
        REQUIRE(out.size() == 1);
        CHECK(out[0].payload == "Hello");
    }
    SECTION("a ping between fragments") {
        const auto out = decode(codec, bytes({0x01, 0x03, 'H', 'e', 'l', 0x89, 0x01, 'p', 0x80, 0x02, 'l', 'o'}));
        REQUIRE(out.size() == 2);
        CHECK(out[0].opcode == WsOpcode::Ping);
        CHECK(out[0].payload == "p");
        CHECK(out[1].payload == "Hello");
    }
    SECTION("a 256-byte binary message") {
        const std::string payload(256, '\x7f');
        const auto out = decode(codec, bytes({0x82, 0x7e, 0x01, 0x00}) + payload);
        REQUIRE(out.size() == 1);
        CHECK(out[0].opcode == WsOpcode::Binary);
        CHECK(out[0].payload == payload);
    }
    SECTION("a close frame") {
        const auto out = decode(codec, bytes({0x88, 0x04, 0x03, 0xe8, 'o', 'k'}));
        REQUIRE(out.size() == 1);
        CHECK(out[0].opcode == WsOpcode::Close);
        CHECK(out[0].payload == bytes({0x03, 0xe8, 'o', 'k'}));
    }
}

TEST_CASE("decode waits for a frame's every byte", "[ws_codec]") {
    WsCodec codec(WsDeflateParams{});
    const std::string frame = bytes({0x81, 0x05, 'H', 'e', 'l', 'l', 'o', 0x81, 0x02, 'h', 'i'});
    std::vector<WsIncoming> all;
    for (const char c : frame) {
        for (auto& message : decode(codec, std::string(1, c)))
            all.push_back(message);
    }
    REQUIRE(all.size() == 2);
    CHECK(all[0].payload == "Hello");
    CHECK(all[1].payload == "hi");
}

TEST_CASE("decode fails the connection on a protocol violation", "[ws_codec]") {
    WsCodec codec(WsDeflateParams{});
    const std::string frame =
        GENERATE(bytes({0xa1, 0x00}),                  // RSV2
                 bytes({0x83, 0x00}),                  // reserved opcode
                 bytes({0x80, 0x00}),                  // continuation of nothing
                 bytes({0x01, 0x01, 'a', 0x81, 0x00}), // a new message inside one
                 bytes({0x09, 0x00}),                  // fragmented ping
                 bytes({0xc1, 0x01, 'a'}),             // compressed without permessage-deflate
                 bytes({0x82, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00})); // past kMaxMessage
    decode_error(codec, frame);
}

TEST_CASE("decode inflates RFC 7692's compressed examples", "[ws_codec]") {
    WsCodec codec(deflate_params());

    SECTION("one compressed message, then one sharing its window") {
        auto out = decode(codec, bytes({0xc1, 0x07, 0xf2, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00}));
        REQUIRE(out.size() == 1);
        CHECK(out[0].payload == "Hello");
        out = decode(codec, bytes({0xc1, 0x05, 0xf2, 0x00, 0x11, 0x00, 0x00}));
        REQUIRE(out.size() == 1);
        CHECK(out[0].payload == "Hello");
    }
    SECTION("a compressed message in two fragments") {
        const auto out = decode(codec, bytes({0x41, 0x03, 0xf2, 0x48, 0xcd, 0x80, 0x04, 0xc9, 0xc9, 0x07, 0x00}));
        REQUIRE(out.size() == 1);
        CHECK(out[0].payload == "Hello");
    }
    SECTION("an uncompressed message alongside") {
        const auto out = decode(codec, bytes({0x81, 0x05, 'H', 'e', 'l', 'l', 'o'}));
        REQUIRE(out.size() == 1);
        CHECK(out[0].payload == "Hello");
    }
    SECTION("data that does not inflate") {
        decode_error(codec, bytes({0xc1, 0x02, 0xff, 0xff}));
    }
}

TEST_CASE("compressed frames round-trip through two codecs", "[ws_codec]") {
    const bool no_context = GENERATE(false, true);
    CAPTURE(no_context);
    WsDeflateParams params = deflate_params();
    params.client_no_context_takeover = no_context;
    params.server_no_context_takeover = no_context;
    WsCodec sender(params);
    WsCodec receiver(params);

    for (const std::string& message :
         std::vector<std::string>{"Hello", "Hello", "", std::string(100000, 'z'), "Hello again"}) {
        const std::string frame = sender.encode(WsOpcode::Text, message);
        const auto [first, sent_payload] = unmask(frame);
        CHECK(first == 0xc1); // FIN, RSV1, text
        const auto out = decode(receiver, frame);
        REQUIRE(out.size() == 1);
        CHECK(out[0].payload == message);
    }
}

TEST_CASE("control frames are never compressed", "[ws_codec]") {
    WsCodec codec(deflate_params());
    const auto [first, sent_payload] = unmask(codec.encode(WsOpcode::Ping, "ping"));
    CHECK(first == 0x89);
    CHECK(sent_payload == "ping");
}
