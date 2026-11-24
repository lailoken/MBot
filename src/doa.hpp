#pragma once
// Direction of sound: where a voice comes from, from the time differences between Vector's 4 head mics.
//
// The robot (mictap :9098) sends raw 4-channel audio only while someone is speaking (ring buffer + live).
// For each utterance we measure the time difference of arrival (TDOA) for the 6 mic pairs with GCC-PHAT
// (sub-sample precise). The mic layout isn't published, so nothing about it is assumed: the mapping from
// TDOAs to a bearing is LEARNED. For a far voice, TDOA = A * (cos b, sin b) for a fixed 6x2 matrix A that
// depends only on where the mics sit. Every time someone speaks while exactly one face is in view, the
// camera gives the true bearing - one labelled sample. A few dozen of those pin A down, for all directions.
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mbot {

struct DoaObs {
    bool ok = false;
    std::array<float, 6> tau{};  // pairs 01 02 03 12 13 23, in samples @ 15625 Hz (+ = reached the first mic later)
    float quality = 0;           // 0..1: how coherent / consistent the measurement is
    int frames = 0;              // speech frames used
    bool moving = false;         // his motors were running (noisy)
    int64_t at = 0;              // unix time
};

struct DoaCapture {
    std::vector<int16_t> ch[4];
    int pkts = 0, moving_pkts = 0;
};
DoaObs measureCapture(const DoaCapture& c);

class DirectionFinder {
public:
    ~DirectionFinder();
    void start(const std::string& host, int port = 9098);
    void stop();
    void begin();         // someone started speaking: ask the robot for the raw channels
    DoaCapture end();     // they stopped: what was captured (measure it with measureCapture, off the audio thread)
    bool connected() const { return connected_.load(); }

private:
    void loop();
    std::string host_;
    int port_ = 9098;
    std::thread th_;
    std::atomic<bool> running_{false}, connected_{false}, want_{false};
    std::atomic<int> fd_{-1};
    std::mutex mu_;
    std::vector<int16_t> ch_[4];  // collected samples per channel
    int pkts_ = 0, moving_pkts_ = 0;
};

// Measure the TDOAs of a 4-channel recording (exposed for tests).
DoaObs measureDoa(const std::vector<int16_t> ch[4]);

class DirectionModel {
public:
    void load(const std::string& path);
    void save() const;
    // A measurement with the true bearing (degrees, + = to his left), e.g. from the face that was speaking.
    void addSample(const DoaObs& o, float bearing_deg, float weight);
    bool ready() const { std::lock_guard<std::mutex> lk(mu_); return ready_; }
    // Bearing in degrees (+ = left, -180..180) and a confidence 0..1. False when unknown.
    bool predict(const DoaObs& o, float& bearing_deg, float& confidence) const;
    std::string summary() const;
    void reset();

private:
    struct Sample { std::array<float, 6> tau; float bearing, weight; int64_t at; };
    void fit();
    bool predictLocked(const DoaObs& o, float& bearing_deg, float& confidence) const;
    std::vector<Sample> samples_;
    std::array<std::array<float, 2>, 6> A_{};
    bool ready_ = false;
    float fit_err_deg_ = 0;
    std::string path_;
    mutable std::mutex mu_;
};

}  // namespace mbot
