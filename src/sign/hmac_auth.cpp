#include "hmac_auth.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace vp {
namespace hmac_auth {

namespace {

// Base64URL decode (no padding required)
std::vector<uint8_t> b64url_decode(const std::string& input) {
    // Convert base64url to standard base64
    std::string b64 = input;
    for (char& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // Add padding
    while (b64.size() % 4) b64 += '=';

    std::vector<uint8_t> out;
    BIO* b64_bio = BIO_new(BIO_f_base64());
    BIO* mem_bio = BIO_new_mem_buf(b64.data(), (int)b64.size());
    mem_bio = BIO_push(b64_bio, mem_bio);
    BIO_set_flags(mem_bio, BIO_FLAGS_BASE64_NO_NL);
    out.resize(b64.size()); // upper bound
    int n = BIO_read(mem_bio, out.data(), (int)out.size());
    BIO_free_all(mem_bio);
    if (n > 0) out.resize(n);
    else out.clear();
    return out;
}

// Base64URL encode (no padding, URL-safe)
std::string b64url_encode(const uint8_t* data, size_t len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, (int)len);
    BIO_flush(b64);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    // Convert to base64url
    for (char& c : result) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    // NOTE: keep '=' padding. Polymarket CLOB requires the padded form
    // (matches python's base64.urlsafe_b64encode which preserves padding).
    // Stripping the trailing '=' yields HTTP 401 "Unauthorized/Invalid api key".
    return result;
}

// HMAC-SHA256
std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                  const std::string& msg) {
    unsigned int out_len = 32;
    std::vector<uint8_t> out(out_len);
    HMAC(EVP_sha256(),
         key.data(), (int)key.size(),
         (const unsigned char*)msg.data(), msg.size(),
         out.data(), &out_len);
    out.resize(out_len);
    return out;
}

} // namespace

Headers build(const std::string& address,
              const std::string& api_key,
              const std::string& api_secret,
              const std::string& api_passphrase,
              const std::string& method,
              const std::string& path,
              const std::string& body) {
    using namespace std::chrono;
    int64_t ts = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    std::string ts_str = std::to_string(ts);

    // Build message: timestamp + method + path [+ body]
    // NOTE: body single-quotes replaced with double-quotes (py_clob_client behavior)
    std::string msg = ts_str + method + path;
    if (!body.empty()) {
        std::string b = body;
        for (char& c : b) if (c == '\'') c = '"';
        msg += b;
    }

    // HMAC-SHA256 with base64url-decoded secret
    std::vector<uint8_t> key;
    try {
        key = b64url_decode(api_secret);
    } catch (...) {
        spdlog::error("hmac_auth: failed to decode api_secret");
        key = std::vector<uint8_t>(api_secret.begin(), api_secret.end());
    }
    auto mac = hmac_sha256(key, msg);
    std::string sig = b64url_encode(mac.data(), mac.size());

    Headers h;
    h["POLY_ADDRESS"]    = address;
    h["POLY_TIMESTAMP"]  = ts_str;
    h["POLY_SIGNATURE"]  = sig;
    h["POLY_API_KEY"]    = api_key;
    h["POLY_PASSPHRASE"] = api_passphrase;
    return h;
}

} // namespace hmac_auth
} // namespace vp
