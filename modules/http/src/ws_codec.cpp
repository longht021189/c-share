#include "ws_codec.hpp"

#include <zlib.h>

// What this file uses, even where CLion finds it through other headers.
// ReSharper disable CppUnusedIncludeDirective
#include <algorithm>
#include <cctype>
#include <stdexcept>
// ReSharper restore CppUnusedIncludeDirective

namespace c_share::http::detail {

namespace {

constexpr std::uint8_t kFin = 0x80;
constexpr std::uint8_t kRsv1 = 0x40;
constexpr std::uint8_t kRsvOther = 0x30;
constexpr std::uint8_t kMasked = 0x80;

// The tail a sync flush ends compressed data with, which permessage-deflate
// leaves off the wire (RFC 7692 7.2.1).
constexpr char kDeflateTail[] = {'\x00', '\x00', '\xff', '\xff'};

// A received message may not inflate to more than this: a few compressed
// bytes can otherwise expand without bound.
constexpr std::size_t kMaxMessage = std::size_t{64} * 1024 * 1024;

// The result views what `text` views, never `text` itself.
// ReSharper disable once CppDFALocalValueEscapesFunction
std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

bool iequals(const std::string_view a, const std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::vector<std::string_view> split(std::string_view text, const char separator) {
    std::vector<std::string_view> parts;
    while (true) {
        const auto at = text.find(separator);
        parts.push_back(trim(text.substr(0, at)));
        if (at == std::string_view::npos) return parts;
        text.remove_prefix(at + 1);
    }
}

bool is_control(WsOpcode opcode) {
    return static_cast<std::uint8_t>(opcode) >= 0x8;
}

} // namespace

std::optional<WsDeflateParams> parse_ws_extensions(const std::vector<std::string>& values) {
    WsDeflateParams params;
    for (const auto& value : values) {
        for (const auto extension : split(value, ',')) {
            if (extension.empty()) continue;
            const auto parts = split(extension, ';');
            // Only what we offered, and only once.
            if (!iequals(parts[0], "permessage-deflate") || params.enabled) return std::nullopt;
            params.enabled = true;
            for (std::size_t i = 1; i < parts.size(); ++i) {
                const auto eq = parts[i].find('=');
                const auto name = trim(parts[i].substr(0, eq));
                auto argument = eq == std::string_view::npos ? std::string_view{} : trim(parts[i].substr(eq + 1));
                if (argument.size() >= 2 && argument.front() == '"' && argument.back() == '"') {
                    argument = argument.substr(1, argument.size() - 2);
                }
                if (iequals(name, "server_no_context_takeover") && argument.empty()) {
                    params.server_no_context_takeover = true;
                } else if (iequals(name, "client_no_context_takeover") && argument.empty()) {
                    params.client_no_context_takeover = true;
                } else if (iequals(name, "server_max_window_bits")) {
                    // Any window the server compresses with inflates with 15.
                    int bits = 0;
                    for (const char c : argument) {
                        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
                        bits = bits * 10 + (c - '0');
                        if (bits > 15) return std::nullopt;
                    }
                    if (bits < 8) return std::nullopt;
                } else {
                    // Including client_max_window_bits, which we did not offer.
                    return std::nullopt;
                }
            }
        }
    }
    return params;
}

struct WsCodec::Zlib {
    z_stream deflater{};
    z_stream inflater{};
    bool deflater_ready = false;
    bool inflater_ready = false;

    Zlib(const Zlib&) = delete;
    Zlib& operator=(const Zlib&) = delete;
    Zlib(Zlib&&) = delete;
    Zlib& operator=(Zlib&&) = delete;

    Zlib() {
        // Raw deflate (negative window bits): permessage-deflate has no
        // zlib/gzip header.
        if (deflateInit2(&deflater, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            throw std::runtime_error("deflateInit2 failed");
        }
        deflater_ready = true;
        if (inflateInit2(&inflater, -15) != Z_OK) throw std::runtime_error("inflateInit2 failed");
        inflater_ready = true;
    }

    ~Zlib() {
        if (deflater_ready) deflateEnd(&deflater);
        if (inflater_ready) inflateEnd(&inflater);
    }

    std::string compress(std::string_view input) {
        std::string out;
        deflater.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
        deflater.avail_in = static_cast<uInt>(input.size());
        char chunk[16 * 1024];
        do {
            deflater.next_out = reinterpret_cast<Bytef*>(chunk);
            deflater.avail_out = sizeof chunk;
            deflate(&deflater, Z_SYNC_FLUSH);
            out.append(chunk, sizeof chunk - deflater.avail_out);
        } while (deflater.avail_out == 0);
        if (out.size() >= 4 && out.compare(out.size() - 4, 4, kDeflateTail, 4) == 0) out.resize(out.size() - 4);
        // An empty message compresses to a single empty block.
        if (out.empty()) out.push_back('\x00');
        return out;
    }

    // False if `input` is not valid deflate data, or inflates past
    // kMaxMessage.
    bool decompress(std::string input, std::string& out) {
        input.append(kDeflateTail, 4);
        inflater.next_in = reinterpret_cast<Bytef*>(input.data());
        inflater.avail_in = static_cast<uInt>(input.size());
        char chunk[16 * 1024];
        while (true) {
            inflater.next_out = reinterpret_cast<Bytef*>(chunk);
            inflater.avail_out = sizeof chunk;
            const int rc = inflate(&inflater, Z_SYNC_FLUSH);
            if (rc != Z_OK && rc != Z_BUF_ERROR && rc != Z_STREAM_END) return false;
            out.append(chunk, sizeof chunk - inflater.avail_out);
            if (out.size() > kMaxMessage) return false;
            if (rc == Z_STREAM_END) {
                // The server ended the deflate stream (a final block): the
                // next message starts a new one.
                inflateReset(&inflater);
                return true;
            }
            if (inflater.avail_in == 0 && inflater.avail_out != 0) return true;
            if (rc == Z_BUF_ERROR && inflater.avail_in != 0 && inflater.avail_out != 0) return false;
        }
    }
};

WsCodec::WsCodec(const WsDeflateParams params)
    : params_(params), zlib_(params.enabled ? std::make_unique<Zlib>() : nullptr), mask_rng_(std::random_device{}()) {}

WsCodec::~WsCodec() = default;

std::string WsCodec::encode(WsOpcode opcode, std::string_view payload) {
    std::uint8_t first = kFin | static_cast<std::uint8_t>(opcode);
    std::string compressed;
    if (zlib_ && !is_control(opcode)) {
        compressed = zlib_->compress(payload);
        if (params_.client_no_context_takeover) deflateReset(&zlib_->deflater);
        payload = compressed;
        first |= kRsv1;
    }

    std::string frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<char>(first));
    if (const std::uint64_t size = payload.size(); size < 126) {
        frame.push_back(static_cast<char>(kMasked | size));
    } else if (size <= 0xffff) {
        frame.push_back(static_cast<char>(kMasked | 126));
        frame.push_back(static_cast<char>(size >> 8));
        frame.push_back(static_cast<char>(size & 0xff));
    } else {
        frame.push_back(static_cast<char>(kMasked | 127));
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<char>((size >> shift) & 0xff));
    }
    // A client masks every frame it sends (RFC 6455 5.3).
    const std::uint32_t key = mask_rng_();
    const char mask[4] = {static_cast<char>(key >> 24), static_cast<char>(key >> 16), static_cast<char>(key >> 8),
                          static_cast<char>(key)};
    frame.append(mask, 4);
    for (std::size_t i = 0; i < payload.size(); ++i)
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    return frame;
}

bool WsCodec::decode(const char* data, const std::size_t size, std::vector<WsIncoming>& out, std::string& error) {
    pending_.append(data, size);
    std::size_t at = 0;
    while (true) {
        const std::size_t available = pending_.size() - at;
        if (available < 2) break;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(pending_.data() + at);
        const bool fin = bytes[0] & kFin;
        const bool rsv1 = bytes[0] & kRsv1;
        const auto opcode = static_cast<WsOpcode>(bytes[0] & 0x0f);
        const bool masked = bytes[1] & kMasked;
        std::uint64_t length = bytes[1] & 0x7f;
        std::size_t header = 2;
        if (length == 126) {
            if (available < 4) break;
            length = (std::uint64_t{bytes[2]} << 8) | bytes[3];
            header = 4;
        } else if (length == 127) {
            if (available < 10) break;
            length = 0;
            for (int i = 0; i < 8; ++i)
                length = (length << 8) | bytes[2 + i];
            header = 10;
        }
        if (masked) header += 4;
        if (length > kMaxMessage) {
            error = "a frame is too large";
            return false;
        }
        if (available < header + length) break;

        if (bytes[0] & kRsvOther) {
            error = "a frame uses a reserved bit";
            return false;
        }
        // Not redundant where std::size_t is 32 bits; kMaxMessage bounds `length`.
        // ReSharper disable once CppRedundantCastExpression
        std::string payload = pending_.substr(at + header, static_cast<std::size_t>(length));
        if (masked) {
            // Servers must not mask; undo it anyway rather than fail.
            const auto* mask = bytes + header - 4;
            for (std::size_t i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }
        at += header + static_cast<std::size_t>(length);

        switch (opcode) {
            case WsOpcode::Close:
            case WsOpcode::Ping:
            case WsOpcode::Pong:
                if (!fin || rsv1 || length > 125) {
                    error = "an invalid control frame";
                    return false;
                }
                out.push_back(WsIncoming{opcode, std::move(payload)});
                break;
            case WsOpcode::Text:
            case WsOpcode::Binary:
                if (in_message_) {
                    error = "a new message started before the last one ended";
                    return false;
                }
                if (rsv1 && !zlib_) {
                    error = "a compressed message without permessage-deflate";
                    return false;
                }
                in_message_ = true;
                message_opcode_ = opcode;
                message_compressed_ = rsv1;
                message_ = std::move(payload);
                break;
            case WsOpcode::Continuation:
                if (!in_message_ || rsv1) {
                    error = "an unexpected continuation frame";
                    return false;
                }
                if (message_.size() + payload.size() > kMaxMessage) {
                    error = "a message is too large";
                    return false;
                }
                message_ += payload;
                break;
            default: error = "a frame with a reserved opcode"; return false;
        }

        if (fin && !is_control(opcode)) {
            in_message_ = false;
            std::string message = std::move(message_);
            message_.clear();
            if (message_compressed_) {
                std::string inflated;
                if (!zlib_->decompress(std::move(message), inflated)) {
                    error = "a message that does not decompress";
                    return false;
                }
                if (params_.server_no_context_takeover) inflateReset(&zlib_->inflater);
                message = std::move(inflated);
            }
            out.push_back(WsIncoming{message_opcode_, std::move(message)});
        }
    }
    pending_.erase(0, at);
    return true;
}

} // namespace c_share::http::detail
