#include "order_book.hpp"
#include "market_data.hpp"
#include "strategy.hpp"
#include "spsc_queue.hpp"

#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <chrono>

using namespace hft;

// ---------------------------------------------------------------------------
// Demonstration: simulate a market-making session with synthetic tick data
// ---------------------------------------------------------------------------

static void print_tob(const TopOfBook& tob) {
    std::cout << std::fixed << std::setprecision(2)
              << "  BID " << tob.bid_qty  << " @ " << tob.bid_price
              << "  |  ASK " << tob.ask_qty << " @ " << tob.ask_price
              << '\n';
}

int main() {
    constexpr uint64_t SYMBOL = 1;

    // --- Build an order book --------------------------------------------------
    auto on_trade = [](uint64_t agg, uint64_t pas, double price, uint64_t qty) {
        std::cout << "[TRADE] aggressor=" << agg << " passive=" << pas
                  << " price=" << price << " qty=" << qty << '\n';
    };

    auto on_update = [](uint64_t /*sym*/, const TopOfBook& tob) {
        std::cout << "[BOOK ] ";
        print_tob(tob);
    };

    OrderBook book(SYMBOL, on_trade, on_update);

    // --- Wire up the strategy -------------------------------------------------
    StrategyParams params{};
    params.symbol_id       = SYMBOL;
    params.target_spread   = 0.10;   // 10 cents
    params.max_position    = 1000;
    params.order_qty       = 100;
    params.skew_factor     = 0.001;  // 0.1 cent per unit of inventory
    params.quote_lifetime_ns = 500'000'000LL; // 500 ms
    params.tick_size       = 0.01;

    MarketMakerStrategy mm(params, book);

    // --- Synthetic tick feed --------------------------------------------------
    MarketDataFeed feed([&mm](const Quote& q) { mm.on_quote(q); });

    // Seed book with a few resting orders from an external participant.
    {
        std::cout << "\n=== Seeding initial book ===\n";
        Order ext_bid{2001, SYMBOL, Side::Buy,  OrderType::Limit, 99.90, 500, 0, now_ns(), OrderStatus::New};
        Order ext_ask{2002, SYMBOL, Side::Sell, OrderType::Limit, 100.10, 500, 0, now_ns(), OrderStatus::New};
        book.add_order(ext_bid);
        book.add_order(ext_ask);
    }

    // Simulate 10 market data ticks with a slowly drifting mid.
    std::cout << "\n=== Simulating ticks ===\n";
    double mid = 100.00;
    for (int tick = 0; tick < 10; ++tick) {
        mid += (tick % 3 == 0) ? 0.05 : -0.02;

        Quote q{};
        q.symbol_id      = SYMBOL;
        q.bid_price      = mid - 0.05;
        q.bid_qty        = 200;
        q.ask_price      = mid + 0.05;
        q.ask_qty        = 200;
        q.exchange_ts_ns = now_ns();
        q.recv_ts_ns     = now_ns();

        std::cout << "\n[TICK " << tick << "] mid=" << mid << '\n';
        feed.inject_quote(q);

        // On tick 5 simulate an external market order hitting our bid.
        if (tick == 5) {
            std::cout << "[SIM ] External seller hits our bid\n";
            Order market{3001, SYMBOL, Side::Sell, OrderType::Market, 0, 100, 0, now_ns(), OrderStatus::New};
            book.add_order(market);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // --- SPSC queue demo ------------------------------------------------------
    std::cout << "\n=== SPSC Queue demo ===\n";
    SPSCQueue<Quote, 1024> q_queue;

    std::atomic<bool> done{false};

    std::thread producer([&] {
        for (int i = 0; i < 5; ++i) {
            Quote q{};
            q.symbol_id  = SYMBOL;
            q.bid_price  = 100.0 + i * 0.01;
            q.ask_price  = q.bid_price + 0.02;
            q.bid_qty    = 100;
            q.ask_qty    = 100;
            q.recv_ts_ns = now_ns();
            while (!q_queue.push(q)) { /* spin */ }
            std::cout << "[PROD] pushed bid=" << q.bid_price << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        done = true;
    });

    std::thread consumer([&] {
        while (!done || !q_queue.empty()) {
            if (auto item = q_queue.pop()) {
                std::cout << "[CONS] popped bid=" << item->bid_price << '\n';
            }
        }
    });

    producer.join();
    consumer.join();

    // --- Final PnL report -----------------------------------------------------
    const double mark = mid;
    std::cout << "\n=== Final Report ===\n"
              << "Net position : " << mm.position().net_qty   << '\n'
              << "Avg cost     : " << mm.position().avg_cost  << '\n'
              << "Realized PnL : " << mm.position().realized_pnl << '\n'
              << "Total PnL    : " << mm.pnl(mark)            << '\n'
              << "Fills        : " << mm.position().fill_count << '\n';

    return 0;
}
