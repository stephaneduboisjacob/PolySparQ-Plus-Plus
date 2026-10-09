#pragma once
// Simple synchronous HTTP client wrapping libcurl.
// Used by the live executor and LLM advisor for REST calls.
//
// Thread-safe: each call creates its own CURL handle.
#include <string>
#include <unordered_map>

namespace vp {
namespace http {

using Headers = std::unordered_map<std::string, std::string>;

struct Response {
    int         status = 0;       // HTTP status code (0 = transport error)
    std::string body;
    std::string error;            // non-empty on transport error
};

// Synchronous POST with JSON body.
Response post(const std::string& url,
              const std::string& body,
              const Headers& extra_headers = {},
              int timeout_ms = 10000);

// Synchronous GET.
Response get(const std::string& url,
             const Headers& extra_headers = {},
             int timeout_ms = 10000);

// Synchronous DELETE with an optional JSON body (required by CLOB cancel APIs).
Response del(const std::string& url,
             const std::string& body,
             const Headers& extra_headers = {},
             int timeout_ms = 10000);

} // namespace http
} // namespace vp
