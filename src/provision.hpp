#pragma once
// Getting what the brain needs, on demand: model files it can download itself, and local (Ollama) models it
// can pull. So a fresh install only needs the program; the rest arrives the first time it's wanted.
#include <string>
#include <vector>

namespace mbot {

// Where a known model file comes from ("" if unknown). Matches by file name.
std::string knownModelUrl(const std::string& path);
// Download url -> dest (atomically: dest only appears when complete). Uses curl, no shell. Blocking.
bool downloadFile(const std::string& url, const std::string& dest, std::string* err);
// If `path` is missing and it's a known model, download it (blocking). True if the file is there afterwards.
bool ensureModelFile(const std::string& path, const std::string& what);
// Pull any of these Ollama models that the local server doesn't have yet (blocking; run it on a thread).
// base: e.g. "http://localhost:11434" (a trailing "/v1" is fine). Logs what it does.
void ensureOllamaModels(const std::string& base, const std::vector<std::string>& models);

}  // namespace mbot
