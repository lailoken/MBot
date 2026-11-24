#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mbot {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;  // transport error; empty on success
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// Minimal blocking HTTP/1.1 client for http:// and https:// URLs.
// HTTPS verifies the server certificate against the system CA store.
HttpResponse httpPost(const std::string& url, const std::string& body,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      int timeout_sec = 120);
HttpResponse httpGet(const std::string& url, int timeout_sec = 10);
// Like httpPost, but hands the response body to on_body as it arrives (e.g. server-sent events).
// The whole body is still returned at the end.
HttpResponse httpPostStream(const std::string& url, const std::string& body,
                            const std::vector<std::pair<std::string, std::string>>& headers, int timeout_sec,
                            const std::function<bool(const std::string&)>& on_body);  // return false to stop (error "aborted")

}  // namespace mbot
