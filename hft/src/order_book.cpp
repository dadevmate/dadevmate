#include "order_book.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace hft {

OrderBook::OrderBook(uint64_t symbol_id,
                     TradeCallback   on_trade,
                     BookUpdateCallback on_update)
    : symbol_id_(symbol_id)
    , on_trade_(std::move(on_trade))
    , on_update_(std::move(on_update))
{}

OrderStatus OrderBook::add_order(Order& order) {
    if (order.type == OrderType::Market) {
        order.price = (order.side == Side::Buy)
                          ? std::numeric_limits<double>::max()
                          : 0.0;
    }

    match(order);

    if (order.status == OrderStatus::Filled)
        return order.status;

    if (order.type == OrderType::IOC || order.type == OrderType::FOK) {
        order.status = OrderStatus::Cancelled;
        return order.status;
    }

    // Resting limit order — insert into book.
    order.status = (order.filled_qty > 0)
                       ? OrderStatus::PartiallyFilled
                       : OrderStatus::New;

    PriceLevel& lvl = (order.side == Side::Buy)
                          ? bids_[order.price]
                          : asks_[order.price];
    lvl.price     = order.price;
    lvl.total_qty += order.quantity - order.filled_qty;
    lvl.order_count++;

    orders_.emplace(order.order_id, order);
    notify_update();
    return order.status;
}

OrderStatus OrderBook::cancel_order(uint64_t order_id) {
    auto it = orders_.find(order_id);
    if (it == orders_.end())
        return OrderStatus::Rejected;

    Order& o = it->second;
    const uint64_t remaining = o.quantity - o.filled_qty;

    auto remove_from_level = [&](auto& side_map) {
        auto lvl_it = side_map.find(o.price);
        if (lvl_it != side_map.end()) {
            lvl_it->second.total_qty -= remaining;
            lvl_it->second.order_count--;
            if (lvl_it->second.order_count == 0)
                side_map.erase(lvl_it);
        }
    };

    if (o.side == Side::Buy)
        remove_from_level(bids_);
    else
        remove_from_level(asks_);

    o.status = OrderStatus::Cancelled;
    orders_.erase(it);
    notify_update();
    return OrderStatus::Cancelled;
}

OrderStatus OrderBook::modify_order(uint64_t order_id, double /*new_price*/, uint64_t /*new_qty*/) {
    if (cancel_order(order_id) == OrderStatus::Rejected)
        return OrderStatus::Rejected;
    // Re-add is handled by the caller re-submitting; return Cancelled to signal
    // the slot is free so the caller can place a fresh order.
    return OrderStatus::Cancelled;
}

TopOfBook OrderBook::top_of_book() const {
    TopOfBook tob{};
    if (!bids_.empty()) {
        const auto& [p, lvl] = *bids_.begin();
        tob.bid_price = p;
        tob.bid_qty   = lvl.total_qty;
    }
    if (!asks_.empty()) {
        const auto& [p, lvl] = *asks_.begin();
        tob.ask_price = p;
        tob.ask_qty   = lvl.total_qty;
    }
    return tob;
}

double OrderBook::mid_price() const {
    const TopOfBook tob = top_of_book();
    if (tob.bid_price == 0.0 || tob.ask_price == 0.0) return 0.0;
    return (tob.bid_price + tob.ask_price) / 2.0;
}

double OrderBook::spread() const {
    const TopOfBook tob = top_of_book();
    return tob.ask_price - tob.bid_price;
}

// --- private ---

void OrderBook::match(Order& incoming) {
    auto try_match = [&](auto& opposite_map) {
        while (!opposite_map.empty() && incoming.filled_qty < incoming.quantity) {
            auto lvl_it = opposite_map.begin();
            const double lvl_price = lvl_it->first;

            bool price_matches = (incoming.side == Side::Buy)
                                     ? (incoming.price >= lvl_price)
                                     : (incoming.price <= lvl_price);
            if (!price_matches) break;

            // Walk orders at this level.
            for (auto ord_it = orders_.begin();
                 ord_it != orders_.end() && incoming.filled_qty < incoming.quantity; ) {
                Order& passive = ord_it->second;
                if (passive.price != lvl_price ||
                    passive.side == incoming.side ||
                    passive.symbol_id != incoming.symbol_id) {
                    ++ord_it;
                    continue;
                }

                const uint64_t passive_avail = passive.quantity - passive.filled_qty;
                const uint64_t fill_qty = std::min(passive_avail,
                                                    incoming.quantity - incoming.filled_qty);

                fill(incoming, passive, lvl_price, fill_qty);

                lvl_it->second.total_qty -= fill_qty;

                if (passive.filled_qty == passive.quantity) {
                    passive.status = OrderStatus::Filled;
                    ord_it = orders_.erase(ord_it);
                    lvl_it->second.order_count--;
                } else {
                    ++ord_it;
                }
            }

            if (lvl_it->second.order_count == 0)
                opposite_map.erase(lvl_it);
        }
    };

    if (incoming.side == Side::Buy)
        try_match(asks_);
    else
        try_match(bids_);

    if (incoming.filled_qty == incoming.quantity)
        incoming.status = OrderStatus::Filled;
}

void OrderBook::fill(Order& aggressor, Order& passive, double price, uint64_t qty) {
    aggressor.filled_qty += qty;
    passive.filled_qty   += qty;
    if (on_trade_) on_trade_(aggressor.order_id, passive.order_id, price, qty);
}

void OrderBook::notify_update() {
    if (on_update_) on_update_(symbol_id_, top_of_book());
}

} // namespace hft
