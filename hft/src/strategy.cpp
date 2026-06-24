#include "strategy.hpp"
#include "market_data.hpp"
#include <cmath>
#include <algorithm>

namespace hft {

MarketMakerStrategy::MarketMakerStrategy(StrategyParams params, OrderBook& book)
    : params_(params)
    , book_(book)
{}

void MarketMakerStrategy::on_quote(const Quote& q) {
    if (q.symbol_id != params_.symbol_id) return;
    if (q.bid_price <= 0.0 || q.ask_price <= 0.0) return;

    const double mid = (q.bid_price + q.ask_price) / 2.0;
    const int64_t now = now_ns();

    // Requote if spread has moved or quote lifetime expired.
    const bool stale = (now - last_quote_ts_ns_) > params_.quote_lifetime_ns;
    const double cur_spread = book_.spread();
    const bool spread_changed = std::abs(cur_spread - params_.target_spread) >
                                 params_.tick_size * 0.5;

    if (!stale && !spread_changed) return;

    cancel_existing_quotes();
    send_quotes(compute_bid(mid), compute_ask(mid));
    last_quote_ts_ns_ = now;
}

void MarketMakerStrategy::on_fill(uint64_t /*order_id*/, double price,
                                   uint64_t qty, Side side) {
    const double signed_qty = (side == Side::Buy)
                                  ? static_cast<double>(qty)
                                  : -static_cast<double>(qty);

    const double old_cost   = pos_.avg_cost * pos_.net_qty;
    const double trade_cost = price * signed_qty;

    pos_.net_qty += signed_qty;

    if (std::abs(pos_.net_qty) > 1e-9)
        pos_.avg_cost = (old_cost + trade_cost) / pos_.net_qty;
    else {
        pos_.realized_pnl += (pos_.avg_cost > 0.0)
                                 ? (price - pos_.avg_cost) * static_cast<double>(qty)
                                 : 0.0;
        pos_.avg_cost = 0.0;
        pos_.net_qty  = 0.0;
    }
    pos_.fill_count++;
}

void MarketMakerStrategy::on_timer() {
    // Periodic risk check: flatten if breaching position limit.
    if (std::abs(pos_.net_qty) > params_.max_position) {
        cancel_existing_quotes();
        // A real system would fire a market order to flatten; omitted here.
    }
}

double MarketMakerStrategy::pnl(double mark_price) const {
    const double unrealized = (mark_price - pos_.avg_cost) * pos_.net_qty;
    return pos_.realized_pnl + unrealized;
}

// --- private ---

double MarketMakerStrategy::compute_bid(double mid) const {
    const double half_spread = params_.target_spread / 2.0;
    // Skew: positive inventory → lower bid (don't want to buy more).
    const double skew = params_.skew_factor * pos_.net_qty;
    return round_to_tick(mid - half_spread - skew);
}

double MarketMakerStrategy::compute_ask(double mid) const {
    const double half_spread = params_.target_spread / 2.0;
    const double skew = params_.skew_factor * pos_.net_qty;
    return round_to_tick(mid + half_spread - skew);
}

double MarketMakerStrategy::round_to_tick(double price) const {
    return std::round(price / params_.tick_size) * params_.tick_size;
}

void MarketMakerStrategy::cancel_existing_quotes() {
    if (bid_order_id_) { book_.cancel_order(bid_order_id_); bid_order_id_ = 0; }
    if (ask_order_id_) { book_.cancel_order(ask_order_id_); ask_order_id_ = 0; }
}

void MarketMakerStrategy::send_quotes(double bid, double ask) {
    if (bid <= 0.0 || ask <= bid) return;

    Order bid_order{};
    bid_order.order_id  = next_order_id_++;
    bid_order.symbol_id = params_.symbol_id;
    bid_order.side      = Side::Buy;
    bid_order.type      = OrderType::Limit;
    bid_order.price     = bid;
    bid_order.quantity  = static_cast<uint64_t>(params_.order_qty);
    bid_order.timestamp_ns = now_ns();
    book_.add_order(bid_order);
    bid_order_id_ = bid_order.order_id;

    Order ask_order{};
    ask_order.order_id  = next_order_id_++;
    ask_order.symbol_id = params_.symbol_id;
    ask_order.side      = Side::Sell;
    ask_order.type      = OrderType::Limit;
    ask_order.price     = ask;
    ask_order.quantity  = static_cast<uint64_t>(params_.order_qty);
    ask_order.timestamp_ns = now_ns();
    book_.add_order(ask_order);
    ask_order_id_ = ask_order.order_id;
}

} // namespace hft
