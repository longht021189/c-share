"""Checks every RPC URL in modules/evm/src/networks.cpp against its network.

    python3 scripts/check_rpc_endpoints.py

Each URL must answer, without an API key or a User-Agent (http::Client sends
none): eth_chainId with its network's chain id, eth_getBlockByNumber("latest")
with a block that has a baseFeePerGas (the evm module signs EIP-1559
transactions only), and eth_getBalance. Prints one line per URL and exits 1
if any failed: free endpoints come and go, so a failing one is due a
replacement from https://chainid.network/chains.json.
"""

import concurrent.futures
import http.client
import json
import os
import re
import sys
import time
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "modules", "evm", "src", "networks.cpp")


def endpoints():
    """(chain id, name, url) for every URL in networks.cpp."""
    text = open(SOURCE, encoding="utf-8").read()
    arrays = {name: re.findall(r'"([^"]+)"', body)
              for name, body in re.findall(r"constexpr const char\* (k\d+)\[\] = \{([^}]*)\};", text)}
    out = []
    for chain_id, name, array in re.findall(r'\{(\d+)ULL, "([^"]+)", "[^"]*", \d, "[^"]*", (k\d+), ', text):
        for url in arrays[array]:
            out.append((int(chain_id), name, url))
    return out


def call(url, method, params):
    parts = urllib.parse.urlsplit(url)
    connection = http.client.HTTPSConnection(parts.hostname, parts.port or 443, timeout=10)
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params})
    connection.request("POST", parts.path or "/", body, {"Content-Type": "application/json"})
    response = connection.getresponse()
    data = json.loads(response.read())
    connection.close()
    if "result" not in data:
        raise RuntimeError(f"{method}: {data.get('error', data)}")
    return data["result"]


def check(endpoint):
    chain_id, name, url = endpoint
    start = time.monotonic()
    try:
        got = int(call(url, "eth_chainId", []), 16)
        if got != chain_id:
            raise RuntimeError(f"chain id {got}")
        block = call(url, "eth_getBlockByNumber", ["latest", False])
        if not isinstance(block, dict) or "baseFeePerGas" not in block:
            raise RuntimeError("no baseFeePerGas")
        call(url, "eth_getBalance", ["0x" + "00" * 20, "latest"])
        return True, f"ok   {int((time.monotonic() - start) * 1000):5d} ms  {chain_id:>10}  {name}: {url}"
    except Exception as error:  # noqa: BLE001 - every failure is reported the same way
        return False, f"FAIL            {chain_id:>10}  {name}: {url} ({error})"


def main():
    all_endpoints = endpoints()
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        results = list(pool.map(check, all_endpoints))
    for _, line in results:
        print(line)
    failed = sum(1 for ok, _ in results if not ok)
    print(f"\n{len(results) - failed}/{len(results)} endpoints answered")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
