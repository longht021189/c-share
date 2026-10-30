#include "c_share/evm/client.hpp"
#include "c_share/evm/networks.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

// The table itself; whether its endpoints still answer is
// scripts/check_rpc_endpoints.py's to say (these tests stay off the network).
TEST_CASE("the network list is well-formed", "[evm][networks]") {
    std::size_t count = 0;
    const c_share_evm_network_t* networks = c_share_evm_networks(&count);
    REQUIRE(networks);
    REQUIRE(count > 0);
    std::set<std::uint64_t> chain_ids;
    std::set<std::string> urls;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& network = networks[i];
        CAPTURE(network.name);
        CHECK(chain_ids.insert(network.chain_id).second);
        CHECK_FALSE(std::string(network.name).empty());
        CHECK_FALSE(std::string(network.symbol).empty());
        CHECK((network.testnet == 0 || network.testnet == 1));
        REQUIRE(network.rpc_urls_len > 0);
        for (std::size_t u = 0; u < network.rpc_urls_len; ++u) {
            const std::string url = network.rpc_urls[u];
            CAPTURE(url);
            CHECK(url.rfind("https://", 0) == 0);
            CHECK(urls.insert(url).second);
        }
    }
    CHECK(urls.size() == 100);
}

TEST_CASE("networks are found by chain id, and make configs", "[evm][networks]") {
    for (const std::uint64_t chain_id : {1ULL, 11155111ULL, 56ULL, 137ULL, 42161ULL, 10ULL, 8453ULL, 43114ULL}) {
        CAPTURE(chain_id);
        REQUIRE(c_share_evm_network_find(chain_id));
    }
    CHECK(c_share_evm_network_find(0) == nullptr);

    const auto* base = c_share_evm_network_find(8453);
    REQUIRE(base);
    CHECK(std::string(base->name) == "Base");
    CHECK(std::string(base->symbol) == "ETH");
    CHECK(base->testnet == 0);
    CHECK(c_share_evm_network_find(84532)->testnet == 1);

    const auto config = c_share::evm::config_for(*base);
    CHECK(config.url == base->rpc_urls[0]);
    CHECK(config.chain_id == 8453U);
    // Past the end: the last one.
    CHECK(c_share::evm::config_for(*base, 99).url == base->rpc_urls[base->rpc_urls_len - 1]);
}
