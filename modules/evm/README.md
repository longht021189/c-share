# c_share::evm

An EVM JSON-RPC client: balances and other reads, contract calls
(`eth_call`), contract writes and transfers as signed EIP-1559
transactions, receipts, and any other JSON-RPC method. A C++ API
(`client.hpp`) and a C one (`client_c.h`) for bindgen.

It builds on two other modules and owns neither's resources:

- **http**: every request goes through a `c_share::http::Client` it is
  given (a `c_share_http_client_t` in C). It never creates one and never runs
  one: whoever owns that client runs its loop, and every callback here runs
  on that loop's thread. The waits between receipt polls are that client's
  timers too.
- **crypto**: Keccak-256, keys, addresses and recoverable secp256k1
  signatures.

JSON (requests, responses, ABI values) is nlohmann-json's, behind the
headers.

## Usage

```cpp
#include "c_share/evm/client.hpp"

c_share::http::Client http;                     // owned and run by the caller
std::thread loop([&] { http.run(); });

c_share::evm::Config config;
config.url = "https://sepolia.example/rpc";
config.headers.push_back({"Authorization", "Bearer ..."});
c_share::evm::Client evm(http, config);

// Every operation reports once, through a function pointer and a context.
evm.get_balance("0x9d8A62f656a8d1615C1294fd71e9CFb3E4855A4F", "latest", ctx,
                [](void* ctx, const c_share::evm::Error& error, const std::string& wei) { /* "1000..." */ });

evm.read_contract(token, "function balanceOf(address owner) view returns (uint256)",
                  R"(["0x9d8A62f656a8d1615C1294fd71e9CFb3E4855A4F"])", {}, ctx, on_result); // ["1000"]

c_share::evm::PrivateKey key = ...;             // c_share_crypto_secp256k1_generate_keypair
evm.write_contract(key, token, "function transfer(address to, uint256 amount) returns (bool)",
                   R"(["0x3535353535353535353535353535353535353535", "1000"])", {}, ctx, on_hash);

evm.wait_for_receipt(hash, {}, ctx, on_receipt); // {"status":"0x1",...}
```

The C API is the same operations with `(pointer, length)` strings and a
`c_share_evm_result_cb(void* context, c_share_evm_error_t error, const char*
result, size_t result_len)`; see `client_c.h`.

## Public networks

`networks.h` lists 74 popular EVM networks, mainnets and testnets, with
100 free public RPC URLs between them that need no API key, each network's
in order of preference (the chain's own endpoint first):

```cpp
#include "c_share/evm/networks.h"

const c_share_evm_network_t* base = c_share_evm_network_find(8453);
c_share::evm::Client client(http, c_share::evm::config_for(*base));   // its first URL
c_share::evm::Client fallback(http, c_share::evm::config_for(*base, 1)); // its second

size_t count = 0;
const c_share_evm_network_t* all = c_share_evm_networks(&count);
```

Every URL answered `eth_chainId` with its chain id, `eth_getBlockByNumber`
with a base fee and `eth_getBalance`, without an API key or a User-Agent,
twice, when the list was made (2026-10-16, from the
[ethereum-lists/chains](https://chainid.network/chains.json) registry); networks without
EIP-1559 were left out, as `write_contract` needs it. Free endpoints are
rate-limited and change over time: fine for development and light use, not a
production SLA. `python3 scripts/check_rpc_endpoints.py` checks them all
again and exits 1 if any stopped answering.

| Chain id  | Network                 | Currency | Testnet | RPC URLs |
|-----------|-------------------------|----------|---------|----------|
| 1         | Ethereum                | ETH      |         | 3        |
| 11155111  | Ethereum Sepolia        | ETH      | yes     | 2        |
| 560048    | Ethereum Hoodi          | ETH      | yes     | 1        |
| 56        | BNB Smart Chain         | BNB      |         | 2        |
| 97        | BNB Smart Chain Testnet | tBNB     | yes     | 2        |
| 137       | Polygon                 | POL      |         | 2        |
| 80002     | Polygon Amoy            | POL      | yes     | 2        |
| 42161     | Arbitrum One            | ETH      |         | 2        |
| 421614    | Arbitrum Sepolia        | ETH      | yes     | 2        |
| 42170     | Arbitrum Nova           | ETH      |         | 2        |
| 10        | OP Mainnet              | ETH      |         | 2        |
| 11155420  | OP Sepolia              | ETH      | yes     | 2        |
| 8453      | Base                    | ETH      |         | 2        |
| 84532     | Base Sepolia            | ETH      | yes     | 2        |
| 43114     | Avalanche C-Chain       | AVAX     |         | 2        |
| 43113     | Avalanche Fuji          | AVAX     | yes     | 2        |
| 100       | Gnosis                  | XDAI     |         | 2        |
| 10200     | Gnosis Chiado           | XDAI     | yes     | 2        |
| 59144     | Linea                   | ETH      |         | 2        |
| 59141     | Linea Sepolia           | ETH      | yes     | 2        |
| 324       | ZKsync Era              | ETH      |         | 1        |
| 300       | ZKsync Era Sepolia      | ETH      | yes     | 1        |
| 534352    | Scroll                  | ETH      |         | 2        |
| 146       | Sonic                   | S        |         | 2        |
| 5000      | Mantle                  | MNT      |         | 2        |
| 5003      | Mantle Sepolia          | MNT      | yes     | 1        |
| 81457     | Blast                   | ETH      |         | 2        |
| 168587773 | Blast Sepolia           | ETH      | yes     | 1        |
| 130       | Unichain                | ETH      |         | 2        |
| 1301      | Unichain Sepolia        | ETH      | yes     | 2        |
| 25        | Cronos                  | CRO      |         | 1        |
| 338       | Cronos Testnet          | TCRO     | yes     | 1        |
| 42220     | Celo                    | CELO     |         | 1        |
| 204       | opBNB                   | BNB      |         | 1        |
| 5611      | opBNB Testnet           | tBNB     | yes     | 1        |
| 1284      | Moonbeam                | GLMR     |         | 1        |
| 1287      | Moonbase Alpha          | DEV      | yes     | 1        |
| 1285      | Moonriver               | MOVR     |         | 1        |
| 80094     | Berachain               | BERA     |         | 1        |
| 80069     | Berachain Bepolia       | BERA     | yes     | 1        |
| 7777777   | Zora                    | ETH      |         | 1        |
| 999999999 | Zora Sepolia            | ETH      | yes     | 1        |
| 34443     | Mode                    | ETH      |         | 1        |
| 919       | Mode Sepolia            | ETH      | yes     | 1        |
| 252       | Fraxtal                 | FRAX     |         | 1        |
| 1329      | Sei                     | SEI      |         | 1        |
| 1328      | Sei Testnet             | SEI      | yes     | 1        |
| 8217      | Kaia                    | KAIA     |         | 1        |
| 1001      | Kaia Kairos             | KAIA     | yes     | 1        |
| 480       | World Chain             | ETH      |         | 1        |
| 4801      | World Chain Sepolia     | ETH      | yes     | 1        |
| 57073     | Ink                     | ETH      |         | 1        |
| 763373    | Ink Sepolia             | ETH      | yes     | 1        |
| 2020      | Ronin                   | RON      |         | 1        |
| 13371     | Immutable zkEVM         | IMX      |         | 1        |
| 33139     | ApeChain                | APE      |         | 1        |
| 33111     | ApeChain Curtis         | APE      | yes     | 1        |
| 2741      | Abstract                | ETH      |         | 1        |
| 11124     | Abstract Sepolia        | ETH      | yes     | 1        |
| 1868      | Soneium                 | ETH      |         | 1        |
| 1946      | Soneium Minato          | ETH      | yes     | 1        |
| 169       | Manta Pacific           | ETH      |         | 1        |
| 3441006   | Manta Pacific Sepolia   | ETH      | yes     | 1        |
| 7000      | ZetaChain               | ZETA     |         | 1        |
| 7001      | ZetaChain Athens        | ZETA     | yes     | 1        |
| 1030      | Conflux eSpace          | CFX      |         | 1        |
| 71        | Conflux eSpace Testnet  | CFX      | yes     | 1        |
| 1116      | Core                    | CORE     |         | 1        |
| 1135      | Lisk                    | ETH      |         | 1        |
| 747       | Flow EVM                | FLOW     |         | 1        |
| 545       | Flow EVM Testnet        | FLOW     | yes     | 1        |
| 288       | Boba Network            | ETH      |         | 1        |
| 1514      | Story                   | DATA     |         | 1        |
| 1315      | Story Aeneid            | DATA     | yes     | 1        |

## Results and errors

Every result is text: quantities (balance, block number, nonce, chain id, gas
price) in decimal; `read_contract` the decoded values as a JSON array (or the
raw return data as a JSON string, for a signature without `returns (...)`);
writes the transaction hash; receipts as the node's JSON (`"null"` while
pending); `request` the method's result as JSON.

Each operation calls its callback exactly once. Invalid arguments are
reported before the call returns, everything else from the loop. An
`Error` says where a failure happened: `InvalidArgument`, `Transport`
(connection, TLS, timeout), `Http` (a non-2xx status without a JSON-RPC
error), `Rpc` (with `rpc_code` and `rpc_data`), `Reverted` (with the reason
decoded from `Error(string)` or `Panic(uint256)` revert data),
`InvalidResponse`, `Signing`, `Aborted` (the client was destroyed) and
`Timeout` (`wait_for_receipt`).

## ABI values

Functions are written as Solidity, ethers and viem write them:
`"function transfer(address to, uint256 amount) external returns (bool)"`
or just `"transfer(address,uint256)"`. Parameter names, data locations and
mutability are ignored; tuples are `(...)` or `tuple(...)`.

Arguments come as a JSON array, results go out as one:

| Type                  | In                                                                  | Out              |
|-----------------------|---------------------------------------------------------------------|------------------|
| `uintN`, `intN`       | a number, or a string: decimal (`intN` may be negative) or `0x` hex | a decimal string |
| `address`             | `0x` and 40 hex digits; mixed case must be its EIP-55 checksum      | checksummed      |
| `bool`                | `true` / `false`                                                    | the same         |
| `bytesN`, `bytes`     | `0x` and hex digits (`bytesN`: exactly N bytes)                     | the same         |
| `string`              | a string                                                            | the same         |
| `T[]`, `T[k]`, tuples | arrays                                                              | arrays           |

Integers come out as strings because JSON numbers lose precision past 2^53;
pass large amounts in as strings too.

## Transactions

`write_contract` and `send_transaction` build an EIP-1559 (type 2)
transaction, sign it (RFC 6979 deterministic, low `s`) and send it with
`eth_sendRawTransaction`. What `TxOptions` does not give, the node is asked
for, one request at a time:

| Field     | Default                                                        |
|-----------|----------------------------------------------------------------|
| chain id  | `Config::chain_id`, else `eth_chainId` (asked once per client) |
| nonce     | `eth_getTransactionCount(sender, "pending")`                   |
| tip       | `eth_maxPriorityFeePerGas`, 1 gwei if the node lacks it        |
| max fee   | 2 × the latest block's `baseFeePerGas` + the tip               |
| gas limit | `eth_estimateGas`                                              |

A chain whose blocks have no `baseFeePerGas` (no EIP-1559) is refused:
legacy transactions are not supported. Two transactions from one sender
started at the same time would get the same nonce; start the next one in the
previous one's callback, or give each its nonce.

## Not supported (yet)

Legacy and EIP-2930 transactions, access lists, EIP-712 typed data and
`personal_sign` messages, decoding events and logs (`request("eth_getLogs",
...)` returns them raw), and fixed-point ABI types.

## Tests

`tests/evm/`: the Solidity ABI specification's examples, EIP-55's addresses,
RLP's examples, and transactions signed exactly as eth-account 0.13.7 signs
the same fields (both use RFC 6979, so the bytes match). The client runs
against `fake_node.hpp`, a loopback JSON-RPC node each test scripts.
