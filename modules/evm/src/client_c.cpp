#include "c_share/evm/client_c.h"

#include "c_share/evm/client.hpp"
#include "c_share/http/client_c.hpp"

#include <algorithm>
#include <memory>
#include <string>

namespace evm = c_share::evm;

namespace {

std::string text(const char* data, const std::size_t size) {
    return data ? std::string(data, size) : std::string();
}

c_share_evm_error_t to_c(const evm::Error& error) {
    return c_share_evm_error_t{static_cast<c_share_evm_error_code_t>(error.code),
                               error.message.data(),
                               error.message.size(),
                               error.rpc_code,
                               error.rpc_data.data(),
                               error.rpc_data.size()};
}

// A C callback and its context, as the C++ API's context.
struct Bridge {
    void* context;
    c_share_evm_result_cb callback;
};

void on_result(void* context, const evm::Error& error, const std::string& result) {
    const std::unique_ptr<Bridge> bridge(static_cast<Bridge*>(context));
    bridge->callback(bridge->context, to_c(error), result.data(), result.size());
}

void report(void* context, const c_share_evm_result_cb callback, const evm::Error& error, const std::string& result) {
    if (callback) callback(context, to_c(error), result.data(), result.size());
}

evm::Error invalid(const char* message) {
    evm::Error error;
    error.code = evm::ErrorCode::InvalidArgument;
    error.message = message;
    return error;
}

evm::PrivateKey key_of(const uint8_t private_key[32]) {
    evm::PrivateKey key{};
    std::copy_n(private_key, key.size(), key.begin());
    return key;
}

evm::TxOptions tx_options(const c_share_evm_tx_options_t* options) {
    evm::TxOptions out;
    if (!options) return out;
    out.value = text(options->value, options->value_len);
    if (options->has_nonce) out.nonce = options->nonce;
    if (options->has_gas_limit) out.gas_limit = options->gas_limit;
    out.max_fee_per_gas = text(options->max_fee_per_gas, options->max_fee_per_gas_len);
    out.max_priority_fee_per_gas = text(options->max_priority_fee_per_gas, options->max_priority_fee_per_gas_len);
    return out;
}

} // namespace

// The type client_c.h declares, so it cannot move into the namespace above.
// ReSharper disable once CppUseInternalLinkage
struct c_share_evm_client {
    evm::Client client;
};

namespace {

// The Bridge an operation hands the C++ API (on_result frees it), or NULL
// after reporting a NULL client; nothing at all without a callback.
Bridge* start(const c_share_evm_client_t* client, void* context, const c_share_evm_result_cb callback) {
    if (!callback) return nullptr;
    if (!client) {
        report(context, callback, invalid("client is NULL"), {});
        return nullptr;
    }
    return new Bridge{context, callback};
}

} // namespace

extern "C" {

c_share_evm_client_t* c_share_evm_client_create(c_share_http_client_t* http, const c_share_evm_config_t* config) {
    if (!http || !config) return nullptr;
    evm::Config cpp;
    cpp.url = text(config->url, config->url_len);
    for (std::size_t i = 0; i < config->headers_len; ++i) {
        const auto& [name, name_len, value, value_len] = config->headers[i];
        cpp.headers.push_back({text(name, name_len), text(value, value_len)});
    }
    if (config->timeout_ms > 0)
        cpp.timeout = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(config->timeout_ms));
    if (config->chain_id > 0) cpp.chain_id = config->chain_id;
    try {
        return new c_share_evm_client{evm::Client(c_share::http::client_of(http), std::move(cpp))};
    } catch (const std::exception&) {
        return nullptr;
    }
}

// Not const: it is the client's end.
// ReSharper disable once CppParameterMayBeConstPtrOrRef
void c_share_evm_client_destroy(c_share_evm_client_t* client) {
    delete client;
}

void c_share_evm_request(c_share_evm_client_t* client, const char* method, const size_t method_len,
                         const char* params_json, const size_t params_json_len, void* context,
                         const c_share_evm_result_cb callback) {
    auto* bridge = start(client, context, callback);
    if (!bridge) return;
    client->client.request(text(method, method_len), text(params_json, params_json_len), bridge, &on_result);
}

void c_share_evm_get_chain_id(c_share_evm_client_t* client, void* context, const c_share_evm_result_cb callback) {
    auto* bridge = start(client, context, callback);
    if (!bridge) return;
    client->client.get_chain_id(bridge, &on_result);
}

void c_share_evm_get_block_number(c_share_evm_client_t* client, void* context, const c_share_evm_result_cb callback) {
    auto* bridge = start(client, context, callback);
    if (!bridge) return;
    client->client.get_block_number(bridge, &on_result);
}

void c_share_evm_get_gas_price(c_share_evm_client_t* client, void* context, const c_share_evm_result_cb callback) {
    auto* bridge = start(client, context, callback);
    if (!bridge) return;
    client->client.get_gas_price(bridge, &on_result);
}

void c_share_evm_get_balance(c_share_evm_client_t* client, const char* address, const size_t address_len,
                             const char* block, const size_t block_len, void* context,
                             const c_share_evm_result_cb callback) {
    auto* bridge = start(client, context, callback);
    if (!bridge) return;
    client->client.get_balance(text(address, address_len), text(block, block_len), bridge, &on_result);
}

void c_share_evm_get_transaction_count(c_share_evm_client_t* client, const char* address, const size_t address_len,
                                       const char* block, const size_t block_len, void* context,
                                       const c_share_evm_result_cb callback) {
    auto* bridge = start(client, context, callback);
    if (!bridge) return;
    client->client.get_transaction_count(text(address, address_len), text(block, block_len), bridge, &on_result);
}

void c_share_evm_read_contract(c_share_evm_client_t* client, const char* to, const size_t to_len, const char* function,
                               const size_t function_len, const char* args_json, const size_t args_json_len,
                               const c_share_evm_read_options_t* options, void* context,
                               const c_share_evm_result_cb callback) {
    auto* bridge = start(client, context, callback);
    if (!bridge) return;
    evm::ReadOptions read;
    if (options) {
        read.block = text(options->block, options->block_len);
        read.from = text(options->from, options->from_len);
    }
    client->client.read_contract(text(to, to_len), text(function, function_len), text(args_json, args_json_len), read,
                                 bridge, &on_result);
}

void c_share_evm_write_contract(c_share_evm_client_t* client, const uint8_t private_key[32], const char* to,
                                const size_t to_len, const char* function, const size_t function_len,
                                const char* args_json, const size_t args_json_len,
                                const c_share_evm_tx_options_t* options, void* context,
                                const c_share_evm_result_cb callback) {
    if (callback && !private_key) {
        report(context, callback, invalid("private_key is NULL"), {});
        return;
    }
    auto* bridge = start(client, context, callback);
    if (!bridge) return;
    client->client.write_contract(key_of(private_key), text(to, to_len), text(function, function_len),
                                  text(args_json, args_json_len), tx_options(options), bridge, &on_result);
}

void c_share_evm_send_transaction(c_share_evm_client_t* client, const uint8_t private_key[32], const char* to,
                                  const size_t to_len, const char* data_hex, const size_t data_hex_len,
                                  const c_share_evm_tx_options_t* options, void* context,
                                  const c_share_evm_result_cb callback) {
    if (callback && !private_key) {
        report(context, callback, invalid("private_key is NULL"), {});
        return;
    }
    auto* bridge = start(client, context, callback);
    if (!bridge) return;
    client->client.send_transaction(key_of(private_key), text(to, to_len), text(data_hex, data_hex_len),
                                    tx_options(options), bridge, &on_result);
}

void c_share_evm_get_transaction_receipt(c_share_evm_client_t* client, const char* hash, const size_t hash_len,
                                         void* context, const c_share_evm_result_cb callback) {
    auto* bridge = start(client, context, callback);
    if (!bridge) return;
    client->client.get_transaction_receipt(text(hash, hash_len), bridge, &on_result);
}

void c_share_evm_wait_for_receipt(c_share_evm_client_t* client, const char* hash, const size_t hash_len,
                                  const c_share_evm_wait_options_t* options, void* context,
                                  const c_share_evm_result_cb callback) {
    auto* bridge = start(client, context, callback);
    if (!bridge) return;
    evm::WaitOptions wait;
    if (options && options->poll_interval_ms > 0)
        wait.poll_interval =
            std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(options->poll_interval_ms));
    if (options && options->timeout_ms > 0)
        wait.timeout = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(options->timeout_ms));
    client->client.wait_for_receipt(text(hash, hash_len), wait, bridge, &on_result);
}

void c_share_evm_encode_function_call(const char* function, const size_t function_len, const char* args_json,
                                      const size_t args_json_len, void* context, const c_share_evm_result_cb callback) {
    std::string out;
    const evm::Error error =
        evm::encode_function_call(text(function, function_len), text(args_json, args_json_len), out);
    report(context, callback, error, out);
}

void c_share_evm_decode_function_result(const char* function, const size_t function_len, const char* data_hex,
                                        const size_t data_hex_len, void* context,
                                        const c_share_evm_result_cb callback) {
    std::string out;
    const evm::Error error =
        evm::decode_function_result(text(function, function_len), text(data_hex, data_hex_len), out);
    report(context, callback, error, out);
}

void c_share_evm_address_from_private_key(const uint8_t private_key[32], void* context,
                                          const c_share_evm_result_cb callback) {
    if (!private_key) {
        report(context, callback, invalid("private_key is NULL"), {});
        return;
    }
    std::string out;
    const evm::Error error = evm::address_from_private_key(key_of(private_key), out);
    report(context, callback, error, out);
}

} // extern "C"
