#include "voice_id.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>

#include <nlohmann/json.hpp>

#ifdef MBOT_VOICE_ID
#include "sherpa-onnx/c-api/c-api.h"
#endif

namespace mbot {
using json = nlohmann::json;

namespace {
int64_t nowSec() { return int64_t(std::time(nullptr)); }
void normalize(std::vector<float>& v) {
    double n = 0;
    for (float x : v) n += double(x) * x;
    n = std::sqrt(n);
    if (n > 0)
        for (auto& x : v) x = float(x / n);
}
}  // namespace

float cosineSim(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.f;
    double d = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) { d += double(a[i]) * b[i]; na += double(a[i]) * a[i]; nb += double(b[i]) * b[i]; }
    return (na > 0 && nb > 0) ? float(d / std::sqrt(na * nb)) : 0.f;
}

// ------------------------------------------------------------------ VoiceId --

VoiceId::~VoiceId() {
#ifdef MBOT_VOICE_ID
    if (ex_) SherpaOnnxDestroySpeakerEmbeddingExtractor(static_cast<const SherpaOnnxSpeakerEmbeddingExtractor*>(ex_));
#endif
}

bool VoiceId::load(const std::string& model_path, std::string* err) {
#ifdef MBOT_VOICE_ID
    std::ifstream probe(model_path);
    if (!probe) { if (err) *err = "speaker model not found: " + model_path; return false; }
    SherpaOnnxSpeakerEmbeddingExtractorConfig cfg{};
    cfg.model = model_path.c_str();
    cfg.num_threads = 2;
    cfg.debug = 0;
    cfg.provider = "cpu";
    ex_ = SherpaOnnxCreateSpeakerEmbeddingExtractor(&cfg);
    if (!ex_ && err) *err = "couldn't load speaker model " + model_path;
    return ex_ != nullptr;
#else
    (void)model_path;
    if (err) *err = "built without voice recognition (sherpa-onnx not found at build time)";
    return false;
#endif
}

std::vector<float> VoiceId::embed(const std::vector<float>& pcm, size_t from, size_t n) {
    std::vector<float> out;
#ifdef MBOT_VOICE_ID
    if (!ex_ || from >= pcm.size()) return out;
    n = std::min(n, pcm.size() - from);
    if (n < 16000) return out;  // under 1 s says too little about a voice
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = pcm[from + i] / 32768.f;
    std::lock_guard<std::mutex> lk(mu_);
    auto* ex = static_cast<const SherpaOnnxSpeakerEmbeddingExtractor*>(ex_);
    const SherpaOnnxOnlineStream* st = SherpaOnnxSpeakerEmbeddingExtractorCreateStream(ex);
    SherpaOnnxOnlineStreamAcceptWaveform(st, 16000, x.data(), int32_t(x.size()));
    SherpaOnnxOnlineStreamInputFinished(st);
    if (SherpaOnnxSpeakerEmbeddingExtractorIsReady(ex, st)) {
        const float* v = SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding(ex, st);
        int dim = SherpaOnnxSpeakerEmbeddingExtractorDim(ex);
        out.assign(v, v + dim);
        SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding(v);
        normalize(out);
    }
    SherpaOnnxDestroyOnlineStream(st);
#else
    (void)pcm; (void)from; (void)n;
#endif
    return out;
}

// -------------------------------------------------------------- VoicePrints --

namespace {
constexpr size_t kMaxSamples = 40;
constexpr float kNewStrength = 0.3f;     // a fresh sample is provisional
constexpr float kReinforce = 0.1f;       // each confident match it agrees with
constexpr float kFadePerDay = 0.02f;     // when it hasn't matched for 3+ days
constexpr float kForgetBelow = 0.05f;
}  // namespace

void VoicePrints::load(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    people_.clear();
    std::ifstream f(path);
    if (!f) return;
    try {
        json j;
        f >> j;
        last_decay_ = j.value("last_decay", int64_t(0));
        json people = j.value("people", json::object());  // (a named copy: items() on a temporary dangles)
        for (auto& [name, arr] : people.items())
            for (auto& sj : arr) {
                Sample s;
                s.e = sj.value("e", std::vector<float>{});
                s.strength = sj.value("strength", kNewStrength);
                s.added = sj.value("added", int64_t(0));
                s.last_hit = sj.value("last_hit", int64_t(0));
                s.hits = sj.value("hits", 0);
                if (!s.e.empty()) people_[name].push_back(s);
            }
    } catch (...) {
    }
}

void VoicePrints::save() {
    std::lock_guard<std::mutex> lk(mu_);
    if (path_.empty()) return;
    json p = json::object();
    for (auto& [name, ss] : people_) {
        json arr = json::array();
        for (auto& s : ss)
            arr.push_back({{"e", s.e}, {"strength", std::round(s.strength * 1000) / 1000}, {"added", s.added},
                           {"last_hit", s.last_hit}, {"hits", s.hits}});
        p[name] = arr;
    }
    std::string tmp = path_ + ".tmp";
    { std::ofstream f(tmp); f << json{{"last_decay", last_decay_}, {"people", p}}.dump(); }
    std::rename(tmp.c_str(), path_.c_str());
}

std::vector<float> VoicePrints::centroid(const std::vector<Sample>& ss) {
    std::vector<float> c;
    for (auto& s : ss) {
        if (c.empty()) c.assign(s.e.size(), 0.f);
        if (s.e.size() != c.size()) continue;
        for (size_t i = 0; i < c.size(); ++i) c[i] += s.strength * s.e[i];  // strong samples count more
    }
    normalize(c);
    return c;
}

float VoicePrints::solidity(const std::vector<Sample>& ss) {
    float total = 0;
    for (auto& s : ss) total += s.strength;
    return std::min(1.f, total / 3.f);  // ~ three solid samples' worth = fully established
}

std::vector<VoiceMatch> VoicePrints::rank(const std::vector<float>& e) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<VoiceMatch> out;
    for (auto& [name, ss] : people_)
        if (!ss.empty()) out.push_back({name, cosineSim(centroid(ss), e), solidity(ss)});
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.score > b.score; });
    return out;
}

void VoicePrints::enroll(const std::string& name, const std::vector<float>& e) {
    if (e.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto& ss = people_[name];
    for (auto& s : ss)
        if (cosineSim(s.e, e) > 0.92f) { s.strength = std::min(1.f, s.strength + kReinforce); s.last_hit = nowSec(); return; }
    if (ss.size() >= kMaxSamples) {  // replace the weakest
        auto w = std::min_element(ss.begin(), ss.end(), [](auto& a, auto& b) { return a.strength < b.strength; });
        ss.erase(w);
    }
    ss.push_back({e, kNewStrength, nowSec(), nowSec(), 0});
}

void VoicePrints::reinforce(const std::string& name, const std::vector<float>& e) {
    if (e.empty()) return;
    bool agrees_with_none = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = people_.find(name);
        if (it == people_.end()) return;
        for (auto& s : it->second)
            if (cosineSim(s.e, e) >= 0.5f) {
                s.strength = std::min(1.f, s.strength + kReinforce);
                s.last_hit = nowSec();
                s.hits++;
                agrees_with_none = false;
            }
    }
    // A confident match that none of the stored samples resemble closely is a new facet of the voice
    // (another mood, distance, a cold): keep it, provisionally.
    if (agrees_with_none) enroll(name, e);
}

void VoicePrints::decay(int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    if (last_decay_ == 0) { last_decay_ = now; return; }
    double days = double(now - last_decay_) / 86400.0;
    if (days < 0.25) return;  // a few times a day is plenty
    last_decay_ = now;
    for (auto it = people_.begin(); it != people_.end();) {
        auto& ss = it->second;
        for (auto& s : ss)
            if (now - s.last_hit > 3 * 86400) s.strength -= float(kFadePerDay * days);
        ss.erase(std::remove_if(ss.begin(), ss.end(), [](auto& s) { return s.strength < kForgetBelow; }), ss.end());
        it = ss.empty() ? people_.erase(it) : std::next(it);
    }
}

bool VoicePrints::forget(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    return people_.erase(name) > 0;
}

void VoicePrints::rename(const std::string& from, const std::string& to) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = people_.find(from);
    if (it == people_.end() || from == to) return;
    auto& dst = people_[to];
    dst.insert(dst.end(), it->second.begin(), it->second.end());
    people_.erase(from);
}

std::map<std::string, std::pair<int, float>> VoicePrints::summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, std::pair<int, float>> out;
    for (auto& [name, ss] : people_) out[name] = {int(ss.size()), solidity(ss)};
    return out;
}

}  // namespace mbot
