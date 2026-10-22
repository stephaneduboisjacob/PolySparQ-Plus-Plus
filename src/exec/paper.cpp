#include "exec/paper.hpp"
#include "core/clock.hpp"
#include "core/uuid.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
namespace vp {
namespace {
inline std::string make_uuid() { return vp::core::make_uuid(); }
double passive_buy_price(const OrderBook& book, const OrderRequest& req) {
    const double tick = req.tick_size > 0 ? req.tick_size : 0.01;
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    double px = bid.price > 0 ? bid.price + std::max(0, req.maker_improve_ticks) * tick
                              : ask.price - tick;
    if (req.maker_fixed_price && req.limit_price > 0) px = req.limit_price;
    if (ask.price > 0) px = std::min(px, ask.price - tick);
    if (req.limit_price > 0) px = std::min(px, req.limit_price);
    px = std::floor(px / tick + 1e-9) * tick;
    return std::clamp(px, tick, 1.0 - tick);
}
double passive_sell_price(const OrderBook& book, const OrderRequest& req) {
    const double tick = req.tick_size > 0 ? req.tick_size : 0.01;
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    double px = req.maker_fixed_price && req.limit_price > 0 ? req.limit_price : ask.price;
    px = std::max({px, req.limit_price, bid.price + tick});
    return std::ceil(px/tick-1e-9)*tick;
}
double visible_queue_ahead(const OrderBook& book, OrderDir dir, double price,
                           double tick_size) {
    const double tolerance = std::max(1e-6, tick_size * 0.25);
    const auto levels = dir == OrderDir::Buy ? book.copy_bids() : book.copy_asks();
    double total = 0.0;
    for (const auto& level : levels) {
        const bool ahead = dir == OrderDir::Buy
            ? level.price + tolerance >= price
            : level.price - tolerance <= price;
        if (!ahead) break; // bids descend, asks ascend
        if (std::isfinite(level.size) && level.size > 0.0) total += level.size;
    }
    return total;
}
} // namespace
PaperExecutor::PaperExecutor(Positions& pos, Journal j,
                             double max_order_usd, double max_deployed_usd,
                             double max_per_window_usd, double min_order_usd,
                             double clob_min_shares_fallback)
    : positions_(pos), journal_(std::move(j)),
      max_order_usd_(max_order_usd),
      max_deployed_usd_(max_deployed_usd),
      max_per_window_usd_(max_per_window_usd),
      min_order_usd_(min_order_usd),
      clob_min_shares_fallback_(clob_min_shares_fallback) {}
OrderResult PaperExecutor::submit(
    const OrderRequest& req,
    const std::function<const OrderBook*(const std::string&, ShareSide)>& book_lookup) {
    OrderResult r;
    r.client_order_id = req.intent_id.empty() ? make_uuid() : req.intent_id;
    if (journal_.before_submit && !journal_.before_submit(req, r.client_order_id)) {
        r.status = "rejected";
        r.terminal = true;
        r.reject_reason = "intent already reserved or persistence unavailable";
        r.reject_class = OrderResult::RejectClass::RiskCap;
        return r; // Never overwrite a previously reserved order's state.
    }
    const bool authorized_stop_fak =
        req.stop_loss_fak_authorized &&
        (req.source == "CB_LEAD_SCALP_HARD_STOP" ||
         req.source == "LATE_SETTLEMENT_HARD_STOP" ||
         req.source == "PREDICTION_2M_STOP") &&
        req.dir == OrderDir::Sell;
    if (req.execution_mode == ExecutionMode::Taker && !authorized_stop_fak) {
        r.status = "rejected";
        r.terminal = true;
        r.reject_class = OrderResult::RejectClass::RiskCap;
        r.reject_reason = "taker execution disabled except for an authorized tick hard stop";
        if (journal_.on_order) journal_.on_order(req, r);
        return r;
    }
    // Risk checks
    if (req.dir == OrderDir::Buy) {
        if (req.size_usd <= 0) {
            r.reject_reason = "buy size_usd <= 0"; goto done;
        }
        if (req.size_usd + 1e-9 < min_order_usd_) {
            r.reject_reason = fmt::format("size ${:.2f} < min ${:.2f}",
                                          req.size_usd, min_order_usd_);
            goto done;
        }
        if (req.size_usd > max_order_usd_ + 1e-9) {
            r.reject_reason = fmt::format("size ${:.2f} > cap ${:.2f}",
                                          req.size_usd, max_order_usd_);
            goto done;
        }
        double open_notional = 0.0;
        double open_in_window = 0.0;
        {
            std::lock_guard<std::mutex> lk(open_mu_);
            for (const auto& [_, o] : open_orders_) {
                if (o.req.dir != OrderDir::Buy) continue;
                const double n = o.price * std::max(0.0, o.shares);
                open_notional += n;
                if (o.req.condition_id == req.condition_id) open_in_window += n;
            }
        }
        double already = positions_.total_deployed_usd() + open_notional;
        if (already + req.size_usd > max_deployed_usd_ + 1e-9) {
            r.reject_reason = fmt::format(
                "deployed ${:.2f} + ${:.2f} > cap ${:.2f}",
                already, req.size_usd, max_deployed_usd_);
            goto done;
        }
        double in_win = positions_.deployed_in_market(req.condition_id) + open_in_window;
        if (in_win + req.size_usd > max_per_window_usd_ + 1e-9) {
            r.reject_reason = fmt::format(
                "window ${:.2f} + ${:.2f} > cap ${:.2f}",
                in_win, req.size_usd, max_per_window_usd_);
            goto done;
        }
    }
    {
        const OrderBook* book = book_lookup(req.condition_id, req.side);
        if (!book) { r.reject_reason = "no book for market/side"; goto done; }
        double exec_price = 0;
        if (req.dir == OrderDir::Buy) {
            auto a = book->best_ask();
            if (a.price <= 0 || a.size <= 0) {
                r.reject_reason = "no asks on book"; goto done;
            }
            if (req.execution_mode != ExecutionMode::Taker) {
                exec_price = passive_buy_price(*book, req);
                if (exec_price <= 0 || exec_price >= a.price - 1e-9) {
                    r.reject_class = OrderResult::RejectClass::PostOnlyCross;
                    r.reject_reason = "no passive maker price available";
                    goto done;
                }
                if (req.maker_min_price > 0.0 &&
                    exec_price + 1e-9 < req.maker_min_price) {
                    r.reject_reason = fmt::format(
                        "maker quote {:.3f} below required minimum {:.3f}",
                        exec_price, req.maker_min_price);
                    goto done;
                }
                const double budget = std::min(req.size_usd, max_order_usd_);
                r.order_shares = req.size_shares > 0.0
                    ? std::floor(std::min(req.size_shares, budget / exec_price) * 100.0) / 100.0
                    : std::floor((budget / exec_price) * 100.0) / 100.0;
                const double min_shares = std::max(0.0, clob_min_shares_fallback_);
                if (r.order_shares + 1e-9 < min_shares) {
                    const double needed = min_shares * exec_price;
                    if (req.size_shares > 0.0 || needed > budget + 1e-9) {
                        r.reject_class = OrderResult::RejectClass::SizeBelowMinimum;
                        r.reject_reason = fmt::format("maker minimum {:.2f} shares needs ${:.2f}",
                                                      min_shares, needed);
                        goto done;
                    }
                    r.order_shares = min_shares;
                }
                r.accepted = true;
                r.resting = true;
                r.status = "acked";
                r.order_price = exec_price;
                OrderRequest stored = req;
                const auto now_ms = Clock::wall_ns() / 1'000'000LL;
                if (stored.maker_root_deadline_ms <= 0 && stored.maker_max_age_ms > 0)
                    stored.maker_root_deadline_ms = now_ms + stored.maker_max_age_ms;
                {
                    std::lock_guard<std::mutex> lk(open_mu_);
                    OpenOrder resting{
                        stored, r.client_order_id, exec_price, r.order_shares,
                        now_ms, now_ms, now_ms,
                        r.order_shares,
                        visible_queue_ahead(*book, req.dir, exec_price, req.tick_size)};
                    const double tolerance = std::max(1e-6, req.tick_size * 0.25);
                    for (const auto& [id, other] : open_orders_) {
                        if (id == r.client_order_id || other.req.condition_id != req.condition_id ||
                            other.req.token_id != req.token_id || other.req.side != req.side ||
                            other.req.dir != req.dir) continue;
                        const bool ahead = req.dir == OrderDir::Buy
                            ? other.price + tolerance >= exec_price
                            : other.price - tolerance <= exec_price;
                        if (ahead) resting.queue_ahead_shares += other.shares;
                    }
                    open_orders_[r.client_order_id] = std::move(resting);
                }
                spdlog::info("PAPER MAKER REST {} {:.2f}@{:.3f} ${:.2f} ({})",
                             to_string(req.side), r.order_shares, exec_price,
                             r.order_shares * exec_price, req.source);
                goto done;
            }
            exec_price = a.price;
            if (req.limit_price > 0 && exec_price > req.limit_price) {
                r.reject_reason = fmt::format("ask {:.3f} > limit {:.3f}",
                                              exec_price, req.limit_price);
                goto done;
            }
            r.fill_shares = req.size_usd / exec_price;
            r.fill_shares = std::floor(r.fill_shares*100.0)/100.0;
            // CLOB minimum: 5 shares. Bump up shares if below minimum instead of
            // rejecting — but stay within max_order_usd cap.
            const double clob_min_shares = std::max(0.0, clob_min_shares_fallback_);
            if (r.fill_shares < clob_min_shares) {
                double needed_usd = clob_min_shares * exec_price;
                if (needed_usd <= std::min(req.size_usd,max_order_usd_) + 1e-9) {
                    r.fill_shares = clob_min_shares;
                } else {
                    r.reject_reason = fmt::format(
                        "CLOB min {:.2f} shares: ${:.2f} @ {:.3f} buys {:.0f}sh; need ${:.2f} > cap ${:.2f}",
                        clob_min_shares, req.size_usd, exec_price, r.fill_shares,
                        needed_usd, max_order_usd_);
                    goto done;
                }
            }
            // Conservative top-of-book FAK: do not invent unlimited fills at
            // the best ask. Deeper levels require a separate execution walk.
            r.fill_shares = std::min(r.fill_shares,std::floor(a.size*100.0)/100.0);
            if (r.fill_shares <= 0) { r.reject_reason="no executable ask depth"; goto done; }
        } else { // SELL — reserve inventory for passive orders, otherwise fill at bid
            auto b = book->best_bid();
            if (b.price <= 0 || b.size <= 0) { r.reject_reason = "no bids on book"; goto done; }
            exec_price = b.price;
            // determine how many shares to sell
            double held = 0;
            for (auto& p : positions_.snapshot()) {
                if (p.condition_id == req.condition_id && p.side == req.side) {
                    held = p.shares; break;
                }
            }
            {
                std::lock_guard<std::mutex> lk(open_mu_);
                for (const auto& [_, o] : open_orders_)
                    if (o.req.dir==OrderDir::Sell && o.req.condition_id==req.condition_id && o.req.side==req.side)
                        held -= o.shares;
            }
            double want = req.size_shares > 0 ? req.size_shares : held;
            if (req.execution_mode != ExecutionMode::Taker) {
                const double px = passive_sell_price(*book,req);
                const double shares = std::floor(std::min(want,held)*100.0+1e-8)/100.0;
                if (px<=b.price) {
                    r.reject_class=OrderResult::RejectClass::PostOnlyCross;
                    r.reject_reason="passive sell price would cross the best bid"; goto done;
                }
                if (px>=1.0) {
                    r.reject_class=OrderResult::RejectClass::InvalidPayload;
                    r.reject_reason="passive sell price is outside the valid share-price range"; goto done;
                }
                if (shares+1e-9<std::max(0.0,clob_min_shares_fallback_) || shares<=0) {
                    r.reject_class=OrderResult::RejectClass::SizeBelowMinimum;
                    r.reject_reason=fmt::format(
                        "CLOB min {:.2f} shares: passive sell has {:.2f} unreserved shares",
                        std::max(0.0,clob_min_shares_fallback_), shares);
                    goto done;
                }
                auto stored=req;
                const auto now=Clock::wall_ns()/1'000'000LL;
                if (stored.maker_root_deadline_ms<=0 && stored.maker_max_age_ms>0)
                    stored.maker_root_deadline_ms=now+stored.maker_max_age_ms;
                r.accepted=true;r.resting=true;r.status="acked";
                r.order_price=px;r.order_shares=shares;
                {
                    std::lock_guard<std::mutex> lk(open_mu_);
                    OpenOrder resting{stored,r.client_order_id,px,shares,now,now,now,
                        shares,
                        visible_queue_ahead(*book, req.dir, px, req.tick_size)};
                    const double tolerance=std::max(1e-6,req.tick_size*0.25);
                    for(const auto& [id,other]:open_orders_){
                        if(id==r.client_order_id||other.req.condition_id!=req.condition_id||
                           other.req.token_id!=req.token_id||other.req.side!=req.side||
                           other.req.dir!=req.dir)continue;
                        const bool ahead=req.dir==OrderDir::Buy
                            ?other.price+tolerance>=px:other.price-tolerance<=px;
                        if(ahead)resting.queue_ahead_shares+=other.shares;
                    }
                    open_orders_[r.client_order_id]=std::move(resting);
                }
                spdlog::info("PAPER MAKER SELL REST {} {:.2f}@{:.3f} ({})",req.asset,shares,px,req.source);
                goto done;
            }
            if (req.limit_price > 0 && exec_price < req.limit_price) {
                r.reject_reason = "bid below sell limit"; goto done;
            }
            const double sellable = std::min(want, held);
            if (sellable+1e-9<std::max(0.0,clob_min_shares_fallback_)) {
                r.reject_class=OrderResult::RejectClass::SizeBelowMinimum;
                r.reject_reason=fmt::format(
                    "CLOB min {:.2f} shares: sell size {:.2f} too small (holding retained)",
                    std::max(0.0,clob_min_shares_fallback_), sellable);
                goto done;
            }
            r.fill_shares = std::min(sellable, b.size);
            if (r.fill_shares <= 0) { r.reject_reason = "no position to sell"; goto done; }
        }
        r.fill_price = exec_price;
        r.accepted = true;
        r.terminal = true;
        r.status = "filled";
        r.order_price = exec_price;
        r.order_shares = r.fill_shares;
        // Compute taker fee: rate * p^exp * (1-p)^exp * shares
        {
            double p = exec_price;
            double pe = 1.0;
            for (int i = 0; i < req.fee_exponent; ++i) pe *= p;
            double qe = 1.0;
            for (int i = 0; i < req.fee_exponent; ++i) qe *= (1.0 - p);
            r.fee_usd = req.fee_rate * pe * qe * r.fill_shares;
        }
        r.after = positions_.apply_fill(
            req.condition_id, req.asset, req.horizon_s, req.token_id,
            req.side, req.dir, r.fill_price, r.fill_shares,
            req.window_close_ms, r.fee_usd);
        spdlog::info("PAPER {} {} {}@{:.3f} = ${:.2f}  fee=${:.4f}  ({})",
                     to_string(req.dir), to_string(req.side),
                     r.fill_shares, r.fill_price,
                     r.fill_shares * r.fill_price, r.fee_usd, req.source);
    }
done:
    if (!r.accepted) {
        r.status = "rejected";
        r.terminal = true;
        spdlog::warn("PAPER REJECT ({}): {}", req.source, r.reject_reason);
    }
    if (journal_.on_order) journal_.on_order(req, r);
    return r;
}

std::size_t PaperExecutor::maintain_open_orders(
    const std::function<const OrderBook*(const std::string&, ShareSide)>& book_lookup,
    std::int64_t now_ms) {
    struct Action { OpenOrder order; double quantity; bool terminal; bool fallback; };
    struct Reprice { OrderRequest req; std::string client_order_id; double price; double shares; };
    std::vector<Action> actions;
    std::vector<Reprice> reprices;
    {
        std::lock_guard<std::mutex> lk(open_mu_);
        for (auto it = open_orders_.begin(); it != open_orders_.end();) {
            auto& o = it->second;
            const OrderBook* book = book_lookup(o.req.condition_id, o.req.side);
            const bool closing = o.req.window_close_ms > 0 &&
                now_ms >= o.req.window_close_ms - std::max(0, o.req.maker_cancel_before_close_ms);
            const bool expired = o.req.maker_root_deadline_ms > 0 && now_ms >= o.req.maker_root_deadline_ms;
            double quantity = 0;
            // A BBO touch/cross is not a maker execution. Fills are driven only
            // by on_public_trade(), which requires an aggressor print at the
            // resting price after estimated queue ahead has traded.
            if (quantity>0 || closing || expired) {
                const bool terminal=closing || expired || o.shares-quantity<=1e-8;
                actions.push_back({o,quantity,terminal,expired && !closing &&
                    o.req.execution_mode==ExecutionMode::Hybrid});
                o.shares-=quantity;
                if (terminal) { it=open_orders_.erase(it); continue; }
            }
            if (book && !o.req.maker_fixed_price && o.req.maker_reprice_ms>0 &&
                now_ms-o.last_reprice_ms>=o.req.maker_reprice_ms) {
                const double px=o.req.dir==OrderDir::Buy ? passive_buy_price(*book,o.req) : passive_sell_price(*book,o.req);
                if (px>0 && px<1 && std::fabs(px-o.price)>1e-9) {
                    // Preserve the remaining dollar budget across replacements
                    // in either direction. When a passive BUY reprices down,
                    // keeping the old share count would silently shrink the
                    // requested notional and cap the eventual fill below the
                    // configured order size.
                    if (o.req.dir==OrderDir::Buy)
                        o.shares=std::floor(o.shares*o.price/px*100.0)/100.0;
                    o.price=px;
                    o.active_since_ms=now_ms;
                    o.queue_ahead_shares=visible_queue_ahead(
                        *book,o.req.dir,px,o.req.tick_size);
                    const double tolerance=std::max(1e-6,o.req.tick_size*0.25);
                    for(const auto& [other_id,other]:open_orders_){
                        if(other_id==o.client_order_id||
                           other.req.condition_id!=o.req.condition_id||
                           other.req.token_id!=o.req.token_id||
                           other.req.side!=o.req.side||
                           other.req.dir!=o.req.dir)continue;
                        const bool ahead=o.req.dir==OrderDir::Buy
                            ?other.price+tolerance>=px
                            :other.price-tolerance<=px;
                        if(ahead)o.queue_ahead_shares+=other.shares;
                    }
                    reprices.push_back({o.req,o.client_order_id,px,o.shares});
                }
                o.last_reprice_ms=now_ms;
            }
            ++it;
        }
    }
    for (auto& a:actions) {
        OrderResult u;
        u.client_order_id=a.order.client_order_id;
        u.exchange_order_id=a.order.client_order_id;
        u.order_price=a.order.price;
        u.order_shares=a.order.original_shares;
        u.accepted=true; u.terminal=a.terminal; u.resting=!a.terminal;
        if (a.quantity>0) {
            u.status=a.terminal ? "filled" : "partial";
            u.fill_price=a.order.price; u.fill_shares=a.quantity;
            u.after=positions_.apply_fill(a.order.req.condition_id,a.order.req.asset,
                a.order.req.horizon_s,a.order.req.token_id,a.order.req.side,a.order.req.dir,
                u.fill_price,u.fill_shares,a.order.req.window_close_ms,0.0);
            spdlog::info("PAPER MAKER {} {} {:.2f}@{:.3f} ({})",to_string(a.order.req.dir),
                u.status,u.fill_shares,u.fill_price,a.order.req.source);
        } else { u.status="cancelled"; }
        if (journal_.on_order) journal_.on_order(a.order.req,u);
        // HYBRID fallback exists for entry buys only. A maker SELL may never
        // quietly become a taker as its TTL expires.
        if (a.fallback && a.order.req.dir==OrderDir::Buy) {
            auto req=a.order.req;
            req.intent_id.clear(); req.execution_mode=ExecutionMode::Taker;
            req.size_usd=a.order.shares*a.order.price; req.size_shares=a.order.shares;
            req.source+="_HYBRID_FAK";
            submit(req,book_lookup);
        }
    }
    for (const auto& r:reprices)
        if (journal_.on_reprice)
            journal_.on_reprice(r.req,r.client_order_id,r.price,r.shares);
    return actions.size();
}

std::size_t PaperExecutor::on_public_trade(
    const std::string& condition_id, const std::string& token_id,
    ShareSide side, std::optional<OrderDir> aggressor, double price, double size,
    std::int64_t exchange_ms, std::int64_t received_ms) {
    if (condition_id.empty() || token_id.empty() || received_ms <= 0 ||
        !std::isfinite(price) || !std::isfinite(size) || price <= 0.0 ||
        price >= 1.0 || size <= 0.0) return 0;
    // Polymarket's trade event timestamp is exchange time, while received_ms
    // is local callback time. Prefer exchange time so a delayed websocket
    // print from before placement (or before a reprice) cannot create a paper
    // fill after it arrives. Fall back only when the feed omits its timestamp.
    const std::int64_t trade_ms = exchange_ms > 0 ? exchange_ms : received_ms;

    struct FilledOrder {
        OrderRequest req;
        std::string id;
        double order_price = 0.0;
        double original_shares = 0.0;
        double fill_shares = 0.0;
        bool terminal = false;
        Position after;
    };
    std::vector<FilledOrder> fills;
    {
        std::lock_guard<std::mutex> lk(open_mu_);
        const double epsilon = 1e-9;
        for (auto it = open_orders_.begin(); it != open_orders_.end();) {
            auto& order = it->second;
            const double tick = order.req.tick_size > 0.0
                ? order.req.tick_size : 0.01;
            const double tolerance = std::max(1e-6, tick * 0.25);
            const double difference = price - order.price;
            const bool at_order_price = std::fabs(difference) <= tolerance;
            const bool better_queue_price = order.req.dir == OrderDir::Buy
                ? difference > tolerance : difference < -tolerance;
            // A print beyond our limit proves the intervening price level was
            // traversed, so a resting order at the better limit must have
            // matched. This remains valid even when the feed's BBO-based
            // aggressor inference is unavailable or disagrees after the book
            // has already moved. At the exact limit we still require the
            // correctly directed aggressor print.
            const bool traded_through = order.req.dir == OrderDir::Buy
                ? difference < -tolerance : difference > tolerance;
            const bool matching_order =
                order.req.condition_id == condition_id &&
                order.req.token_id == token_id && order.req.side == side &&
                ((aggressor &&
                  ((*aggressor == OrderDir::Sell && order.req.dir == OrderDir::Buy) ||
                   (*aggressor == OrderDir::Buy && order.req.dir == OrderDir::Sell))) ||
                 traded_through);
            if (!matching_order || received_ms < order.active_since_ms ||
                trade_ms <= order.active_since_ms) {
                ++it;
                continue;
            }

            // Better-priced prints consume displayed queue ahead. At the exact
            // order price, only volume beyond that queue can fill our shares.
            // A trade through the limit clears the quoted price level, even if
            // the feed does not preserve the pre-trade BBO needed to infer its
            // aggressor reliably.
            if (better_queue_price && !traded_through) {
                order.queue_ahead_shares = std::max(0.0,
                    order.queue_ahead_shares - size);
                ++it;
                continue;
            }
            if (!at_order_price && !better_queue_price) {
                // Only a through-limit print reaches here. Its price is worse
                // than our limit; the order must have cleared the displayed
                // queue at our level before the market traded beyond it.
                order.queue_ahead_shares = 0.0;
            }
            const double queue_before = order.queue_ahead_shares;
            double quantity = at_order_price
                ? std::min(order.shares, std::max(0.0, size - queue_before))
                : std::min(order.shares, size);
            if (at_order_price)
                order.queue_ahead_shares = std::max(0.0, queue_before - size);
            quantity = std::floor(quantity * 100.0 + epsilon) / 100.0;
            if (quantity <= 0.0) {
                ++it;
                continue;
            }

            const bool terminal = order.shares - quantity <= 1e-8;
            FilledOrder fill{order.req, order.client_order_id, order.price,
                order.original_shares, quantity, terminal, {}};
            fill.after = positions_.apply_fill(order.req.condition_id,
                order.req.asset, order.req.horizon_s, order.req.token_id,
                order.req.side, order.req.dir, order.price, quantity,
                order.req.window_close_ms, 0.0);
            order.shares -= quantity;
            fills.push_back(std::move(fill));
            if (terminal) it = open_orders_.erase(it);
            else ++it;
        }
    }

    for (const auto& fill : fills) {
        OrderResult update;
        update.client_order_id = fill.id;
        update.exchange_order_id = fill.id;
        update.accepted = true;
        update.terminal = fill.terminal;
        update.resting = !fill.terminal;
        update.status = fill.terminal ? "filled" : "partial";
        update.order_price = fill.order_price;
        update.order_shares = fill.original_shares;
        update.fill_price = fill.order_price;
        update.fill_shares = fill.fill_shares;
        update.after = fill.after;
        spdlog::info("PAPER MAKER TRADE_FILL {} {:.2f}@{:.3f} ({})",
            to_string(fill.req.dir), fill.fill_shares, fill.order_price,
            fill.req.source);
        if (journal_.on_order) journal_.on_order(fill.req, update);
    }
    return fills.size();
}

std::size_t PaperExecutor::open_order_count() const {
    std::lock_guard<std::mutex> lk(open_mu_);
    return open_orders_.size();
}

std::vector<PaperExecutor::OpenOrderView> PaperExecutor::open_orders() const {
    std::lock_guard<std::mutex> lk(open_mu_);
    std::vector<OpenOrderView> out;
    for (const auto& [_,o]:open_orders_) out.push_back({o.req,o.client_order_id,o.price,o.shares});
    return out;
}
bool PaperExecutor::cancel_order(const std::string& id) {
    OpenOrder o;
    {
        std::lock_guard<std::mutex> lk(open_mu_);
        auto it=open_orders_.find(id);
        if (it==open_orders_.end()) return false;
        o=it->second; open_orders_.erase(it);
    }
    OrderResult r;
    r.client_order_id=id; r.exchange_order_id=id; r.accepted=true;
    r.terminal=true; r.status="cancelled";
    r.order_price=o.price; r.order_shares=o.original_shares;
    if (journal_.on_order) journal_.on_order(o.req,r);
    return true;
}

void PaperExecutor::cancel_open_orders() {
    std::vector<OpenOrder> orders;
    {
        std::lock_guard<std::mutex> lk(open_mu_);
        for (auto& [_, o] : open_orders_) orders.push_back(o);
        open_orders_.clear();
    }
    for (auto& o : orders) {
        OrderResult u;
        u.client_order_id = o.client_order_id;
        u.exchange_order_id = o.client_order_id;
        u.accepted = true;
        u.terminal = true;
        u.status = "cancelled";
        u.order_price = o.price;
        u.order_shares = o.shares;
        if (journal_.on_order) journal_.on_order(o.req, u);
    }
}
} // namespace vp
