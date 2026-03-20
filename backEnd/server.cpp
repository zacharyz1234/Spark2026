#include "httplib.h"
#include "nlohmann/json.hpp"
#include "threadpool.hpp"
#include "classes.hpp"
#include "cors_validator.hpp"
#include "rate_limiter.hpp"

#include <aws/core/Aws.h>
#include <aws/bedrock-runtime/BedrockRuntimeClient.h>
#include <aws/bedrock-runtime/model/InvokeModelRequest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── Globals ──────────────────────────────────────────────────────────────────

ThreadPool pool;

// SSE: map of session_id → sink (written under sinkMutex)
std::mutex sinkMutex;
std::unordered_map<std::string, httplib::DataSink*> sinks;

static const std::string MODEL_ID = "anthropic.claude-3-sonnet-20240229-v1:0";

// ── Helpers ───────────────────────────────────────────────────────────────────

// Read the system prompt from disk once
static std::string loadPrompt() {
    std::ifstream f("../AWS_Bedrock_Prompt/Prompt.txt");
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Read a file's contents into a string
static std::string readFile(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Collect all regular files under a directory
static std::vector<fs::path> collectFiles(const fs::path& dir) {
    std::vector<fs::path> files;
    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    return files;
}

// Send an SSE event to a session
static void sendSSE(const std::string& sessionId, const std::string& data) {
    std::lock_guard<std::mutex> lock(sinkMutex);
    auto it = sinks.find(sessionId);
    if (it == sinks.end()) return;
    std::string msg = "data: " + data + "\n\n";
    it->second->write(msg.c_str(), msg.size());
}

// Call Bedrock Claude 3 Sonnet with the file content
static std::string callBedrock(
    Aws::BedrockRuntime::BedrockRuntimeClient& client,
    const std::string& systemPrompt,
    const std::string& filename,
    const std::string& fileContent)
{
    json requestBody = {
        {"anthropic_version", "bedrock-2023-05-31"},
        {"max_tokens", 2048},
        {"system", systemPrompt},
        {"messages", json::array({
            {{"role", "user"}, {"content", "File: " + filename + "\n\n" + fileContent}}
        })}
    };

    auto bodyStr = requestBody.dump();
    auto bodyStream = Aws::MakeShared<Aws::StringStream>("BedrockRequest");
    *bodyStream << bodyStr;

    Aws::BedrockRuntime::Model::InvokeModelRequest req;
    req.SetModelId(MODEL_ID.c_str());
    req.SetContentType("application/json");
    req.SetBody(bodyStream);

    auto outcome = client.InvokeModel(req);
    if (!outcome.IsSuccess()) {
        return R"({"error": "Bedrock call failed"})";
    }

    auto& result = outcome.GetResult();
    std::ostringstream ss;
    ss << result.GetBody().rdbuf();
    auto responseJson = json::parse(ss.str());
    return responseJson["content"][0]["text"].get<std::string>();
}

// ── Route handlers ────────────────────────────────────────────────────────────

// POST /debug  { "url": "https://github.com/user/repo" }
void handleDebug(const httplib::Request& req, httplib::Response& res) {
    json body;
    try { body = json::parse(req.body); }
    catch (...) { res.status = 400; res.set_content("Invalid JSON", "text/plain"); return; }

    std::string url = body.value("url", "");
    std::string sessionId = body.value("session_id", "");
    if (url.empty() || sessionId.empty()) {
        res.status = 400; res.set_content("Missing url or session_id", "text/plain"); return;
    }

    // Optionally inject GitHub token for private repos
    std::string token = body.value("token", "");
    std::string cloneUrl = url;
    if (!token.empty()) {
        // Insert token: https://token@github.com/user/repo
        cloneUrl.insert(8, token + "@"); // after "https://"
    }

    // Clone into /tmp/<session_id>
    std::string cloneDir = "/tmp/" + sessionId;
    std::string cmd = "git clone --depth=1 " + cloneUrl + " " + cloneDir + " 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        res.status = 500; res.set_content("Clone failed", "text/plain"); return;
    }

    auto files = collectFiles(cloneDir);
    std::string prompt = loadPrompt();

    for (auto& filePath : files) {
        std::string content = readFile(filePath);
        std::string filename = filePath.filename().string();

        pool.enqueue([sessionId, filename, content, prompt]() {
            Aws::SDKOptions options;
            Aws::InitAPI(options);
            {
                Aws::BedrockRuntime::BedrockRuntimeClient client;
                std::string result = callBedrock(client, prompt, filename, content);
                sendSSE(sessionId, result);
            }
            Aws::ShutdownAPI(options);
        });
    }

    // Signal how many files were queued
    json resp = { {"queued", (int)files.size()} };
    res.set_content(resp.dump(), "application/json");
}

// GET /stream?session_id=<id>  — SSE stream
void handleStream(const httplib::Request& req, httplib::Response& res) {
    std::string sessionId = req.get_param_value("session_id");
    if (sessionId.empty()) { res.status = 400; return; }

    res.set_chunked_content_provider("text/event-stream",
        [sessionId](size_t, httplib::DataSink& sink) {
            {
                std::lock_guard<std::mutex> lock(sinkMutex);
                sinks[sessionId] = &sink;
            }
            // Keep alive until client disconnects
            while (sinks.count(sessionId)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return true;
        },
        [sessionId](bool) {
            std::lock_guard<std::mutex> lock(sinkMutex);
            sinks.erase(sessionId);
        }
    );
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    httplib::Server svr;

    CorsValidator corsValidator;
    RateLimiter rateLimiter({
        {"/debug",  {5,  60}},
        {"/stream", {30, 60}},
        {"/health", {60, 60}},
    });

    svr.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res)
        -> httplib::Server::HandlerResponse
    {
        std::string origin = req.get_header_value("Origin");

        // 1. CORS check
        auto corsResult = corsValidator.validate(origin);
        if (!corsResult.allowed) {
            res.status = 403;
            res.set_content("Forbidden", "text/plain");
            std::cerr << "[CORS] blocked " << req.remote_addr << " origin=" << origin << "\n";
            return httplib::Server::HandlerResponse::Handled;
        }
        if (!corsResult.originHeader.empty()) {
            res.set_header("Access-Control-Allow-Origin", corsResult.originHeader);
        }

        // 2. Handle OPTIONS preflight (after CORS passes)
        if (req.method == "OPTIONS") {
            res.status = 204;
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            return httplib::Server::HandlerResponse::Handled;
        }

        // 3. Rate limit check
        auto rlResult = rateLimiter.check(req.remote_addr, req.path);
        if (!rlResult.allowed) {
            res.status = 429;
            res.set_header("Retry-After", std::to_string(rlResult.retryAfter));
            res.set_content("Rate limit exceeded", "text/plain");
            std::cerr << "[RATE] blocked " << req.remote_addr << " path=" << req.path << "\n";
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    svr.Post("/debug", handleDebug);
    svr.Get("/stream", handleStream);
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("healthy", "text/plain");
    });

    svr.listen("0.0.0.0", 8080);

    Aws::ShutdownAPI(options);
    return 0;
}
