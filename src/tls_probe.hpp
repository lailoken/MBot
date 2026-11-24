#pragma once
#include <string>

namespace mbot {

struct ProbeResult {
    bool ok = false;
    std::string error;
    std::string cert_pem;     // robot's self-signed certificate
    std::string common_name;  // e.g. "Vector-A1B2" -> used as TLS target name
    std::string fingerprint;  // SHA-256, hex
};

// Trust-on-first-use: open a TLS connection to the robot's gRPC port and grab
// the self-signed certificate it presents. We pin this cert afterwards, which
// is exactly what the official SDK does with the cert it downloads from the
// cloud — but this needs no cloud at all.
ProbeResult probeRobotCert(const std::string& host, int port = 443, int timeout_sec = 5);

}  // namespace mbot
