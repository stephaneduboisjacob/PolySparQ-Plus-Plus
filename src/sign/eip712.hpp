#pragma once
// EIP-712 order signing for Polymarket CLOB.
//
// Builds the signed order payload required by the CLOB REST API.
// Uses libsecp256k1 (already linked) + Keccak-256 (vendored header).
//
// Thread-safe: all state is passed in by value; no global state.
// NOTE: Never echoes the private key. Key is consumed once per call.
#include <array>
#include <cstdint>
#include <string>

namespace vp {
namespace eip712 {

// Chain and contract constants
static constexpr int kChainId = 137;  // Polygon
// CLOB V2 Standard CTF Exchange (chain_id=137, from py_clob_client_v2 config: exchange_v2)
// NOTE: the V1 addresses (0x4bFb…/0xC5d5…) sign valid orders for the V1 typed-data
// (domain version="1") only. We sign V2 typed-data (version="2"), so we MUST point
// verifyingContract at the V2 exchange contracts or every order is "Invalid order payload".
static constexpr const char* kExchangeStd     = "0xE111180000d2663C0091e4f400237545B87B996B";
// CLOB V2 Neg-risk CTF Exchange (chain_id=137, from py_clob_client_v2 config: neg_risk_exchange_v2)
static constexpr const char* kExchangeNegRisk = "0xe2222d279d744050d28e00520010520000310F59";

struct OrderParams {
    // Addresses (lowercase hex with 0x prefix, 20 bytes)
    std::string maker;        // proxy wallet (POLY_FUNDER)
    std::string signer;       // EOA (owner of maker)

    // Order fields
    std::string salt;         // random entropy, decimal string like the official SDK
    std::string token_id;     // condition token ID (decimal string, very large number)
    uint64_t    maker_amount; // USDC in 1e6 units (BUY: USD spent; SELL: shares to sell)
    uint64_t    taker_amount; // shares in 1e6 units (BUY: shares to receive; SELL: USD to receive)
    uint64_t    expiration = 0;   // 0 = no expiry (in JSON only; not part of V2 struct hash)
    uint64_t    timestamp = 0;    // current time in milliseconds (order uniqueness in V2)
    int         side = 0;         // 0=BUY, 1=SELL
    int         sig_type = 0;     // 0=EOA, 1=POLY_PROXY
    bool        neg_risk = false; // true for neg-risk markets
    std::string builder_code;     // bytes32 hex (with 0x) — from Polymarket builder registration
};

// Returns the signed order as a JSON string ready to POST to /order
// private_key_hex: 64-char hex string (no 0x prefix)
// On error returns empty string and logs via spdlog.
std::string sign_order(const OrderParams& p, const std::string& private_key_hex);

// Helper: convert price/size (in share units) to maker/taker amounts in 1e6 units.
// For tick_size=0.01 (BTC/ETH UP/DOWN markets).
struct Amounts {
    uint64_t maker_amount;
    uint64_t taker_amount;
};
// Limit-order amount rounding matching py_clob_client.create_order for
// tick_size=0.01: price decimals=2, size decimals=2, amount decimals=4.
// side: 0=BUY (size is shares), 1=SELL (size is shares).
Amounts calc_amounts(int side, double price, double size, double tick_size = 0.01);

// Market-style BUY amount rounding matching py_clob_client.create_market_order:
// amount_usd is the USDC notional to spend; price is rounded to tick first;
// taker shares are rounded to 4 amount decimals. This is what we use for
// FAK buys because the UI config is dollar-notional ($2/order), not an exact
// integer-share limit order.
Amounts calc_buy_market_amounts(double price, double amount_usd, double tick_size = 0.01);
// Market-style SELL (FAK): shares to sell; maker=shares (2dp), taker=shares*price.
Amounts calc_sell_market_amounts(double price, double shares, double tick_size = 0.01);

// Helper: generate a random salt
std::string make_salt();

} // namespace eip712
} // namespace vp
