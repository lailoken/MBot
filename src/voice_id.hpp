#pragma once
// Speaker (voice) recognition: voiceprints from short utterances, matched locally.
// Embeddings come from a speaker model run with sherpa-onnx (built only when MBOT_VOICE_ID is defined).
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace mbot {

class VoiceId {
public:
    ~VoiceId();
    bool load(const std::string& model_path, std::string* err);
    bool ready() const { return ex_ != nullptr; }
    // pcm: 16 kHz mono in int16 range (as the ears keep it). [from, from+n). Empty if too short or failed.
    std::vector<float> embed(const std::vector<float>& pcm, size_t from = 0, size_t n = size_t(-1));

private:
    const void* ex_ = nullptr;
    std::mutex mu_;
};

struct VoiceMatch {
    std::string name;
    float score = 0;      // cosine similarity to that person's voiceprint
    float solidity = 0;   // 0..1: how established the voiceprint is (provisional prints give tentative matches)
};

// Everyone's voiceprints: each person has samples with a strength. New samples start provisional,
// confident matches reinforce (and can add) samples, unused samples fade and are forgotten.
// Stored in <data>/voices/voiceprints.json.
class VoicePrints {
public:
    void load(const std::string& path);
    void save();
    std::vector<VoiceMatch> rank(const std::vector<float>& e) const;  // best first
    void enroll(const std::string& name, const std::vector<float>& e);  // verified sample (provisional)
    void reinforce(const std::string& name, const std::vector<float>& e);  // a confident match
    void decay(int64_t now);  // fade samples that haven't matched for a while; forget faded ones
    bool forget(const std::string& name);
    void rename(const std::string& from, const std::string& to);  // people renamed / merged
    std::map<std::string, std::pair<int, float>> summary() const;  // name -> (samples, solidity)

private:
    struct Sample {
        std::vector<float> e;
        float strength = 0.3f;
        int64_t added = 0, last_hit = 0;
        int hits = 0;
    };
    std::map<std::string, std::vector<Sample>> people_;
    std::string path_;
    int64_t last_decay_ = 0;
    mutable std::mutex mu_;
    static std::vector<float> centroid(const std::vector<Sample>& s);
    static float solidity(const std::vector<Sample>& s);
};

float cosineSim(const std::vector<float>& a, const std::vector<float>& b);

}  // namespace mbot
