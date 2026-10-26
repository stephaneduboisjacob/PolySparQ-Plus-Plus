#include "collector/book_snapshot_capture.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

int main() {
    using namespace vp;

    BookSnapshotAccumulator accumulator;
    PolymarketMD::BookUpdate delta;
    delta.token_id = "token-1";
    delta.is_snapshot = false;
    delta.bids = {{0.61, 2.0}};
    if (accumulator.apply(delta) || accumulator.snapshot(delta.token_id))
        throw std::runtime_error("a delta without a full seed created a snapshot");

    PolymarketMD::BookUpdate book;
    book.token_id = delta.token_id;
    book.is_snapshot = true;
    book.bids = {{0.42, 7.0}, {0.61, 3.0},
                 {std::numeric_limits<double>::quiet_NaN(), 4.0},
                 {0.59, 0.0}, {1.01, 5.0}};
    book.asks = {{0.66, 6.0}, {0.54, 2.0}, {0.57, 4.0}, {0.0, 3.0}};

    if (!accumulator.apply(book))
        throw std::runtime_error("full book snapshot was rejected");
    auto captured = accumulator.snapshot(book.token_id);
    if (!captured) throw std::runtime_error("seeded book could not be copied");
    if (captured->bids.size() != 2 || captured->asks.size() != 3)
        throw std::runtime_error("invalid levels were not removed");
    if (std::abs(captured->best_bid - 0.61) > 1e-12 ||
        std::abs(captured->best_ask - 0.54) > 1e-12)
        throw std::runtime_error("BBO did not come from the best snapshot levels");
    if (captured->bids[0].price != 0.61 || captured->bids[1].price != 0.42 ||
        captured->asks[0].price != 0.54 || captured->asks[1].price != 0.57 ||
        captured->asks[2].price != 0.66)
        throw std::runtime_error("book sides were not normalized best-first");

    delta.bids = {{0.62, 1.5}, {0.61, 0.0}};
    delta.asks = {{0.54, 0.0}, {0.55, 3.0}};
    if (!accumulator.apply(delta))
        throw std::runtime_error("delta after a full seed was ignored");
    captured = accumulator.snapshot(book.token_id);
    if (!captured || std::abs(captured->best_bid - 0.62) > 1e-12 ||
        std::abs(captured->best_ask - 0.55) > 1e-12 ||
        captured->bids.size() != 2 || captured->asks.size() != 3)
        throw std::runtime_error("persistable book did not reflect applied deltas");

    delta.bids = {{0.62, -4.0}};
    delta.asks.clear();
    if (!accumulator.apply(delta))
        throw std::runtime_error("book accumulator rejected an update with invalid size");
    captured = accumulator.snapshot(book.token_id);
    if (!captured || captured->best_bid != 0.62 || captured->bids.front().size != 1.5)
        throw std::runtime_error("negative delta size incorrectly removed a valid level");

    PolymarketMD::BookUpdate one_sided;
    one_sided.token_id = "token-2";
    one_sided.is_snapshot = true;
    one_sided.asks = {{0.73, 1.0}};
    if (!accumulator.apply(one_sided))
        throw std::runtime_error("one-sided full snapshot was rejected");
    const auto thin = accumulator.snapshot(one_sided.token_id);
    if (!thin || thin->best_bid != 0.0 || thin->best_ask != 0.73)
        throw std::runtime_error("one-sided full snapshot lost its available quote");

    std::cout << "collector full-book snapshot capture passed\n";
}
