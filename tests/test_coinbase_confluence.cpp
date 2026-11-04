#include "engine/window_bots.hpp"

#include <stdexcept>

namespace {

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

vp::engine::CoinbaseConfluenceInput aligned(int horizon_s) {
    vp::engine::CoinbaseConfluenceInput in;
    in.horizon_s = horizon_s;
    in.market_strike = 100.0;
    in.coinbase_now = 100.11;       // +11bp vs strike
    in.coinbase_window_open = 100.005; // +10.5bp vs Coinbase window open
    in.coinbase_10s_ago = 100.10;   // +1bp momentum
    return in;
}

} // namespace

int main() {
    vp::engine::CoinbaseConfluenceConfig cfg;

    auto five_up = vp::engine::coinbase_confluence_entry(aligned(300), cfg);
    require(five_up && five_up->side == vp::ShareSide::UP,
            "5m should accept aligned strike and window-open moves");

    auto fifteen_up = vp::engine::coinbase_confluence_entry(aligned(900), cfg);
    require(fifteen_up && fifteen_up->side == vp::ShareSide::UP,
            "15m should accept aligned 10s momentum");

    auto disagreement = aligned(300);
    disagreement.coinbase_window_open = 100.20;
    require(!vp::engine::coinbase_confluence_entry(disagreement, cfg),
            "opposing strike and window-open moves must be rejected");

    auto weak_move = aligned(300);
    weak_move.coinbase_now = 100.09;
    require(!vp::engine::coinbase_confluence_entry(weak_move, cfg),
            "sub-10bp strike move must be rejected");

    auto opposing_momentum = aligned(900);
    opposing_momentum.coinbase_10s_ago = 100.13;
    require(!vp::engine::coinbase_confluence_entry(opposing_momentum, cfg),
            "opposing 15m momentum must be rejected");

    auto no_momentum = aligned(900);
    no_momentum.coinbase_10s_ago = 0.0;
    require(!vp::engine::coinbase_confluence_entry(no_momentum, cfg),
            "15m requires a valid 10s Coinbase sample");

    cfg.rule = vp::engine::CoinbaseConfluenceRule::WindowMove;
    auto window_move_only = aligned(300);
    window_move_only.market_strike = 100.5;
    auto window_move_signal =
        vp::engine::coinbase_confluence_entry(window_move_only, cfg);
    require(window_move_signal && window_move_signal->side == vp::ShareSide::UP,
            "5m window-move candidate must not require strike agreement");
    require(!vp::engine::coinbase_confluence_entry(aligned(900), cfg),
            "5m window-move candidate must reject 15m markets");

    cfg.rule = vp::engine::CoinbaseConfluenceRule::StrikeMomentum;
    auto strike_momentum = aligned(900);
    strike_momentum.coinbase_window_open = 100.25;
    auto strike_momentum_signal =
        vp::engine::coinbase_confluence_entry(strike_momentum, cfg);
    require(strike_momentum_signal &&
                strike_momentum_signal->side == vp::ShareSide::UP,
            "15m strike-momentum candidate must not require window-move agreement");
    require(!vp::engine::coinbase_confluence_entry(aligned(300), cfg),
            "15m strike-momentum candidate must reject 5m markets");

    cfg.rule = vp::engine::CoinbaseConfluenceRule::StrikeWindowMomentum;
    auto five_confluence_momentum = aligned(300);
    auto five_confluence_momentum_signal =
        vp::engine::coinbase_confluence_entry(five_confluence_momentum, cfg);
    require(five_confluence_momentum_signal &&
                five_confluence_momentum_signal->side == vp::ShareSide::UP,
            "5m confluence-momentum candidate accepts aligned strike, window, and momentum");
    auto five_weak_momentum = aligned(300);
    five_weak_momentum.coinbase_10s_ago = 100.1095;
    require(!vp::engine::coinbase_confluence_entry(five_weak_momentum, cfg),
            "5m confluence-momentum candidate rejects weak 10s momentum");
    auto five_opposing_momentum = aligned(300);
    five_opposing_momentum.coinbase_10s_ago = 100.13;
    require(!vp::engine::coinbase_confluence_entry(five_opposing_momentum, cfg),
            "5m confluence-momentum candidate rejects opposing 10s momentum");
    require(!vp::engine::coinbase_confluence_entry(aligned(900), cfg),
            "5m confluence-momentum candidate rejects 15m markets");

    auto unsupported = aligned(600);
    cfg.rule = vp::engine::CoinbaseConfluenceRule::StrikeWindow;
    require(!vp::engine::coinbase_confluence_entry(unsupported, cfg),
            "unsupported horizons must be rejected");

    cfg.max_price = 0.91;
    require(!vp::engine::coinbase_confluence_entry(aligned(300), cfg),
            "price cap must not exceed the replay-tested 0.90");
}
