#include "eip712.hpp"
#include "third_party/keccak256.hpp"

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <random>
#include <string_view>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace vp {
namespace eip712 {

namespace {

// ---- Hex utilities -------------------------------------------------------

std::array<uint8_t, 32> hex_to_bytes32(const std::string& hex) {
    // Accepts with or without 0x prefix
    std::string h = hex;
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    if (h.size() != 64) throw std::invalid_argument("hex_to_bytes32: bad length");
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 32; ++i) {
        auto c = [](char v) -> uint8_t {
            if (v >= '0' && v <= '9') return v - '0';
            if (v >= 'a' && v <= 'f') return 10 + v - 'a';
            if (v >= 'A' && v <= 'F') return 10 + v - 'A';
            throw std::invalid_argument("bad hex char");
        };
        out[i] = (c(h[i*2]) << 4) | c(h[i*2+1]);
    }
    return out;
}

std::array<uint8_t, 20> hex_to_addr(const std::string& addr) {
    std::string h = addr;
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    if (h.size() != 40) throw std::invalid_argument("hex_to_addr: bad length " + addr);
    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 20; ++i) {
        auto c = [](char v) -> uint8_t {
            if (v >= '0' && v <= '9') return v - '0';
            if (v >= 'a' && v <= 'f') return 10 + v - 'a';
            if (v >= 'A' && v <= 'F') return 10 + v - 'A';
            throw std::invalid_argument("bad hex char in addr");
        };
        out[i] = (c(h[i*2]) << 4) | c(h[i*2+1]);
    }
    return out;
}

std::string bytes_to_hex(const uint8_t* b, size_t n) {
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
        s += fmt::format("{:02x}", b[i]);
    return s;
}

// ---- ABI encoding helpers ------------------------------------------------
// EIP-712 ABI encoding: each slot is 32 bytes, big-endian.

void encode_uint256_from_str(const std::string& decimal, std::vector<uint8_t>& out) {
    // Parse big decimal (may not fit uint64). We need 256-bit big-endian.
    // Simple arbitrary-precision approach via hex conversion.
    // tokenId can be very large (256-bit token ID). We convert via string arithmetic.
    std::array<uint8_t, 32> val{};
    // Convert decimal string to big-endian bytes
    for (char dc : decimal) {
        if (dc < '0' || dc > '9') continue;
        int carry = dc - '0';
        for (int i = 31; i >= 0; --i) {
            int x = val[i] * 10 + carry;
            val[i] = x & 0xff;
            carry = x >> 8;
        }
    }
    out.insert(out.end(), val.begin(), val.end());
}

void encode_uint256(uint64_t v, std::vector<uint8_t>& out) {
    // 32 bytes, big-endian, zero-padded on the left
    for (int i = 0; i < 24; ++i) out.push_back(0);
    for (int i = 7; i >= 0; --i) out.push_back((v >> (i*8)) & 0xff);
}

void encode_uint8(uint8_t v, std::vector<uint8_t>& out) {
    for (int i = 0; i < 31; ++i) out.push_back(0);
    out.push_back(v);
}

void encode_address(const std::string& addr, std::vector<uint8_t>& out) {
    // 32 bytes: 12 zero bytes + 20-byte address
    auto a = hex_to_addr(addr);
    for (int i = 0; i < 12; ++i) out.push_back(0);
    out.insert(out.end(), a.begin(), a.end());
}

// keccak256 of a UTF-8 string (for type hashes)
std::array<uint8_t, 32> keccak_str(const char* s) {
    return keccak::hash(s, std::strlen(s));
}

// ---- EIP-712 type hashes -------------------------------------------------

// CLOB V2 order type string (186 bytes, also used as suffix in type-3 sig)
static constexpr const char kOrderTypeString[] =
    "Order(uint256 salt,address maker,address signer,uint256 tokenId,"
    "uint256 makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,"
    "uint256 timestamp,bytes32 metadata,bytes32 builder)";

static const std::array<uint8_t, 32> kOrderTypeHash = keccak_str(kOrderTypeString);

// EIP-712 domain separator:
// "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)"
static const std::array<uint8_t, 32> kDomainTypeHash = keccak_str(
    "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");

static const std::array<uint8_t, 32> kNameHash =
    keccak_str("Polymarket CTF Exchange");
static const std::array<uint8_t, 32> kVersionHash = keccak_str("2");

// Type-3 (POLY_1271) constants
// TypedDataSign type hash — includes nested Order type string
static const std::array<uint8_t, 32> kTdsTypeHash = keccak_str(
    "TypedDataSign(Order contents,string name,string version,uint256 chainId,"
    "address verifyingContract,bytes32 salt)"
    "Order(uint256 salt,address maker,address signer,uint256 tokenId,"
    "uint256 makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,"
    "uint256 timestamp,bytes32 metadata,bytes32 builder)");

// Deposit wallet EIP-712 domain constants
static const std::array<uint8_t, 32> kDwNameHash    = keccak_str("DepositWallet");
static const std::array<uint8_t, 32> kDwVersionHash = keccak_str("1");

// Compute the domain separator for a given exchange address
std::array<uint8_t, 32> domain_separator(const std::string& exchange_addr) {
    std::vector<uint8_t> enc;
    enc.reserve(5 * 32);
    // typeHash
    enc.insert(enc.end(), kDomainTypeHash.begin(), kDomainTypeHash.end());
    // name hash
    enc.insert(enc.end(), kNameHash.begin(), kNameHash.end());
    // version hash
    enc.insert(enc.end(), kVersionHash.begin(), kVersionHash.end());
    // chainId
    encode_uint256(kChainId, enc);
    // verifyingContract
    encode_address(exchange_addr, enc);
    return keccak::hash(enc.data(), enc.size());
}

// Compute the CLOB V2 order struct hash
std::array<uint8_t, 32> order_struct_hash(const OrderParams& p) {
    std::vector<uint8_t> enc;
    enc.reserve(12 * 32);
    // typeHash
    enc.insert(enc.end(), kOrderTypeHash.begin(), kOrderTypeHash.end());
    // salt
    encode_uint256_from_str(p.salt, enc);
    // maker
    encode_address(p.maker, enc);
    // signer
    encode_address(p.signer, enc);
    // tokenId (big decimal string — may be 256-bit)
    encode_uint256_from_str(p.token_id, enc);
    // makerAmount
    encode_uint256(p.maker_amount, enc);
    // takerAmount
    encode_uint256(p.taker_amount, enc);
    // side (uint8, padded to 32)
    encode_uint8((uint8_t)p.side, enc);
    // signatureType (uint8, padded to 32)
    encode_uint8((uint8_t)p.sig_type, enc);
    // timestamp (uint256)
    encode_uint256(p.timestamp, enc);
    // metadata (bytes32 — all zeros)
    for (int i = 0; i < 32; ++i) enc.push_back(0);
    // builder (bytes32 — from builder registration, or zeros if not set)
    if (!p.builder_code.empty()) {
        try {
            auto b = hex_to_bytes32(p.builder_code);
            enc.insert(enc.end(), b.begin(), b.end());
        } catch (...) {
            for (int i = 0; i < 32; ++i) enc.push_back(0);
        }
    } else {
        for (int i = 0; i < 32; ++i) enc.push_back(0);
    }
    return keccak::hash(enc.data(), enc.size());
}

// Final EIP-712 hash to sign: keccak256("\x19\x01" || domain || struct)
std::array<uint8_t, 32> signing_hash(const std::array<uint8_t,32>& dom,
                                     const std::array<uint8_t,32>& str) {
    uint8_t buf[66];
    buf[0] = 0x19; buf[1] = 0x01;
    memcpy(buf + 2,  dom.data(), 32);
    memcpy(buf + 34, str.data(), 32);
    return keccak::hash(buf, 66);
}

// Deposit-wallet domain separator (type-3): name="DepositWallet", version="1",
// chainId=137, verifyingContract=maker_addr
std::array<uint8_t, 32> dw_domain_separator(const std::string& maker_addr) {
    std::vector<uint8_t> enc;
    enc.reserve(5 * 32);
    enc.insert(enc.end(), kDomainTypeHash.begin(), kDomainTypeHash.end());
    enc.insert(enc.end(), kDwNameHash.begin(), kDwNameHash.end());
    enc.insert(enc.end(), kDwVersionHash.begin(), kDwVersionHash.end());
    encode_uint256(kChainId, enc);
    encode_address(maker_addr, enc);
    return keccak::hash(enc.data(), enc.size());
}

// TypedDataSign struct hash for type-3 (ERC-7739).
// The TypedDataSign struct carries the SMART WALLET's own domain as fields:
//   typeHash || contentsHash || keccak("DepositWallet") || keccak("1")
//          || chainId || verifyingContract(=order.signer, i.e. the proxy) || salt(0)
// NOTE: this is the wallet domain, NOT the CTF Exchange domain. The exchange
// domain is the OUTER EIP-712 domain separator (see sign_order, type-3 branch).
// Matches @polymarket/clob-client-v2 ExchangeOrderBuilderV2.buildOrderSignature.
std::array<uint8_t, 32> tds_struct_hash(
    const std::array<uint8_t, 32>& contents_hash,
    const std::string& wallet_addr)
{
    std::vector<uint8_t> enc;
    enc.reserve(7 * 32);
    enc.insert(enc.end(), kTdsTypeHash.begin(), kTdsTypeHash.end());
    enc.insert(enc.end(), contents_hash.begin(), contents_hash.end());
    enc.insert(enc.end(), kDwNameHash.begin(), kDwNameHash.end());      // "DepositWallet"
    enc.insert(enc.end(), kDwVersionHash.begin(), kDwVersionHash.end()); // "1"
    encode_uint256(kChainId, enc);
    encode_address(wallet_addr, enc);                                    // = order.signer (proxy)
    for (int i = 0; i < 32; ++i) enc.push_back(0); // salt = bytes32(0)
    return keccak::hash(enc.data(), enc.size());
}

// secp256k1 singleton context (created once, read-only after init)
secp256k1_context* secp_ctx() {
    static secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    return ctx;
}

} // namespace

// ---- Public API ----------------------------------------------------------

std::string make_salt() {
    // Match @polymarket/clob-client-v2: `${Math.round(Math.random() * Date.now())}`.
    // Keeping this as a decimal string avoids JSON number precision loss when the
    // CLOB reconstructs the signed payload.
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const auto now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::uniform_int_distribution<uint64_t> dist(0, now_ms);
    return std::to_string(dist(rng));
}

namespace {
int price_decimals_for_tick(double tick_size) {
    if (tick_size <= 0.00011) return 4;
    if (tick_size <= 0.0011)  return 3;
    if (tick_size <= 0.011)   return 2;
    return 1;
}
// Count decimal places of the SHORTEST round-trip representation of x — the C++
// equivalent of JavaScript's Number.toString(). This is what the official
// @polymarket/clob-client-v2 uses (utilities.ts decimalPlaces) and is the key to
// matching its rounding: a value like 2.85 reports 2 (not 16), so it is left
// untouched, while float noise like 2.8499999999999996 reports many places and
// gets cleaned up. Mismatching this is what produced off-grid prices → CLOB 400.
int decimal_places(double x) {
    char buf[40];
    auto res = std::to_chars(buf, buf + sizeof(buf), x);
    std::string_view s(buf, (size_t)(res.ptr - buf));
    auto dot = s.find('.');
    if (dot == std::string_view::npos) return 0;   // integer or "1e-7"-style → 0 (matches JS)
    return (int)(s.size() - dot - 1);
}
// roundNormal / roundDown / roundUp — faithful ports of clob-client-v2
// utilities.ts: each is a no-op when the value already fits in `dec` places,
// which avoids introducing float error on already-clean numbers.
double round_normal(double x, int dec) {
    if (decimal_places(x) <= dec) return x;
    double m = std::pow(10.0, dec);
    return std::round(x * m) / m;
}
double round_down(double x, int dec) {
    if (decimal_places(x) <= dec) return x;
    double m = std::pow(10.0, dec);
    return std::floor(x * m) / m;
}
double round_up(double x, int dec) {
    if (decimal_places(x) <= dec) return x;
    double m = std::pow(10.0, dec);
    return std::ceil(x * m) / m;
}
// Faithful port of getOrderRawAmounts() — returns {maker_micro, taker_micro}.
// side: 0 = BUY (maker=USDC, taker=shares), 1 = SELL (maker=shares, taker=USDC).
Amounts raw_order_amounts(int side, double price, double size, double tick_size) {
    int price_dec  = price_decimals_for_tick(tick_size);
    int amount_dec = price_dec + 2;     // ROUNDING_CONFIG: amount = price + 2
    auto to_1e6 = [](double x) -> uint64_t { return (uint64_t)std::llround(x * 1e6); };
    double raw_price = round_normal(price, price_dec);
    if (side == 0) { // BUY: taker = shares (rounded down to 2dp), maker = shares*price
        double raw_taker = round_down(size, 2);
        double raw_maker = raw_taker * raw_price;
        if (decimal_places(raw_maker) > amount_dec) {
            raw_maker = round_up(raw_maker, amount_dec + 4);
            if (decimal_places(raw_maker) > amount_dec) raw_maker = round_down(raw_maker, amount_dec);
        }
        return { to_1e6(raw_maker), to_1e6(raw_taker) };
    } else {         // SELL: maker = shares (rounded down to 2dp), taker = shares*price
        double raw_maker = round_down(size, 2);
        double raw_taker = raw_maker * raw_price;
        if (decimal_places(raw_taker) > amount_dec) {
            raw_taker = round_up(raw_taker, amount_dec + 4);
            if (decimal_places(raw_taker) > amount_dec) raw_taker = round_down(raw_taker, amount_dec);
        }
        return { to_1e6(raw_maker), to_1e6(raw_taker) };
    }
}
// Faithful port of getMarketOrderRawAmounts() — for MARKET/FAK orders (what the
// bot sends). Differs from the limit path: rawPrice uses roundDOWN, and the BUY
// is parameterized by the USDC AMOUNT (not share size). The CLOB enforces that a
// market buy's maker (USDC) has ≤2 decimals and taker (shares) ≤amount decimals;
// the limit path rounded maker to 4dp → "invalid amounts … max accuracy of 2
// decimals" 400. side: 0=BUY (amount=USDC, maker=amount, taker=amount/price),
// 1=SELL (amount=shares, maker=shares, taker=shares*price).
Amounts raw_market_amounts(int side, double amount, double price, double tick_size) {
    int price_dec  = price_decimals_for_tick(tick_size);
    int amount_dec = price_dec + 2;
    auto to_1e6 = [](double x) -> uint64_t { return (uint64_t)std::llround(x * 1e6); };
    double raw_price = round_down(price, price_dec);
    if (raw_price <= 0) return {0, 0};
    double raw_maker = round_down(amount, 2);
    double raw_taker = (side == 0) ? (raw_maker / raw_price) : (raw_maker * raw_price);
    if (decimal_places(raw_taker) > amount_dec) {
        raw_taker = round_up(raw_taker, amount_dec + 4);
        if (decimal_places(raw_taker) > amount_dec) raw_taker = round_down(raw_taker, amount_dec);
    }
    return { to_1e6(raw_maker), to_1e6(raw_taker) };
}
} // namespace

Amounts calc_amounts(int side, double price, double size, double tick_size) {
    // Exact port of @polymarket/clob-client-v2 getOrderRawAmounts() — see
    // raw_order_amounts() above. side: 0=BUY (size is shares), 1=SELL (size is
    // shares). Produces maker/taker whose implied price lands exactly on the tick
    // grid, matching the official client; mismatching it caused CLOB 400s.
    return raw_order_amounts(side, price, size, tick_size);
}

Amounts calc_buy_market_amounts(double price, double amount_usd, double tick_size) {
    // Market BUY (FAK): use getMarketOrderRawAmounts — maker = the USDC amount
    // rounded to 2dp, taker = amount/price (shares) to amount-dp. This satisfies
    // the CLOB's market-buy rule (maker ≤2 decimals, taker ≤4). See raw_market_amounts.
    return raw_market_amounts(0 /*BUY*/, amount_usd, price, tick_size);
}

Amounts calc_sell_market_amounts(double price, double shares, double tick_size) {
    // Market SELL (FAK): maker = shares rounded to 2dp, taker = shares*price.
    return raw_market_amounts(1 /*SELL*/, shares, price, tick_size);
}

std::string sign_order(const OrderParams& p, const std::string& private_key_hex) {
    try {
        const char* exchange = p.neg_risk ? kExchangeNegRisk : kExchangeStd;
        auto key_bytes = hex_to_bytes32(private_key_hex);
        auto* ctx = secp_ctx();

        static const char* kBytes32Zero =
            "0x0000000000000000000000000000000000000000000000000000000000000000";
        const char* side_str = (p.side == 0) ? "BUY" : "SELL";

        // builder in JSON must match what was signed in the struct
        std::string builder_json = p.builder_code.empty() ? kBytes32Zero : p.builder_code;

        std::string sig_hex;

        if (p.sig_type == 3) {
            // ── Type 3 (POLY_1271) ───────────────────────────────────────────
            // innerSig = sign TypedDataSign struct against DepositWallet domain
            // Final sig = innerSig(65) || appDomainSep(32) || contentsHash(32)
            //             || ORDER_TYPE_STRING(186 bytes) || 0x00BA(2 bytes)

            auto contents_hash = order_struct_hash(p);
            // OUTER EIP-712 domain separator = CTF Exchange domain
            // (name="Polymarket CTF Exchange", version="2", verifyingContract=exchange).
            // This is ALSO the appDomainSep appended to the final signature.
            auto app_dom = domain_separator(exchange);
            // TypedDataSign struct carries the wallet (DepositWallet) domain, with
            // verifyingContract = order.signer (the proxy).
            auto tds_str = tds_struct_hash(contents_hash, p.signer);
            auto hash    = signing_hash(app_dom, tds_str);

            secp256k1_ecdsa_recoverable_signature sig{};
            if (!secp256k1_ecdsa_sign_recoverable(ctx, &sig,
                    hash.data(), key_bytes.data(), nullptr, nullptr)) {
                spdlog::error("eip712: secp256k1 sign failed (type3)");
                return {};
            }
            uint8_t compact[64]; int rec_id;
            secp256k1_ecdsa_recoverable_signature_serialize_compact(
                ctx, compact, &rec_id, &sig);

            // innerSig: r(32) + s(32) + v(1)
            uint8_t inner[65];
            memcpy(inner, compact, 64);
            inner[64] = (uint8_t)(27 + rec_id);

            // Assemble 317-byte signature as hex string
            sig_hex = "0x";
            sig_hex += bytes_to_hex(inner, 65);
            sig_hex += bytes_to_hex(app_dom.data(), 32);
            sig_hex += bytes_to_hex(contents_hash.data(), 32);
            // ORDER_TYPE_STRING as raw UTF-8 bytes (186 bytes)
            for (char c : std::string_view(kOrderTypeString))
                sig_hex += bytes_to_hex(reinterpret_cast<const uint8_t*>(&c), 1);
            sig_hex += "00ba"; // length = 186 = 0x00BA

        } else {
            // ── Type 0/1/2 (EOA / proxy / safe) ─────────────────────────────
            auto dom  = domain_separator(exchange);
            auto str  = order_struct_hash(p);
            auto hash = signing_hash(dom, str);

            secp256k1_ecdsa_recoverable_signature sig{};
            if (!secp256k1_ecdsa_sign_recoverable(ctx, &sig,
                    hash.data(), key_bytes.data(), nullptr, nullptr)) {
                spdlog::error("eip712: secp256k1 sign failed");
                return {};
            }
            uint8_t compact[64]; int rec_id;
            secp256k1_ecdsa_recoverable_signature_serialize_compact(
                ctx, compact, &rec_id, &sig);

            uint8_t full_sig[65];
            memcpy(full_sig, compact, 64);
            full_sig[64] = (uint8_t)(27 + rec_id);
            sig_hex = "0x" + bytes_to_hex(full_sig, 65);
        }

        // Salt must be a NUMBER in the JSON payload, not a string. The reference
        // SDK does parseInt(order.salt, 10) which produces a JSON number. The CLOB
        // rejects string-encoded salts with "Invalid order payload".
        uint64_t salt_num = 0;
        try { salt_num = std::stoull(p.salt); } catch (...) {}

        return fmt::format(
            R"({{"salt":{},"maker":"{}","signer":"{}",)"
            R"("tokenId":"{}","makerAmount":"{}","takerAmount":"{}",)"
            R"("side":"{}","expiration":"{}","signatureType":{},)"
            R"("timestamp":"{}","metadata":"{}","builder":"{}","signature":"{}")"
            R"(}})",
            salt_num, p.maker, p.signer,
            p.token_id, p.maker_amount, p.taker_amount,
            side_str, p.expiration, p.sig_type,
            p.timestamp, kBytes32Zero, builder_json, sig_hex);

    } catch (const std::exception& e) {
        spdlog::error("eip712::sign_order failed: {}", e.what());
        return {};
    }
}

} // namespace eip712
} // namespace vp
