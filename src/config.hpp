#pragma once
#include <string>
#include <vector>

namespace mbot {

// One robot entry. Stored in the same INI format the Anki / wire-pod Python SDK
// uses (~/.anki_vector/sdk_config.ini), so robots set up with either tool work
// here, and robots set up here work with the Python SDK.
//
//   [00e20145]              <- serial (ESN)
//   cert = /home/me/.anki_vector/Vector-A1B2-00e20145.cert
//   ip = 192.168.1.50
//   name = Vector-A1B2
//   guid = <client token from UserAuthentication>
struct RobotConfig {
    std::string serial;
    std::string name;      // TLS certificate CN, e.g. "Vector-A1B2"
    std::string ip;
    std::string cert_path;
    std::string guid;
    int port = 443;        // gRPC port (unlocked firmware may serve plaintext gRPC on 9090)

    std::string target() const { return ip + ":" + std::to_string(port); }
    // No cert => plaintext (unlocked / community firmware with an open gateway).
    bool insecure() const { return cert_path.empty() || cert_path == "insecure"; }
    std::string readCert() const;
};

std::string defaultConfigPath();
std::vector<RobotConfig> loadConfigs(const std::string& path = defaultConfigPath());
// Insert or replace (matched by serial, else name).
bool saveConfig(const RobotConfig& cfg, const std::string& path = defaultConfigPath());
// Pick by serial or name; empty selector = first robot.
bool findRobot(const std::string& selector, RobotConfig& out,
               const std::string& path = defaultConfigPath());

std::string readFile(const std::string& path);
bool writeFile(const std::string& path, const std::string& data);
std::string homeDir();

}  // namespace mbot
