#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace mbot {

namespace {
std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
}  // namespace

std::string homeDir() {
    const char* h = std::getenv("HOME");
    return h ? h : ".";
}

std::string defaultConfigPath() {
    if (const char* p = std::getenv("MBOT_CONFIG")) return p;
    return homeDir() + "/.anki_vector/sdk_config.ini";
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeFile(const std::string& path, const std::string& data) {
    std::error_code ec;
    auto parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << data;
    return bool(f);
}

std::string RobotConfig::readCert() const { return readFile(cert_path); }

std::vector<RobotConfig> loadConfigs(const std::string& path) {
    std::vector<RobotConfig> out;
    std::istringstream in(readFile(path));
    std::string line;
    RobotConfig* cur = nullptr;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            out.push_back({});
            cur = &out.back();
            cur->serial = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (!cur || eq == std::string::npos) continue;
        auto key = trim(line.substr(0, eq));
        auto val = trim(line.substr(eq + 1));
        if (key == "cert") cur->cert_path = val;
        else if (key == "ip") cur->ip = val;
        else if (key == "name") cur->name = val;
        else if (key == "guid") cur->guid = val;
        else if (key == "port") cur->port = std::stoi(val);
    }
    return out;
}

bool saveConfig(const RobotConfig& cfg, const std::string& path) {
    auto all = loadConfigs(path);
    bool replaced = false;
    for (auto& c : all) {
        bool same = (!cfg.serial.empty() && c.serial == cfg.serial) ||
                    (cfg.serial.empty() && c.name == cfg.name);
        if (same) {
            c = cfg;
            if (c.serial.empty()) c.serial = cfg.name;
            replaced = true;
        }
    }
    if (!replaced) {
        all.push_back(cfg);
        if (all.back().serial.empty()) all.back().serial = cfg.name;
    }
    std::ostringstream ss;
    for (auto& c : all) {
        ss << "[" << c.serial << "]\n"
           << "cert = " << c.cert_path << "\n"
           << "ip = " << c.ip << "\n"
           << "name = " << c.name << "\n"
           << "guid = " << c.guid << "\n";
        if (c.port != 443) ss << "port = " << c.port << "\n";
        ss << "\n";
    }
    return writeFile(path, ss.str());
}

bool findRobot(const std::string& selector, RobotConfig& out, const std::string& path) {
    auto all = loadConfigs(path);
    for (auto& c : all) {
        if (selector.empty() || c.serial == selector || c.name == selector || c.ip == selector) {
            out = c;
            return true;
        }
    }
    return false;
}

}  // namespace mbot
