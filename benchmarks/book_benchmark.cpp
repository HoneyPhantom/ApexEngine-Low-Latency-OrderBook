#include <benchmark/benchmark.h>
#include <memory>
#include "low_latency/engine.hpp"
#include "low_latency/thread_utils.hpp"

static void BM_OrderBook_AddLimit(benchmark::State& state) {
    pin_thread_to_core(4); 

    auto book_ptr = std::make_unique<OrderBook>();
    auto& book = *book_ptr;
    bool toggle_side = true;
    int base_price = 1000;
    uint32_t qty = 10;

    for (auto _ : state) {
        book.add_in_limit(toggle_side, base_price, qty);
        toggle_side = !toggle_side;
        
        benchmark::DoNotOptimize(book);
    }
}
BENCHMARK(BM_OrderBook_AddLimit);

BENCHMARK_MAIN();