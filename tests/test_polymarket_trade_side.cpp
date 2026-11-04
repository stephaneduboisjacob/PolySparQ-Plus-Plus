#include "feeds/polymarket_md.hpp"

#include <cassert>

int main() {
    using vp::OrderDir;

    assert(vp::parse_reported_trade_side("BUY") == OrderDir::Buy);
    assert(vp::parse_reported_trade_side("SELL") == OrderDir::Sell);
    assert(vp::parse_reported_trade_side("buy") == OrderDir::Buy);
    assert(vp::parse_reported_trade_side("sell") == OrderDir::Sell);
    assert(!vp::parse_reported_trade_side(""));
    assert(!vp::parse_reported_trade_side("TAKER"));
    assert(!vp::parse_reported_trade_side("unknown"));

    assert(vp::resolve_trade_timestamp_ms(1'700'000'000'000LL,
                                          1'700'000'000'050LL) ==
           1'700'000'000'000LL);
    assert(vp::resolve_trade_timestamp_ms(0, 1'700'000'000'050LL) ==
           1'700'000'000'050LL);
    assert(vp::resolve_trade_timestamp_ms(-1, 1'700'000'000'050LL) ==
           1'700'000'000'050LL);
    assert(vp::resolve_trade_timestamp_ms(0, 0) == 0);

    assert(vp::parse_reported_trade_size("12.5") == 12.5);
    assert(vp::parse_reported_trade_size("0") == 0.0);
    assert(!vp::parse_reported_trade_size(""));
    assert(!vp::parse_reported_trade_size("12.5 shares"));
    assert(!vp::parse_reported_trade_size("nan"));
}
