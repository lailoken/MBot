#pragma once
// Microphone -> speech segments -> local Whisper transcription.
//
// Vector's raw microphone is NOT reachable on this firmware. Speech is captured
// from a microphone on the PC (PipeWire/PulseAudio via `parec`). The robot's
// :8193 stream turned out to be its *speaker output*; we use it as an echo gate:
// whenever Vector makes a sound, the ears mute briefly so it doesn't hear itself.
//
// :8193 frames look like
//   32-byte header: "TCEV", u32 seq, u32 rate(32000), u32 channels(1),
//                   u32 samples(320), u32 0, u32 timestamp, u32 0
//   followed by samples*channels int16 little-endian PCM (10 ms per frame).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "doa.hpp"

struct whisper_context;

namespace mbot {

struct Heard {
    std::string text;
    float seconds = 0;      // length of the utterance
    float loudness = 0;     // mean RMS
    std::shared_ptr<const std::vector<float>> audio;  // the utterance itself (16 kHz), kept in memory only
    DoaObs doa;             // where it came from (time differences between his 4 mics), if measured
};

// Rotating debug files dir/<prefix>N<ext>, N < count: the slot after the newest one (so a restart keeps
// the latest files instead of overwriting them).
int nextSlot(const std::string& dir, const std::string& prefix, const std::string& ext, int count);

class Ears {
public:
    // pc_device: PipeWire/Pulse source name for the PC mic ("" = system default).
    // If use_pc_mic is false, :8193 is used as the audio source (old behaviour).
    Ears(std::string host, int port, std::string whisper_model, int threads = 6,
         bool use_pc_mic = true, std::string pc_device = "");
    ~Ears();

    bool start(std::function<void(const Heard&)> on_heard, std::string* err = nullptr);
    // Diagnostics: every detected segment, transcription and drop reason.
    void setLogger(std::function<void(const std::string&)> log) { log_ = std::move(log); }
    // Debug: keep the last few detected segments as WAV files in this directory.
    void saveSegmentsTo(const std::string& dir) { std::lock_guard<std::mutex> lk(vocab_mu_); save_dir_ = dir; }
    void stop();
    // Use Vector's OWN microphones via the on-robot mictap daemon (TCP, wake-word
    // triggered, works anywhere Vector roams). Disables the PC mic / :8193 source.
    void setTapMode(int port = 9099) { use_tap_ = true; tap_port_ = port; }
    // Words to bias recognition toward (household names, "Vector") - whisper's initial prompt.
    // Direction of sound: raw 4-channel snippets while someone speaks (robot's mictap :9098).
    void setDirectionFinder(DirectionFinder* d) { dir_ = d; }
    // Called (from the audio thread) when the robot's mic stream has been connected but silent - no bytes at
    // all - for a minute: the robot side needs a kick. The brain restarts the streamer on the robot.
    void setStallHandler(std::function<void()> h) { on_stall_ = std::move(h); }
    void setVocabulary(const std::string& v) { std::lock_guard<std::mutex> lk(vocab_mu_); vocab_ = v; }

    // Ignore the mic while Vector itself is talking/animating (it hears itself).
    void muteFor(std::chrono::milliseconds d);
    void setMuted(bool m);
    // Someone is talking right now (the voice detector is inside an utterance).
    bool hearingSpeech() const { return in_speech_.load() && !isMuted(); }

    float noiseFloor() const { return floor_.load(); }
    float takePeak() { return peak_.exchange(0.f); }  // loudest 10 ms frame since last call
    bool connected() const { return connected_.load(); }

    // Offline helper: transcribe 16 kHz mono float samples.
    std::string transcribe(const std::vector<float>& pcm16k, float* no_speech = nullptr, bool words_only = false);

private:
    void netLoop();
    void pcLoop();
    void tapLoop();
    void sttLoop();
    void onEchoFrame(const int16_t* s, size_t n);
    void onFrame(const int16_t* s, size_t n, uint32_t rate);
    bool isMuted() const;

    std::string host_;
    int port_;
    std::string model_path_;
    int threads_;
    whisper_context* wctx_ = nullptr;   // used by the STT thread only (and before it starts)
    bool gpu_wanted_ = false, on_gpu_ = false;
    std::chrono::steady_clock::time_point last_gpu_try_{};
    void maybeMoveToGpu();
    void warmUp();              // STT thread: retry the GPU after a fallback (e.g. VRAM was full)
    std::function<void(const Heard&)> on_heard_;
    std::function<void(const std::string&)> log_;
    std::string save_dir_;
    int save_n_ = -1;  // next rotating slot (-1: continue after the newest file on disk)
    void log(const std::string& m) { if (log_) log_(m); }

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    bool warned_ = false;  // net thread only
    std::atomic<bool> muted_{false};
    std::atomic<int64_t> mute_until_ms_{0};
    std::thread net_, stt_, pc_, tap_;
    bool use_tap_ = false;
    int tap_port_ = 9099;
    bool use_pc_mic_;
    std::string pc_device_;
    std::atomic<int> pc_pid_{0};

    // VAD state (net thread only)
    std::atomic<float> floor_{300.f};
    std::atomic<float> peak_{0.f};
    std::vector<float> pre_roll_;      // last ~300 ms of 16 kHz audio
    std::vector<float> utter_;         // current utterance, 16 kHz
    std::atomic<bool> in_speech_{false};
    int loud_run_ = 0, quiet_run_ = 0;
    double rms_sum_ = 0;
    int rms_n_ = 0;

    std::mutex qmu_;
    std::mutex vocab_mu_;
    std::string vocab_;
    std::condition_variable qcv_;
    struct Pending { std::vector<float> audio; float loudness; DoaCapture dir; };
    std::deque<Pending> queue_;
    DirectionFinder* dir_ = nullptr;
    std::function<void()> on_stall_;
    int64_t silent_since_ms_ = 0;  // tap thread: when bytes last stopped arriving (0 = flowing)
    bool dir_open_ = false;  // net thread only
};

}  // namespace mbot
