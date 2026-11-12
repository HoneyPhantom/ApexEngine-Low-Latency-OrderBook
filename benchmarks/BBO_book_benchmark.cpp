#include <benchmark/benchmark.h>
#include <memory>
#include "low_latency/BBO_engine.hpp"
#include "low_latency/concepts.hpp"
#include "low_latency/thread_utils.hpp"

static void BM_BBOBook_OnQuoteUpdate(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<BBOBook>();
    auto& book = *book_ptr;

    Side side = Side::Buy;
    int32_t price = 1000;
    uint32_t qty = 100;

    for (auto _ : state) {
        book.on_quote_update(side, price, qty);
        side = (side == Side::Buy) ? Side::Sell : Side::Buy;
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BBOBook_OnQuoteUpdate)->Repetitions(20)->ReportAggregatesOnly(true);;

static void BM_BBOBook_ProcessPacket(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<BBOBook>();
    auto& book = *book_ptr;

    CME_FuturePacket pkt;
    pkt.type = 'Q';
    pkt.timestamp = 0;
    pkt.order_id = 1;
    pkt.price = 1000;
    pkt.qty = 500;
    pkt.side = 'B';

    for (auto _ : state) {
        pkt.side = (pkt.side == 'B') ? 'S' : 'B';
        book.process_packet(pkt);
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BBOBook_ProcessPacket)->Repetitions(20)->ReportAggregatesOnly(true);;

static void BM_BBOBook_OnTradePrint(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<BBOBook>();
    auto& book = *book_ptr;

    int32_t trade_price = 1002;
    uint32_t trade_qty = 25;

    for (auto _ : state) {
        book.on_trade_print(trade_price, trade_qty);
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BBOBook_OnTradePrint)->Repetitions(20)->ReportAggregatesOnly(true);;

static void BM_BBOBook_ComputeAnalytics(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<BBOBook>();
    auto& book = *book_ptr;

    book.on_quote_update(Side::Buy, 1000, 350);
    book.on_quote_update(Side::Sell, 1002, 150);

    for (auto _ : state) {
        auto spread = book.spread();
        auto mid = book.mid_price();
        auto micro = book.micro_price();
        auto obi = book.imbalance();

        benchmark::DoNotOptimize(spread);
        benchmark::DoNotOptimize(mid);
        benchmark::DoNotOptimize(micro);
        benchmark::DoNotOptimize(obi);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BBOBook_ComputeAnalytics)->Repetitions(20)->ReportAggregatesOnly(true);;

BENCHMARK_MAIN();