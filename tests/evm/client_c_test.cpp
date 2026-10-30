#include "c_share/evm/client_c.h"

#include "fake_node.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using c_share::test::FakeNode;
using c_share::test::RpcRequest;
using nlohmann::json;

namespace {

// One C callback's outcome, copied out of its arguments.
struct CResult {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    c_share_evm_error_code_t code = C_SHARE_EVM_ERROR_NONE;
    std::string message;
    std::string result;

    // By value, as the C API's callback type takes it.
    // ReSharper disable once CppPassValueParameterByConstReference
    static void on_result(void* context, const c_share_evm_error_t error, const char* result, const size_t result_len) {
        auto* r = static_cast<CResult*>(context);
        std::scoped_lock lock(r->mutex);
        r->done = true;
        r->code = error.code;
        r->message.assign(error.message, error.message_len);
        r->result.assign(result, result_len);
        r->changed.notify_all();
    }

    bool wait() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10), [&] { return done; });
    }
};

struct HttpHandle {
    HttpHandle() : client(c_share_http_client_create()) { REQUIRE(client); }
    ~HttpHandle() { c_share_http_client_destroy(client); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    HttpHandle(HttpHandle&&) = delete;
    HttpHandle& operator=(HttpHandle&&) = delete;

    c_share_http_client_t* client;
};

constexpr std::string_view kSender = "0x9d8A62f656a8d1615C1294fd71e9CFb3E4855A4F";

} // namespace

TEST_CASE("the C API reads from a node through a given http client", "[evm][client_c]") {
    FakeNode node([](const RpcRequest& request) -> json {
        if (request.method == "eth_getBalance") return {{"result", "0x2a"}};
        return {{"error", {{"code", -32601}, {"message", "no such method"}}}};
    });
    const HttpHandle http;
    const std::string url = node.url();
    constexpr c_share_http_header_t header{"X-Api-Key", 9, "secret", 6};
    const c_share_evm_config_t config{url.data(), url.size(), &header, 1, 0, 0};
    CHECK(c_share_evm_client_create(nullptr, &config) == nullptr);
    CHECK(c_share_evm_client_create(http.client, nullptr) == nullptr);
    c_share_evm_client_t* client = c_share_evm_client_create(http.client, &config);
    REQUIRE(client);

    CResult balance;
    c_share_evm_get_balance(client, kSender.data(), kSender.size(), nullptr, 0, &balance, &CResult::on_result);
    REQUIRE(balance.wait());
    CHECK(balance.code == C_SHARE_EVM_ERROR_NONE);
    CHECK(balance.result == "42");

    CResult missing;
    c_share_evm_request(client, "eth_foo", 7, nullptr, 0, &missing, &CResult::on_result);
    REQUIRE(missing.wait());
    CHECK(missing.code == C_SHARE_EVM_ERROR_RPC);
    CHECK(missing.message == "no such method");

    CResult bad;
    c_share_evm_get_balance(client, "0x12", 4, nullptr, 0, &bad, &CResult::on_result);
    CHECK(bad.done);
    CHECK(bad.code == C_SHARE_EVM_ERROR_INVALID_ARGUMENT);

    CResult no_client;
    c_share_evm_get_block_number(nullptr, &no_client, &CResult::on_result);
    CHECK(no_client.code == C_SHARE_EVM_ERROR_INVALID_ARGUMENT);
    c_share_evm_get_block_number(client, nullptr, nullptr); // no callback: nothing to do

    c_share_evm_client_destroy(client);
    c_share_evm_client_destroy(nullptr);
}

TEST_CASE("the C API encodes, decodes and derives addresses", "[evm][client_c]") {
    const std::string function = "transfer(address,uint256) returns (bool)";
    const std::string args = R"(["0x3535353535353535353535353535353535353535", 1])";
    CResult encoded;
    c_share_evm_encode_function_call(function.data(), function.size(), args.data(), args.size(), &encoded,
                                     &CResult::on_result);
    CHECK(encoded.code == C_SHARE_EVM_ERROR_NONE);
    CHECK(encoded.result.rfind("0xa9059cbb", 0) == 0);

    const std::string data = "0x" + std::string(63, '0') + "1";
    CResult decoded;
    c_share_evm_decode_function_result(function.data(), function.size(), data.data(), data.size(), &decoded,
                                       &CResult::on_result);
    CHECK(decoded.result == "[true]");

    uint8_t key[32];
    std::fill(std::begin(key), std::end(key), uint8_t{0x46});
    CResult address;
    c_share_evm_address_from_private_key(key, &address, &CResult::on_result);
    CHECK(address.result == kSender);
    CResult no_key;
    c_share_evm_address_from_private_key(nullptr, &no_key, &CResult::on_result);
    CHECK(no_key.code == C_SHARE_EVM_ERROR_INVALID_ARGUMENT);
}
