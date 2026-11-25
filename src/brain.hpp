#pragma once
#include <string>

#include "vector_client.hpp"

namespace mbot {

struct BrainOptions {
    // Mind backend: "claude" (Anthropic API), or an OpenAI-compatible base URL
    // such as http://localhost:11434/v1 (Ollama). Empty = Claude if a key is
    // set, else local Ollama.
    std::string backend;
    std::string model;            // conversations; default claude-sonnet-5-5 / local qwen2.5:7b
    std::string fast_model;       // background turns + reflection. Default: the free local
                                  // model (qwen2.5:7b via Ollama, or $MBOT_LOCAL_MODEL). "claude-haiku-5-5" = cheap Claude.
    double daily_budget = 1.0;    // USD/day for Claude; then local only until midnight (0 = no limit)
    // Local vision: his eyes run free on a local model (minicpm-v) that DESCRIBES the scene
    // as text; that text (never the image) goes to the interactive mind.
    std::string vision_url = "http://localhost:11434/v1";
    std::string vision_model = "minicpm-v";  // "" disables sight entirely
    // Careful look (only when asked: "look carefully", "what can you read on my screen"): a high-res frame
    // goes to this ONLINE model for detail and text reading. Costs a little per look; "" = local model only.
    std::string detail_vision_model = "claude-sonnet-5-5";
    // Looking things up on the web when he needs a fact he doesn't know ("" = never).
    std::string lookup_model = "claude-haiku-5-5";  // cheap; Sonnet reads more carefully (~5x the cost)
    bool reflect_with_main = false;  // use the main (paid) mind for memory reflection
    // Mind for memory consolidation + dreams (what he immortalises). "" = the fast/local model;
    // e.g. "claude-sonnet-5-5" for careful Claude with thinking (local fallback).
    std::string reflect_model;
    std::string effort = "low";   // Claude only: low keeps replies quick

    std::string whisper_model;    // ggml model for speech-to-text; empty = no ears
    std::string speaker_model = "models/speaker-campplus-en.onnx";  // voice recognition ("" = off)
    int mic_port = 8193;          // robot speaker feed (used as an echo gate)
    // Which microphone: "pc" = default PC input, "robot" = :8193 stream, or a
    // PipeWire/Pulse source name (see `pactl list short sources`).
    std::string mic = "pc";
    int whisper_threads = 6;

    // Optional external TTS: text on stdin, writes a 16-bit WAV to {out}.
    std::string tts_cmd;

    std::string data_dir;         // default ~/.local/share/mbot/<serial>
    int idle_sec = 120;           // how often an idle robot considers doing something
    bool keep_control = false;    // hold the body permanently (native personality off)
    bool use_camera = true;
    bool read_stdin = true;
    std::string input_fifo;       // named pipe for typed lines when running as a service (vtalk)
    // Answer any clear speech, not only when addressed by name / in conversation.
    bool listen_all = false;
    // Recognising people: voiceprints, learning faces, keeping portraits. Off unless you turn it on
    // (--recognition, or `vtalk "/recognition on"`), because it stores biometric data about people.
    bool recognition = false;
    bool save_ears = false;       // keep the last 10 heard segments as WAV files in <data>/ears (debugging)
    bool save_eyes = false;       // keep the last 10 pictures he looked at (+ what he made of them) in <data>/eyes
    bool debug_ears = false;      // print every segment / transcription to the terminal       // typed lines count as speech (handy for testing)
    uint32_t volume = 80;
    // Battery care: head to the charger below min %, and (only when a person is around) leave it
    // around max %. Wider, safer band than the cell's ideal 20-80: more margin before empty.
    int battery_min = 20;           // head home here (or earlier if that leaves < 2 min, once calibrated)
    int battery_max = 95;          // 100 = stay on the charger until full
};

// Runs until Ctrl-C.
int runBrain(VectorClient& client, const BrainOptions& opt);
// Consolidate recent memories now (normally happens during quiet "sleep" time).
int runReflection(VectorClient& client, const BrainOptions& opt);

}  // namespace mbot
