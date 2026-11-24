#include "net.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace mbot {

namespace {

struct Url {
    bool tls = false;
    std::string host;
    int port = 80;
    std::string path = "/";
};

bool parseUrl(const std::string& u, Url& out) {
    std::string rest;
    if (u.rfind("https://", 0) == 0) {
        out.tls = true;
        out.port = 443;
        rest = u.substr(8);
    } else if (u.rfind("http://", 0) == 0) {
        rest = u.substr(7);
    } else {
        return false;
    }
    auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        out.port = std::atoi(hostport.c_str() + colon + 1);
    } else {
        out.host = hostport;
    }
    return !out.host.empty();
}

int connectTcp(const std::string& host, int port, int timeout_sec, std::string& err) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        err = "cannot resolve " + host;
        return -1;
    }
    int fd = -1;
    for (auto* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        timeval tv{timeout_sec, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) err = "cannot connect to " + host + ":" + std::to_string(port);
    return fd;
}

// Byte pipe over either a raw socket or a TLS session.
class Conn {
public:
    ~Conn() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }
        if (ctx_) SSL_CTX_free(ctx_);
        if (fd_ >= 0) close(fd_);
    }
    bool open(const Url& u, int timeout_sec, std::string& err) {
        fd_ = connectTcp(u.host, u.port, timeout_sec, err);
        if (fd_ < 0) return false;
        if (!u.tls) return true;
        ctx_ = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_default_verify_paths(ctx_);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        ssl_ = SSL_new(ctx_);
        SSL_set_tlsext_host_name(ssl_, u.host.c_str());  // SNI
        SSL_set1_host(ssl_, u.host.c_str());             // hostname verification
        SSL_set_fd(ssl_, fd_);
        if (SSL_connect(ssl_) != 1) {
            char buf[256];
            ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
            err = std::string("TLS handshake with ") + u.host + " failed: " + buf;
            return false;
        }
        return true;
    }
    bool writeAll(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            long n = ssl_ ? SSL_write(ssl_, s.data() + off, int(s.size() - off))
                          : send(fd_, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += size_t(n);
        }
        return true;
    }
    // Returns bytes read, 0 on clean EOF, -1 on error.
    long read(char* buf, size_t len) {
        if (ssl_) {
            int n = SSL_read(ssl_, buf, int(len));
            if (n > 0) return n;
            int e = SSL_get_error(ssl_, n);
            return (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL) ? 0 : -1;
        }
        return recv(fd_, buf, len, 0);
    }

private:
    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
};

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool decodeChunked(const std::string& in, std::string& out) {
    size_t pos = 0;
    while (pos < in.size()) {
        auto eol = in.find("\r\n", pos);
        if (eol == std::string::npos) return false;
        size_t n = std::strtoul(in.substr(pos, eol - pos).c_str(), nullptr, 16);
        pos = eol + 2;
        if (n == 0) return true;
        if (pos + n > in.size()) return false;
        out.append(in, pos, n);
        pos += n + 2;
    }
    return true;
}

}  // namespace

namespace {
HttpResponse httpRequest(const char* method, const std::string& url, const std::string& body,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         int timeout_sec, const std::function<bool(const std::string&)>* on_body = nullptr);
}

HttpResponse httpPostStream(const std::string& url, const std::string& body,
                            const std::vector<std::pair<std::string, std::string>>& headers, int timeout_sec,
                            const std::function<bool(const std::string&)>& on_body) {
    return httpRequest("POST", url, body, headers, timeout_sec, &on_body);
}

HttpResponse httpPost(const std::string& url, const std::string& body,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      int timeout_sec) {
    return httpRequest("POST", url, body, headers, timeout_sec);
}

HttpResponse httpGet(const std::string& url, int timeout_sec) {
    return httpRequest("GET", url, "", {}, timeout_sec);
}

namespace {
HttpResponse httpRequest(const char* method, const std::string& url, const std::string& body,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         int timeout_sec, const std::function<bool(const std::string&)>* on_body) {
    HttpResponse r;
    Url u;
    if (!parseUrl(url, u)) {
        r.error = "bad URL: " + url;
        return r;
    }
    Conn conn;
    if (!conn.open(u, timeout_sec, r.error)) return r;

    std::string req = std::string(method) + " " + u.path + " HTTP/1.1\r\nHost: " + u.host +
                      "\r\nConnection: close\r\n";
    if (std::strcmp(method, "GET") != 0)
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    bool has_ct = false;
    for (auto& [k, v] : headers) {
        req += k + ": " + v + "\r\n";
        if (lower(k) == "content-type") has_ct = true;
    }
    if (!has_ct && std::strcmp(method, "GET") != 0) req += "Content-Type: application/json\r\n";
    req += "\r\n" + body;
    if (!conn.writeAll(req)) {
        r.error = "send failed";
        return r;
    }

    std::string raw;
    char buf[16384];
    // Streaming: once the headers are in, hand over body bytes as they come (de-chunked).
    size_t body_at = std::string::npos, scan = 0;
    bool chunked = false;
    std::string streamed;
    bool aborted = false;
    auto pump = [&]() {
        if (!on_body || aborted) return;
        if (body_at == std::string::npos) {
            auto he = raw.find("\r\n\r\n");
            if (he == std::string::npos) return;
            body_at = scan = he + 4;
            chunked = lower(raw.substr(0, he)).find("transfer-encoding: chunked") != std::string::npos;
        }
        if (!chunked) {
            if (scan < raw.size()) { std::string piece = raw.substr(scan); scan = raw.size(); streamed += piece; if (!(*on_body)(piece)) aborted = true; }
            return;
        }
        for (;;) {
            auto eol = raw.find("\r\n", scan);
            if (eol == std::string::npos) return;
            size_t n = std::strtoul(raw.substr(scan, eol - scan).c_str(), nullptr, 16);
            if (n == 0) return;
            if (raw.size() < eol + 2 + n + 2) return;  // the rest of this chunk isn't here yet
            std::string piece = raw.substr(eol + 2, n);
            scan = eol + 2 + n + 2;
            streamed += piece;
            if (!(*on_body)(piece)) { aborted = true; return; }
        }
    };
    for (;;) {
        long n = conn.read(buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;  // a signal, not a network error
        if (n < 0) {
            r.error = "receive failed or timed out";
            return r;
        }
        if (n == 0) break;
        raw.append(buf, size_t(n));
        pump();
        if (aborted) {
            r.error = "aborted";
            return r;
        }
    }
    auto hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        r.error = "malformed HTTP response";
        return r;
    }
    std::string hdrs = lower(raw.substr(0, hdr_end));
    auto sp = hdrs.find(' ');
    r.status = sp != std::string::npos ? std::atoi(hdrs.c_str() + sp + 1) : 0;
    std::string payload = raw.substr(hdr_end + 4);
    if (hdrs.find("transfer-encoding: chunked") != std::string::npos) {
        if (!decodeChunked(payload, r.body)) r.error = "bad chunked encoding";
    } else {
        r.body = std::move(payload);
    }
    return r;
}

}  // namespace

}  // namespace mbot
