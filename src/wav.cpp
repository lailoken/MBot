#include "wav.hpp"

#include <algorithm>
#include <cstring>

namespace mbot {

namespace {
uint32_t u32(const std::string& b, size_t o) {
    return uint32_t(uint8_t(b[o])) | uint32_t(uint8_t(b[o + 1])) << 8 |
           uint32_t(uint8_t(b[o + 2])) << 16 | uint32_t(uint8_t(b[o + 3])) << 24;
}
uint16_t u16(const std::string& b, size_t o) {
    return uint16_t(uint8_t(b[o]) | uint8_t(b[o + 1]) << 8);
}
}  // namespace

bool loadWav(const std::string& b, Pcm& out, std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    if (b.size() < 12 || b.compare(0, 4, "RIFF") || b.compare(8, 4, "WAVE"))
        return fail("not a RIFF/WAVE file");
    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t pos = 12;
    while (pos + 8 <= b.size()) {
        std::string id = b.substr(pos, 4);
        uint32_t len = u32(b, pos + 4);
        size_t body = pos + 8;
        if (id == "fmt " && body + 16 <= b.size()) {
            fmt = u16(b, body);
            channels = u16(b, body + 2);
            rate = u32(b, body + 4);
            bits = u16(b, body + 14);
        } else if (id == "data") {
            if (fmt != 1 || bits != 16 || channels == 0) return fail("need 16-bit PCM WAV");
            // Streamed WAVs (e.g. from stdout) may carry a bogus data length.
            size_t avail = b.size() - body;
            size_t n = (len == 0 || len == 0xFFFFFFFF || len > avail) ? avail : len;
            size_t frames = n / (2 * channels);
            out.rate = rate;
            out.samples.resize(frames);
            for (size_t f = 0; f < frames; ++f) {
                int acc = 0;
                for (uint16_t c = 0; c < channels; ++c)
                    acc += int16_t(u16(b, body + (f * channels + c) * 2));
                out.samples[f] = int16_t(acc / channels);
            }
            return true;
        }
        pos = body + len + (len & 1);
    }
    return fail("no data chunk");
}

Pcm resample(const Pcm& in, uint32_t rate) {
    if (in.rate == rate || in.samples.empty()) return in;
    Pcm out;
    out.rate = rate;
    size_t n = size_t(double(in.samples.size()) * rate / in.rate);
    out.samples.resize(n);
    double step = double(in.rate) / rate;
    for (size_t i = 0; i < n; ++i) {
        double src = i * step;
        size_t a = size_t(src);
        size_t bidx = std::min(a + 1, in.samples.size() - 1);
        double t = src - a;
        out.samples[i] = int16_t(in.samples[a] * (1 - t) + in.samples[bidx] * t);
    }
    return out;
}

std::string makeWav(const Pcm& pcm) {
    auto put32 = [](std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o += char((v >> (8 * i)) & 0xff); };
    auto put16 = [](std::string& o, uint16_t v) { o += char(v & 0xff); o += char(v >> 8); };
    uint32_t bytes = uint32_t(pcm.samples.size() * 2);
    std::string o = "RIFF";
    put32(o, 36 + bytes);
    o += "WAVEfmt ";
    put32(o, 16); put16(o, 1); put16(o, 1); put32(o, pcm.rate); put32(o, pcm.rate * 2); put16(o, 2); put16(o, 16);
    o += "data";
    put32(o, bytes);
    for (int16_t v : pcm.samples) put16(o, uint16_t(v));
    return o;
}

}  // namespace mbot
