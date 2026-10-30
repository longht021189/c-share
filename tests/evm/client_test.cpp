#include "c_share/evm/client.hpp"

#include "fake_node.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <curl/curl.h>

#include <atomic>
#include <string>

using c_share::evm::Client;
using c_share::evm::Config;
using c_share::evm::ErrorCode;
using c_share::evm::PrivateKey;
using c_share::evm::ReadOptions;
using c_share::evm::TxOptions;
using c_share::evm::WaitOptions;
using c_share::test::FakeNode;
using c_share::test::Outcome;
using c_share::test::RpcRequest;
using c_share::test::RunningHttpClient;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

// libcurl's process-wide setup, which http::Client leaves to its caller.
struct CurlGlobal {
    CurlGlobal() noexcept { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
    CurlGlobal(CurlGlobal&&) = delete;
    CurlGlobal& operator=(CurlGlobal&&) = delete;
};
// Used for its constructor and destructor alone.
// ReSharper disable once CppDeclaratorNeverUsed
const CurlGlobal curl_global;

constexpr auto kToken = "0x3535353535353535353535353535353535353535";
// EIP-155's example key, and its address.
constexpr auto kSender = "0x9d8A62f656a8d1615C1294fd71e9CFb3E4855A4F";

PrivateKey example_key() {
    PrivateKey key{};
    key.fill(0x46);
    return key;
}

Config config_for(const FakeNode& node) {
    Config config;
    config.url = node.url();
    config.headers.push_back({"X-Api-Key", "secret"});
    return config;
}

json result(json value) {
    return {{"result", std::move(value)}};
}

// What eth-account 0.13.7 signs for transfer(0x35...35, 1000) on chain 1
// with nonce 9, a 1 gwei tip, a 21 gwei maximum fee and 60000 gas (see
// transaction_test.cpp).
constexpr auto kSignedTransfer =
    "0x02f8b00109843b9aca008504e3b2920082ea6094353535353535353535353535353535353535353580b844a9059cbb0000000000000000"
    "00000000353535353535353535353535353535353535353500000000000000000000000000000000000000000000000000000000000003e8"
    "c080a07b8d08ceceb78b5865f18a1fc8c06663a62563349fffb904d998d0a958d7f06da06818a580272d0e5463d3548a5d950adfa4020670"
    "d4b0428de41360bfabb543a7";
constexpr auto kTransferHash = "0x50b6a9d6ee077b0ad95a0b9de331a4de513a3438298f57e480994b27f15db634";

// A node with everything a transaction asks for: chain 1, nonce 9, a 1 gwei
// tip, a 10 gwei base fee and 60000 gas.
json transaction_answers(const RpcRequest& request) {
    if (request.method == "eth_chainId") return result("0x1");
    if (request.method == "eth_getTransactionCount") return result("0x9");
    if (request.method == "eth_maxPriorityFeePerGas") return result("0x3b9aca00");
    if (request.method == "eth_getBlockByNumber") return result({{"number", "0x10"}, {"baseFeePerGas", "0x2540be400"}});
    if (request.method == "eth_estimateGas") return result("0xea60");
    if (request.method == "eth_sendRawTransaction") return result(kTransferHash);
    return {{"error", {{"code", -32601}, {"message", "the method does not exist"}}}};
}

constexpr auto kTransfer = "function transfer(address to, uint256 amount) returns (bool)";

} // namespace

TEST_CASE("quantities come back in decimal", "[evm][client]") {
    FakeNode node([](const RpcRequest& request) -> json {
        if (request.method == "eth_chainId") return result("0xaa36a7");
        if (request.method == "eth_blockNumber") return result("0x10");
        if (request.method == "eth_gasPrice") return result("0x3b9aca00");
        if (request.method == "eth_getBalance") return result("0xde0b6b3a7640000");
        if (request.method == "eth_getTransactionCount") return result("0x0");
        return {};
    });
    RunningHttpClient http;
    Client client(http.get(), config_for(node));
    const auto run = [&](auto operation) {
        Outcome outcome;
        operation(&outcome);
        REQUIRE(outcome.wait());
        CHECK_FALSE(outcome.error);
        return outcome.result;
    };
    CHECK(run([&](Outcome* o) { client.get_chain_id(o, &Outcome::on_result); }) == "11155111");
    CHECK(run([&](Outcome* o) { client.get_block_number(o, &Outcome::on_result); }) == "16");
    CHECK(run([&](Outcome* o) { client.get_gas_price(o, &Outcome::on_result); }) == "1000000000");
    // Lowercase in, checksummed out; a block number in decimal goes as hex.
    CHECK(run([&](Outcome* o) {
              client.get_balance("0x9d8a62f656a8d1615c1294fd71e9cfb3e4855a4f", "", o, &Outcome::on_result);
          }) == "1000000000000000000");
    CHECK(run([&](Outcome* o) { client.get_transaction_count(kSender, "1000", o, &Outcome::on_result); }) == "0");
    const auto requests = node.requests();
    REQUIRE(requests.size() == 5);
    CHECK(requests[3].params == json::array({kSender, "latest"}));
    CHECK(requests[4].params == json::array({kSender, "0x3e8"}));
}

TEST_CASE("request() passes any method through", "[evm][client]") {
    FakeNode node(
        [](const RpcRequest& request) { return result({{"method", request.method}, {"params", request.params}}); });
    RunningHttpClient http;
    Client client(http.get(), config_for(node));
    Outcome outcome;
    client.request("eth_getLogs", R"([{"address": "0x01"}])", &outcome, &Outcome::on_result);
    REQUIRE(outcome.wait());
    CHECK_FALSE(outcome.error);
    CHECK(json::parse(outcome.result) ==
          json{{"method", "eth_getLogs"}, {"params", json::array({{{"address", "0x01"}}})}});
}

TEST_CASE("read_contract encodes the call and decodes what it returns", "[evm][client]") {
    FakeNode node([](const RpcRequest& request) -> json {
        const std::string data = request.params[0]["data"];
        if (data.rfind("0x70a08231", 0) == 0) return result("0x" + std::string(61, '0') + "3e8"); // balanceOf: 1000
        if (data == "0x06fdde03")                                                                 // name()
            return result("0x0000000000000000000000000000000000000000000000000000000000000020"
                          "0000000000000000000000000000000000000000000000000000000000000004"
                          "5465737400000000000000000000000000000000000000000000000000000000");
        // Error("Not enough Ether provided.")
        return {{"error",
                 {{"code", 3},
                  {"message", "execution reverted"},
                  {"data", "0x08c379a0"
                           "0000000000000000000000000000000000000000000000000000000000000020"
                           "000000000000000000000000000000000000000000000000000000000000001a"
                           "4e6f7420656e6f7567682045746865722070726f76696465642e000000000000"}}}};
    });
    RunningHttpClient http;
    Client client(http.get(), config_for(node));

    SECTION("decoded") {
        Outcome outcome;
        ReadOptions options;
        options.block = "finalized";
        options.from = kSender;
        client.read_contract(kToken, "function balanceOf(address owner) view returns (uint256)", json({kSender}).dump(),
                             options, &outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK_FALSE(outcome.error);
        CHECK(outcome.result == R"(["1000"])");
        const auto [method, params] = node.requests().at(0);
        CHECK(method == "eth_call");
        CHECK(params[0] ==
              json{{"to", kToken},
                   {"from", kSender},
                   {"data", "0x70a082310000000000000000000000009d8a62f656a8d1615c1294fd71e9cfb3e4855a4f"}});
        CHECK(params[1] == "finalized");
    }
    SECTION("a string") {
        Outcome outcome;
        client.read_contract(kToken, "name() returns (string)", "", {}, &outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.result == R"(["Test"])");
    }
    SECTION("raw, without returns (...)") {
        Outcome outcome;
        client.read_contract(kToken, "balanceOf(address)", json({kSender}).dump(), {}, &outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.result == "\"0x" + std::string(61, '0') + "3e8\"");
    }
    SECTION("a revert, with its reason") {
        Outcome outcome;
        client.read_contract(kToken, "withdraw(uint256) returns (bool)", "[1]", {}, &outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.error.code == ErrorCode::Reverted);
        CHECK(outcome.error.message == "execution reverted: Not enough Ether provided.");
        CHECK(outcome.error.rpc_code == 3);
        CHECK_THAT(outcome.error.rpc_data, Catch::Matchers::StartsWith("\"0x08c379a0"));
    }
}

TEST_CASE("invalid arguments are reported before the call returns", "[evm][client]") {
    FakeNode node([](const RpcRequest&) { return result("0x0"); });
    RunningHttpClient http;
    Client client(http.get(), config_for(node));
    const auto check = [](const auto& operation) {
        Outcome outcome;
        operation(&outcome);
        CHECK(outcome.done); // already
        CHECK(outcome.error.code == ErrorCode::InvalidArgument);
        CHECK_FALSE(outcome.error.message.empty());
    };
    check([&](Outcome* o) { client.get_balance("0x1234", "", o, &Outcome::on_result); });
    check([&](Outcome* o) { client.get_balance(kSender, "yesterday", o, &Outcome::on_result); });
    check([&](Outcome* o) { client.read_contract(kToken, "balanceOf(address", "[]", {}, o, &Outcome::on_result); });
    check([&](Outcome* o) { client.read_contract(kToken, "balanceOf(address)", "[1]", {}, o, &Outcome::on_result); });
    check([&](Outcome* o) { client.read_contract(kToken, "balanceOf(address)", "{}", {}, o, &Outcome::on_result); });
    check([&](Outcome* o) { client.request("eth_call", "not json", o, &Outcome::on_result); });
    check([&](Outcome* o) { client.get_transaction_receipt("0x12", o, &Outcome::on_result); });
    TxOptions bad_value;
    bad_value.value = "1 ether";
    check([&](Outcome* o) { client.send_transaction(example_key(), kToken, "", bad_value, o, &Outcome::on_result); });
    check([&](Outcome* o) { client.send_transaction(example_key(), kToken, "0x1", {}, o, &Outcome::on_result); });
    check([&](Outcome* o) { client.send_transaction(PrivateKey{}, kToken, "", {}, o, &Outcome::on_result); });
    check([&](Outcome* o) { client.write_contract(example_key(), "", kTransfer, "[]", {}, o, &Outcome::on_result); });
    CHECK(node.requests().empty());
}

TEST_CASE("failures say where they happened", "[evm][client]") {
    RunningHttpClient http;
    SECTION("a JSON-RPC error") {
        FakeNode node([](const RpcRequest&) -> json {
            return {{"error", {{"code", -32000}, {"message", "header not found"}, {"data", {{"block", 1}}}}}};
        });
        Client client(http.get(), config_for(node));
        Outcome outcome;
        client.get_block_number(&outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.error.code == ErrorCode::Rpc);
        CHECK(outcome.error.rpc_code == -32000);
        CHECK(outcome.error.message == "header not found");
        CHECK(outcome.error.rpc_data == R"({"block":1})");
    }
    SECTION("an HTTP error") {
        auto server = c_share::test::TestServer::http(
            [](const c_share::test::HttpRequest&) { return c_share::test::http_response(503, "overloaded"); });
        Config config;
        config.url = server.url("/");
        Client client(http.get(), config);
        Outcome outcome;
        client.get_block_number(&outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.error.code == ErrorCode::Http);
        CHECK(outcome.error.message == "HTTP 503: overloaded");
    }
    SECTION("not JSON-RPC") {
        FakeNode node([](const RpcRequest&) { return json{{"answer", 42}}; });
        Client client(http.get(), config_for(node));
        Outcome outcome;
        client.get_block_number(&outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.error.code == ErrorCode::InvalidResponse);
    }
    SECTION("not a quantity") {
        FakeNode node([](const RpcRequest&) { return result("ten"); });
        Client client(http.get(), config_for(node));
        Outcome outcome;
        client.get_block_number(&outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.error.code == ErrorCode::InvalidResponse);
    }
    SECTION("no node") {
        Config config;
        config.url = "http://127.0.0.1:" + std::to_string(c_share::test::unused_port()) + "/";
        Client client(http.get(), config);
        Outcome outcome;
        client.get_block_number(&outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.error.code == ErrorCode::Transport);
    }
}

TEST_CASE("write_contract fills in, signs and sends a transaction", "[evm][client]") {
    FakeNode node(&transaction_answers);
    RunningHttpClient http;
    Client client(http.get(), config_for(node));
    Outcome outcome;
    client.write_contract(example_key(), kToken, kTransfer, json({kToken, 1000}).dump(), {}, &outcome,
                          &Outcome::on_result);
    REQUIRE(outcome.wait());
    REQUIRE_FALSE(outcome.error);
    CHECK(outcome.result == kTransferHash);

    const auto requests = node.requests();
    CHECK(node.methods() == std::vector<std::string>{"eth_chainId", "eth_getTransactionCount",
                                                     "eth_maxPriorityFeePerGas", "eth_getBlockByNumber",
                                                     "eth_estimateGas", "eth_sendRawTransaction"});
    CHECK(requests[1].params == json::array({kSender, "pending"}));
    CHECK(requests[4].params[0]["from"] == kSender);
    CHECK(requests[4].params[0]["to"] == kToken);
    CHECK(requests[5].params == json::array({kSignedTransfer}));

    SECTION("the chain id is asked once") {
        Outcome second;
        client.write_contract(example_key(), kToken, kTransfer, json({kToken, 1000}).dump(), {}, &second,
                              &Outcome::on_result);
        REQUIRE(second.wait());
        CHECK_FALSE(second.error);
        CHECK(node.methods().size() == requests.size() + 5);
    }
}

TEST_CASE("a transaction asks the node only for what its options leave out", "[evm][client]") {
    FakeNode node(&transaction_answers);
    RunningHttpClient http;
    Config config = config_for(node);
    config.chain_id = 1;
    Client client(http.get(), config);
    TxOptions options;
    options.nonce = 9;
    options.gas_limit = 60000;
    options.max_fee_per_gas = "21000000000";
    options.max_priority_fee_per_gas = "0x3b9aca00";
    Outcome outcome;
    client.write_contract(example_key(), kToken, kTransfer, json({kToken, "1000"}).dump(), options, &outcome,
                          &Outcome::on_result);
    REQUIRE(outcome.wait());
    CHECK_FALSE(outcome.error);
    CHECK(node.methods() == std::vector<std::string>{"eth_sendRawTransaction"});
    CHECK(node.requests()[0].params == json::array({kSignedTransfer}));
}

TEST_CASE("fees fall back when the node lacks a method or EIP-1559", "[evm][client]") {
    RunningHttpClient http;
    SECTION("no eth_maxPriorityFeePerGas: a 1 gwei tip") {
        FakeNode node([](const RpcRequest& request) {
            if (request.method == "eth_maxPriorityFeePerGas")
                return json{{"error", {{"code", -32601}, {"message", "method not found"}}}};
            return transaction_answers(request);
        });
        Client client(http.get(), config_for(node));
        Outcome outcome;
        client.write_contract(example_key(), kToken, kTransfer, json({kToken, 1000}).dump(), {}, &outcome,
                              &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK_FALSE(outcome.error);
        // The same transaction: its tip was 1 gwei.
        CHECK(node.requests().back().params == json::array({kSignedTransfer}));
    }
    SECTION("no base fee") {
        FakeNode node([](const RpcRequest& request) {
            if (request.method == "eth_getBlockByNumber") return result({{"number", "0x10"}});
            return transaction_answers(request);
        });
        Client client(http.get(), config_for(node));
        Outcome outcome;
        client.write_contract(example_key(), kToken, kTransfer, json({kToken, 1000}).dump(), {}, &outcome,
                              &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.error.code == ErrorCode::InvalidResponse);
        CHECK_THAT(outcome.error.message, Catch::Matchers::ContainsSubstring("EIP-1559"));
    }
    SECTION("a revert while estimating gas") {
        FakeNode node([](const RpcRequest& request) {
            if (request.method == "eth_estimateGas")
                return json{{"error", {{"code", 3}, {"message", "execution reverted: paused"}}}};
            return transaction_answers(request);
        });
        Client client(http.get(), config_for(node));
        Outcome outcome;
        client.write_contract(example_key(), kToken, kTransfer, json({kToken, 1000}).dump(), {}, &outcome,
                              &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.error.code == ErrorCode::Reverted);
        CHECK(outcome.error.message == "execution reverted: paused");
    }
}

// eth-account 0.13.7 signs the same transfer (see transaction_test.cpp).
TEST_CASE("send_transaction sends a plain transfer", "[evm][client]") {
    FakeNode node([](const RpcRequest&) { return json{{"result", nullptr}}; });
    RunningHttpClient http;
    Config config = config_for(node);
    config.chain_id = 11155111;
    Client client(http.get(), config);
    TxOptions options;
    options.value = "1000000000000000000";
    options.nonce = 0;
    options.gas_limit = 21000;
    options.max_fee_per_gas = "30000000000";
    options.max_priority_fee_per_gas = "2";
    Outcome outcome;
    client.send_transaction(example_key(), kToken, "", options, &outcome, &Outcome::on_result);
    REQUIRE(outcome.wait());
    CHECK_FALSE(outcome.error);
    CHECK(node.requests()[0].params ==
          json::array({"0x02f87283aa36a780028506fc23ac00825208943535353535353535353535353535353535353535880de0b6b3a764"
                       "000080c080a0605a369793d65fd87a1ea9f56f07c5a2ec7be8d225369014d58e120a6a8b9b95a03fb44456d0272d61"
                       "2097fef8b42ad007a881188545b18cfa5b003efce25063d4"}));
    // The node answered null: the hash is the transaction's own.
    CHECK(outcome.result.size() == 66);
}

TEST_CASE("wait_for_receipt polls until the transaction is mined", "[evm][client]") {
    std::atomic polls{0};
    FakeNode node([&polls](const RpcRequest&) {
        if (++polls < 3) return result(nullptr);
        return result({{"status", "0x1"}, {"blockNumber", "0x10"}});
    });
    RunningHttpClient http;
    Client client(http.get(), config_for(node));
    WaitOptions options;
    options.poll_interval = 20ms;
    options.timeout = 5s;

    SECTION("mined") {
        Outcome outcome;
        client.wait_for_receipt(kTransferHash, options, &outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK_FALSE(outcome.error);
        CHECK(json::parse(outcome.result) == json{{"status", "0x1"}, {"blockNumber", "0x10"}});
        CHECK(polls == 3);
    }
    SECTION("not in time") {
        options.timeout = 30ms;
        Outcome outcome;
        client.wait_for_receipt(kTransferHash, options, &outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.error.code == ErrorCode::Timeout);
    }
    SECTION("pending, asked once") {
        Outcome outcome;
        client.get_transaction_receipt(kTransferHash, &outcome, &Outcome::on_result);
        REQUIRE(outcome.wait());
        CHECK(outcome.result == "null");
    }
}

TEST_CASE("destroying a client aborts what is in flight", "[evm][client]") {
    FakeNode node([](const RpcRequest&) {
        std::this_thread::sleep_for(300ms);
        return result(nullptr);
    });
    RunningHttpClient http;
    // Outlive the client: their callbacks may come after it is gone.
    Outcome request;
    Outcome waiting;
    {
        Client client(http.get(), config_for(node));
        client.get_block_number(&request, &Outcome::on_result);
        WaitOptions options;
        options.poll_interval = 10s;
        client.wait_for_receipt(kTransferHash, options, &waiting, &Outcome::on_result);
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(request.wait());
    CHECK(request.error.code == ErrorCode::Aborted);
    REQUIRE(waiting.wait());
    CHECK(waiting.error.code == ErrorCode::Aborted);
    CHECK(request.calls == 1);
}

TEST_CASE("encoding, decoding and addresses work without a node", "[evm][client]") {
    std::string data;
    REQUIRE_FALSE(c_share::evm::encode_function_call(kTransfer, json({kToken, 1000}).dump(), data));
    CHECK(data == "0xa9059cbb000000000000000000000000353535353535353535353535353535353535353500000000000000000000"
                  "000000000000000000000000000000000000000003e8");
    std::string decoded;
    REQUIRE_FALSE(c_share::evm::decode_function_result(kTransfer, "0x" + std::string(63, '0') + "1", decoded));
    CHECK(decoded == "[true]");
    CHECK(c_share::evm::encode_function_call("transfer(address)", "[1]", data).code == ErrorCode::InvalidArgument);
    std::string address;
    REQUIRE_FALSE(c_share::evm::address_from_private_key(example_key(), address));
    CHECK(address == kSender);
    CHECK(c_share::evm::address_from_private_key(PrivateKey{}, address).code == ErrorCode::InvalidArgument);
}
