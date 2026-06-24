#include "market_data.hpp"
#include <cstring>
#include <ctime>
#include <iostream>

namespace hft {

int64_t now_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

MarketDataFeed::MarketDataFeed(QuoteHandler handler)
    : handler_(std::move(handler))
{}

void MarketDataFeed::on_packet(const uint8_t* data, std::size_t len) {
    if (len < sizeof(MarketDataMsg)) return;

    const MarketDataMsg* msg = reinterpret_cast<const MarketDataMsg*>(data);
    const std::size_t count  = len / sizeof(MarketDataMsg);

    for (std::size_t i = 0; i < count; ++i)
        dispatch(msg[i]);
}

void MarketDataFeed::dispatch(const MarketDataMsg& msg) {
    if (msg.type == MsgType::Quote) {
        // A pre-assembled best-bid-offer message.
        // Reconstruct from packed fields (bid in price, ask in a second quantity field is
        // a simplification; real protocols use separate bid/ask structs).
        Quote q{};
        q.symbol_id   = msg.symbol_id;
        q.bid_price   = (msg.side == 0) ? msg.price : 0.0;
        q.ask_price   = (msg.side == 1) ? msg.price : 0.0;
        q.bid_qty     = (msg.side == 0) ? msg.quantity : 0;
        q.ask_qty     = (msg.side == 1) ? msg.quantity : 0;
        q.exchange_ts_ns = msg.timestamp_ns;
        q.recv_ts_ns     = now_ns();
        if (handler_) handler_(q);
    }
}

void MarketDataFeed::inject_quote(const Quote& q) {
    if (handler_) handler_(q);
}

} // namespace hft
