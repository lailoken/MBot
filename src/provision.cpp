#include "provision.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <set>

#include <nlohmann/json.hpp>

#include "net.hpp"

extern char** environ;

namespace mbot {
using json = nlohmann::json;
namespace fs = std::filesystem;

std::string knownModelUrl(const std::string& path) {
    std::string f = fs::path(path).filename().string();
    if (f.rfind("ggml-", 0) == 0 && f.size() > 9 && f.substr(f.size() - 4) == ".bin")
        return "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/" + f;
    if (f == "speaker-campplus-en.onnx")
        return "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
               "wespeaker_en_voxceleb_CAM++_LM.onnx";
    return "";
}

bool downloadFile(const std::string& url, const std::string& dest, std::string* err) {
    std::error_code ec;
    fs::create_directories(fs::path(dest).parent_path(), ec);
    std::string part = dest + ".part";
    std::vector<std::string> args = {"curl", "-fsSL", "--retry", "3", "--connect-timeout", "20", "-o", part, url};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, "curl", nullptr, nullptr, argv.data(), environ) != 0) {
        if (err) *err = "curl not found (install curl)";
        return false;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || fs::file_size(part, ec) == 0) {
        fs::remove(part, ec);
        if (err) *err = "download failed: " + url;
        return false;
    }
    fs::rename(part, dest, ec);
    if (ec) { if (err) *err = ec.message(); return false; }
    return true;
}

bool ensureModelFile(const std::string& path, const std::string& what) {
    std::error_code ec;
    if (path.empty() || fs::exists(path, ec)) return !path.empty();
    std::string url = knownModelUrl(path);
    if (url.empty()) {
        std::fprintf(stderr, "[setup] %s missing (%s) and I don't know where to get it\n", what.c_str(), path.c_str());
        return false;
    }
    std::printf("[setup] %s missing - downloading it now (once): %s\n", what.c_str(), url.c_str());
    std::fflush(stdout);
    std::string err;
    if (!downloadFile(url, path, &err)) {
        std::fprintf(stderr, "[setup] couldn't get the %s: %s\n", what.c_str(), err.c_str());
        return false;
    }
    std::printf("[setup] %s ready: %s\n", what.c_str(), path.c_str());
    return true;
}

void ensureOllamaModels(const std::string& base_in, const std::vector<std::string>& models) {
    std::string base = base_in;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.size() > 3 && base.substr(base.size() - 3) == "/v1") base.resize(base.size() - 3);
    auto tags = httpGet(base + "/api/tags", 5);
    if (!tags.ok()) {
        std::fprintf(stderr, "[setup] no local model server at %s - his eyes and the free local mind need Ollama "
                             "(https://ollama.com; then it fetches the models by itself)\n", base.c_str());
        return;
    }
    std::set<std::string> have;
    try {
        for (auto& m : json::parse(tags.body).value("models", json::array())) {
            std::string n = m.value("name", "");
            have.insert(n);
            if (n.size() > 7 && n.substr(n.size() - 7) == ":latest") have.insert(n.substr(0, n.size() - 7));
        }
    } catch (...) {
    }
    std::set<std::string> done;
    for (auto& m : models) {
        if (m.empty() || m.rfind("claude", 0) == 0 || have.count(m) || done.count(m)) continue;
        done.insert(m);
        std::printf("[setup] local model %s not installed - pulling it (this can take a while)\n", m.c_str());
        std::fflush(stdout);
        auto r = httpPost(base + "/api/pull", json{{"model", m}, {"stream", false}}.dump(), {}, 6 * 3600);
        if (r.ok()) std::printf("[setup] local model %s ready\n", m.c_str());
        else std::fprintf(stderr, "[setup] pulling %s failed: %s\n", m.c_str(), r.error.empty() ? r.body.substr(0, 200).c_str() : r.error.c_str());
    }
}

}  // namespace mbot
