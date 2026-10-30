#ifndef C_SHARE_EVM_NETWORKS_H
#define C_SHARE_EVM_NETWORKS_H

/*
 * Popular EVM networks, mainnets and testnets, each with free public JSON-RPC
 * endpoints that need no API key: a starting point, e.g. for
 * c_share::evm::config_for (client.hpp) or a c_share_evm_config_t.
 *
 * Free endpoints are rate-limited and come and go: fine for development and
 * light use, not a production SLA. Each network lists its endpoints in order
 * of preference (the chain's own first), so a caller can move on to the next
 * one when one fails. scripts/check_rpc_endpoints.py checks them all again.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t chain_id;
    const char* name;     /* e.g. "Ethereum", "Base Sepolia" */
    const char* symbol;   /* the native currency's, e.g. "ETH" */
    int testnet;          /* 1 for a testnet */
    const char* explorer; /* the block explorer's URL; "" if none */
    const char* const* rpc_urls;
    size_t rpc_urls_len; /* at least 1 */
} c_share_evm_network_t;

/* All of them; `out_count` (not NULL) gets how many. Static data: never
 * freed, safe from any thread. */
const c_share_evm_network_t* c_share_evm_networks(size_t* out_count);

/* The network with `chain_id`, or NULL. */
const c_share_evm_network_t* c_share_evm_network_find(uint64_t chain_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* C_SHARE_EVM_NETWORKS_H */
