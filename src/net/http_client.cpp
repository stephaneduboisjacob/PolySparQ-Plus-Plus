#include "http_client.hpp"
#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <cstring>

namespace vp {
namespace http {

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    auto* buf = static_cast<std::string*>(user);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

Response do_request(const std::string& url,
                    const std::string* body,   // nullptr => GET
                    const char* method,
                    const Headers& extra,
                    int timeout_ms) {
    Response r;
    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl_easy_init failed"; return r; }

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    for (auto& [k, v] : extra)
        hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &r.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,     (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)std::min(timeout_ms, 5000));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
    if (method && std::strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body->size());
        }
    } else if (body) {
        curl_easy_setopt(curl, CURLOPT_POST,       1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body->size());
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        r.error = curl_easy_strerror(res);
    } else {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        r.status = (int)code;
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return r;
}

} // namespace

Response post(const std::string& url, const std::string& body,
              const Headers& extra, int timeout_ms) {
    return do_request(url, &body, "POST", extra, timeout_ms);
}

Response get(const std::string& url, const Headers& extra, int timeout_ms) {
    return do_request(url, nullptr, "GET", extra, timeout_ms);
}

Response del(const std::string& url, const std::string& body,
             const Headers& extra, int timeout_ms) {
    return do_request(url, &body, "DELETE", extra, timeout_ms);
}

} // namespace http
} // namespace vp
