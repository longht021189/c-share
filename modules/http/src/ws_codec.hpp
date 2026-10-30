#pragma once

// WebSocket framing (RFC 6455) with the permessage-deflate extension
// (RFC 7692), for Client's WebSockets that negotiate compression. libcurl
// does the handshake and TLS for those, but cannot take compressed frames
// (its frame decoder rejects the RSV1 bit they carry, even in
// CURLWS_RAW_MODE), so their bytes are framed here instead and moved with
// curl_easy_send/curl_easy_recv. Internal to the http module.

// What this header uses, even where CLion finds it through other headers.
// ReSharper disable CppUnusedIncludeDirective
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>
// ReSharper restore CppUnusedIncludeDirective

namespace c_share::http::detail {

// What the server accepted of our permessage-deflate offer.
struct WsDeflateParams {
    bool enabled = false;
    // The server resets its compressor after each message, so ours
    // decompressing must too.
    bool server_no_context_takeover = false;
    // We must reset our compressor after each message.
    bool client_no_context_takeover = false;
};

// The Sec-WebSocket-Extensions value we offer. No client_max_window_bits:
// zlib cannot compress with the 8-bit window a server could then ask for.
inline constexpr auto kWsDeflateOffer = "permessage-deflate";

// Parses the server's Sec-WebSocket-Extensions values (none: no extension).
// Returns nullopt if they name anything we did not offer, or invalid
// parameters -- RFC 6455 then requires failing the connection.
std::optional<WsDeflateParams> parse_ws_extensions(const std::vector<std::string>& values);

enum class WsOpcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xa,
};

// One complete message or control frame received.
struct WsIncoming {
    WsOpcode opcode; // Text, Binary, Close, Ping or Pong
    std::string payload;
};

class WsCodec {
public:
    explicit WsCodec(WsDeflateParams params);
    ~WsCodec();

    WsCodec(const WsCodec&) = delete;
    WsCodec& operator=(const WsCodec&) = delete;

    // One masked frame carrying the whole of `payload`, compressed when
    // deflate was negotiated and `opcode` is Text or Binary.
    std::string encode(WsOpcode opcode, std::string_view payload);

    // Takes the next `size` bytes from the server and appends every message
    // or control frame they complete to `out`. Returns false, with `error`
    // set, on a protocol violation: the connection must then be failed.
    bool decode(const char* data, std::size_t size, std::vector<WsIncoming>& out, std::string& error);

private:
    struct Zlib;

    WsDeflateParams params_;
    std::unique_ptr<Zlib> zlib_;
    std::mt19937 mask_rng_;

    // Received bytes not yet making up a whole frame.
    std::string pending_;
    // The data message being received, across frames.
    bool in_message_ = false;
    WsOpcode message_opcode_ = WsOpcode::Text;
    bool message_compressed_ = false;
    std::string message_;
};

} // namespace c_share::http::detail
