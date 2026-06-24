#pragma once

#include <cstdint>
#include <string>
#include <functional>

namespace hft {

enum class MsgType : uint8_t {
    AddOrder,
    CancelOrder,
    Trade,
    Quote,
};

#pragma pack(push, 1)
struct MarketDataMsg {
    MsgType  type;
    uint64_t symbol_id;
    uint64_t order_id;
    double   price;
    uint64_t quantity;
    uint8_t  side;       // 0=bid, 1=ask
    int64_t  timestamp_ns;
};
#pragma pack(pop)

struct Quote {
    uint64_t symbol_id;
    double   bid_price;
    uint64_t bid_qty;
    double   ask_price;
    uint64_t ask_qty;
    int64_t  exchange_ts_ns;
    int64_t  recv_ts_ns;
};

using QuoteHandler = std::function<void(const Quote&)>;

// Simulates a fast UDP multicast market data feed decoder.
class MarketDataFeed {
public:
    explicit MarketDataFeed(QuoteHandler handler);

    // Parse a raw network packet and dispatch decoded messages.
    // In production this is called directly from a kernel-bypass NIC callback.
    void on_packet(const uint8_t* data, std::size_t len);

    // Inject a synthetic quote (for backtesting/simulation).
    void inject_quote(const Quote& q);

private:
    void dispatch(const MarketDataMsg& msg);

    QuoteHandler handler_;
    uint64_t     seq_expected_{1};
    uint64_t     gaps_detected_{0};
};

// Nanosecond-precision clock.
int64_t now_ns() noexcept;

} // namespace hft
