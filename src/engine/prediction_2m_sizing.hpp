#pragma once

#include "config/config.hpp"

#include <cmath>

namespace vp::engine {

// Convert the configured USD clip to an exchange-compatible share size without
// exceeding the budget. Legacy profiles with no USD clip keep fixed-share sizing.
inline double prediction_2m_shares_for_limit(const StrategyHot& strategy,
                                              double limit_price) noexcept {
    if (!std::isfinite(limit_price) || limit_price <= 0.0 || limit_price >= 1.0 ||
        !std::isfinite(strategy.prediction_2m_order_usd) ||
        strategy.prediction_2m_order_usd < 0.0)
        return 0.0;
    if (strategy.prediction_2m_order_usd == 0.0)
        return strategy.prediction_2m_order_shares;
    const double raw_shares = strategy.prediction_2m_order_usd / limit_price;
    if (!std::isfinite(raw_shares) || raw_shares <= 0.0 || raw_shares > 1.0e12)
        return 0.0;
    return std::floor(raw_shares * 100.0 + 1e-9) / 100.0;
}

} // namespace vp::engine
