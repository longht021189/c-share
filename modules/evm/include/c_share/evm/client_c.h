#ifndef C_SHARE_EVM_CLIENT_C_H
#define C_SHARE_EVM_CLIENT_C_H

/*
 * Plain C API over c_share::evm::Client (client.hpp, which documents each
 * operation and the results), for bindgen and other C FFIs.
 *
 * A client works through a c_share_http_client_t it is given and never
 * creates or runs one: c_share_http_client_create starts that client's loop,
 * and every callback here runs on it.
 *
 * Strings come in as (pointer, length) pairs; a NULL pointer with length 0
 * is the empty string. Every operation reports through its
 * c_share_evm_result_cb exactly once: for invalid arguments, before the call
 * returns. The result and error strings passed to it are only valid during
 * the call.
 */

#include "c_share/http/client_c.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct c_share_evm_client c_share_evm_client_t;

typedef enum {
    C_SHARE_EVM_ERROR_NONE = 0,
    C_SHARE_EVM_ERROR_INVALID_ARGUMENT,
    C_SHARE_EVM_ERROR_TRANSPORT,
    C_SHARE_EVM_ERROR_HTTP,
    C_SHARE_EVM_ERROR_RPC,
    C_SHARE_EVM_ERROR_REVERTED,
    C_SHARE_EVM_ERROR_INVALID_RESPONSE,
    C_SHARE_EVM_ERROR_SIGNING,
    C_SHARE_EVM_ERROR_ABORTED,
    C_SHARE_EVM_ERROR_TIMEOUT,
} c_share_evm_error_code_t;

typedef struct {
    c_share_evm_error_code_t code;
    const char* message;
    size_t message_len;
    int64_t rpc_code;     /* a JSON-RPC error's code */
    const char* rpc_data; /* its "data", as JSON; empty if none */
    size_t rpc_data_len;
} c_share_evm_error_t;

typedef void (*c_share_evm_result_cb)(void* context, c_share_evm_error_t error, const char* result, size_t result_len);

typedef struct {
    const char* url;
    size_t url_len;
    const c_share_http_header_t* headers; /* sent with every request */
    size_t headers_len;
    uint64_t timeout_ms; /* per request; 0 = 30 s */
    uint64_t chain_id;   /* 0: ask the node */
} c_share_evm_config_t;

/* Which block a read is at ("latest" if empty), and eth_call's sender. */
typedef struct {
    const char* block;
    size_t block_len;
    const char* from;
    size_t from_len;
} c_share_evm_read_options_t;

/* Amounts in wei, decimal or "0x" hex; empty = ask the node (value: 0). */
typedef struct {
    const char* value;
    size_t value_len;
    int has_nonce;
    uint64_t nonce;
    int has_gas_limit;
    uint64_t gas_limit;
    const char* max_fee_per_gas;
    size_t max_fee_per_gas_len;
    const char* max_priority_fee_per_gas;
    size_t max_priority_fee_per_gas_len;
} c_share_evm_tx_options_t;

typedef struct {
    uint64_t poll_interval_ms; /* 0 = 1 s */
    uint64_t timeout_ms;       /* 0 = 120 s */
} c_share_evm_wait_options_t;

/* NULL if `http` or `config` is NULL. `http` must outlive the client. */
c_share_evm_client_t* c_share_evm_client_create(c_share_http_client_t* http, const c_share_evm_config_t* config);
/* Aborts what is in flight: those callbacks still run, with
 * C_SHARE_EVM_ERROR_ABORTED, from the http client's loop -- possibly after
 * this returns. NULL is a no-op. */
void c_share_evm_client_destroy(c_share_evm_client_t* client);

void c_share_evm_request(c_share_evm_client_t* client, const char* method, size_t method_len, const char* params_json,
                         size_t params_json_len, void* context, c_share_evm_result_cb callback);
void c_share_evm_get_chain_id(c_share_evm_client_t* client, void* context, c_share_evm_result_cb callback);
void c_share_evm_get_block_number(c_share_evm_client_t* client, void* context, c_share_evm_result_cb callback);
void c_share_evm_get_gas_price(c_share_evm_client_t* client, void* context, c_share_evm_result_cb callback);
void c_share_evm_get_balance(c_share_evm_client_t* client, const char* address, size_t address_len, const char* block,
                             size_t block_len, void* context, c_share_evm_result_cb callback);
void c_share_evm_get_transaction_count(c_share_evm_client_t* client, const char* address, size_t address_len,
                                       const char* block, size_t block_len, void* context,
                                       c_share_evm_result_cb callback);

/* `options` may be NULL. */
void c_share_evm_read_contract(c_share_evm_client_t* client, const char* to, size_t to_len, const char* function,
                               size_t function_len, const char* args_json, size_t args_json_len,
                               const c_share_evm_read_options_t* options, void* context,
                               c_share_evm_result_cb callback);
/* `options` may be NULL. The private key is 32 bytes. */
void c_share_evm_write_contract(c_share_evm_client_t* client, const uint8_t private_key[32], const char* to,
                                size_t to_len, const char* function, size_t function_len, const char* args_json,
                                size_t args_json_len, const c_share_evm_tx_options_t* options, void* context,
                                c_share_evm_result_cb callback);
void c_share_evm_send_transaction(c_share_evm_client_t* client, const uint8_t private_key[32], const char* to,
                                  size_t to_len, const char* data_hex, size_t data_hex_len,
                                  const c_share_evm_tx_options_t* options, void* context,
                                  c_share_evm_result_cb callback);

void c_share_evm_get_transaction_receipt(c_share_evm_client_t* client, const char* hash, size_t hash_len, void* context,
                                         c_share_evm_result_cb callback);
/* `options` may be NULL. */
void c_share_evm_wait_for_receipt(c_share_evm_client_t* client, const char* hash, size_t hash_len,
                                  const c_share_evm_wait_options_t* options, void* context,
                                  c_share_evm_result_cb callback);

/* The same encoding and decoding without a node; `callback` runs before
 * these return. */
void c_share_evm_encode_function_call(const char* function, size_t function_len, const char* args_json,
                                      size_t args_json_len, void* context, c_share_evm_result_cb callback);
void c_share_evm_decode_function_result(const char* function, size_t function_len, const char* data_hex,
                                        size_t data_hex_len, void* context, c_share_evm_result_cb callback);
void c_share_evm_address_from_private_key(const uint8_t private_key[32], void* context, c_share_evm_result_cb callback);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* C_SHARE_EVM_CLIENT_C_H */
