#include "doa.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#include <nlohmann/json.hpp>

namespace mbot {
using json = nlohmann::json;
using cf = std::complex<float>;

namespace {
constexpr int kPkt = 1293, kCh = 160, kRate = 15625;
constexpr int kN = 1024, kHop = 512;
constexpr int kPairs[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
constexpr float kPi = 3.14159265358979f;

int dial(const std::string& host, int port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (auto* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        timeval tv{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        timeval rtv{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
        linger lg{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

// In-place radix-2 FFT (n a power of two).
void fft(std::vector<cf>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = -2 * kPi / float(len);
        cf wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cf w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                cf u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

float wrapDeg(float d) {
    while (d > 180) d -= 360;
    while (d <= -180) d += 360;
    return d;
}
}  // namespace

// ---------------------------------------------------------------- measuring --

DoaObs measureDoa(const std::vector<int16_t> ch[4]) {
    DoaObs o;
    o.at = std::time(nullptr);
    size_t len = std::min({ch[0].size(), ch[1].size(), ch[2].size(), ch[3].size()});
    if (len < size_t(kN * 3)) return o;
    const int kLo = int(250.f * kN / kRate), kHi = int(5000.f * kN / kRate);
    const int nb = kHi - kLo;
    std::vector<float> win(kN);
    for (int i = 0; i < kN; ++i) win[i] = 0.5f - 0.5f * std::cos(2 * kPi * i / (kN - 1));

    // Spectra of every frame, per channel (band only).
    size_t nf = (len - kN) / kHop + 1;
    std::vector<std::array<std::vector<cf>, 4>> spec(nf);
    std::vector<float> energy(nf, 0.f);
    std::vector<cf> buf(kN);
    for (size_t f = 0; f < nf; ++f) {
        for (int c = 0; c < 4; ++c) {
            double mean = 0;
            for (int i = 0; i < kN; ++i) mean += ch[c][f * kHop + i];
            mean /= kN;
            for (int i = 0; i < kN; ++i) buf[i] = cf(float(ch[c][f * kHop + i] - mean) * win[i], 0.f);
            fft(buf);
            spec[f][c].assign(buf.begin() + kLo, buf.begin() + kHi);
            for (auto& x : spec[f][c]) energy[f] += std::norm(x);
        }
    }
    // Noise: the quietest third of the frames. Speech: frames well above it.
    std::vector<float> sorted = energy;
    std::sort(sorted.begin(), sorted.end());
    float floor_e = sorted[sorted.size() / 3];
    std::vector<float> noise(nb, 0.f);
    int nq = 0;
    for (size_t f = 0; f < nf; ++f)
        if (energy[f] <= floor_e) {
            ++nq;
            for (int k = 0; k < nb; ++k)
                for (int c = 0; c < 4; ++c) noise[k] += std::norm(spec[f][c][k]) / 4;
        }
    for (auto& v : noise) v /= std::max(1, nq);

    std::array<std::vector<cf>, 6> G;
    for (auto& g : G) g.assign(nb, cf(0));
    int used = 0;
    for (size_t f = 0; f < nf; ++f) {
        if (energy[f] < 4.f * floor_e) continue;
        ++used;
        for (int p = 0; p < 6; ++p) {
            auto& a = spec[f][kPairs[p][0]];
            auto& b = spec[f][kPairs[p][1]];
            for (int k = 0; k < nb; ++k) {
                float pk = (std::norm(a[k]) + std::norm(b[k])) / 2;
                float snr_w = std::max(0.f, 1.f - noise[k] / std::max(pk, 1e-9f));  // bins drowned in noise count little
                cf x = a[k] * std::conj(b[k]);
                float m = std::abs(x);
                if (m > 1e-9f) G[p][k] += x / m * snr_w;
            }
        }
    }
    o.frames = used;
    if (used < 4) return o;

    // Cross-correlation at fractional lags: r(l) = Re sum_k G_k e^{+i w_k l}. Coarse, then fine.
    auto corr = [&](int p, float lag) {
        float r = 0;
        for (int k = 0; k < nb; ++k) {
            float w = 2 * kPi * float(k + kLo) / kN * lag;
            r += G[p][k].real() * std::cos(w) - G[p][k].imag() * std::sin(w);
        }
        return r;
    };
    float peak_sum = 0;
    for (int p = 0; p < 6; ++p) {
        float norm = 0;
        for (auto& g : G[p]) norm += std::abs(g);
        float best = -1e9f, bl = 0;
        for (float l = -6.f; l <= 6.f; l += 0.05f) {
            float r = corr(p, l);
            if (r > best) { best = r; bl = l; }
        }
        for (float l = bl - 0.05f; l <= bl + 0.05f; l += 0.005f) {
            float r = corr(p, l);
            if (r > best) { best = r; bl = l; }
        }
        o.tau[p] = bl;  // + = the sound reached the pair's first mic later
        peak_sum += norm > 0 ? best / norm : 0;
    }
    // The 6 pairs come from 3 independent delays: a real sound adds up (01+12=02 ...).
    float e = std::fabs(o.tau[0] + o.tau[3] - o.tau[1]) + std::fabs(o.tau[0] + o.tau[4] - o.tau[2]) +
              std::fabs(o.tau[1] + o.tau[5] - o.tau[2]);
    o.quality = std::clamp(peak_sum / 6, 0.f, 1.f) * std::exp(-e / 3 / 0.15f);
    o.ok = o.quality > 0.15f;
    return o;
}

// ----------------------------------------------------------- the network --

DirectionFinder::~DirectionFinder() { stop(); }

void DirectionFinder::start(const std::string& host, int port) {
    if (running_.exchange(true)) return;
    host_ = host;
    port_ = port;
    th_ = std::thread([this] { loop(); });
}

void DirectionFinder::stop() {
    if (!running_.exchange(false)) return;
    if (int fd = fd_.exchange(-1); fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); }
    if (th_.joinable()) th_.join();
}

void DirectionFinder::loop() {
    bool warned = false;
    while (running_) {
        int fd = dial(host_, port_);
        if (fd < 0) {
            if (!warned) std::fprintf(stderr, "[direction] robot's raw mic server (:%d) unreachable - retrying\n", port_);
            warned = true;
            connected_ = false;
            for (int i = 0; i < 50 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (warned) std::fprintf(stderr, "[direction] connected\n");
        warned = false;
        fd_ = fd;
        connected_ = true;
        std::vector<unsigned char> pend;
        unsigned char buf[16384];
        while (running_) {
            ssize_t r = recv(fd, buf, sizeof buf, 0);
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (r <= 0) break;
            pend.insert(pend.end(), buf, buf + r);
            size_t at = 0;
            while (pend.size() - at >= size_t(kPkt)) {
                if (pend[at] != 0xf2) { ++at; continue; }  // resync
                const unsigned char* p = pend.data() + at;
                if (want_) {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (ch_[0].size() < size_t(kRate * 20)) {
                        for (int c = 0; c < 4; ++c)
                            for (int i = 0; i < kCh; ++i) {
                                int16_t v;
                                std::memcpy(&v, p + 1 + 2 * (c * kCh + i), 2);
                                ch_[c].push_back(v);
                            }
                        uint32_t st;
                        std::memcpy(&st, p + 1285, 4);
                        ++pkts_;
                        if (st & 0x1) ++moving_pkts_;  // ROBOT_STATUS_IS_MOVING
                    }
                }
                at += kPkt;
            }
            pend.erase(pend.begin(), pend.begin() + at);
        }
        connected_ = false;
        if (int f = fd_.exchange(-1); f >= 0) close(f);
    }
}

void DirectionFinder::begin() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& c : ch_) c.clear();
        pkts_ = moving_pkts_ = 0;
    }
    want_ = true;
    int fd = fd_.load();
    if (fd >= 0) (void)!send(fd, "S", 1, MSG_NOSIGNAL);
}

DoaCapture DirectionFinder::end() {
    int fd = fd_.load();
    if (fd >= 0) (void)!send(fd, "E", 1, MSG_NOSIGNAL);
    want_ = false;
    DoaCapture c;
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < 4; ++i) c.ch[i].swap(ch_[i]);
    c.pkts = pkts_;
    c.moving_pkts = moving_pkts_;
    return c;
}

DoaObs measureCapture(const DoaCapture& c) {
    DoaObs o = measureDoa(c.ch);
    o.moving = c.pkts > 0 && c.moving_pkts * 2 > c.pkts;
    return o;
}

// -------------------------------------------------------------- the model --

void DirectionModel::load(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    samples_.clear();
    std::ifstream f(path);
    if (f) {
        try {
            json j;
            f >> j;
            for (auto& s : j.value("samples", json::array())) {
                Sample x;
                auto t = s.value("tau", std::vector<float>{});
                if (t.size() != 6) continue;
                std::copy(t.begin(), t.end(), x.tau.begin());
                x.bearing = s.value("bearing", 0.f);
                x.weight = s.value("weight", 1.f);
                x.at = s.value("at", int64_t(0));
                samples_.push_back(x);
            }
        } catch (...) {
        }
    }
    fit();
}

void DirectionModel::save() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (path_.empty()) return;
    json arr = json::array();
    for (auto& s : samples_) {
        std::vector<float> t(s.tau.begin(), s.tau.end());
        for (auto& v : t) v = std::round(v * 1000) / 1000;
        arr.push_back({{"tau", t}, {"bearing", std::round(s.bearing * 10) / 10}, {"weight", s.weight}, {"at", s.at}});
    }
    std::string tmp = path_ + ".tmp";
    { std::ofstream f(tmp); f << json{{"samples", arr}}.dump() << "\n"; }
    std::rename(tmp.c_str(), path_.c_str());
}

void DirectionModel::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    samples_.clear();
    ready_ = false;
}

void DirectionModel::addSample(const DoaObs& o, float bearing_deg, float weight) {
    if (!o.ok) return;
    std::lock_guard<std::mutex> lk(mu_);
    samples_.push_back({o.tau, wrapDeg(bearing_deg), std::clamp(weight * o.quality, 0.05f, 1.f), o.at});
    if (samples_.size() > 300) samples_.erase(samples_.begin());
    fit();
}

void DirectionModel::fit() {
    // Weighted least squares A = (sum w tau u^T)(sum w u u^T)^-1, twice: the second time without the
    // samples the first fit says were someone else talking (off by a lot).
    ready_ = false;
    if (samples_.size() < 12) return;
    std::vector<bool> use(samples_.size(), true);
    for (int pass = 0; pass < 2; ++pass) {
        double M[2][2] = {{0, 0}, {0, 0}}, C[6][2] = {};
        double sw = 0, mb = 0, mb2 = 0;
        for (size_t i = 0; i < samples_.size(); ++i) {
            if (!use[i]) continue;
            auto& s = samples_[i];
            double b = s.bearing * kPi / 180, u[2] = {std::cos(b), std::sin(b)}, w = s.weight;
            for (int r = 0; r < 2; ++r)
                for (int c = 0; c < 2; ++c) M[r][c] += w * u[r] * u[c];
            for (int p = 0; p < 6; ++p)
                for (int c = 0; c < 2; ++c) C[p][c] += w * s.tau[p] * u[c];
            sw += w; mb += w * s.bearing; mb2 += w * s.bearing * s.bearing;
        }
        if (sw <= 0) return;
        double spread = std::sqrt(std::max(0.0, mb2 / sw - (mb / sw) * (mb / sw)));
        double det = M[0][0] * M[1][1] - M[0][1] * M[1][0];
        if (spread < 7.0 || std::fabs(det) < 1e-6 * sw * sw) return;  // all from one direction: can't tell yet
        double Mi[2][2] = {{M[1][1] / det, -M[0][1] / det}, {-M[1][0] / det, M[0][0] / det}};
        for (int p = 0; p < 6; ++p)
            for (int c = 0; c < 2; ++c) A_[p][c] = float(C[p][0] * Mi[0][c] + C[p][1] * Mi[1][c]);
        ready_ = true;  // provisionally, so predict() works for the error check
        std::vector<float> err;
        for (size_t i = 0; i < samples_.size(); ++i) {
            DoaObs o;
            o.ok = true; o.quality = 1; o.tau = samples_[i].tau;
            float b, c;
            float e = predictLocked(o, b, c) ? std::fabs(wrapDeg(b - samples_[i].bearing)) : 180.f;
            err.push_back(e);
        }
        std::vector<float> se = err;
        std::sort(se.begin(), se.end());
        float med = se[se.size() / 2];
        fit_err_deg_ = med;
        if (pass == 0)
            for (size_t i = 0; i < err.size(); ++i) use[i] = err[i] <= std::max(25.f, 3 * med);
    }
    if (fit_err_deg_ > 25.f) ready_ = false;  // not consistent enough to trust yet
}

bool DirectionModel::predict(const DoaObs& o, float& bearing_deg, float& confidence) const {
    std::lock_guard<std::mutex> lk(mu_);
    return predictLocked(o, bearing_deg, confidence);
}

bool DirectionModel::predictLocked(const DoaObs& o, float& bearing_deg, float& confidence) const {
    if (!ready_ || !o.ok) return false;
    // Least squares u = (A^T A)^-1 A^T tau.
    double AtA[2][2] = {}, Att[2] = {};
    for (int p = 0; p < 6; ++p)
        for (int r = 0; r < 2; ++r) {
            Att[r] += A_[p][r] * o.tau[p];
            for (int c = 0; c < 2; ++c) AtA[r][c] += A_[p][r] * A_[p][c];
        }
    double det = AtA[0][0] * AtA[1][1] - AtA[0][1] * AtA[1][0];
    if (std::fabs(det) < 1e-12) return false;
    double u0 = (AtA[1][1] * Att[0] - AtA[0][1] * Att[1]) / det;
    double u1 = (-AtA[1][0] * Att[0] + AtA[0][0] * Att[1]) / det;
    double res = 0, nt = 0;
    for (int p = 0; p < 6; ++p) {
        double d = o.tau[p] - (A_[p][0] * u0 + A_[p][1] * u1);
        res += d * d;
        nt += double(o.tau[p]) * o.tau[p];
    }
    double rel = std::sqrt(res) / std::max(0.05, std::sqrt(nt));
    double mag = std::hypot(u0, u1);
    bearing_deg = float(std::atan2(u1, u0) * 180 / kPi);
    float c = o.quality * float(std::clamp(1.0 - rel * 1.5, 0.0, 1.0));
    if (mag < 0.35 || mag > 1.7) c *= 0.4f;  // too faint / too strong for a voice in the room: an echo?
    if (o.moving) c *= 0.5f;
    c *= std::clamp(1.f - fit_err_deg_ / 45.f, 0.2f, 1.f);
    confidence = std::clamp(c, 0.f, 1.f);
    return true;
}

std::string DirectionModel::summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    char b[160];
    if (ready_)
        std::snprintf(b, sizeof b, "learned (%zu samples, typical error %.0f deg)", samples_.size(), fit_err_deg_);
    else
        std::snprintf(b, sizeof b, "still learning (%zu samples; needs ~12+ from a few directions, from people "
                      "talking while he sees their face)", samples_.size());
    return b;
}

}  // namespace mbot
