#include "tls_probe.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstdio>
#include <memory>

namespace mbot {

namespace {
int connectTcp(const std::string& host, int port, int timeout_sec, std::string& err) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0) {
        err = std::string("resolve failed: ") + gai_strerror(rc);
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
    if (fd < 0) err = "could not connect to " + host + ":" + std::to_string(port);
    return fd;
}

std::string sslError() {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
    return buf;
}
}  // namespace

ProbeResult probeRobotCert(const std::string& host, int port, int timeout_sec) {
    ProbeResult r;
    int fd = connectTcp(host, port, timeout_sec, r.error);
    if (fd < 0) return r;

    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx(SSL_CTX_new(TLS_client_method()),
                                                          SSL_CTX_free);
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);  // TOFU: we inspect it ourselves
    // gRPC servers require ALPN "h2"; some refuse the handshake without it.
    static const unsigned char alpn[] = {2, 'h', '2'};
    SSL_CTX_set_alpn_protos(ctx.get(), alpn, sizeof alpn);

    std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ctx.get()), SSL_free);
    SSL_set_fd(ssl.get(), fd);
    if (SSL_connect(ssl.get()) != 1) {
        r.error = "TLS handshake failed: " + sslError();
        close(fd);
        return r;
    }

    X509* cert = SSL_get1_peer_certificate(ssl.get());
    if (!cert) {
        r.error = "robot presented no certificate";
        SSL_shutdown(ssl.get());
        close(fd);
        return r;
    }

    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    r.cert_pem.assign(data, len);
    BIO_free(bio);

    char cn[256] = {0};
    X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof cn);
    r.common_name = cn;

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    X509_digest(cert, EVP_sha256(), md, &mdlen);
    for (unsigned i = 0; i < mdlen; ++i) {
        char hex[4];
        std::snprintf(hex, sizeof hex, i ? ":%02X" : "%02X", md[i]);
        r.fingerprint += hex;
    }

    X509_free(cert);
    SSL_shutdown(ssl.get());
    close(fd);
    r.ok = true;
    return r;
}

}  // namespace mbot
