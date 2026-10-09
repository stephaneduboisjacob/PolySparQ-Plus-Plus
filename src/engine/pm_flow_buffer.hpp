#pragma once
// PmFlowBuffer — rolling per-market flow imbalance from Polymarket token
// trades. The public CLOB trade feed carries no taker side, so the caller
// classifies via tick rule (trade price vs the live book: >= ask -> buy,
// <= bid -> sell; mid prints are dropped) and feeds a signed volume:
//   +1 = flow supports UP  (buy UP token / sell DOWN token)
//   -1 = flow supports DOWN (sell UP token / buy DOWN token)
// Thread-safe: all public methods hold an internal mutex.

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vp::engine {

class PmFlowBuffer {
public:
    static constexpr int kDefaultMaxRecordsPerMarket = 20000;

    explicit PmFlowBuffer(int max_records_per_market =
                          kDefaultMaxRecordsPerMarket);

    // Feed one classified trade. sign: +1 supports UP, -1 supports DOWN.
    void on_trade(const std::string& condition_id, double qty, int sign,
                  int64_t ts_ms);

    // Signed volume imbalance over the last window_seconds:
    // -1.0 (all DOWN-supporting) .. +1.0 (all UP-supporting). 0.0 = balanced
    // or no classifiable data.
    double imbalance_secs(const std::string& condition_id,
                          int window_seconds = 10) const;

    int size(const std::string& condition_id) const;

private:
    struct Rec {
        int64_t ts_ms;
        double  qty;
        int     sign;
    };
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::deque<Rec>> records_;
    int max_records_per_market_ = kDefaultMaxRecordsPerMarket;
};

} // namespace vp::engine
