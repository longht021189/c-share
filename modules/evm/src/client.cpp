#include "c_share/evm/client.hpp"

#include "abi.hpp"
#include "hex.hpp"
#include "transaction.hpp"
#include "u256.hpp"

#include "c_share/crypto/crypto.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace c_share::evm {

using nlohmann::json;

namespace detail {

// What every operation shares with the Client, and outlives it with: the
// operations still in flight when the Client goes keep it alive.
struct ClientState {
    ClientState(http::Client& http_client, Config client_config)
        : http(http_client), config(std::move(client_config)) {}

    http::Client& http;
    const Config config;

    std::mutex mutex; // guards the rest
    bool closed = false;
    // The http::Client handles of the requests and timers in flight, for
    // the destructor to cancel.
    std::unordered_set<http::Client::Handle> in_flight;
    std::optional<U256> chain_id;
    std::uint64_t next_id = 1;
};

} // namespace detail

namespace {

using State = std::shared_ptr<detail::ClientState>;

std::string dump(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

Error error(const ErrorCode code, std::string message) {
    Error out;
    out.code = code;
    out.message = std::move(message);
    return out;
}

// One operation: its callback, called exactly once.
class Operation {
public:
    Operation(void* context, const ResultCallback callback) : context_(context), callback_(callback) {}

    void finish(const Error& failure, const std::string& result = {}) {
        if (done_) return;
        done_ = true;
        if (callback_) callback_(context_, failure, result);
    }

    void succeed(const std::string& result) { finish(Error{}, result); }

private:
    void* context_;
    ResultCallback callback_;
    bool done_ = false;
};

using OperationPtr = std::shared_ptr<Operation>;
// What to do with a JSON-RPC call's outcome: the error, or the result.
using Continuation = std::function<void(const Error&, const json&)>;

// A JSON-RPC error, a revert's made readable.
Error rpc_error(const json& body) {
    Error out = error(ErrorCode::Rpc, body.value("message", std::string("JSON-RPC error")));
    if (const auto code = body.find("code"); code != body.end() && code->is_number_integer())
        out.rpc_code = code->get<std::int64_t>();
    if (const auto data = body.find("data"); data != body.end()) {
        out.rpc_data = dump(*data);
        if (data->is_string()) {
            if (const auto bytes = from_hex(data->get_ref<const std::string&>())) {
                if (const auto reason = decode_revert_reason(*bytes)) {
                    out.code = ErrorCode::Reverted;
                    out.message = "execution reverted: " + *reason;
                }
            }
        }
    }
    if (out.message.find("revert") != std::string::npos) out.code = ErrorCode::Reverted;
    return out;
}

struct RpcCall {
    State state;
    OperationPtr operation;
    Continuation then;
    http::Client::Handle handle = 0;
};

// By value, as http::CompletionCallback takes them.
// ReSharper disable once CppPassValueParameterByConstReference
// NOLINTNEXTLINE(performance-unnecessary-value-param)
void on_rpc_done(void* context, http::Response response, http::Error failure) {
    const std::unique_ptr<RpcCall> call(static_cast<RpcCall*>(context));
    {
        std::scoped_lock lock(call->state->mutex);
        call->state->in_flight.erase(call->handle);
    }
    Error outcome;
    json result;
    if (failure) {
        outcome = error(failure.code == http::ErrorCode::Aborted ? ErrorCode::Aborted : ErrorCode::Transport,
                        failure.message);
    } else {
        const json body = json::parse(response.body, nullptr, false);
        const bool ok_status = response.status_code >= 200 && response.status_code < 300;
        if (body.is_object() && body.contains("error") && body["error"].is_object()) {
            outcome = rpc_error(body["error"]);
        } else if (!ok_status) {
            outcome = error(ErrorCode::Http,
                            "HTTP " + std::to_string(response.status_code) + ": " + response.body.substr(0, 200));
        } else if (!body.is_object() || !body.contains("result")) {
            outcome = error(ErrorCode::InvalidResponse, "not a JSON-RPC response: " + response.body.substr(0, 200));
        } else {
            result = body["result"];
        }
    }
    // Whatever goes wrong in a step ends its operation, not the loop.
    try {
        call->then(outcome, result);
    } catch (const AbiError& e) {
        call->operation->finish(error(ErrorCode::InvalidResponse, e.what()));
    } catch (const std::exception& e) {
        call->operation->finish(error(ErrorCode::InvalidResponse, std::string("unexpected response: ") + e.what()));
    }
}

void rpc(const State& state, const OperationPtr& operation, const std::string& method, json params, Continuation then) {
    auto call = std::make_unique<RpcCall>();
    call->state = state;
    call->operation = operation;
    call->then = std::move(then);

    http::Request request;
    request.url = state->config.url;
    request.method = http::Method::Post;
    request.headers = state->config.headers;
    request.headers.push_back({"Content-Type", "application/json"});
    request.timeout = state->config.timeout;

    std::unique_lock lock(state->mutex);
    if (state->closed) {
        lock.unlock();
        operation->finish(error(ErrorCode::Aborted, "the evm::Client was destroyed"));
        return;
    }
    request.body =
        dump(json{{"jsonrpc", "2.0"}, {"id", state->next_id++}, {"method", method}, {"params", std::move(params)}});
    // Under the lock: on_rpc_done takes it to erase the handle, so it cannot
    // run before the handle is in.
    RpcCall* raw = call.release();
    raw->handle = state->http.send(std::move(request), raw, nullptr, &on_rpc_done);
    state->in_flight.insert(raw->handle);
}

// A quantity result ("0x..."), as a U256.
U256 quantity(const json& result) {
    if (!result.is_string()) throw AbiError("expected a quantity, got " + dump(result));
    const auto value = U256::from_quantity(result.get_ref<const std::string&>());
    if (!value) throw AbiError("expected a quantity, got " + dump(result));
    return *value;
}

// Runs `method` and reports its quantity result in decimal.
void quantity_request(const State& state, const OperationPtr& operation, const std::string& method, json params) {
    rpc(state, operation, method, std::move(params), [operation](const Error& failure, const json& result) {
        if (failure) {
            operation->finish(failure);
            return;
        }
        operation->succeed(quantity(result).to_decimal());
    });
}

std::optional<std::string> block_parameter(const std::string& block) {
    if (block.empty()) return "latest";
    for (const char* tag : {"latest", "pending", "safe", "finalized", "earliest"}) {
        if (block == tag) return block;
    }
    const auto number = U256::parse(block);
    if (!number) return std::nullopt;
    return number->to_quantity();
}

// The checksummed address, or the InvalidArgument error saying why not.
std::optional<std::string> checked_address(const std::string& text, const char* what, const OperationPtr& operation) {
    const auto address = parse_address(text);
    if (!address) {
        operation->finish(
            error(ErrorCode::InvalidArgument,
                  std::string(what) + " is not an address (or has a wrong EIP-55 checksum): \"" + text + "\""));
        return std::nullopt;
    }
    return to_checksum_address(*address);
}

std::optional<json> parse_json_array(const std::string& text, const char* what, const OperationPtr& operation) {
    if (text.empty()) return json::array();
    json value = json::parse(text, nullptr, false);
    if (!value.is_array()) {
        operation->finish(error(ErrorCode::InvalidArgument, std::string(what) + " must be a JSON array: " + text));
        return std::nullopt;
    }
    return value;
}

std::optional<U256> parse_amount(const std::string& text, const char* what, const OperationPtr& operation) {
    if (text.empty()) return U256();
    const auto value = U256::parse(text);
    if (!value) {
        operation->finish(error(ErrorCode::InvalidArgument,
                                std::string(what) + R"( must be a decimal or "0x" hex integer: ")" + text + "\""));
    }
    return value;
}

// eth_maxPriorityFeePerGas's fallback for nodes without it.
constexpr std::uint64_t kDefaultPriorityFee = 1'000'000'000; // 1 gwei

// A transaction on its way: each step asks the node for one thing the
// options left out, then signs and sends.
class SendTransaction : public std::enable_shared_from_this<SendTransaction> {
public:
    SendTransaction(State state, OperationPtr operation, const PrivateKey& private_key)
        : state_(std::move(state)), operation_(std::move(operation)), private_key_(private_key) {}

    SendTransaction(const SendTransaction&) = delete;
    SendTransaction& operator=(const SendTransaction&) = delete;
    SendTransaction(SendTransaction&&) = delete;
    SendTransaction& operator=(SendTransaction&&) = delete;
    ~SendTransaction() { std::fill(private_key_.begin(), private_key_.end(), std::uint8_t{0}); }

    // Validates everything that can be without the node; false if it
    // already finished the operation with the reason.
    bool prepare(const std::string& to, const Bytes& data, const TxOptions& options) {
        Address from{};
        if (c_share_crypto_evm_address_from_private_key(private_key_.data(), from.data()) != 0) {
            operation_->finish(error(ErrorCode::InvalidArgument, "the private key is not a secp256k1 key"));
            return false;
        }
        from_ = to_checksum_address(from);
        if (!to.empty()) {
            const auto address = parse_address(to);
            if (!address) {
                operation_->finish(error(ErrorCode::InvalidArgument,
                                         "to is not an address (or has a wrong EIP-55 checksum): \"" + to + "\""));
                return false;
            }
            tx_.to = *address;
        }
        tx_.data = data;
        const auto value = parse_amount(options.value, "value", operation_);
        if (!value) return false;
        tx_.value = *value;
        if (options.nonce) {
            tx_.nonce = U256(*options.nonce);
            have_nonce_ = true;
        }
        if (options.gas_limit) {
            tx_.gas_limit = U256(*options.gas_limit);
            have_gas_limit_ = true;
        }
        if (!options.max_fee_per_gas.empty()) {
            const auto fee = parse_amount(options.max_fee_per_gas, "max_fee_per_gas", operation_);
            if (!fee) return false;
            tx_.max_fee_per_gas = *fee;
            have_max_fee_ = true;
        }
        if (!options.max_priority_fee_per_gas.empty()) {
            const auto fee = parse_amount(options.max_priority_fee_per_gas, "max_priority_fee_per_gas", operation_);
            if (!fee) return false;
            tx_.max_priority_fee_per_gas = *fee;
            have_priority_fee_ = true;
        }
        return true;
    }

    void start() { chain_id(); }

private:
    // Each step's continuation: an error ends the operation.
    template <typename Step> Continuation then(const Step& step) {
        return [self = shared_from_this(), step](const Error& failure, const json& result) {
            if (failure) {
                self->operation_->finish(failure);
                return;
            }
            step(*self, result);
        };
    }

    void chain_id() {
        {
            std::scoped_lock lock(state_->mutex);
            if (state_->config.chain_id) state_->chain_id = U256(*state_->config.chain_id);
            if (state_->chain_id) tx_.chain_id = *state_->chain_id;
        }
        if (!tx_.chain_id.is_zero()) {
            nonce();
            return;
        }
        rpc(state_, operation_, "eth_chainId", json::array(), then([](SendTransaction& self, const json& result) {
                self.tx_.chain_id = quantity(result);
                {
                    std::scoped_lock lock(self.state_->mutex);
                    self.state_->chain_id = self.tx_.chain_id;
                }
                self.nonce();
            }));
    }

    void nonce() {
        if (have_nonce_) {
            priority_fee();
            return;
        }
        rpc(state_, operation_, "eth_getTransactionCount", json::array({from_, "pending"}),
            then([](SendTransaction& self, const json& result) {
                self.tx_.nonce = quantity(result);
                self.priority_fee();
            }));
    }

    void priority_fee() {
        if (have_priority_fee_) {
            base_fee();
            return;
        }
        rpc(state_, operation_, "eth_maxPriorityFeePerGas", json::array(),
            [self = shared_from_this()](const Error& failure, const json& result) {
                if (failure.code == ErrorCode::Rpc) {
                    self->tx_.max_priority_fee_per_gas = U256(kDefaultPriorityFee); // the node lacks the method
                } else if (failure) {
                    self->operation_->finish(failure);
                    return;
                } else {
                    self->tx_.max_priority_fee_per_gas = quantity(result);
                }
                self->base_fee();
            });
    }

    void base_fee() {
        if (have_max_fee_) {
            fees_known();
            return;
        }
        rpc(state_, operation_, "eth_getBlockByNumber", json::array({"latest", false}),
            then([](SendTransaction& self, const json& block) {
                if (!block.is_object() || !block.contains("baseFeePerGas")) {
                    self.operation_->finish(
                        error(ErrorCode::InvalidResponse,
                              "the latest block has no baseFeePerGas: the chain does not support EIP-1559"));
                    return;
                }
                // Twice the base fee, plus the tip: room for the base fee to
                // rise for a few blocks.
                U256 max_fee = quantity(block["baseFeePerGas"]);
                if (!max_fee.multiply(2) || !max_fee.add(self.tx_.max_priority_fee_per_gas)) {
                    self.operation_->finish(error(ErrorCode::InvalidResponse, "base fee overflows"));
                    return;
                }
                self.tx_.max_fee_per_gas = max_fee;
                self.fees_known();
            }));
    }

    void fees_known() {
        // A tip above the maximum fee is invalid; the maximum wins.
        if (tx_.max_fee_per_gas < tx_.max_priority_fee_per_gas) tx_.max_priority_fee_per_gas = tx_.max_fee_per_gas;
        gas_limit();
    }

    void gas_limit() {
        if (have_gas_limit_) {
            sign_and_send();
            return;
        }
        json call{{"from", from_}, {"data", to_hex(tx_.data)}, {"value", tx_.value.to_quantity()}};
        if (tx_.to) call["to"] = to_checksum_address(*tx_.to);
        rpc(state_, operation_, "eth_estimateGas", json::array({call}),
            then([](SendTransaction& self, const json& result) {
                self.tx_.gas_limit = quantity(result);
                self.sign_and_send();
            }));
    }

    void sign_and_send() {
        const auto hash = tx_.signing_hash();
        std::array<std::uint8_t, 65> signature{};
        if (c_share_crypto_secp256k1_sign_recoverable(private_key_.data(), hash.data(), signature.data()) != 0) {
            operation_->finish(error(ErrorCode::Signing, "signing the transaction failed"));
            return;
        }
        const Bytes raw = tx_.encode_signed(signature);
        expected_hash_ = to_hex(keccak256(raw).data(), 32);
        rpc(state_, operation_, "eth_sendRawTransaction", json::array({to_hex(raw)}),
            then([](const SendTransaction& self, const json& result) {
                // Some nodes answer with the hash, some with null.
                self.operation_->succeed(result.is_string() ? result.get<std::string>() : self.expected_hash_);
            }));
    }

    State state_;
    OperationPtr operation_;
    PrivateKey private_key_;
    std::string from_;
    Transaction tx_;
    std::string expected_hash_; // the signed transaction's own hash
    bool have_nonce_ = false;
    bool have_gas_limit_ = false;
    bool have_max_fee_ = false;
    bool have_priority_fee_ = false;
};

// Polls for a receipt, waiting on http::Client timers in between.
class WaitForReceipt : public std::enable_shared_from_this<WaitForReceipt> {
public:
    WaitForReceipt(State state, OperationPtr operation, std::string hash, const WaitOptions& options)
        : state_(std::move(state)), operation_(std::move(operation)), hash_(std::move(hash)),
          interval_(options.poll_interval), deadline_(std::chrono::steady_clock::now() + options.timeout) {}

    void poll() {
        rpc(state_, operation_, "eth_getTransactionReceipt", json::array({hash_}),
            [self = shared_from_this()](const Error& failure, const json& result) {
                if (failure) {
                    self->operation_->finish(failure);
                    return;
                }
                if (!result.is_null()) {
                    self->operation_->succeed(dump(result));
                    return;
                }
                self->wait();
            });
    }

private:
    struct Timer {
        std::shared_ptr<WaitForReceipt> wait;
        http::Client::Handle handle = 0;
    };

    void wait() {
        if (std::chrono::steady_clock::now() + interval_ > deadline_) {
            operation_->finish(error(ErrorCode::Timeout, "no receipt for " + hash_ + " in time"));
            return;
        }
        auto timer = std::make_unique<Timer>();
        timer->wait = shared_from_this();
        std::unique_lock lock(state_->mutex);
        if (state_->closed) {
            lock.unlock();
            {
                operation_->finish(error(ErrorCode::Aborted, "the evm::Client was destroyed"));
                return;
            }
        }
        Timer* raw = timer.release();
        raw->handle = state_->http.set_timer(interval_, raw, &WaitForReceipt::on_timer);
        state_->in_flight.insert(raw->handle);
    }

    static void on_timer(void* context, const bool cancelled) {
        const std::unique_ptr<Timer> timer(static_cast<Timer*>(context));
        const auto& self = timer->wait;
        {
            std::scoped_lock lock(self->state_->mutex);
            self->state_->in_flight.erase(timer->handle);
        }
        if (cancelled) {
            self->operation_->finish(error(ErrorCode::Aborted, "the evm::Client was destroyed"));
            return;
        }
        self->poll();
    }

    State state_;
    OperationPtr operation_;
    std::string hash_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point deadline_;
};

// The function and calldata for a contract call, or nullopt after
// finishing the operation with the reason.
std::optional<std::pair<AbiFunction, Bytes>> prepare_call(const std::string& function, const std::string& args_json,
                                                          const OperationPtr& operation) {
    try {
        AbiFunction parsed = parse_function(function);
        const auto args = parse_json_array(args_json, "args", operation);
        if (!args) return std::nullopt;
        Bytes data = encode_call(parsed, *args);
        return std::make_pair(std::move(parsed), std::move(data));
    } catch (const AbiError& e) {
        operation->finish(error(ErrorCode::InvalidArgument, e.what()));
        return std::nullopt;
    }
}

} // namespace

// Client's methods all work through state_, where CLion does not look, so it
// would make each of them const.
// ReSharper disable CppMemberFunctionMayBeConst

Client::Client(http::Client& http, Config config)
    : state_(std::make_shared<detail::ClientState>(http, std::move(config))) {}

// Only running out of memory could throw here -- terminating, as a
// destructor is noexcept.
// NOLINTNEXTLINE(bugprone-exception-escape)
Client::~Client() {
    // Each operation in flight reports Aborted, from the loop, once its
    // request or timer is cancelled; one between two steps finds `closed`
    // and stops there.
    std::vector<http::Client::Handle> in_flight;
    {
        std::scoped_lock lock(state_->mutex);
        state_->closed = true;
        in_flight.assign(state_->in_flight.begin(), state_->in_flight.end());
    }
    for (const auto handle : in_flight)
        state_->http.cancel(handle);
}

void Client::request(const std::string& method, const std::string& params_json, void* context,
                     const ResultCallback callback) {
    const auto operation = std::make_shared<Operation>(context, callback);
    const auto params = parse_json_array(params_json, "params", operation);
    if (!params) return;
    rpc(state_, operation, method, *params, [operation](const Error& failure, const json& result) {
        if (failure) {
            operation->finish(failure);
            return;
        }
        operation->succeed(dump(result));
    });
}

void Client::get_chain_id(void* context, const ResultCallback callback) {
    quantity_request(state_, std::make_shared<Operation>(context, callback), "eth_chainId", json::array());
}

void Client::get_block_number(void* context, const ResultCallback callback) {
    quantity_request(state_, std::make_shared<Operation>(context, callback), "eth_blockNumber", json::array());
}

void Client::get_gas_price(void* context, const ResultCallback callback) {
    quantity_request(state_, std::make_shared<Operation>(context, callback), "eth_gasPrice", json::array());
}

void Client::get_balance(const std::string& address, const std::string& block, void* context,
                         const ResultCallback callback) {
    const auto operation = std::make_shared<Operation>(context, callback);
    const auto checked = checked_address(address, "address", operation);
    if (!checked) return;
    const auto tag = block_parameter(block);
    if (!tag) {
        operation->finish(error(ErrorCode::InvalidArgument, "not a block: \"" + block + "\""));
        return;
    }
    quantity_request(state_, operation, "eth_getBalance", json::array({*checked, *tag}));
}

void Client::get_transaction_count(const std::string& address, const std::string& block, void* context,
                                   const ResultCallback callback) {
    const auto operation = std::make_shared<Operation>(context, callback);
    const auto checked = checked_address(address, "address", operation);
    if (!checked) return;
    const auto tag = block_parameter(block);
    if (!tag) {
        operation->finish(error(ErrorCode::InvalidArgument, "not a block: \"" + block + "\""));
        return;
    }
    quantity_request(state_, operation, "eth_getTransactionCount", json::array({*checked, *tag}));
}

void Client::read_contract(const std::string& to, const std::string& function, const std::string& args_json,
                           const ReadOptions& options, void* context, const ResultCallback callback) {
    const auto operation = std::make_shared<Operation>(context, callback);
    const auto contract = checked_address(to, "to", operation);
    if (!contract) return;
    const auto tag = block_parameter(options.block);
    if (!tag) {
        operation->finish(error(ErrorCode::InvalidArgument, "not a block: \"" + options.block + "\""));
        return;
    }
    auto call = prepare_call(function, args_json, operation);
    if (!call) return;
    json request{{"to", *contract}, {"data", to_hex(call->second)}};
    if (!options.from.empty()) {
        const auto from = checked_address(options.from, "from", operation);
        if (!from) return;
        request["from"] = *from;
    }
    rpc(state_, operation, "eth_call", json::array({request, *tag}),
        [operation, function = std::move(call->first)](const Error& failure, const json& result) {
            if (failure) {
                operation->finish(failure);
                return;
            }
            if (!result.is_string()) throw AbiError("eth_call returned " + dump(result));
            if (!function.has_outputs) {
                operation->succeed(dump(result));
                return;
            }
            const auto data = from_hex(result.get_ref<const std::string&>());
            if (!data) throw AbiError("eth_call returned " + dump(result));
            operation->succeed(dump(abi_decode(function.outputs, *data)));
        });
}

void Client::write_contract(const PrivateKey& private_key, const std::string& to, const std::string& function,
                            const std::string& args_json, const TxOptions& options, void* context,
                            const ResultCallback callback) {
    const auto operation = std::make_shared<Operation>(context, callback);
    const auto call = prepare_call(function, args_json, operation);
    if (!call) return;
    if (to.empty()) {
        operation->finish(error(ErrorCode::InvalidArgument, "write_contract needs a contract (to)"));
        return;
    }
    const auto send = std::make_shared<SendTransaction>(state_, operation, private_key);
    if (send->prepare(to, call->second, options)) send->start();
}

void Client::send_transaction(const PrivateKey& private_key, const std::string& to, const std::string& data_hex,
                              const TxOptions& options, void* context, const ResultCallback callback) {
    const auto operation = std::make_shared<Operation>(context, callback);
    Bytes data;
    if (!data_hex.empty()) {
        auto parsed = from_hex(data_hex);
        if (!parsed) {
            operation->finish(error(ErrorCode::InvalidArgument, "data is not \"0x\" and hex digits: " + data_hex));
            return;
        }
        data = std::move(*parsed);
    }
    const auto send = std::make_shared<SendTransaction>(state_, operation, private_key);
    if (send->prepare(to, data, options)) send->start();
}

void Client::get_transaction_receipt(const std::string& hash, void* context, const ResultCallback callback) {
    const auto operation = std::make_shared<Operation>(context, callback);
    if (const auto bytes = from_hex(hash); !bytes || bytes->size() != 32) {
        operation->finish(error(ErrorCode::InvalidArgument, "not a transaction hash: \"" + hash + "\""));
        return;
    }
    rpc(state_, operation, "eth_getTransactionReceipt", json::array({hash}),
        [operation](const Error& failure, const json& result) {
            if (failure) {
                operation->finish(failure);
                return;
            }
            operation->succeed(dump(result));
        });
}

void Client::wait_for_receipt(const std::string& hash, const WaitOptions& options, void* context,
                              const ResultCallback callback) {
    const auto operation = std::make_shared<Operation>(context, callback);
    if (const auto bytes = from_hex(hash); !bytes || bytes->size() != 32) {
        operation->finish(error(ErrorCode::InvalidArgument, "not a transaction hash: \"" + hash + "\""));
        return;
    }
    std::make_shared<WaitForReceipt>(state_, operation, hash, options)->poll();
}

// ReSharper restore CppMemberFunctionMayBeConst

Error encode_function_call(const std::string& function, const std::string& args_json, std::string& out_data_hex) {
    try {
        const json args = args_json.empty() ? json::array() : json::parse(args_json);
        out_data_hex = to_hex(encode_call(parse_function(function), args));
        return {};
    } catch (const std::exception& e) {
        return error(ErrorCode::InvalidArgument, e.what());
    }
}

Error decode_function_result(const std::string& function, const std::string& data_hex, std::string& out_json) {
    try {
        const auto data = from_hex(data_hex);
        if (!data) return error(ErrorCode::InvalidArgument, "data is not \"0x\" and hex digits");
        out_json = dump(abi_decode(parse_function(function).outputs, *data));
        return {};
    } catch (const std::exception& e) {
        return error(ErrorCode::InvalidArgument, e.what());
    }
}

Error address_from_private_key(const PrivateKey& private_key, std::string& out_address) {
    Address address{};
    if (c_share_crypto_evm_address_from_private_key(private_key.data(), address.data()) != 0)
        return error(ErrorCode::InvalidArgument, "the private key is not a secp256k1 key");
    out_address = to_checksum_address(address);
    return {};
}

} // namespace c_share::evm
