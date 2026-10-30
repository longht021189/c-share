#pragma once

// An EVM JSON-RPC client: balances, contract reads and writes (signed
// EIP-1559 transactions), receipts, and any other JSON-RPC method.
//
// It does its HTTP through a c_share::http::Client it is given, never one of
// its own, and never runs that client's loop: whoever owns the http::Client
// runs it (http::Client::run(), on a thread of their choosing), and every
// callback here runs on that thread.
//
// Every operation reports through a ResultCallback, exactly once. Invalid
// arguments (a malformed address, a function signature that does not parse,
// arguments that do not fit it) are reported before the call returns;
// everything else later, from the loop.
//
// Results are text:
//   - quantities (balance, block number, chain id, nonce, gas price): a
//     decimal string, e.g. "1000000000000000000";
//   - read_contract: the decoded return values as a JSON array, e.g.
//     ["1000"], or, for a function without "returns (...)", the raw return
//     data as a JSON string, e.g. "0x...";
//   - write_contract, send_transaction: the transaction hash, "0x...";
//   - get_transaction_receipt, wait_for_receipt: the receipt as JSON (as the
//     node returned it), or "null" for a transaction not mined yet;
//   - request: the method's result as JSON.
// See src/abi.hpp for how ABI values are written in JSON.

#include "c_share/evm/networks.h"
#include "c_share/http/client.hpp"

// What this file uses, even where CLion finds it through other headers.
// ReSharper disable CppUnusedIncludeDirective
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
// ReSharper restore CppUnusedIncludeDirective

namespace c_share::evm {

namespace detail {
struct ClientState;
} // namespace detail

enum class ErrorCode {
    None,
    InvalidArgument, // see Error::message
    Transport,       // the request did not complete (connection, TLS, timeout)
    Http,            // an HTTP status other than 2xx, without a JSON-RPC error
    Rpc,             // a JSON-RPC error: Error::rpc_code, rpc_data
    Reverted,        // a JSON-RPC error that is a revert; message has its reason
    InvalidResponse, // not the JSON-RPC response expected
    Signing,         // the private key could not sign
    Aborted,         // the Client was destroyed first
    Timeout,         // wait_for_receipt ran out of time
};

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;
    // A JSON-RPC error's code, and its "data" as JSON ("" if none). For a
    // revert, the data is usually the revert's ABI-encoded error.
    std::int64_t rpc_code = 0;
    std::string rpc_data;

    explicit operator bool() const { return code != ErrorCode::None; }
};

using ResultCallback = void (*)(void* context, const Error& error, const std::string& result);

using PrivateKey = std::array<std::uint8_t, 32>;

struct Config {
    std::string url; // the node's JSON-RPC endpoint
    // Sent with every request, e.g. an API key header.
    std::vector<http::Header> headers;
    std::chrono::milliseconds timeout{30000}; // per request
    // The chain's id, if known: saves asking the node (eth_chainId) before
    // the first transaction. Asked once and remembered otherwise.
    std::optional<std::uint64_t> chain_id;
};

// Which block a read is at: "latest" (also when empty), "pending", "safe",
// "finalized", "earliest", or a block number (decimal or "0x" hex).
struct ReadOptions {
    std::string block;
    std::string from; // eth_call's sender, "0x..." ("" for none)
};

// What a transaction takes from the node unless given here. Amounts are in
// wei, decimal or "0x" hex; "" means "ask the node".
struct TxOptions {
    std::string value;                      // wei sent with it; "" is 0
    std::optional<std::uint64_t> nonce;     // default: eth_getTransactionCount(from, "pending")
    std::optional<std::uint64_t> gas_limit; // default: eth_estimateGas
    // Defaults: the node's eth_maxPriorityFeePerGas (1 gwei if it lacks the
    // method), and twice the latest block's base fee plus that tip.
    std::string max_fee_per_gas;
    std::string max_priority_fee_per_gas;
};

struct WaitOptions {
    std::chrono::milliseconds poll_interval{1000};
    std::chrono::milliseconds timeout{120000};
};

class Client {
public:
    // `http` must outlive this Client, and be running (or about to run)
    // its loop.
    Client(http::Client& http, Config config);
    // Aborts what is in flight: those callbacks still run, with
    // ErrorCode::Aborted, from the loop -- possibly after this returns, so
    // their contexts must outlive it.
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    // Any JSON-RPC method; `params_json` is its params array, e.g.
    // R"(["0x...", "latest"])" ("" for none).
    void request(const std::string& method, const std::string& params_json, void* context, ResultCallback callback);

    void get_chain_id(void* context, ResultCallback callback);
    void get_block_number(void* context, ResultCallback callback);
    void get_gas_price(void* context, ResultCallback callback);
    // In wei.
    void get_balance(const std::string& address, const std::string& block, void* context, ResultCallback callback);
    void get_transaction_count(const std::string& address, const std::string& block, void* context,
                               ResultCallback callback);

    // Calls `function` of the contract at `to` without a transaction
    // (eth_call). `function` is its signature, e.g.
    // "function balanceOf(address) view returns (uint256)", `args_json` its
    // arguments as a JSON array, e.g. R"(["0x..."])".
    void read_contract(const std::string& to, const std::string& function, const std::string& args_json,
                       const ReadOptions& options, void* context, ResultCallback callback);

    // Calls `function` of the contract at `to` in a transaction signed with
    // `private_key`: fills in what `options` leaves out from the node, signs
    // it (EIP-1559) and sends it. The result is the transaction's hash, once
    // the node accepted it; wait_for_receipt says when it is mined.
    //
    // The nonce comes from the node's count of the sender's pending
    // transactions, so two transactions from one sender started at once
    // would take the same nonce: start the next after the first one's
    // callback, or give each its nonce.
    void write_contract(const PrivateKey& private_key, const std::string& to, const std::string& function,
                        const std::string& args_json, const TxOptions& options, void* context, ResultCallback callback);

    // A transaction with raw calldata: `data_hex` "0x..." ("" for none), to
    // `to` ("" deploys `data_hex` as a contract's creation code). With
    // neither, a plain transfer of `options.value`.
    void send_transaction(const PrivateKey& private_key, const std::string& to, const std::string& data_hex,
                          const TxOptions& options, void* context, ResultCallback callback);

    void get_transaction_receipt(const std::string& hash, void* context, ResultCallback callback);
    // Polls get_transaction_receipt until the transaction is mined; the
    // waits between polls are http::Client timers, on its loop. The receipt's
    // "status" is "0x1" for success, "0x0" for a reverted transaction.
    void wait_for_receipt(const std::string& hash, const WaitOptions& options, void* context, ResultCallback callback);

private:
    std::shared_ptr<detail::ClientState> state_;
};

// A Config for one of c_share_evm_networks()'s networks: its chain id, and
// its `rpc_index`-th RPC URL (the last one past the end), e.g.
//   Client client(http, config_for(*c_share_evm_network_find(8453)));
// Headers and the timeout keep their defaults.
Config config_for(const c_share_evm_network_t& network, std::size_t rpc_index = 0);

// The same encoding and decoding, without a node.

// The calldata calling `function` with `args_json`: "0x...".
Error encode_function_call(const std::string& function, const std::string& args_json, std::string& out_data_hex);
// `data_hex` decoded as what `function` returns: a JSON array.
Error decode_function_result(const std::string& function, const std::string& data_hex, std::string& out_json);
// The checksummed address of `private_key`.
Error address_from_private_key(const PrivateKey& private_key, std::string& out_address);

} // namespace c_share::evm
