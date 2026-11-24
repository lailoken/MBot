#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mbot {

using nlohmann::json;

struct ChatMessage {
    std::string role;  // "user" | "assistant"
    std::string text;
    std::string jpeg;  // optional image (user messages only)
};

// One "mind" backend. Every call returns a JSON object matching `schema`.
class LlmBackend {
public:
    virtual ~LlmBackend() = default;
    virtual bool complete(const std::string& system, const std::vector<ChatMessage>& msgs,
                          const json& schema, json& out, std::string* err) = 0;
    virtual std::string describe() const = 0;
    virtual bool supportsImages() const { return true; }
    // Who answered the last call and what it cost (0 for local models).
    virtual std::string lastUsed() const { return describe(); }
    virtual double lastCost() const { return 0; }
    virtual bool lastWasPaid() const { return false; }
};

// Claude via the Anthropic Messages API (raw HTTPS; there is no official C++ SDK).
// Default model: claude-sonnet-5-5 (claude-haiku-5-5 is used for cheap background turns).
// Credentials: ANTHROPIC_API_KEY, or ANTHROPIC_AUTH_TOKEN (OAuth bearer).
// effort: low | medium | high | xhigh | max  (low keeps conversation snappy)
std::unique_ptr<LlmBackend> makeClaudeBackend(const std::string& model, const std::string& effort);
// The local (Ollama) model used when none is given: $MBOT_LOCAL_MODEL, else qwen2.5:7b (Apache-2.0).
std::string defaultLocalModel();
// Local model for memory consolidation when Claude isn't available: a strong one (quality matters more
// than speed here). $MBOT_REFLECT_LOCAL_MODEL, else qwen2.5:14b (Apache-2.0).
std::string reflectLocalModel();
// Mind for memory consolidation and dreams: "claude-..." = careful Claude (adaptive thinking, high effort)
// falling back to the local model; anything else = that local Ollama model.
std::unique_ptr<LlmBackend> makeReflectionBackend(const std::string& model);

// Any OpenAI-compatible /v1/chat/completions server on your LAN:
// Ollama http://localhost:11434/v1, LM Studio http://localhost:1234/v1, llama.cpp ...
std::unique_ptr<LlmBackend> makeOpenAIBackend(const std::string& base_url,
                                              const std::string& model);

// "claude[:model]" or an http(s):// base URL (+ model). Empty spec picks Claude
// when credentials exist, else local Ollama.
std::unique_ptr<LlmBackend> makeBackend(const std::string& spec, const std::string& model,
                                        const std::string& effort, std::string* err);

// Claude spending tracker, persisted per day. A daily budget (USD) makes the
// fallback wrapper switch to the local mind once it is used up.
struct Usage {
    std::string date;  // YYYY-MM-DD (local)
    int calls = 0;
    long input = 0, output = 0, cache_read = 0, cache_write = 0;
    double usd = 0;
};
void setUsageFile(const std::string& path);  // e.g. <mind dir>/usage.json; also writes
                                             // claude_calls.log next to it
void setCallReason(const std::string& why);  // context written to claude_calls.log
void setColorOutput(bool on);                // ANSI highlight for the cost banner
void setDailyBudget(double usd);             // <= 0 = unlimited
Usage usageToday();
bool overBudget();

// True when `spec`/environment would select Claude.
bool wantsClaude(const std::string& spec);

std::string base64(const std::string& bytes);

// While set (on this thread), Claude calls stream: the callback gets the reply text so far as it is written.
// The brain uses it to start speaking a reply before the whole reply (actions, memories...) is done.
// Return false from it to abandon the call: complete() then fails with err "superseded".
void setPartialReplyCallback(std::function<bool(const std::string&)> cb);

// Describe a JPEG with a LOCAL vision model (free; e.g. minicpm-v on Ollama). Returns a
// short text description, or "" with *err set. Keeps images off the paid/interactive mind.
std::string describeImage(const std::string& base_url, const std::string& model,
                          const std::string& jpeg, const std::string& prompt, std::string* err,
                          int timeout_sec = 60);

// Pull the first {...} JSON object out of free text (for models that ignore the format).
bool extractJsonObject(const std::string& text, json& out);

}  // namespace mbot
