#include <benchmark/benchmark.h>
#include <memory>
#include <vector>
#include "low_latency/L3_engine.hpp"
#include "low_latency/concepts.hpp"
#include "low_latency/thread_utils.hpp"

static void BM_L3OrderBook_AddOrder_Passive(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L3OrderBook<>>();
    auto& book = *book_ptr;

    uint32_t order_id = 1;
    int32_t price_bid = 999;
    int32_t price_ask = 1001;
    uint32_t qty = 100;

    for (auto _ : state) {
        book.add_order<L3Side::Buy>(order_id++, price_bid, qty, 0);
        book.add_order<L3Side::Sell>(order_id++, price_ask, qty, 0);
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_L3OrderBook_AddOrder_Passive)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L3OrderBook_AddAndCancel_Cycle(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L3OrderBook<>>();
    auto& book = *book_ptr;

    uint32_t id = 1;

    for (auto _ : state) {
        book.add_order<L3Side::Buy>(id, 999, 100, 0);
        book.cancel_order(id);
        ++id;
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_L3OrderBook_AddAndCancel_Cycle)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L3OrderBook_ModifyOrder_ReduceQty(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L3OrderBook<>>();
    auto& book = *book_ptr;

    constexpr size_t BATCH_SIZE = 1024;
    constexpr size_t MASK = BATCH_SIZE - 1;

    for (uint32_t i = 1; i <= BATCH_SIZE; ++i) {
        book.add_order<L3Side::Buy>(i, 999, 1000, 0);
    }

    uint32_t id = 1;
    uint32_t new_qty = 900;

    for (auto _ : state) {
        book.modify_order<L3Side::Buy>(id, new_qty);
        benchmark::DoNotOptimize(book);

        id = (id & MASK) + 1;
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L3OrderBook_ModifyOrder_ReduceQty)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L3OrderBook_ExecuteOrder_PartialFill(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L3OrderBook<>>();
    auto& book = *book_ptr;

    constexpr size_t BATCH_SIZE = 1024;
    constexpr size_t MASK = BATCH_SIZE - 1;

    for (uint32_t i = 1; i <= BATCH_SIZE; ++i) {
        book.add_order<L3Side::Buy>(i, 999, 50000000, 0);
    }

    uint32_t id = 1;

    for (auto _ : state) {
        book.execute_order<L3Side::Buy>(id, 10);
        benchmark::DoNotOptimize(book);

        id = (id & MASK) + 1;
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L3OrderBook_ExecuteOrder_PartialFill)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L3OrderBook_CrossMatching_AggressiveSweep(benchmark::State& state) {
    pin_thread_to_core(4);

    using MicroL3Book = L3OrderBook<2048, 4096>;

    constexpr size_t BATCH_SIZE = 64;
    constexpr size_t MASK = BATCH_SIZE - 1;
    std::vector<std::unique_ptr<MicroL3Book>> books;
    books.reserve(BATCH_SIZE);

    for (size_t i = 0; i < BATCH_SIZE; ++i) {
        auto b = std::make_unique<MicroL3Book>();
        uint32_t order_seed = 1;
        for (int p = 1001; p <= 1010; ++p) {
            for (int k = 0; k < 5; ++k) {
                b->add_order<L3Side::Sell>(order_seed++, p, 10, 0);
            }
        }
        books.push_back(std::move(b));
    }

    size_t idx = 0;
    uint32_t aggressive_order_id = 1000;

    for (auto _ : state) {
        books[idx]->add_order<L3Side::Buy>(aggressive_order_id, 1010, 50, 0);
        benchmark::DoNotOptimize(*books[idx]);

        idx = (idx + 1) & MASK;
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L3OrderBook_CrossMatching_AggressiveSweep)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L3OrderBook_ProcessPacket(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L3OrderBook<>>();
    auto& book = *book_ptr;

    L3MarketPacket pkt{};
    pkt.order_id = 1;
    pkt.price = 999;
    pkt.qty = 100;
    pkt.type = L3PacketType::AddOrder;
    pkt.side = L3Side::Buy;

    for (auto _ : state) {
        pkt.side = (pkt.side == L3Side::Buy) ? L3Side::Sell : L3Side::Buy;
        pkt.price = (pkt.side == L3Side::Buy) ? 999 : 1001;
        book.process_packet(pkt, 0);
        ++pkt.order_id;
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L3OrderBook_ProcessPacket)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L3OrderBook_QueuePosition_Lookup(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L3OrderBook<>>();
    auto& book = *book_ptr;

    uint32_t target_order_id = 50;
    for (uint32_t i = 1; i <= 100; ++i) {
        book.add_order<L3Side::Buy>(i, 999, 10, 0);
    }

    for (auto _ : state) {
        QueuePosition pos = book.get_order_queue_position(target_order_id);
        benchmark::DoNotOptimize(pos);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L3OrderBook_QueuePosition_Lookup)->Repetitions(20)->ReportAggregatesOnly(true);

static void BM_L3OrderBook_TelemetrySnapshot_5x5(benchmark::State& state) {
    pin_thread_to_core(4);

    auto book_ptr = std::make_unique<L3OrderBook<>>();
    auto& book = *book_ptr;

    uint32_t order_seed = 1;
    for (int p = 991; p <= 999; ++p) {
        for (int k = 0; k < 5; ++k) {
            book.add_order<L3Side::Buy>(order_seed++, p, 10, 0);
        }
    }
    for (int p = 1001; p <= 1010; ++p) {
        for (int k = 0; k < 5; ++k) {
            book.add_order<L3Side::Sell>(order_seed++, p, 10, 0);
        }
    }

    for (auto _ : state) {
        auto snap = book.get_telemetry_snapshot<5, 5>();
        auto spread = snap.spread();
        auto mid    = snap.mid_price();
        auto micro  = snap.micro_price();
        auto imb    = snap.order_book_imbalance();
        auto vwap   = snap.vwap();

        benchmark::DoNotOptimize(snap);
        benchmark::DoNotOptimize(spread);
        benchmark::DoNotOptimize(mid);
        benchmark::DoNotOptimize(micro);
        benchmark::DoNotOptimize(imb);
        benchmark::DoNotOptimize(vwap);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_L3OrderBook_TelemetrySnapshot_5x5)->Repetitions(20)->ReportAggregatesOnly(true);

BENCHMARK_MAIN();