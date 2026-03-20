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
#include <atomic>
#include <memory>
#include <unordered_set>

using json = nlohmann::json;
namespace fs = std::filesystem;




ThreadPool pool;

// SSE: map of session_id sink (written under sinkMutex)
std::mutex sinkMutex;
std::unordered_map<std::string, httplib::DataSink*> sinks;

static const std::string MODEL_ID = "anthropic.claude-3-sonnet-20240229-v1:0";

// ── Helpers ───────────────────────────────────────────────────────────────────

// Read the system prompt from disk once

static std::string loadPromptFromDisk() {
    using namespace std;
    static string prompt = []() {
        for (const char* path : {
            "../AWS_Bedrock_Prompt/Prompt.txt",
            "AWS_Bedrock_Prompt/Prompt.txt",
            "../../AWS_Bedrock_Prompt/Prompt.txt"
        }) {
            ifstream f(path);
            if (f.is_open()) {
                ostringstream ss;
                ss << f.rdbuf();
                string content = ss.str();
                if (!content.empty()) {
                    cerr << "[DEBUG] Loaded prompt from: " << path << " (" << content.size() << " bytes)\n";
                    return content;
                }
            }
        }
        cerr << "[WARN] Failed to load prompt — using empty prompt\n";
        return string{};
    }();
    return prompt;
}

// Read a file's contents into a string
static std::string readFile(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Collect all regular files under a directory, skipping .git internals and binary files
static std::vector<fs::path> collectFiles(const fs::path& dir) {
    std::vector<fs::path> files;
    static const std::unordered_set<std::string> skipExts = {
        ".pack", ".idx", ".rev", ".bin", ".exe", ".so", ".dylib", ".o", ".a"
    };
    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        // Skip .git directory
        auto rel = fs::relative(entry.path(), dir);
        if (rel.begin() != rel.end() && rel.begin()->string() == ".git") continue;
        // Skip known binary extensions
        std::string ext = entry.path().extension().string();
        if (skipExts.count(ext)) continue;
        files.push_back(entry.path());
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

    std::string bodyStr;
    try {
        bodyStr = requestBody.dump();
    } catch (...) {
        return R"({"error": "Failed to serialize request — file may contain binary content"})";
    }
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
    std::cerr << "[DEBUG] POST /debug received from " << req.remote_addr << "\n";
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
    std::cerr << "[DEBUG] Cloning " << url << " into " << cloneDir << "\n";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "[DEBUG] Clone failed with exit code " << rc << "\n";
        res.status = 500; res.set_content("Clone failed", "text/plain"); return;
    }
    std::cerr << "[DEBUG] Clone succeeded\n";

    auto files = collectFiles(cloneDir);
    std::cerr << "[DEBUG] Found " << files.size() << " files to analyze\n";
    std::string prompt = loadPromptFromDisk();
    std::cerr << "[DEBUG] Prompt loaded (" << prompt.size() << " bytes)\n";

    // Shared counter — last task to finish cleans up the cloned directory
    auto pending = std::make_shared<std::atomic<int>>((int)files.size());

    for (auto& filePath : files) {
        std::string content = readFile(filePath);
        std::string filename = filePath.filename().string();

        pool.enqueue([sessionId, filename, content, prompt, cloneDir, pending]() {
            std::cerr << "[DEBUG] Processing file: " << filename << "\n";
            Aws::SDKOptions options;
            Aws::InitAPI(options);
            {
                Aws::BedrockRuntime::BedrockRuntimeClient client;
                std::cerr << "[DEBUG] Calling Bedrock for: " << filename << "\n";
                std::string result = callBedrock(client, prompt, filename, content);
                std::cerr << "[DEBUG] Bedrock response for " << filename << " (" << result.size() << " bytes)\n";
                sendSSE(sessionId, result);
                std::cerr << "[DEBUG] SSE sent for: " << filename << "\n";
            }
            Aws::ShutdownAPI(options);

            // Last task cleans up the cloned repo
            if (--(*pending) == 0) {
                std::cerr << "[DEBUG] All files done, cleaning up " << cloneDir << "\n";
                std::error_code ec;
                fs::remove_all(cloneDir, ec);
                if (ec) {
                    std::cerr << "[CLEANUP] Failed to remove " << cloneDir << ": " << ec.message() << "\n";
                }
            }
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

        // 1. CORS check with the validator
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
