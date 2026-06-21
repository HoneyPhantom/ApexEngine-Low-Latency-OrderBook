#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <memory>
#include "low_latency/engine.hpp"
#include "low_latency/concepts.hpp"

#define RUN_TEST(test_func) \
    do { \
        std::cout << "[ RUN      ] " << #test_func << std::endl; \
        try { \
            test_func(); \
            std::cout << "[       OK ] " << #test_func << std::endl; \
        } catch (const std::exception& e) { \
            std::cerr << "[  FAILED  ] " << #test_func << " with exception: " << e.what() << std::endl; \
            std::exit(1); \
        } \
    } while (0)

void test_spsc_queue_bounds() {
    ZeroCopySPSC<int, 4> queue;

    assert(queue.get_write_slot() != nullptr); queue.commit_write();
    assert(queue.get_write_slot() != nullptr); queue.commit_write();
    assert(queue.get_write_slot() != nullptr); queue.commit_write();
    assert(queue.get_write_slot() != nullptr); queue.commit_write();

    // Queue is full; verify it rejects further writes
    assert(queue.get_write_slot() == nullptr);

    const int* read_ptr = queue.peek_read_slot();
    assert(read_ptr != nullptr);
    (void)read_ptr;
    queue.commit_read();

    assert(queue.get_write_slot() != nullptr);
}

void test_pricemap_bitmask_operations() {
    Pricemap<LEVELS> pm;

    assert(pm.find_max() == -1);
    assert(pm.find_min() == -1);

    // Test boundary limits (extreme edge cases)
    pm.set(idx(MIN_PRICE)); 
    pm.set(idx(MAX_PRICE)); 

    assert(pm.find_min() == idx(MIN_PRICE));
    assert(pm.find_max() == idx(MAX_PRICE));

    pm.clear(idx(MIN_PRICE));
    assert(pm.find_min() == idx(MAX_PRICE));
}

void test_orderbook_price_time_priority() {
    auto book_ptr = std::make_unique<OrderBook>();
    auto& book = *book_ptr;

    // Post passive liquidity to build the depth layers
    book.add_in_limit(false, 1005, 100); 
    book.add_in_limit(false, 1005, 50);  
    book.add_in_limit(false, 1010, 200); 

    assert(book.get_best_ask() == 1005);
    assert(book.get_best_bid() == -1);

    // Verify partial fill matching logic and queue time priority
    book.add_in_limit(true, 1005, 120);
    assert(book.get_best_ask() == 1005);

    // Verify full layer sweep and resting leftover remainder behavior
    book.add_in_limit(true, 1005, 40);

    assert(book.get_best_bid() == 1005); 
    assert(book.get_best_ask() == 1010); 
}

void test_invalid_price_out_of_bounds() {
    auto book_ptr = std::make_unique<OrderBook>();
    auto& book = *book_ptr;
    
    // Verify engine rejects out-of-bounds prices without segfaulting
    book.add_in_limit(true, 850, 100);
    book.add_in_limit(false, 1150, 100);

    assert(book.get_best_bid() == -1);
    assert(book.get_best_ask() == -1);
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "        RUNNING APEXENGINE UNIT TEST SUITE        " << std::endl;
    std::cout << "==================================================" << std::endl;

    RUN_TEST(test_spsc_queue_bounds);
    RUN_TEST(test_pricemap_bitmask_operations);
    RUN_TEST(test_orderbook_price_time_priority);
    RUN_TEST(test_invalid_price_out_of_bounds);

    std::cout << "==================================================" << std::endl;
    std::cout << "         ALL TESTS PASSED SUCCESSFULLY!          " << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}