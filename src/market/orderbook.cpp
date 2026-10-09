#include "orderbook.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace vp {

namespace {
bool is_valid_price(double p) { return std::isfinite(p) && p > 0.0 && p <= 1.0; }

template <class Cmp>
void upsert(std::vector<PriceLevel>& v, double p, double s, Cmp less) {
    if (!is_valid_price(p) || !std::isfinite(s)) return;
    auto it = std::lower_bound(v.begin(), v.end(), p,
                               [&](const PriceLevel& a, double price) {
                                   return less(a.price, price);
                               });
    if (it != v.end() && it->price == p) {
        if (s == 0.0 || s <= 0.0) v.erase(it);
        else          it->size = s;
        return;
    }
    if (s > 0.0) v.insert(it, {p, s});
}
} // namespace

void OrderBook::apply_snapshot(std::vector<PriceLevel> b,
                               std::vector<PriceLevel> a) noexcept {
    // Filter out NaN / out-of-range levels before sorting to avoid
    // corrupting the vector's ordering (NaN breaks strict weak ordering).
    auto clean = [](std::vector<PriceLevel>& v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](const PriceLevel& l) {
                                   return !is_valid_price(l.price) || !std::isfinite(l.size);
                               }),
                v.end());
    };
    clean(b); clean(a);
    std::sort(b.begin(), b.end(), [](auto& x, auto& y){ return x.price > y.price; });
    std::sort(a.begin(), a.end(), [](auto& x, auto& y){ return x.price < y.price; });
    {
        std::unique_lock lock(mu_);
        bids_ = std::move(b);
        asks_ = std::move(a);
    }
    gen_.fetch_add(1, std::memory_order_release);
}

void OrderBook::apply_delta_bid(double p, double s) noexcept {
    {
        std::unique_lock lock(mu_);
        upsert(bids_, p, s, [](double a, double b){ return a > b; });   // descending
    }
    gen_.fetch_add(1, std::memory_order_release);
}
void OrderBook::apply_delta_ask(double p, double s) noexcept {
    {
        std::unique_lock lock(mu_);
        upsert(asks_, p, s, [](double a, double b){ return a < b; });   // ascending
    }
    gen_.fetch_add(1, std::memory_order_release);
}

PriceLevel OrderBook::best_bid() const noexcept {
    std::shared_lock lock(mu_);
    return bids_.empty() ? PriceLevel{} : bids_.front();
}
PriceLevel OrderBook::best_ask() const noexcept {
    std::shared_lock lock(mu_);
    return asks_.empty() ? PriceLevel{} : asks_.front();
}

double OrderBook::midprice() const noexcept {
    std::shared_lock lock(mu_);
    if (bids_.empty() || asks_.empty()) return 0.0;
    return 0.5 * (bids_.front().price + asks_.front().price);
}
double OrderBook::microprice() const noexcept {
    std::shared_lock lock(mu_);
    if (bids_.empty() || asks_.empty()) return 0.0;
    auto bb = bids_.front(); auto aa = asks_.front();
    double tot = bb.size + aa.size;
    if (tot <= 0) return midprice();
    return (bb.price * aa.size + aa.price * bb.size) / tot;
}
double OrderBook::imbalance_top1() const noexcept {
    std::shared_lock lock(mu_);
    if (bids_.empty() || asks_.empty()) return 0.0;
    double bb = bids_.front().size, aa = asks_.front().size;
    double t = bb + aa;
    return t > 0 ? (bb - aa) / t : 0.0;
}
double OrderBook::imbalance_topN(int n) const noexcept {
    std::shared_lock lock(mu_);
    double b = 0, a = 0;
    int nb = std::min<int>(n, static_cast<int>(bids_.size()));
    int na = std::min<int>(n, static_cast<int>(asks_.size()));
    for (int i = 0; i < nb; ++i) b += bids_[static_cast<size_t>(i)].size;
    for (int i = 0; i < na; ++i) a += asks_[static_cast<size_t>(i)].size;
    double t = b + a;
    return t > 0 ? (b - a) / t : 0.0;
}
double OrderBook::spread() const noexcept {
    std::shared_lock lock(mu_);
    if (bids_.empty() || asks_.empty()) return 0.0;
    return asks_.front().price - bids_.front().price;
}

std::vector<PriceLevel> OrderBook::copy_bids() const noexcept {
    std::shared_lock lock(mu_);
    return bids_;
}
std::vector<PriceLevel> OrderBook::copy_asks() const noexcept {
    std::shared_lock lock(mu_);
    return asks_;
}

} // namespace vp