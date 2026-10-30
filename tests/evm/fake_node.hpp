#pragma once

// A JSON-RPC node on loopback that each test scripts, and an http::Client
// whose loop runs on a thread of its own: the evm module only ever uses a
// running client it is given.

#include "c_share/evm/client.hpp"

#include "test_server.hpp"

#include <nlohmann/json.hpp>

// What this file uses, even where CLion finds it through other headers.
// ReSharper disable CppUnusedIncludeDirective
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
// ReSharper restore CppUnusedIncludeDirective

namespace c_share::test {

class RunningHttpClient {
public:
    RunningHttpClient() : loop_([this] { client_.run(); }) {}
    ~RunningHttpClient() {
        client_.stop();
        loop_.join();
    }
    RunningHttpClient(const RunningHttpClient&) = delete;
    RunningHttpClient& operator=(const RunningHttpClient&) = delete;
    RunningHttpClient(RunningHttpClient&&) = delete;
    RunningHttpClient& operator=(RunningHttpClient&&) = delete;

    http::Client& get() { return client_; }

private:
    http::Client client_;
    std::thread loop_;
};

struct RpcRequest {
    std::string method;
    nlohmann::json params;
};

// Answers each JSON-RPC request with what `answer` returns for it: the
// response's "result" or "error" member, e.g. {{"result", "0x1"}}. The
// requests are kept, in order.
class FakeNode {
public:
    using Answer = std::function<nlohmann::json(const RpcRequest&)>;

    explicit FakeNode(Answer answer)
        : answer_(std::move(answer)), server_(TestServer::http([this](const HttpRequest& request) {
              const auto body = nlohmann::json::parse(request.body, nullptr, false);
              RpcRequest rpc{body.value("method", std::string()), body.value("params", nlohmann::json::array())};
              nlohmann::json response = answer_(rpc);
              {
                  std::scoped_lock lock(mutex_);
                  requests_.push_back(std::move(rpc));
              }
              response["jsonrpc"] = "2.0";
              response["id"] = body.value("id", nlohmann::json());
              return http_response(200, response.dump(), {{"Content-Type", "application/json"}});
          })) {}

    [[nodiscard]] std::string url() const { return server_.url("/rpc"); }

    std::vector<RpcRequest> requests() {
        std::scoped_lock lock(mutex_);
        return requests_;
    }

    std::vector<std::string> methods() {
        std::vector<std::string> out;
        for (const auto& [method, params] : requests())
            out.push_back(method);
        return out;
    }

private:
    Answer answer_;
    std::mutex mutex_;
    std::vector<RpcRequest> requests_;
    TestServer server_;
};

// One operation's outcome, waited for.
struct Outcome {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    evm::Error error;
    std::string result;
    int calls = 0;

    static void on_result(void* context, const evm::Error& error, const std::string& result) {
        auto* outcome = static_cast<Outcome*>(context);
        std::scoped_lock lock(outcome->mutex);
        outcome->done = true;
        outcome->error = error;
        outcome->result = result;
        ++outcome->calls;
        outcome->changed.notify_all();
    }

    bool wait() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10), [&] { return done; });
    }
};

} // namespace c_share::test
