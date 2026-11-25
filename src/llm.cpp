#include "llm.hpp"

#include <openssl/evp.h>

#include <chrono>
#include <algorithm>
#include <ctime>
#include <mutex>
#include <cstdio>
#include <cstdlib>

#include "net.hpp"

namespace mbot {

static thread_local std::function<bool(const std::string&)> t_partial;
void setPartialReplyCallback(std::function<bool(const std::string&)> cb) { t_partial = std::move(cb); }

std::string base64(const std::string& bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            reinterpret_cast<const unsigned char*>(bytes.data()),
                            int(bytes.size()));
    out.resize(n);
    return out;
}

bool extractJsonObject(const std::string& text, json& out) {
    for (size_t start = text.find('{'); start != std::string::npos;
         start = text.find('{', start + 1)) {
        int depth = 0;
        bool in_str = false, esc = false;
        for (size_t i = start; i < text.size(); ++i) {
            char c = text[i];
            if (in_str) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') in_str = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                try {
                    out = json::parse(text.substr(start, i - start + 1));
                    return true;
                } catch (...) {
                    break;
                }
            }
        }
    }
    return false;
}

namespace {

// JSON objects here sort their keys, but a model writes a structured reply in the order the schema lists
// its properties. Dump a schema with each "properties" in the order of its "required" list (the order
// the code declares), so e.g. "thought" and "say" come first and speech can start early.
std::string dumpSchemaOrdered(const json& j) {
    if (j.is_array()) {
        std::string o = "[";
        for (size_t i = 0; i < j.size(); ++i) o += (i ? "," : "") + dumpSchemaOrdered(j[i]);
        return o + "]";
    }
    if (!j.is_object()) return j.dump();
    std::vector<std::string> req;
    if (j.contains("required") && j["required"].is_array())
        for (auto& r : j["required"]) if (r.is_string()) req.push_back(r.get<std::string>());
    std::string o = "{";
    bool first = true;
    for (auto& [k, v] : j.items()) {
        o += (first ? "" : ",") + json(k).dump() + ":";
        first = false;
        if (k == "properties" && v.is_object() && !req.empty()) {
            std::vector<std::string> keys;
            for (auto& r : req) if (v.contains(r)) keys.push_back(r);
            for (auto& [pk, pv] : v.items())
                if (std::find(keys.begin(), keys.end(), pk) == keys.end()) keys.push_back(pk);
            o += "{";
            for (size_t i = 0; i < keys.size(); ++i) o += (i ? "," : "") + json(keys[i]).dump() + ":" + dumpSchemaOrdered(v[keys[i]]);
            o += "}";
        } else {
            o += dumpSchemaOrdered(v);
        }
    }
    return o + "}";
}


const char* envOr(const char* k);

std::mutex g_usage_mu;
std::string g_usage_path;
std::string g_calls_log;
std::string g_reason;
bool g_color = false;
double g_budget = 0;
Usage g_usage;

std::string today() {
    std::time_t t = std::time(nullptr);
    char b[16];
    std::strftime(b, sizeof b, "%Y-%m-%d", std::localtime(&t));
    return b;
}

void rollDayLocked() {
    std::string d = today();
    if (g_usage.date != d) g_usage = Usage{d};
}

void saveUsageLocked() {
    if (g_usage_path.empty()) return;
    json j = {{"date", g_usage.date},          {"calls", g_usage.calls},
              {"input_tokens", g_usage.input}, {"output_tokens", g_usage.output},
              {"cache_read_tokens", g_usage.cache_read}, {"cache_write_tokens", g_usage.cache_write},
              {"usd", g_usage.usd},           {"daily_budget_usd", g_budget}};
    if (FILE* f = std::fopen(g_usage_path.c_str(), "w")) {
        std::fputs(j.dump(2).c_str(), f);
        std::fclose(f);
    }
    // Keep a history line per call for later analysis.
}

// USD per million tokens: input, output, cache read. Cache writes (1 hour) cost 2x input.
void prices(const std::string& model, double& in, double& out, double& cread) {
    if (model.rfind("claude-haiku-5-5", 0) == 0) { in = 0.10; out = 0.50; cread = 0.01; }
    else if (model.rfind("claude-sonnet", 0) == 0) { in = 2.0; out = 10.0; cread = 0.10; }
    else if (model.rfind("claude-opus-5-5", 0) == 0) { in = 4.0; out = 20.0; cread = 0.20; }
    else if (model.rfind("claude-fable-5-2", 0) == 0) { in = 8.0; out = 40.0; cread = 0.20; }
    else { in = 5.0; out = 25.0; cread = 0.50; }  // conservative default
}

double recordUsage(const std::string& model, const json& u) {
    long in = u.value("input_tokens", 0L), out = u.value("output_tokens", 0L);
    long cr = u.value("cache_read_input_tokens", 0L), cw = u.value("cache_creation_input_tokens", 0L);
    double pin, pout, pcr;
    prices(model, pin, pout, pcr);
    double usd = (in * pin + out * pout + cr * pcr + cw * pin * 2.0) / 1e6;  // 1h cache write = 2x input
    // Server tools: web searches are billed per search ($10 per 1000).
    if (u.contains("server_tool_use") && u["server_tool_use"].is_object())
        usd += u["server_tool_use"].value("web_search_requests", 0L) * 0.01;
    std::lock_guard<std::mutex> lk(g_usage_mu);
    rollDayLocked();
    g_usage.calls++;
    g_usage.input += in;
    g_usage.output += out;
    g_usage.cache_read += cr;
    g_usage.cache_write += cw;
    g_usage.usd += usd;
    saveUsageLocked();

    char budget[48] = "";
    if (g_budget > 0) std::snprintf(budget, sizeof budget, " of $%.2f budget", g_budget);
    // Loud, unmistakable banner for every paid call.
    const char* on = g_color ? "\033[38;5;16;48;5;214m" : "";  // black on orange, readable in any theme
    const char* off = g_color ? "\033[0m" : "";
    std::printf("%s $$$ CLAUDE API CALL $$$  %s  cost $%.4f  (in %ld, cached %ld, cache-write %ld, out %ld)"
                "  |  today $%.4f%s, %d call%s %s\n",
                on, model.c_str(), usd, in, cr, cw, out, g_usage.usd, budget, g_usage.calls,
                g_usage.calls == 1 ? "" : "s", off);

    if (!g_calls_log.empty()) {
        if (FILE* f = std::fopen(g_calls_log.c_str(), "a")) {
            std::time_t t = std::time(nullptr);
            char ts[32];
            std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            std::string why = g_reason.substr(0, 120);
            for (auto& c : why) if (c == '\n' || c == '\t') c = ' ';
            std::fprintf(f, "%s  %-18s $%.5f  in=%ld cached=%ld write=%ld out=%ld  day=$%.4f  why: %s\n", ts,
                         model.c_str(), usd, in, cr, cw, out, g_usage.usd, why.c_str());
            std::fclose(f);
        }
    }
    return usd;
}

const char* envOr(const char* k) {
    const char* v = std::getenv(k);
    return (v && *v) ? v : nullptr;
}

// ---------------------------------------------------------------- Claude ----
class ClaudeBackend : public LlmBackend {
public:
    // deep = slow, careful work (memory consolidation, dreams): adaptive thinking, more room to write.
    ClaudeBackend(std::string model, std::string effort, bool deep = false)
        : model_(std::move(model)), effort_(std::move(effort)), deep_(deep) {}

    std::string describe() const override {
        return "Claude " + model_ + " (effort " + effort_ + (deep_ ? ", thinking" : "") + ")";
    }
    std::string lastUsed() const override { return "CLAUDE " + model_; }
    double lastCost() const override { return last_cost_; }
    bool lastWasPaid() const override { return true; }

    bool complete(const std::string& system, const std::vector<ChatMessage>& msgs,
                  const json& schema, json& out, std::string* err) override {
        json jm = json::array();
        for (auto& m : msgs) {
            if (m.jpeg.empty()) {
                jm.push_back({{"role", m.role}, {"content", m.text}});
                continue;
            }
            jm.push_back({{"role", m.role},
                          {"content",
                           json::array({{{"type", "image"},
                                         {"source",
                                          {{"type", "base64"},
                                           {"media_type", "image/jpeg"},
                                           {"data", base64(m.jpeg)}}}},
                                        {{"type", "text"}, {"text", m.text}}})}});
        }
        json body = {
            {"model", model_},
            // Replies are short JSON; cap output so a runaway answer can't cost much.
            {"max_tokens", deep_ ? 8000 : 2000},
            // Stable persona prompt first, cached across turns.
            {"system", json::array({{{"type", "text"},
                                     {"text", system},
                                     // 1-hour cache: chats with a robot are spread out, and the
                                     // default 5-minute cache would be re-written on almost every call.
                                     {"cache_control", {{"type", "ephemeral"}, {"ttl", "1h"}}}}})},
            {"messages", jm},
        };
        bool streaming = bool(t_partial) && !deep_;
        if (streaming) body["stream"] = true;
        // Hidden "thinking" is billed as output tokens. A robot's quick reaction doesn't
        // need it: turn it off where the model allows (Sonnet 5.5: between_tools,
        // Haiku 5.5: disabled; both only at effort high or below - we use low).
        if (deep_) body["thinking"] = {{"type", "adaptive"}};  // consolidation is worth real thought
        else if (model_.rfind("claude-sonnet-5-5", 0) == 0) body["thinking"] = {{"type", "between_tools"}};
        else if (model_.rfind("claude-haiku-5-5", 0) == 0) body["thinking"] = {{"type", "disabled"}};
        // Server-side refusal fallback exists for Opus/Sonnet/Fable, not Haiku.
        bool fallback_ok = model_.rfind("claude-haiku", 0) != 0;
        if (fallback_ok) body["fallbacks"] = "default";

        std::vector<std::pair<std::string, std::string>> headers = {
            {"anthropic-version", "2026-10-21"},
            {"anthropic-beta", fallback_ok ? "server-side-fallback-2026-07-01" : ""},
        };
        if (const char* key = envOr("ANTHROPIC_API_KEY")) {
            headers.push_back({"x-api-key", key});
        } else if (const char* tok = envOr("ANTHROPIC_AUTH_TOKEN")) {
            headers.push_back({"Authorization", std::string("Bearer ") + tok});
            headers[1].second += headers[1].second.empty() ? "oauth-2025-04-20" : ",oauth-2025-04-20";
        } else {
            if (err) *err = "no ANTHROPIC_API_KEY / ANTHROPIC_AUTH_TOKEN in the environment";
            return false;
        }
        std::string base = envOr("ANTHROPIC_BASE_URL") ? envOr("ANTHROPIC_BASE_URL")
                                                        : "https://api.anthropic.com";

        if (headers[1].second.empty()) headers.erase(headers.begin() + 1);
        // output_config goes in by hand: the schema must keep its property order (see dumpSchemaOrdered).
        std::string payload = body.dump();
        payload.pop_back();
        payload += ",\"output_config\":{\"effort\":" + json(effort_).dump() +
                   ",\"format\":{\"type\":\"json_schema\",\"schema\":" + dumpSchemaOrdered(schema) + "}}}";
        if (streaming) return completeStreamed(base, payload, headers, out, err);
        auto r = httpPost(base + "/v1/messages", payload, headers, deep_ ? 300 : 180);
        if (!r.error.empty()) {
            if (err) *err = r.error;
            return false;
        }
        json resp;
        try {
            resp = json::parse(r.body);
        } catch (...) {
            if (err) *err = "HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, 300);
            return false;
        }
        if (r.status != 200) {
            if (err) *err = "HTTP " + std::to_string(r.status) + ": " +
                            resp.value("error", json::object()).value("message", r.body.substr(0, 300));
            return false;
        }
        if (resp.contains("usage")) last_cost_ = recordUsage(resp.value("model", model_), resp["usage"]);
        std::string stop = resp.value("stop_reason", "");
        if (stop == "refusal") {
            if (err) *err = "model declined this turn";
            return false;
        }
        for (auto& block : resp.value("content", json::array())) {
            if (block.value("type", "") != "text") continue;
            std::string text = block.value("text", "");
            try {
                out = json::parse(text);
                return true;
            } catch (...) {
                if (extractJsonObject(text, out)) return true;
            }
        }
        if (err) *err = "no JSON in reply (stop_reason " + stop + ")";
        return false;
    }

private:
    // Server-sent events: text deltas go to the partial callback as they arrive.
    bool completeStreamed(const std::string& base, const std::string& payload,
                          const std::vector<std::pair<std::string, std::string>>& headers, json& out, std::string* err) {
        std::string pend, text, stop, model = model_, sse_err;
        json usage = json::object();
        bool superseded = false;
        auto on_body = [&](const std::string& piece) {
            pend += piece;
            size_t nl;
            while ((nl = pend.find('\n')) != std::string::npos) {
                std::string line = pend.substr(0, nl);
                pend.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("data:", 0) != 0) continue;
                json ev;
                try { ev = json::parse(line.substr(5)); } catch (...) { continue; }
                std::string type = ev.value("type", "");
                if (type == "message_start") {
                    auto m = ev.value("message", json::object());
                    model = m.value("model", model_);
                    json u = m.value("usage", json::object());  // named: items() on a temporary dangles
                    for (auto& [k, v] : u.items()) if (v.is_number()) usage[k] = v;
                } else if (type == "content_block_delta") {
                    auto d = ev.value("delta", json::object());
                    if (d.value("type", "") == "text_delta") {
                        text += d.value("text", "");
                        if (t_partial && !t_partial(text)) { superseded = true; return false; }
                    }
                } else if (type == "message_delta") {
                    stop = ev.value("delta", json::object()).value("stop_reason", stop);
                    json u = ev.value("usage", json::object());
                    for (auto& [k, v] : u.items()) if (v.is_number()) usage[k] = v;
                } else if (type == "error") {
                    sse_err = ev.value("error", json::object()).value("message", "stream error");
                }
            }
            return true;
        };
        auto r = httpPostStream(base + "/v1/messages", payload, headers, 180, on_body);
        if (superseded) {  // still billed for what was read and written so far
            if (!usage.empty()) last_cost_ = recordUsage(model, usage);
            if (err) *err = "superseded";
            return false;
        }
        if (!r.error.empty()) { if (err) *err = r.error; return false; }
        if (r.status != 200) {
            std::string msg = r.body.substr(0, 300);
            try { msg = json::parse(r.body).value("error", json::object()).value("message", msg); } catch (...) {}
            if (err) *err = "HTTP " + std::to_string(r.status) + ": " + msg;
            return false;
        }
        if (!sse_err.empty()) { if (err) *err = sse_err; return false; }
        if (!usage.empty()) last_cost_ = recordUsage(model, usage);
        if (stop == "refusal") { if (err) *err = "model declined this turn"; return false; }
        try { out = json::parse(text); return true; } catch (...) {}
        if (extractJsonObject(text, out)) return true;
        if (err) *err = "no JSON in streamed reply (stop_reason " + stop + ")";
        return false;
    }

    std::string model_, effort_;
    bool deep_ = false;
    double last_cost_ = 0;
};

// ------------------------------------------------------ OpenAI-compatible ----
class OpenAIBackend : public LlmBackend {
public:
    OpenAIBackend(std::string base, std::string model) : base_(std::move(base)), model_(std::move(model)) {
        while (!base_.empty() && base_.back() == '/') base_.pop_back();
    }
    std::string describe() const override { return model_ + " @ " + base_; }
    std::string lastUsed() const override { return "LOCAL " + model_; }
    // Small local vision models (e.g. qwen2.5vl in Ollama) crash on Vector's camera
    // frames (GGML assert). Only Claude gets images; local turns run text-only.
    bool supportsImages() const override { return false; }

    bool complete(const std::string& system, const std::vector<ChatMessage>& msgs,
                  const json& schema, json& out, std::string* err) override {
        json jm = json::array({{{"role", "system"}, {"content", system}}});
        for (auto& m : msgs) {
            if (m.jpeg.empty()) {
                jm.push_back({{"role", m.role}, {"content", m.text}});
                continue;
            }
            jm.push_back(
                {{"role", m.role},
                 {"content",
                  json::array({{{"type", "text"}, {"text", m.text}},
                               {{"type", "image_url"},
                                {"image_url",
                                 {{"url", "data:image/jpeg;base64," + base64(m.jpeg)}}}}})}});
        }
        json body = {{"model", model_},
                     {"messages", jm},
                     {"stream", false},
                     {"response_format",
                      {{"type", "json_schema"},
                       {"json_schema", {{"name", "reply"}, {"strict", true}, {"schema", schema}}}}}};
        std::vector<std::pair<std::string, std::string>> headers;
        if (const char* key = envOr("OPENAI_API_KEY"))
            headers.push_back({"Authorization", std::string("Bearer ") + key});

        auto r = httpPost(base_ + "/chat/completions", body.dump(), headers, 180);
        if (!r.ok()) {
            if (err) *err = r.error.empty() ? "HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, 300) : r.error;
            return false;
        }
        try {
            auto j = json::parse(r.body);
            std::string text = j.at("choices").at(0).at("message").at("content").get<std::string>();
            try {
                out = json::parse(text);
                return true;
            } catch (...) {
                if (extractJsonObject(text, out)) return true;
            }
            if (err) *err = "no JSON in reply: " + text.substr(0, 200);
        } catch (const std::exception& e) {
            if (err) *err = std::string("bad response: ") + e.what();
        }
        return false;
    }

private:
    std::string base_, model_;
};

// Primary mind with a local fallback: if the primary fails (bad key, no internet,
// outage), the robot keeps thinking with the local model instead of going mute.
class FallbackBackend : public LlmBackend {
public:
    FallbackBackend(std::unique_ptr<LlmBackend> primary, std::unique_ptr<LlmBackend> fallback)
        : primary_(std::move(primary)), fallback_(std::move(fallback)) {}
    std::string describe() const override {
        return primary_->describe() + " (falls back to " + fallback_->describe() + ")";
    }
    bool complete(const std::string& system, const std::vector<ChatMessage>& msgs,
                  const json& schema, json& out, std::string* err) override {
        auto now = std::chrono::steady_clock::now();
        if (overBudget()) {
            if (!budget_noted_) {
                std::fprintf(stderr, "[mind] today's Claude budget is used up ($%.3f); using %s until midnight\n",
                             usageToday().usd, fallback_->describe().c_str());
                budget_noted_ = true;
            }
            last_ = fallback_.get();
            return fallback_->complete(system, msgs, schema, out, err);
        }
        budget_noted_ = false;
        if (now >= primary_retry_at_) {
            std::string e;
            last_ = primary_.get();
            if (primary_->complete(system, msgs, schema, out, &e)) {
                if (degraded_) std::fprintf(stderr, "[mind] %s is working again\n", primary_->describe().c_str());
                degraded_ = false;
                return true;
            }
            if (e == "superseded") { if (err) *err = e; return false; }  // abandoned on purpose, not broken
            bool auth = e.find("HTTP 401") != std::string::npos || e.find("HTTP 403") != std::string::npos ||
                        e.find("no ANTHROPIC") != std::string::npos;
            // Bad credentials won't fix themselves quickly; network blips might.
            primary_retry_at_ = now + (auth ? std::chrono::minutes(30) : std::chrono::minutes(2));
            std::fprintf(stderr, "[mind] %s failed (%s)%s; using %s\n", primary_->describe().c_str(),
                         e.c_str(), auth ? " - check ANTHROPIC_API_KEY" : "",
                         fallback_->describe().c_str());
            degraded_ = true;
        }
        last_ = fallback_.get();
        return fallback_->complete(system, msgs, schema, out, err);
    }
    std::string lastUsed() const override { return last_ ? last_->lastUsed() : describe(); }
    double lastCost() const override { return last_ ? last_->lastCost() : 0; }
    bool lastWasPaid() const override { return last_ && last_->lastWasPaid(); }

private:
    std::unique_ptr<LlmBackend> primary_, fallback_;
    std::chrono::steady_clock::time_point primary_retry_at_{};
    bool degraded_ = false;
    bool budget_noted_ = false;
    LlmBackend* last_ = nullptr;
};

}  // namespace

void setUsageFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_usage_mu);
    g_usage_path = path;
    auto slash = path.rfind('/');
    g_calls_log = (slash == std::string::npos ? std::string(".") : path.substr(0, slash)) + "/claude_calls.log";
    try {
        if (FILE* f = std::fopen(path.c_str(), "r")) {
            std::string data;
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
            std::fclose(f);
            auto j = json::parse(data);
            g_usage.date = j.value("date", "");
            g_usage.calls = j.value("calls", 0);
            g_usage.input = j.value("input_tokens", 0L);
            g_usage.output = j.value("output_tokens", 0L);
            g_usage.cache_read = j.value("cache_read_tokens", 0L);
            g_usage.cache_write = j.value("cache_write_tokens", 0L);
            g_usage.usd = j.value("usd", 0.0);
        }
    } catch (...) {
    }
    rollDayLocked();
}

void setCallReason(const std::string& why) {
    std::lock_guard<std::mutex> lk(g_usage_mu);
    g_reason = why;
}

void setColorOutput(bool on) { g_color = on; }

void setDailyBudget(double usd) {
    std::lock_guard<std::mutex> lk(g_usage_mu);
    g_budget = usd;
}

Usage usageToday() {
    std::lock_guard<std::mutex> lk(g_usage_mu);
    rollDayLocked();
    return g_usage;
}

bool overBudget() {
    std::lock_guard<std::mutex> lk(g_usage_mu);
    rollDayLocked();
    return g_budget > 0 && g_usage.usd >= g_budget;
}

std::string describeImage(const std::string& base_url, const std::string& model,
                          const std::string& jpeg, const std::string& prompt, std::string* err,
                          int timeout_sec) {
    std::string base = base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    json body = {
        {"model", model},
        {"stream", false},
        {"messages",
         json::array({{{"role", "user"},
                       {"content",
                        json::array({{{"type", "text"}, {"text", prompt}},
                                     {{"type", "image_url"},
                                      {"image_url",
                                       {{"url", "data:image/jpeg;base64," + base64(jpeg)}}}}})}}})}};
    auto r = httpPost(base + "/chat/completions", body.dump(), {}, timeout_sec);
    if (!r.ok()) {
        if (err) *err = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
        return "";
    }
    try {
        auto j = json::parse(r.body);
        std::string t = j.at("choices").at(0).at("message").at("content").get<std::string>();
        auto b = t.find_first_not_of(" \t\n");
        auto e = t.find_last_not_of(" \t\n");
        return b == std::string::npos ? "" : t.substr(b, e - b + 1);
    } catch (const std::exception& ex) {
        if (err) *err = std::string("bad vision response: ") + ex.what();
        return "";
    }
}

// Look something up on the web: Claude with Anthropic's server-side web search, answering in a few
// spoken-style sentences. Billed (tokens + $0.01 per search); counted in the daily budget like any call.
std::string webLookup(const std::string& model, const std::string& question, std::string* err) {
    std::vector<std::pair<std::string, std::string>> headers = {{"anthropic-version", "2026-10-21"}};
    if (const char* key = envOr("ANTHROPIC_API_KEY")) headers.push_back({"x-api-key", key});
    else if (const char* tok = envOr("ANTHROPIC_AUTH_TOKEN")) {
        headers.push_back({"Authorization", std::string("Bearer ") + tok});
        headers.push_back({"anthropic-beta", "oauth-2025-04-20"});
    } else {
        if (err) *err = "no ANTHROPIC_API_KEY / ANTHROPIC_AUTH_TOKEN in the environment";
        return "";
    }
    std::string base = envOr("ANTHROPIC_BASE_URL") ? envOr("ANTHROPIC_BASE_URL") : "https://api.anthropic.com";
    char date[32];
    std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof date, "%Y-%m-%d", std::localtime(&now));
    json messages = json::array({{{"role", "user"}, {"content", question}}});
    json body = {{"model", model},
                 {"max_tokens", 2048},
                 {"system", std::string("You look things up for a small home robot who will say your answer out loud to "
                                        "the person who asked. Today is ") + date + ". Search the web as needed, then "
                                        "answer in at most 3 short plain sentences (no lists, no markdown, no URLs), "
                                        "giving the key facts and, if it matters, how recent or certain they are."},
                 // Dynamic filtering (20260209) exists on Sonnet/Opus 5.5 class models; Haiku uses the basic tool.
                 {"tools", json::array({{{"type", model.rfind("claude-haiku", 0) == 0 ? "web_search_20250305" : "web_search_20260209"},
                                         {"name", "web_search"}, {"max_uses", 2}}})},
                 {"output_config", {{"effort", "low"}}}};
    if (model.rfind("claude-sonnet-5-5", 0) == 0) body["thinking"] = {{"type", "between_tools"}};
    else if (model.rfind("claude-haiku-5-5", 0) == 0) body["thinking"] = {{"type", "disabled"}};
    for (int round = 0; round < 4; ++round) {
        body["messages"] = messages;
        auto r = httpPost(base + "/v1/messages", body.dump(), headers, 120);
        if (!r.error.empty()) { if (err) *err = r.error; return ""; }
        json resp;
        try { resp = json::parse(r.body); } catch (...) {
            if (err) *err = "HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, 200);
            return "";
        }
        if (r.status != 200) {
            if (err) *err = "HTTP " + std::to_string(r.status) + ": " +
                            resp.value("error", json::object()).value("message", r.body.substr(0, 200));
            return "";
        }
        if (resp.contains("usage")) recordUsage(resp.value("model", model), resp["usage"]);
        std::string stop = resp.value("stop_reason", "");
        json content = resp.value("content", json::array());
        if (stop == "pause_turn") {  // the server-side search loop wants another round: send it back as is
            messages.push_back({{"role", "assistant"}, {"content", content}});
            continue;
        }
        if (stop == "refusal") { if (err) *err = "declined"; return ""; }
        // Only the final answer: text written after the last search ("Let me look that up..." is dropped).
        std::string text;
        for (auto& b : content) {
            std::string ty = b.value("type", "");
            if (ty == "text") text += b.value("text", "");
            else if (ty == "server_tool_use" || ty.find("tool_result") != std::string::npos) text.clear();
        }
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(0, 1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
        if (text.empty() && err) *err = "no answer (stop_reason " + stop + ")";
        return text;
    }
    if (err) *err = "search took too many rounds";
    return "";
}

std::unique_ptr<LlmBackend> makeClaudeBackend(const std::string& model, const std::string& effort) {
    return std::make_unique<ClaudeBackend>(model.empty() ? "claude-haiku-5-5" : model,
                                           effort.empty() ? "low" : effort);
}

std::unique_ptr<LlmBackend> makeReflectionBackend(const std::string& model) {
    const char* local = envOr("MBOT_LOCAL_MIND");
    std::string url = local ? local : "http://localhost:11434/v1";
    if (model.rfind("claude", 0) == 0)  // careful Claude, with the local model as fallback (offline / over budget)
        return std::make_unique<FallbackBackend>(std::make_unique<ClaudeBackend>(model, "high", true),
                                                 makeOpenAIBackend(url, reflectLocalModel()));
    return makeOpenAIBackend(url, model);
}

std::unique_ptr<LlmBackend> makeOpenAIBackend(const std::string& base_url, const std::string& model) {
    return std::make_unique<OpenAIBackend>(base_url, model);
}

bool wantsClaude(const std::string& spec) {
    bool have = envOr("ANTHROPIC_API_KEY") || envOr("ANTHROPIC_AUTH_TOKEN");
    return spec.rfind("claude", 0) == 0 || (spec.empty() && have);
}

std::unique_ptr<LlmBackend> makeBackend(const std::string& spec, const std::string& model,
                                        const std::string& effort, std::string* err) {
    bool have_claude = envOr("ANTHROPIC_API_KEY") || envOr("ANTHROPIC_AUTH_TOKEN");
    if (spec.rfind("claude", 0) == 0 || (spec.empty() && have_claude)) {
        if (!have_claude) {
            if (err) *err = "Claude selected but ANTHROPIC_API_KEY is not set";
            return nullptr;
        }
        auto claude = makeClaudeBackend(spec.rfind("claude:", 0) == 0 ? spec.substr(7) : model, effort);
        // Keep a local mind on standby (Ollama default).
        const char* local = envOr("MBOT_LOCAL_MIND");
        return std::make_unique<FallbackBackend>(
            std::move(claude), makeOpenAIBackend(local ? local : "http://localhost:11434/v1",
                                                 defaultLocalModel()));
    }
    std::string url = spec.empty() ? "http://localhost:11434/v1" : spec;
    if (url.rfind("http", 0) != 0) {
        if (err) *err = "backend must be 'claude' or an http(s):// URL";
        return nullptr;
    }
    return makeOpenAIBackend(url, model.empty() ? defaultLocalModel() : model);
}

std::string reflectLocalModel() {
    const char* m = std::getenv("MBOT_REFLECT_LOCAL_MODEL");
    return m && *m ? m : "qwen2.5:14b";
}

std::string defaultLocalModel() {
    const char* m = std::getenv("MBOT_LOCAL_MODEL");
    return m && *m ? m : "qwen2.5:7b";
}

}  // namespace mbot
