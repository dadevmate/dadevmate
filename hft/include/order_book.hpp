#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <functional>
#include <string>

namespace hft {

enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : uint8_t { Limit, Market, IOC, FOK };
enum class OrderStatus : uint8_t { New, PartiallyFilled, Filled, Cancelled, Rejected };

struct Order {
    uint64_t order_id;
    uint64_t symbol_id;
    Side     side;
    OrderType type;
    double   price;
    uint64_t quantity;
    uint64_t filled_qty;
    int64_t  timestamp_ns;
    OrderStatus status;
};

struct PriceLevel {
    double   price;
    uint64_t total_qty;
    uint32_t order_count;
};

struct TopOfBook {
    double   bid_price;
    uint64_t bid_qty;
    double   ask_price;
    uint64_t ask_qty;
};

using TradeCallback = std::function<void(uint64_t aggressor_id, uint64_t passive_id,
                                         double price, uint64_t qty)>;
using BookUpdateCallback = std::function<void(uint64_t symbol_id, const TopOfBook&)>;

class OrderBook {
public:
    explicit OrderBook(uint64_t symbol_id,
                       TradeCallback   on_trade,
                       BookUpdateCallback on_update);

    OrderStatus add_order(Order& order);
    OrderStatus cancel_order(uint64_t order_id);
    OrderStatus modify_order(uint64_t order_id, double new_price, uint64_t new_qty);

    TopOfBook top_of_book() const;
    double    mid_price()   const;
    double    spread()      const;

    const std::map<double, PriceLevel, std::greater<double>>& bids() const { return bids_; }
    const std::map<double, PriceLevel>& asks() const { return asks_; }

private:
    void match(Order& incoming);
    void fill(Order& aggressor, Order& passive, double price, uint64_t qty);
    void notify_update();

    uint64_t symbol_id_;
    TradeCallback   on_trade_;
    BookUpdateCallback on_update_;

    std::map<double, PriceLevel, std::greater<double>> bids_;
    std::map<double, PriceLevel>                       asks_;
    std::unordered_map<uint64_t, Order>                orders_;
};

} // namespace hft
