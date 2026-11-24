#include "ears.hpp"

#include <cctype>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "whisper.h"

namespace mbot {

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int connectTcp(const std::string& host, int port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (auto* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        timeval tv{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);  // bounds connect()
        timeval rtv{10, 0};  // no audio for 10 s = stream stalled
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
        // Close with RST so the robot frees the connection immediately. Half-closed
        // connections pile up on the robot and jam its mic server.
        linger lg{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool readExact(int fd, void* buf, size_t n, const std::atomic<bool>& running) {
    auto* p = static_cast<char*>(buf);
    while (n > 0 && running) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return n == 0;
}

// Whisper's classic hallucinations on silence / noise.
bool isJunk(const std::string& t) {
    if (t.size() < 2) return true;
    static const char* junk[] = {"[BLANK_AUDIO]", "(silence)", "[ Silence ]", "[silence]",
                                 "Thank you.",    "Thanks for watching!", "you", "Bye.",
                                 "(upbeat music)", "[Music]", "(music)", "*music*"};
    for (auto* j : junk)
        if (t == j) return true;
    // Entirely bracketed/parenthesised sound descriptions.
    if ((t.front() == '[' && t.back() == ']') || (t.front() == '(' && t.back() == ')') ||
        (t.front() == '*' && t.back() == '*'))
        return true;
    return false;
}

// Real words only: count tokens of 2+ letters that aren't filler noises. Garble like "Hmm.",
// "Uh", "Mm-hmm", "..." scores 0 and never reaches the mind (or the paid AI).
int realWordCount(const std::string& t) {
    static const char* fillers[] = {"hmm", "hm", "uh", "um", "umm", "uhm", "mm", "mmm", "mhm",
                                    "ah", "oh", "huh", "er", "erm", "eh", "ha", "haha", "shh", "hmmm"};
    int n = 0;
    std::string w;
    auto flush = [&] {
        if (w.size() >= 2) {
            bool filler = false;
            for (auto* f : fillers) if (w == f) { filler = true; break; }
            // "mm-hmm" style: only m/h letters
            if (!filler && w.find_first_not_of("mh") == std::string::npos) filler = true;
            if (!filler) ++n;
        }
        w.clear();
    };
    for (char c : t) {
        if (std::isalpha(static_cast<unsigned char>(c))) w += char(std::tolower(static_cast<unsigned char>(c)));
        else if (c == '\'' ) continue;          // don't split contractions
        else flush();
    }
    flush();
    return n;
}

}  // namespace

Ears::Ears(std::string host, int port, std::string model, int threads, bool use_pc_mic,
           std::string pc_device)
    : host_(std::move(host)), port_(port), model_path_(std::move(model)), threads_(threads),
      use_pc_mic_(use_pc_mic), pc_device_(std::move(pc_device)) {}

Ears::~Ears() {
    stop();
    if (wctx_) whisper_free(wctx_);
}

bool Ears::start(std::function<void(const Heard&)> on_heard, std::string* err) {
    whisper_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);
    auto cp = whisper_context_default_params();  // use_gpu is on: GPU if this build has one
    cp.use_gpu = std::getenv("MBOT_WHISPER_CPU") == nullptr;
    wctx_ = whisper_init_from_file_with_params(model_path_.c_str(), cp);
    if (!wctx_ && cp.use_gpu) {  // no usable GPU (or out of memory): the CPU it is
        std::fprintf(stderr, "[ears] speech-to-text couldn't start on the GPU - using the CPU\n");
        cp.use_gpu = false;
        wctx_ = whisper_init_from_file_with_params(model_path_.c_str(), cp);
    }
    if (!wctx_) {
        if (err) *err = "cannot load whisper model " + model_path_;
        return false;
    }
    on_heard_ = std::move(on_heard);
    running_ = true;
    if (use_tap_) {
        tap_ = std::thread([this] { tapLoop(); });
    } else {
        if (port_ > 0) net_ = std::thread([this] { netLoop(); });
        if (use_pc_mic_) pc_ = std::thread([this] { pcLoop(); });
    }
    stt_ = std::thread([this] { sttLoop(); });
    return true;
}

void Ears::stop() {
    if (!running_.exchange(false)) return;
    qcv_.notify_all();
    if (int pid = pc_pid_.load()) kill(pid, SIGTERM);
    if (tap_.joinable()) tap_.join();
    if (pc_.joinable()) pc_.join();
    if (net_.joinable()) net_.join();
    if (stt_.joinable()) stt_.join();
}

void Ears::muteFor(std::chrono::milliseconds d) {
    int64_t until = nowMs() + d.count();
    int64_t cur = mute_until_ms_.load();
    while (until > cur && !mute_until_ms_.compare_exchange_weak(cur, until)) {
    }
}

void Ears::setMuted(bool m) {
    muted_ = m;
    if (!m) muteFor(std::chrono::milliseconds(500));  // let the speaker's echo die down
}

bool Ears::isMuted() const { return muted_ || nowMs() < mute_until_ms_.load(); }

void Ears::netLoop() {
    int backoff_s = 0;
    while (running_) {
        for (int i = 0; i < backoff_s * 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int fd = connectTcp(host_, port_);
        if (fd < 0) {
            backoff_s = std::min(60, std::max(5, backoff_s * 2));
            if (connected_ || !warned_) {
                std::fprintf(stderr, "[ears] can't reach the robot's microphone at %s:%d "
                                     "(robot asleep, off the charger with a flat battery, or off Wi-Fi?) "
                                     "- retrying\n", host_.c_str(), port_);
                warned_ = true;
            }
            connected_ = false;
            continue;
        }
        if (warned_) std::fprintf(stderr, "[ears] microphone connected\n");
        warned_ = false;
        connected_ = true;
        std::vector<int16_t> buf;
        size_t frames = 0;
        for (;;) {
            unsigned char hdr[32];
            if (!readExact(fd, hdr, sizeof hdr, running_)) {
                if (running_) {
                    std::fprintf(stderr, "[ears] microphone stream stalled%s - reconnecting\n",
                                 frames ? "" : " (robot sent no audio; if this repeats, reboot Vector)");
                    log(frames ? "mic stream stalled" : "mic connected but robot sent no audio");
                }
                break;
            }
            if (++frames == 100) backoff_s = 0;  // 1 s of healthy audio resets the backoff
            if (frames == 1) {
                uint32_t r, c, n;
                std::memcpy(&r, hdr + 8, 4); std::memcpy(&c, hdr + 12, 4); std::memcpy(&n, hdr + 16, 4);
                char b[96];
                std::snprintf(b, sizeof b, "mic stream: %u Hz, %u channel(s), %u samples/frame", r, c, n);
                log(b);
            }
            if (std::memcmp(hdr, "TCEV", 4) != 0) {
                std::fprintf(stderr, "[ears] lost frame sync, reconnecting\n");
                break;
            }
            uint32_t rate, ch, n;
            std::memcpy(&rate, hdr + 8, 4);
            std::memcpy(&ch, hdr + 12, 4);
            std::memcpy(&n, hdr + 16, 4);
            if (ch == 0 || ch > 8 || n > 16000) break;
            buf.resize(size_t(n) * ch);
            if (!readExact(fd, buf.data(), buf.size() * 2, running_)) break;
            if (ch > 1) {  // keep channel 0
                for (size_t i = 0; i < n; ++i) buf[i] = buf[i * ch];
                buf.resize(n);
            }
            if (use_pc_mic_) onEchoFrame(buf.data(), n);
            else onFrame(buf.data(), n, rate);
        }
        close(fd);
        connected_ = false;
        if (frames < 100) backoff_s = std::min(60, std::max(5, backoff_s * 2));
    }
}

// Vector's speaker output: mute the ears while it makes any sound (+ a short tail).
void Ears::onEchoFrame(const int16_t* s, size_t n) {
    double e = 0;
    for (size_t i = 0; i < n; ++i) e += double(s[i]) * s[i];
    float rms = n ? float(std::sqrt(e / n)) : 0.f;
    if (rms > 120.f) muteFor(std::chrono::milliseconds(350));
}

// Capture the PC microphone through PipeWire/PulseAudio (`parec`), 16 kHz mono.
void Ears::pcLoop() {
    int backoff_s = 0;
    while (running_) {
        for (int i = 0; i < backoff_s * 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int fds[2];
        if (pipe(fds) != 0) return;
        pid_t pid = fork();
        if (pid == 0) {
            dup2(fds[1], STDOUT_FILENO);
            close(fds[0]);
            close(fds[1]);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDERR_FILENO);
            std::string dev = "--device=" + pc_device_;
            if (pc_device_.empty())
                execlp("parec", "parec", "--raw", "--format=s16le", "--rate=16000", "--channels=1",
                       "--latency-msec=30", (char*)nullptr);
            else
                execlp("parec", "parec", "--raw", "--format=s16le", "--rate=16000", "--channels=1",
                       "--latency-msec=30", dev.c_str(), (char*)nullptr);
            _exit(127);
        }
        close(fds[1]);
        if (pid < 0) {
            close(fds[0]);
            backoff_s = 10;
            continue;
        }
        pc_pid_ = pid;
        connected_ = true;
        log("PC microphone capture started" + (pc_device_.empty() ? std::string(" (default source)")
                                                                   : " (" + pc_device_ + ")"));
        std::vector<int16_t> buf(160);  // 10 ms
        size_t frames = 0;
        while (running_) {
            if (!readExact(fds[0], buf.data(), buf.size() * 2, running_)) break;
            onFrame(buf.data(), buf.size(), 16000);
            ++frames;
        }
        close(fds[0]);
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        pc_pid_ = 0;
        connected_ = false;
        if (running_) {
            std::fprintf(stderr, "[ears] PC microphone capture stopped (is parec installed / a mic connected?) - retrying\n");
            backoff_s = frames > 100 ? 1 : std::min(30, std::max(3, backoff_s * 2));
        }
    }
}

// Read Vector's own mic via the on-robot mictap (16 kHz / 16-bit mono PCM over TCP).
// With the libmbotmic.so shim in vic-anim the audio is CONSTANT (raw mics, no wake word);
// without it, it only flows after "Hey Vector". Either way it runs through the same
// voice-activity detector as the PC mic (onFrame) in 10 ms frames.
void Ears::tapLoop() {
    int backoff = 0;
    while (running_) {
        for (int i = 0; i < backoff * 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int fd = connectTcp(host_, tap_port_);
        if (fd < 0) {
            connected_ = false;
            if (!warned_) {
                std::fprintf(stderr, "[ears] robot mic tap unreachable at %s:%d (is mictap running "
                                     "on the robot?) - retrying\n", host_.c_str(), tap_port_);
                warned_ = true;
            }
            backoff = std::min(15, std::max(2, backoff * 2));
            continue;
        }
        timeval tv{0, 300000};  // no audio for 300 ms (gated fallback between wake words)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        connected_ = true;
        if (warned_) std::fprintf(stderr, "[ears] robot mic tap connected\n");
        warned_ = false;
        log("robot mic tap connected - listening through Vector's own microphones");
        static const int16_t kZeros[160] = {};
        unsigned char raw[8192];
        size_t carry = 0;                      // an odd byte left over from the last read
        std::vector<int16_t> frame;
        frame.reserve(160);
        size_t frames = 0;
        while (running_) {
            ssize_t r = recv(fd, raw + carry, sizeof raw - carry, 0);
            if (r > 0) {
                size_t total = carry + size_t(r), n = total / 2;
                for (size_t i = 0; i < n; ++i) {
                    int16_t v;
                    std::memcpy(&v, raw + 2 * i, 2);
                    frame.push_back(v);
                    if (frame.size() == 160) {
                        onFrame(frame.data(), 160, 16000);
                        frame.clear();
                        if (++frames == 100 * 60) backoff = 0;
                    }
                }
                carry = total % 2;
                if (carry) raw[0] = raw[total - 1];
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Stream paused (wake-word fallback): close out any utterance in progress.
                for (int k = 0; k < 125 && in_speech_; ++k) onFrame(kZeros, 160, 16000);
            } else {
                break;  // connection closed
            }
        }
        close(fd);
        connected_ = false;
        if (running_) backoff = frames > 100 ? 1 : std::min(15, std::max(2, backoff * 2));
    }
}

void Ears::onFrame(const int16_t* s, size_t n, uint32_t rate) {
    // Resample to 16 kHz (Whisper's rate). The robot sends 32 kHz: average pairs.
    std::vector<float> f;
    if (rate == 32000) {
        f.resize(n / 2);
        for (size_t i = 0; i < f.size(); ++i) f[i] = (s[2 * i] + s[2 * i + 1]) * 0.5f;
    } else {
        double step = double(rate) / 16000.0;
        for (double x = 0; x < n; x += step) f.push_back(s[size_t(x)]);
    }
    double e = 0;
    for (float v : f) e += double(v) * v;
    float rms = f.empty() ? 0.f : float(std::sqrt(e / f.size()));
    if (rms > peak_.load()) peak_ = rms;

    if (isMuted()) {
        if (dir_open_) { dir_->end(); dir_open_ = false; }
        in_speech_ = false;
        utter_.clear();
        pre_roll_.clear();
        loud_run_ = quiet_run_ = 0;
        return;
    }

    float floor = floor_.load();
    float thresh = std::max(floor * 3.0f, 200.f);
    bool loud = rms > thresh;

    if (!in_speech_) {
        // Track background noise only when nobody is talking.
        if (rms < floor * 2.f || rms < 200.f) floor = floor * 0.995f + rms * 0.005f;
        floor_ = std::max(20.f, floor);
        pre_roll_.insert(pre_roll_.end(), f.begin(), f.end());
        if (pre_roll_.size() > 4800) pre_roll_.erase(pre_roll_.begin(), pre_roll_.end() - 4800);
        loud_run_ = loud ? loud_run_ + 1 : std::max(0, loud_run_ - 1);
        if (loud_run_ >= 6) {  // ~60 ms of sustained sound
            char b[96];
            std::snprintf(b, sizeof b, "sound start (level %.0f, noise %.0f, threshold %.0f)", rms, floor, thresh);
            log(b);
            in_speech_ = true;
            if (dir_ && dir_->connected()) { dir_->begin(); dir_open_ = true; }
            utter_ = pre_roll_;
            quiet_run_ = 0;
            rms_sum_ = 0;
            rms_n_ = 0;
        }
        return;
    }

    utter_.insert(utter_.end(), f.begin(), f.end());
    rms_sum_ += rms;
    rms_n_++;
    quiet_run_ = loud ? 0 : quiet_run_ + 1;
    // 0.7 s of quiet ends a piece. People pause mid-sentence, so the brain joins pieces, starts thinking
    // early, and drops a half-made reply if more words come before it speaks.
    bool ended = quiet_run_ >= 70;
    bool too_long = utter_.size() > 16000 * 15;    // 15 s cap
    if (ended || too_long) {
        float secs = float(utter_.size()) / 16000.f;
        DoaCapture cap;
        if (dir_open_) { cap = dir_->end(); dir_open_ = false; }
        if (secs >= 0.9f) {  // includes the trailing quiet; real speech >= ~0.2 s
            std::lock_guard<std::mutex> lk(qmu_);
            queue_.push_back({std::move(utter_), float(rms_sum_ / std::max(1, rms_n_)), std::move(cap)});
            if (queue_.size() > 4) queue_.pop_front();  // never fall far behind
            qcv_.notify_one();
        }
        utter_.clear();
        in_speech_ = false;
        loud_run_ = 0;
    }
}

std::string Ears::transcribe(const std::vector<float>& pcm, float* no_speech) {
    std::vector<float> x(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) x[i] = pcm[i] / 32768.f;
    auto p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads = threads_;
    p.language = "en";
    p.no_context = true;
    p.print_progress = false;
    p.print_realtime = false;
    p.print_timestamps = false;
    p.print_special = false;
    p.suppress_blank = true;
    std::string prompt;  // must outlive whisper_full
    { std::lock_guard<std::mutex> lk(vocab_mu_); prompt = vocab_; }
    if (!prompt.empty()) p.initial_prompt = prompt.c_str();  // household names, not near-sounding words
    if (whisper_full(wctx_, p, x.data(), int(x.size())) != 0) return {};
    std::string text;
    float ns = 0;
    int n = whisper_full_n_segments(wctx_);
    for (int i = 0; i < n; ++i) {
        text += whisper_full_get_segment_text(wctx_, i);
        ns = std::max(ns, whisper_full_get_segment_no_speech_prob(wctx_, i));
    }
    if (no_speech) *no_speech = ns;
    auto b = text.find_first_not_of(" \t\n");
    auto e = text.find_last_not_of(" \t\n");
    return b == std::string::npos ? "" : text.substr(b, e - b + 1);
}

void Ears::sttLoop() {
    while (running_) {
        Pending item;
        {
            std::unique_lock<std::mutex> lk(qmu_);
            qcv_.wait_for(lk, std::chrono::milliseconds(200),
                          [this] { return !queue_.empty() || !running_; });
            if (queue_.empty()) continue;
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        std::string saved_path;
        if (!save_dir_.empty()) {  // 16 kHz mono WAV, rotating 10 files
            std::string path = save_dir_ + "/segment-" + std::to_string(save_n_++ % 10) + ".wav";
            saved_path = path;
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                uint32_t n = uint32_t(item.audio.size()), bytes = n * 2, rate = 16000, br = 32000;
                uint16_t one = 1, bits = 16, align = 2;
                uint32_t riff = 36 + bytes, fmtlen = 16;
                std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
                std::fwrite(&fmtlen, 4, 1, f); std::fwrite(&one, 2, 1, f); std::fwrite(&one, 2, 1, f);
                std::fwrite(&rate, 4, 1, f); std::fwrite(&br, 4, 1, f); std::fwrite(&align, 2, 1, f);
                std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
                for (float v : item.audio) {
                    int16_t x = int16_t(std::clamp(v, -32768.f, 32767.f));
                    std::fwrite(&x, 2, 1, f);
                }
                std::fclose(f);
                log("  saved " + path);
            }
        }
        float no_speech = 0;
        auto t0 = std::chrono::steady_clock::now();
        std::string text = transcribe(item.audio, &no_speech);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        char b[512];
        std::snprintf(b, sizeof b, "segment %.1fs level %.0f -> \"%s\" (no-speech %.2f, %.0f ms)",
                      item.audio.size() / 16000.0, item.loudness, text.c_str(), no_speech, ms);
        log(b);
        if (text.empty() || isJunk(text)) { log("  dropped: empty/noise"); continue; }
        if (no_speech > 0.6f) { log("  dropped: whisper thinks it's not speech"); continue; }
        if (realWordCount(text) == 0) { log("  dropped: no real words (filler/garble)"); continue; }
        Heard h;
        h.text = text;
        h.seconds = float(item.audio.size()) / 16000.f;
        h.loudness = item.loudness;
        if (!item.dir.ch[0].empty()) h.doa = measureCapture(item.dir);
        h.audio = std::make_shared<const std::vector<float>>(std::move(item.audio));
        if (on_heard_) on_heard_(h);
    }
}

}  // namespace mbot
