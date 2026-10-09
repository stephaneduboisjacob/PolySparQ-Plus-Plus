#pragma once
// HMAC-SHA256 auth headers for the Polymarket CLOB REST API.
//
// Header spec from py_clob_client/signing/hmac.py:
//   POLY_ADDRESS    = signer / maker address
//   POLY_TIMESTAMP  = unix epoch seconds (string)
//   POLY_SIGNATURE  = base64url(HMAC-SHA256(base64url-decode(secret),
//                               timestamp + method + path [+ body]))
//   POLY_API_KEY    = api key
//   POLY_PASSPHRASE = api passphrase
//
// Thread-safe.
#include <string>
#include <unordered_map>

namespace vp {
namespace hmac_auth {

using Headers = std::unordered_map<std::string, std::string>;

// Build CLOB auth headers for a REST request.
// method: "GET" / "POST" / "DELETE" (uppercase)
// path:   e.g. "/order"
// body:   request body as string (empty for GET)
// secret: base64url-encoded HMAC secret (as stored in config)
Headers build(const std::string& address,
              const std::string& api_key,
              const std::string& api_secret,
              const std::string& api_passphrase,
              const std::string& method,
              const std::string& path,
              const std::string& body = "");

} // namespace hmac_auth
} // namespace vp
