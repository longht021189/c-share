#include "c_share/evm/networks.h"

#include "c_share/evm/client.hpp"

#include <algorithm>
#include <iterator>

namespace {

// The networks c_share_evm_networks() lists. Each RPC URL answered
// eth_chainId with its chain id, eth_getBlockByNumber with a base fee and
// eth_getBalance, without an API key or a User-Agent, when the list was
// made (2026-10-16, from the chain registry at chainid.network); rerun
// scripts/check_rpc_endpoints.py to check them again.

constexpr const char* k1[] = {"https://ethereum-rpc.publicnode.com", "https://rpc.mevblocker.io",
                              "https://rpc.flashbots.net/fast"};
constexpr const char* k11155111[] = {"https://rpc.sepolia.ethpandaops.io",
                                     "https://ethereum-sepolia-rpc.publicnode.com"};
constexpr const char* k560048[] = {"https://rpc.hoodi.ethpandaops.io"};
constexpr const char* k56[] = {"https://bsc-dataseed1.bnbchain.org", "https://bsc-rpc.publicnode.com"};
constexpr const char* k97[] = {"https://data-seed-prebsc-1-s1.bnbchain.org:8545",
                               "https://data-seed-prebsc-2-s2.bnbchain.org:8545"};
constexpr const char* k137[] = {"https://rpc-mainnet.matic.quiknode.pro",
                                "https://polygon-bor-rpc.blockreq.com/v1/rpc/public"};
constexpr const char* k80002[] = {"https://polygon-amoy-bor-rpc.publicnode.com",
                                  "https://polygon-amoy-rpc.blockreq.com/v1/rpc/public"};
constexpr const char* k42161[] = {"https://arb1.arbitrum.io/rpc", "https://arbitrum-one-rpc.publicnode.com"};
constexpr const char* k421614[] = {"https://sepolia-rollup.arbitrum.io/rpc",
                                   "https://arbitrum-sepolia-rpc.publicnode.com"};
constexpr const char* k42170[] = {"https://nova.arbitrum.io/rpc", "https://arbitrum-nova-rpc.publicnode.com"};
constexpr const char* k10[] = {"https://mainnet.optimism.io", "https://optimism-rpc.publicnode.com"};
constexpr const char* k11155420[] = {"https://sepolia.optimism.io",
                                     "https://optimism-sepolia-rpc.blockreq.com/v1/rpc/public"};
constexpr const char* k8453[] = {"https://mainnet.base.org", "https://base-rpc.publicnode.com"};
constexpr const char* k84532[] = {"https://sepolia.base.org", "https://base-sepolia-rpc.publicnode.com"};
constexpr const char* k43114[] = {"https://api.avax.network/ext/bc/C/rpc",
                                  "https://avalanche-c-chain-rpc.publicnode.com"};
constexpr const char* k43113[] = {"https://api.avax-test.network/ext/bc/C/rpc",
                                  "https://avalanche-fuji-c-chain-rpc.publicnode.com"};
constexpr const char* k100[] = {"https://rpc.gnosis.gateway.fm", "https://rpc.gnosischain.com"};
constexpr const char* k10200[] = {"https://rpc.chiadochain.net", "https://rpc.chiado.gnosis.gateway.fm"};
constexpr const char* k59144[] = {"https://rpc.linea.build", "https://linea-rpc.publicnode.com"};
constexpr const char* k59141[] = {"https://rpc.sepolia.linea.build", "https://linea-sepolia-rpc.publicnode.com"};
constexpr const char* k324[] = {"https://mainnet.era.zksync.io"};
constexpr const char* k300[] = {"https://sepolia.era.zksync.dev"};
constexpr const char* k534352[] = {"https://rpc.scroll.io", "https://scroll-rpc.publicnode.com"};
constexpr const char* k146[] = {"https://rpc.soniclabs.com", "https://sonic-rpc.publicnode.com"};
constexpr const char* k5000[] = {"https://rpc.mantle.xyz", "https://mantle-rpc.publicnode.com"};
constexpr const char* k5003[] = {"https://rpc.sepolia.mantle.xyz"};
constexpr const char* k81457[] = {"https://rpc.blast.io", "https://blast-rpc.publicnode.com"};
constexpr const char* k168587773[] = {"https://sepolia.blast.io"};
constexpr const char* k130[] = {"https://mainnet.unichain.org", "https://unichain-rpc.publicnode.com"};
constexpr const char* k1301[] = {"https://sepolia.unichain.org", "https://unichain-sepolia-rpc.publicnode.com"};
constexpr const char* k25[] = {"https://evm.cronos.org"};
constexpr const char* k338[] = {"https://evm-t3.cronos.org"};
constexpr const char* k42220[] = {"https://forno.celo.org"};
constexpr const char* k204[] = {"https://opbnb-mainnet-rpc.bnbchain.org"};
constexpr const char* k5611[] = {"https://opbnb-testnet-rpc.bnbchain.org"};
constexpr const char* k1284[] = {"https://moonbeam.api.onfinality.io/public"};
constexpr const char* k1287[] = {"https://moonbeam-alpha.api.onfinality.io/public"};
constexpr const char* k1285[] = {"https://moonriver.api.onfinality.io/public"};
constexpr const char* k80094[] = {"https://rpc.berachain-apis.com"};
constexpr const char* k80069[] = {"https://bepolia.rpc.berachain.com"};
constexpr const char* k7777777[] = {"https://rpc.zora.energy"};
constexpr const char* k999999999[] = {"https://sepolia.rpc.zora.energy"};
constexpr const char* k34443[] = {"https://mainnet.mode.network"};
constexpr const char* k919[] = {"https://sepolia.mode.network"};
constexpr const char* k252[] = {"https://rpc.frax.com"};
constexpr const char* k1329[] = {"https://evm-rpc.sei-apis.com"};
constexpr const char* k1328[] = {"https://evm-rpc-testnet.sei-apis.com"};
constexpr const char* k8217[] = {"https://public-en.node.kaia.io"};
constexpr const char* k1001[] = {"https://public-en-kairos.node.kaia.io"};
constexpr const char* k480[] = {"https://worldchain-mainnet.g.alchemy.com/public"};
constexpr const char* k4801[] = {"https://worldchain-sepolia.g.alchemy.com/public"};
constexpr const char* k57073[] = {"https://rpc-qnd.inkonchain.com"};
constexpr const char* k763373[] = {"https://rpc-gel-sepolia.inkonchain.com"};
constexpr const char* k2020[] = {"https://api.roninchain.com/rpc"};
constexpr const char* k13371[] = {"https://rpc.immutable.com"};
constexpr const char* k33139[] = {"https://rpc.apechain.com"};
constexpr const char* k33111[] = {"https://rpc.curtis.apechain.com"};
constexpr const char* k2741[] = {"https://api.mainnet.abs.xyz"};
constexpr const char* k11124[] = {"https://api.testnet.abs.xyz"};
constexpr const char* k1868[] = {"https://rpc.soneium.org"};
constexpr const char* k1946[] = {"https://rpc.minato.soneium.org"};
constexpr const char* k169[] = {"https://pacific-rpc.manta.network/http"};
constexpr const char* k3441006[] = {"https://pacific-rpc.sepolia-testnet.manta.network/http"};
constexpr const char* k7000[] = {"https://zetachain-evm.blockpi.network/v1/rpc/public"};
constexpr const char* k7001[] = {"https://zetachain-athens-evm.blockpi.network/v1/rpc/public"};
constexpr const char* k1030[] = {"https://evm.confluxrpc.com"};
constexpr const char* k71[] = {"https://evmtestnet.confluxrpc.com"};
constexpr const char* k1116[] = {"https://rpc.coredao.org"};
constexpr const char* k1135[] = {"https://rpc.api.lisk.com"};
constexpr const char* k747[] = {"https://mainnet.evm.nodes.onflow.org"};
constexpr const char* k545[] = {"https://testnet.evm.nodes.onflow.org"};
constexpr const char* k288[] = {"https://mainnet.boba.network"};
constexpr const char* k1514[] = {"https://mainnet.datarpc.io"};
constexpr const char* k1315[] = {"https://aeneid.datarpc.io"};

constexpr c_share_evm_network_t kNetworks[] = {
    {1ULL, "Ethereum", "ETH", 0, "https://etherscan.io", k1, std::size(k1)},
    {11155111ULL, "Ethereum Sepolia", "ETH", 1, "https://sepolia.etherscan.io", k11155111, std::size(k11155111)},
    {560048ULL, "Ethereum Hoodi", "ETH", 1, "https://light-hoodi.beaconcha.in", k560048, std::size(k560048)},
    {56ULL, "BNB Smart Chain", "BNB", 0, "https://bscscan.com", k56, std::size(k56)},
    {97ULL, "BNB Smart Chain Testnet", "tBNB", 1, "https://testnet.bscscan.com", k97, std::size(k97)},
    {137ULL, "Polygon", "POL", 0, "https://polygonscan.com", k137, std::size(k137)},
    {80002ULL, "Polygon Amoy", "POL", 1, "https://amoy.polygonscan.com", k80002, std::size(k80002)},
    {42161ULL, "Arbitrum One", "ETH", 0, "https://arbiscan.io", k42161, std::size(k42161)},
    {421614ULL, "Arbitrum Sepolia", "ETH", 1, "https://sepolia.arbiscan.io", k421614, std::size(k421614)},
    {42170ULL, "Arbitrum Nova", "ETH", 0, "https://nova-explorer.arbitrum.io", k42170, std::size(k42170)},
    {10ULL, "OP Mainnet", "ETH", 0, "https://optimistic.etherscan.io", k10, std::size(k10)},
    {11155420ULL, "OP Sepolia", "ETH", 1, "https://sepolia-optimism.etherscan.io", k11155420, std::size(k11155420)},
    {8453ULL, "Base", "ETH", 0, "https://basescan.org", k8453, std::size(k8453)},
    {84532ULL, "Base Sepolia", "ETH", 1, "https://sepolia.basescan.org", k84532, std::size(k84532)},
    {43114ULL, "Avalanche C-Chain", "AVAX", 0, "https://snowscan.xyz", k43114, std::size(k43114)},
    {43113ULL, "Avalanche Fuji", "AVAX", 1, "https://testnet.snowscan.xyz", k43113, std::size(k43113)},
    {100ULL, "Gnosis", "XDAI", 0, "https://gnosisscan.io", k100, std::size(k100)},
    {10200ULL, "Gnosis Chiado", "XDAI", 1, "https://gnosis-chiado.blockscout.com", k10200, std::size(k10200)},
    {59144ULL, "Linea", "ETH", 0, "https://lineascan.build", k59144, std::size(k59144)},
    {59141ULL, "Linea Sepolia", "ETH", 1, "https://sepolia.lineascan.build", k59141, std::size(k59141)},
    {324ULL, "ZKsync Era", "ETH", 0, "https://explorer.zksync.io", k324, std::size(k324)},
    {300ULL, "ZKsync Era Sepolia", "ETH", 1, "https://sepolia.explorer.zksync.io", k300, std::size(k300)},
    {534352ULL, "Scroll", "ETH", 0, "https://scrollscan.com", k534352, std::size(k534352)},
    {146ULL, "Sonic", "S", 0, "https://sonicscan.org", k146, std::size(k146)},
    {5000ULL, "Mantle", "MNT", 0, "https://mantlescan.xyz", k5000, std::size(k5000)},
    {5003ULL, "Mantle Sepolia", "MNT", 1, "https://explorer.sepolia.mantle.xyz", k5003, std::size(k5003)},
    {81457ULL, "Blast", "ETH", 0, "https://blastscan.io", k81457, std::size(k81457)},
    {168587773ULL, "Blast Sepolia", "ETH", 1, "https://testnet.blastscan.io", k168587773, std::size(k168587773)},
    {130ULL, "Unichain", "ETH", 0, "https://uniscan.xyz", k130, std::size(k130)},
    {1301ULL, "Unichain Sepolia", "ETH", 1, "https://unichain-sepolia.blockscout.com", k1301, std::size(k1301)},
    {25ULL, "Cronos", "CRO", 0, "https://explorer.cronos.org", k25, std::size(k25)},
    {338ULL, "Cronos Testnet", "TCRO", 1, "https://explorer.cronos.org/testnet", k338, std::size(k338)},
    {42220ULL, "Celo", "CELO", 0, "https://celoscan.io", k42220, std::size(k42220)},
    {204ULL, "opBNB", "BNB", 0, "https://opbnbscan.com", k204, std::size(k204)},
    {5611ULL, "opBNB Testnet", "tBNB", 1, "https://opbnb-testnet.bscscan.com", k5611, std::size(k5611)},
    {1284ULL, "Moonbeam", "GLMR", 0, "https://moonbeam.subscan.io", k1284, std::size(k1284)},
    {1287ULL, "Moonbase Alpha", "DEV", 1, "https://moonbase.subscan.io", k1287, std::size(k1287)},
    {1285ULL, "Moonriver", "MOVR", 0, "https://moonriver.subscan.io", k1285, std::size(k1285)},
    {80094ULL, "Berachain", "BERA", 0, "https://berascan.com", k80094, std::size(k80094)},
    {80069ULL, "Berachain Bepolia", "BERA", 1, "https://testnet.berascan.com", k80069, std::size(k80069)},
    {7777777ULL, "Zora", "ETH", 0, "https://explorer.zora.energy", k7777777, std::size(k7777777)},
    {999999999ULL, "Zora Sepolia", "ETH", 1, "https://sepolia.explorer.zora.energy", k999999999, std::size(k999999999)},
    {34443ULL, "Mode", "ETH", 0, "https://explorer.mode.network", k34443, std::size(k34443)},
    {919ULL, "Mode Sepolia", "ETH", 1, "https://sepolia.explorer.mode.network", k919, std::size(k919)},
    {252ULL, "Fraxtal", "FRAX", 0, "https://fraxscan.com", k252, std::size(k252)},
    {1329ULL, "Sei", "SEI", 0, "https://seiscan.io", k1329, std::size(k1329)},
    {1328ULL, "Sei Testnet", "SEI", 1, "https://testnet.seiscan.io", k1328, std::size(k1328)},
    {8217ULL, "Kaia", "KAIA", 0, "https://kaiascan.io", k8217, std::size(k8217)},
    {1001ULL, "Kaia Kairos", "KAIA", 1, "https://kairos.kaiascan.io", k1001, std::size(k1001)},
    {480ULL, "World Chain", "ETH", 0, "https://worldscan.org", k480, std::size(k480)},
    {4801ULL, "World Chain Sepolia", "ETH", 1, "https://sepolia.worldscan.org", k4801, std::size(k4801)},
    {57073ULL, "Ink", "ETH", 0, "https://explorer.inkonchain.com", k57073, std::size(k57073)},
    {763373ULL, "Ink Sepolia", "ETH", 1, "https://explorer-sepolia.inkonchain.com", k763373, std::size(k763373)},
    {2020ULL, "Ronin", "RON", 0, "https://app.roninchain.com", k2020, std::size(k2020)},
    {13371ULL, "Immutable zkEVM", "IMX", 0, "https://explorer.immutable.com", k13371, std::size(k13371)},
    {33139ULL, "ApeChain", "APE", 0, "https://apescan.io", k33139, std::size(k33139)},
    {33111ULL, "ApeChain Curtis", "APE", 1, "https://explorer.curtis.apechain.com", k33111, std::size(k33111)},
    {2741ULL, "Abstract", "ETH", 0, "https://abscan.org", k2741, std::size(k2741)},
    {11124ULL, "Abstract Sepolia", "ETH", 1, "https://sepolia.abscan.org", k11124, std::size(k11124)},
    {1868ULL, "Soneium", "ETH", 0, "https://soneium.blockscout.com", k1868, std::size(k1868)},
    {1946ULL, "Soneium Minato", "ETH", 1, "https://soneium-minato.blockscout.com", k1946, std::size(k1946)},
    {169ULL, "Manta Pacific", "ETH", 0, "https://pacific-explorer.manta.network", k169, std::size(k169)},
    {3441006ULL, "Manta Pacific Sepolia", "ETH", 1, "https://pacific-explorer.sepolia-testnet.manta.network", k3441006,
     std::size(k3441006)},
    {7000ULL, "ZetaChain", "ZETA", 0, "https://zetascan.com", k7000, std::size(k7000)},
    {7001ULL, "ZetaChain Athens", "ZETA", 1, "https://testnet.zetascan.com", k7001, std::size(k7001)},
    {1030ULL, "Conflux eSpace", "CFX", 0, "https://evm.confluxscan.net", k1030, std::size(k1030)},
    {71ULL, "Conflux eSpace Testnet", "CFX", 1, "https://evmtestnet.confluxscan.net", k71, std::size(k71)},
    {1116ULL, "Core", "CORE", 0, "https://scan.coredao.org", k1116, std::size(k1116)},
    {1135ULL, "Lisk", "ETH", 0, "https://blockscout.lisk.com", k1135, std::size(k1135)},
    {747ULL, "Flow EVM", "FLOW", 0, "https://evm.flowscan.io", k747, std::size(k747)},
    {545ULL, "Flow EVM Testnet", "FLOW", 1, "https://evm-testnet.flowscan.io", k545, std::size(k545)},
    {288ULL, "Boba Network", "ETH", 0, "https://bobascan.com", k288, std::size(k288)},
    {1514ULL, "Story", "DATA", 0, "https://datanetscan.io", k1514, std::size(k1514)},
    {1315ULL, "Story Aeneid", "DATA", 1, "https://aeneid.datanetscan.io", k1315, std::size(k1315)},
};

} // namespace

extern "C" {

const c_share_evm_network_t* c_share_evm_networks(size_t* out_count) {
    if (out_count) *out_count = std::size(kNetworks);
    return kNetworks;
}

const c_share_evm_network_t* c_share_evm_network_find(const uint64_t chain_id) {
    const auto* const found =
        std::find_if(std::begin(kNetworks), std::end(kNetworks),
                     [chain_id](const c_share_evm_network_t& n) { return n.chain_id == chain_id; });
    return found == std::end(kNetworks) ? nullptr : found;
}

} // extern "C"

namespace c_share::evm {

Config config_for(const c_share_evm_network_t& network, const std::size_t rpc_index) {
    Config config;
    config.url = network.rpc_urls[std::min(rpc_index, network.rpc_urls_len - 1)];
    config.chain_id = network.chain_id;
    return config;
}

} // namespace c_share::evm
