#pragma once

#include "order_book.hpp"
#include "market_data.hpp"
#include <cstdint>
#include <string>

namespace hft {

struct StrategyParams {
    uint64_t symbol_id;
    double   target_spread;      // minimum edge per side
    double   max_position;       // max net qty
    double   order_qty;          // default quote size
    double   skew_factor;        // inventory-based price skew
    int64_t  quote_lifetime_ns;  // cancel + requote interval
    double   tick_size;
};

struct PositionState {
    double   net_qty{0};
    double   avg_cost{0};
    double   realized_pnl{0};
    double   unrealized_pnl{0};
    uint64_t fill_count{0};
};

// Simple market-making strategy:
//   - Posts two-sided quotes around mid
//   - Skews quotes based on inventory to manage risk
//   - Cancels stale quotes and requotes on each market data update
class MarketMakerStrategy {
public:
    explicit MarketMakerStrategy(StrategyParams params,
                                  OrderBook&     book);

    void on_quote(const Quote& q);
    void on_fill(uint64_t order_id, double price, uint64_t qty, Side side);
    void on_timer();

    const PositionState& position() const { return pos_; }
    double pnl(double mark_price) const;

private:
    double compute_bid(double mid) const;
    double compute_ask(double mid) const;
    double round_to_tick(double price) const;

    void   cancel_existing_quotes();
    void   send_quotes(double bid, double ask);

    StrategyParams  params_;
    OrderBook&      book_;
    PositionState   pos_;

    uint64_t        bid_order_id_{0};
    uint64_t        ask_order_id_{0};
    int64_t         last_quote_ts_ns_{0};
    uint64_t        next_order_id_{1};
};

} // namespace hft
