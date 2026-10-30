#include "test_server.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include <zlib.h>

// What this file uses, even where CLion finds it through other headers.
// ReSharper disable CppUnusedIncludeDirective
#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
// ReSharper restore CppUnusedIncludeDirective

namespace c_share::test {

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// SHA-1 (RFC 3174), only for Sec-WebSocket-Accept.
std::array<std::uint8_t, 20> sha1(const std::string& input) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string data = input;
    const std::uint64_t bits = std::uint64_t{input.size()} * 8;
    data.push_back(static_cast<char>(0x80));
    while (data.size() % 64 != 56)
        data.push_back('\0');
    for (int shift = 56; shift >= 0; shift -= 8)
        data.push_back(static_cast<char>((bits >> shift) & 0xff));
    const auto rotl = [](const std::uint32_t v, const int n) {
        return (v << n) | (v >> (32 - n));
    };
    for (std::size_t chunk = 0; chunk < data.size(); chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* p =
                reinterpret_cast<const unsigned char*>(data.data() + chunk + static_cast<std::size_t>(i) * 4);
            w[i] = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0];
        std::uint32_t b = h[1];
        std::uint32_t c = h[2];
        std::uint32_t d = h[3];
        std::uint32_t e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<std::uint8_t, 20> digest{};
    for (std::size_t i = 0; i < 20; ++i)
        digest[i] = static_cast<std::uint8_t>(h[i / 4] >> (24 - 8 * (i % 4)));
    return digest;
}

// General on purpose, though the handshake's SHA-1 digest is its only input.
// ReSharper disable once CppDFAConstantParameter
std::string base64(const std::uint8_t* data, const std::size_t size) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t chunk = (std::uint32_t{data[i]} << 16) |
                                    (i + 1 < size ? std::uint32_t{data[i + 1]} << 8 : 0) |
                                    (i + 2 < size ? std::uint32_t{data[i + 2]} : 0);
        out.push_back(kAlphabet[(chunk >> 18) & 63]);
        out.push_back(kAlphabet[(chunk >> 12) & 63]);
        out.push_back(i + 1 < size ? kAlphabet[(chunk >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? kAlphabet[chunk & 63] : '=');
    }
    return out;
}

void shutdown_socket(const socket_handle s) {
#ifdef _WIN32
    shutdown(s, SD_BOTH);
#else
    shutdown(s, SHUT_RDWR);
#endif
}

} // namespace

std::optional<std::string> HttpRequest::header(const std::string& name) const {
    const std::string wanted = lower(name);
    for (const auto& [header_name, value] : headers) {
        if (lower(header_name) == wanted) return value;
    }
    return std::nullopt;
}

bool Connection::fill(const std::size_t size) {
    while (buffered_.size() < size) {
        const std::string more = recv_some(s_);
        if (more.empty()) return false;
        buffered_ += more;
    }
    return true;
}

std::optional<HttpRequest> Connection::read_request() {
    std::size_t end = 0;
    while ((end = buffered_.find("\r\n\r\n")) == std::string::npos) {
        const std::string more = recv_some(s_);
        if (more.empty()) return std::nullopt;
        buffered_ += more;
    }
    const std::string head = buffered_.substr(0, end);
    buffered_.erase(0, end + 4);

    HttpRequest request;
    std::size_t line_end = head.find("\r\n");
    const std::string request_line = head.substr(0, line_end);
    const auto first_space = request_line.find(' ');
    const auto second_space = request_line.find(' ', first_space + 1);
    request.method = request_line.substr(0, first_space);
    request.target = request_line.substr(first_space + 1, second_space - first_space - 1);
    while (line_end != std::string::npos) {
        const std::size_t start = line_end + 2;
        line_end = head.find("\r\n", start);
        const std::string line =
            head.substr(start, line_end == std::string::npos ? std::string::npos : line_end - start);
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(' '));
        request.headers.emplace_back(line.substr(0, colon), value);
    }
    if (const auto length = request.header("content-length")) {
        const std::size_t size = std::stoul(*length);
        if (!fill(size)) return std::nullopt;
        request.body = buffered_.substr(0, size);
        buffered_.erase(0, size);
    }
    return request;
}

bool Connection::write(const std::string& data) const {
    return send_all(s_, data);
}

bool Connection::accept_websocket(const HttpRequest& request, const std::string& extensions) const {
    const auto digest = sha1(request.header("sec-websocket-key").value_or("") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " +
                           base64(digest.data(), digest.size()) + "\r\n";
    if (!extensions.empty()) response += "Sec-WebSocket-Extensions: " + extensions + "\r\n";
    response += "\r\n";
    return write(response);
}

std::optional<WsFrame> Connection::read_frame() {
    if (!fill(2)) return std::nullopt;
    const auto byte = [this](const std::size_t i) {
        return static_cast<std::uint8_t>(buffered_[i]);
    };
    WsFrame frame;
    frame.fin = (byte(0) & 0x80) != 0;
    frame.rsv1 = (byte(0) & 0x40) != 0;
    frame.opcode = byte(0) & 0x0f;
    const bool masked = (byte(1) & 0x80) != 0;
    std::uint64_t length = byte(1) & 0x7f;
    std::size_t header = 2;
    if (length == 126) {
        if (!fill(4)) return std::nullopt;
        length = (std::uint64_t{byte(2)} << 8) | byte(3);
        header = 4;
    } else if (length == 127) {
        if (!fill(10)) return std::nullopt;
        length = 0;
        for (std::size_t i = 2; i < 10; ++i)
            length = (length << 8) | byte(i);
        header = 10;
    }
    const std::size_t mask_at = header;
    if (masked) header += 4;
    if (!fill(header + length)) return std::nullopt;
    // Narrows on 32-bit targets.
    // ReSharper disable once CppRedundantCastExpression
    frame.payload = buffered_.substr(header, static_cast<std::size_t>(length));
    if (masked) {
        for (std::size_t i = 0; i < frame.payload.size(); ++i)
            frame.payload[i] = static_cast<char>(frame.payload[i] ^ buffered_[mask_at + i % 4]);
    }
    buffered_.erase(0, header + static_cast<std::size_t>(length));
    return frame;
}

bool Connection::write_frame(const WsFrame& frame) const {
    std::string bytes;
    bytes.push_back(static_cast<char>((frame.fin ? 0x80 : 0) | (frame.rsv1 ? 0x40 : 0) | frame.opcode));
    if (const std::uint64_t size = frame.payload.size(); size < 126) {
        bytes.push_back(static_cast<char>(size));
    } else if (size <= 0xffff) {
        bytes.push_back(126);
        bytes.push_back(static_cast<char>(size >> 8));
        bytes.push_back(static_cast<char>(size & 0xff));
    } else {
        bytes.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<char>((size >> shift) & 0xff));
    }
    bytes += frame.payload;
    return write(bytes);
}

TestServer::TestServer(Handler handler) : handler_(std::move(handler)) {
    std::tie(listener_, port_) = listen_loopback();
    acceptor_ = std::thread([this] { accept_loop(); });
}

TestServer TestServer::http(std::function<std::string(const HttpRequest&)> respond) {
    return TestServer([respond = std::move(respond)](Connection& connection) {
        while (const auto request = connection.read_request()) {
            if (!connection.write(respond(*request))) return;
        }
    });
}

TestServer::~TestServer() {
    stopping_ = true;
    // Wakes accept() with a connection of our own, then every handler
    // blocked reading from a client.
    try {
        close_socket(connect_loopback(port_));
    } catch (const std::exception&) {
        shutdown_socket(listener_);
    }
    if (acceptor_.joinable()) acceptor_.join();
    close_socket(listener_);
    std::vector<std::thread> workers;
    {
        std::scoped_lock lock(mutex_);
        for (const socket_handle s : connections_)
            shutdown_socket(s);
        workers.swap(workers_);
    }
    for (auto& worker : workers)
        worker.join();
}

std::string TestServer::url(const std::string& path, const std::string& scheme) const {
    return scheme + "://127.0.0.1:" + std::to_string(port_) + path;
}

void TestServer::accept_loop() {
    while (true) {
        const socket_handle s = accept(listener_, nullptr, nullptr);
        if (stopping_ || s == kInvalidSocket) {
            if (s != kInvalidSocket) close_socket(s);
            return;
        }
        std::scoped_lock lock(mutex_);
        connections_.push_back(s);
        workers_.emplace_back([this, s] {
            Connection connection(s);
            handler_(connection);
            shutdown_socket(s);
        });
    }
}

std::string http_response(const int status, const std::string& body,
                          const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string response = "HTTP/1.1 " + std::to_string(status) + " Status\r\n";
    bool has_length = false;
    for (const auto& [name, value] : headers) {
        response.append(name).append(": ").append(value).append("\r\n");
        has_length = has_length || lower(name) == "content-length";
    }
    if (!has_length) response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "\r\n";
    response += body;
    return response;
}

// Deflate's methods all change it through streams_, where CLion does not
// look, so it would make some of them const.
// ReSharper disable CppMemberFunctionMayBeConst

struct Deflate::Streams {
    z_stream deflater{};
    z_stream inflater{};
};

Deflate::Deflate() : streams_(new Streams) {
    if (deflateInit2(&streams_->deflater, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK ||
        inflateInit2(&streams_->inflater, -15) != Z_OK) {
        throw std::runtime_error("zlib init failed");
    }
}

Deflate::~Deflate() {
    deflateEnd(&streams_->deflater);
    inflateEnd(&streams_->inflater);
    delete streams_;
}

std::string Deflate::compress(const std::string& message) {
    std::string out(message.size() + 64, '\0');
    auto& z = streams_->deflater;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(message.data()));
    z.avail_in = static_cast<uInt>(message.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_SYNC_FLUSH);
    out.resize(out.size() - z.avail_out);
    out.resize(out.size() - 4); // the 00 00 ff ff tail
    return out;
}

std::string Deflate::decompress(const std::string& message) {
    std::string input = message + std::string("\x00\x00\xff\xff", 4);
    std::string out;
    auto& z = streams_->inflater;
    z.next_in = reinterpret_cast<Bytef*>(input.data());
    z.avail_in = static_cast<uInt>(input.size());
    char chunk[4096];
    do {
        z.next_out = reinterpret_cast<Bytef*>(chunk);
        z.avail_out = sizeof chunk;
        if (inflate(&z, Z_SYNC_FLUSH) < Z_OK) throw std::runtime_error("inflate failed");
        out.append(chunk, sizeof chunk - z.avail_out);
    } while (z.avail_in > 0 || z.avail_out == 0);
    return out;
}

void Deflate::reset_compressor() {
    deflateReset(&streams_->deflater);
}

// ReSharper restore CppMemberFunctionMayBeConst

std::string gzip(const std::string& data) {
    z_stream z{};
    // 15 + 16: a gzip header and trailer around the deflate data.
    deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    // uLong is 32-bit on Windows.
    // ReSharper disable once CppRedundantCastExpression
    std::string out(deflateBound(&z, static_cast<uLong>(data.size())) + 32, '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_FINISH);
    out.resize(out.size() - z.avail_out);
    deflateEnd(&z);
    return out;
}

} // namespace c_share::test
