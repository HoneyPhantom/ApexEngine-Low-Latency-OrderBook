#include <benchmark/benchmark.h>
#include <memory>
#include <vector>
#include "low_latency/L2_engine.hpp"
#include "low_latency/concepts.hpp"
#include "low_latency/thread_utils.hpp"

static void BM_L2OrderBook_UpdateLevel(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L2OrderBook>();
    auto& book = *book_ptr;

    int price_bid = 999;
    int price_ask = 1001;
    uint32_t qty = 100;

    for (auto _ : state) {
        book.update_level<L2Side::Buy>(price_bid, qty);
        book.update_level<L2Side::Sell>(price_ask, qty);
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_L2OrderBook_UpdateLevel)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L2OrderBook_DeltaLevel_FastPath(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L2OrderBook>();
    auto& book = *book_ptr;

    book.update_level<L2Side::Buy>(999, 1000);
    book.update_level<L2Side::Sell>(1001, 1000);

    int32_t delta = 1;

    for (auto _ : state) {
        book.delta_level<L2Side::Buy>(999, delta);
        book.delta_level<L2Side::Sell>(1001, delta);
        delta = -delta;
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_L2OrderBook_DeltaLevel_FastPath)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L2OrderBook_ProcessPacket(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L2OrderBook>();
    auto& book = *book_ptr;

    L2MarketPacket pkt{};
    pkt.order_id = 1;
    pkt.price = 1000;
    pkt.qty = 500;
    pkt.type = L2PacketType::SetLevel;
    pkt.side = L2Side::Buy;

    for (auto _ : state) {
        pkt.side = (pkt.side == L2Side::Buy) ? L2Side::Sell : L2Side::Buy;
        pkt.price = (pkt.side == L2Side::Buy) ? 999 : 1001;
        book.process_packet(pkt);
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L2OrderBook_ProcessPacket)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L2OrderBook_OnTradeExecution(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L2OrderBook>();
    auto& book = *book_ptr;

    int price = 1000;
    uint32_t qty = 50;

    for (auto _ : state) {
        book.on_trade_execution(price, qty);
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L2OrderBook_OnTradeExecution)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L2OrderBook_SnapshotAndAnalytics_Depth5(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L2OrderBook>();
    auto& book = *book_ptr;

    for (int p = 991; p <= 999; ++p) book.update_level<L2Side::Buy>(p, 100 + p);
    for (int p = 1001; p <= 1010; ++p) book.update_level<L2Side::Sell>(p, 100 + p);
    book.on_trade_execution(1000, 50);

    for (auto _ : state) {
        L2Snapshot<5> snap = book.get_snapshot<5>();
        
        auto spread = snap.spread();
        auto mid    = snap.mid_price();
        auto micro  = snap.micro_price();
        auto imb    = snap.multi_level_imbalance();
        auto vwap   = snap.session_vwap();

        benchmark::DoNotOptimize(snap);
        benchmark::DoNotOptimize(spread);
        benchmark::DoNotOptimize(mid);
        benchmark::DoNotOptimize(micro);
        benchmark::DoNotOptimize(imb);
        benchmark::DoNotOptimize(vwap);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L2OrderBook_SnapshotAndAnalytics_Depth5)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L2OrderBook_SnapshotAndAnalytics_Depth10(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L2OrderBook>();
    auto& book = *book_ptr;

    for (int p = 985; p <= 999; ++p) book.update_level<L2Side::Buy>(p, 100 + p);
    for (int p = 1001; p <= 1015; ++p) book.update_level<L2Side::Sell>(p, 100 + p);
    book.on_trade_execution(1000, 50);

    for (auto _ : state) {
        L2Snapshot<10> snap = book.get_snapshot<10>();
        
        auto spread = snap.spread();
        auto mid    = snap.mid_price();
        auto micro  = snap.micro_price();
        auto imb    = snap.multi_level_imbalance();
        auto vwap   = snap.session_vwap();

        benchmark::DoNotOptimize(snap);
        benchmark::DoNotOptimize(spread);
        benchmark::DoNotOptimize(mid);
        benchmark::DoNotOptimize(micro);
        benchmark::DoNotOptimize(imb);
        benchmark::DoNotOptimize(vwap);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L2OrderBook_SnapshotAndAnalytics_Depth10)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L2OrderBook_SweepCrossingLevels(benchmark::State& state) {
    pin_thread_to_core(4);

    constexpr size_t BATCH_SIZE = 1024;
    constexpr size_t MASK = BATCH_SIZE - 1;
    std::vector<L2OrderBook> books(BATCH_SIZE);

    auto reset_book = [](L2OrderBook& b) {
        for (int p = 980; p <= 995; ++p) b.update_level<L2Side::Buy>(p, 100);
        for (int p = 1005; p <= 1020; ++p) b.update_level<L2Side::Sell>(p, 100);
    };

    for (auto& b : books) {
        reset_book(b);
    }

    size_t idx = 0;
    for (auto _ : state) {
        books[idx].update_level<L2Side::Buy>(1010, 200);
        benchmark::DoNotOptimize(books[idx]);

        idx = (idx + 1) & MASK;
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L2OrderBook_SweepCrossingLevels)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L2OrderBook_ExecuteMarketOrder(benchmark::State& state) {
    pin_thread_to_core(4);

    constexpr size_t BATCH_SIZE = 1024;
    constexpr size_t MASK = BATCH_SIZE - 1;
    std::vector<L2OrderBook> books(BATCH_SIZE);

    for (auto& b : books) {
        for (int p = 1001; p <= 1010; ++p) {
            b.update_level<L2Side::Sell>(p, 50);
        }
    }

    size_t idx = 0;
    for (auto _ : state) {
        books[idx].execute_market_order(L2Side::Buy, 500);
        benchmark::DoNotOptimize(books[idx]);

        idx = (idx + 1) & MASK;
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L2OrderBook_ExecuteMarketOrder)->Repetitions(20)->ReportAggregatesOnly(true);

BENCHMARK_MAIN();