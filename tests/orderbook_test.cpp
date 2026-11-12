#include <gtest/gtest.h>
#include <iostream>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>

namespace l1 {
#include "low_latency/L1_engine.hpp"
}

namespace l2 {
#include "low_latency/L2_engine.hpp"
}

namespace l3 {
#include "low_latency/L3_engine.hpp"
}

// ============================================================================
// L1 ORDER BOOK TESTS
// ============================================================================

class L1OrderBookTest : public ::testing::Test {
protected:
    l1::L1OrderBook book;
};

TEST_F(L1OrderBookTest, InitialStateIsEmptyAndInvalid) {
    auto snap = book.get_snapshot();
    EXPECT_FALSE(snap.is_valid());
    EXPECT_EQ(snap.best_bid_price, -1);
    EXPECT_EQ(snap.best_ask_price, -1);
    EXPECT_EQ(snap.spread(), -1);
    EXPECT_DOUBLE_EQ(snap.mid_price(), 0.0);
    EXPECT_DOUBLE_EQ(snap.session_vwap(), 0.0);
}

TEST_F(L1OrderBookTest, UpdateLevelsAndAnalytics) {
    book.update_level<l1::L1Side::Buy>(998, 100);
    book.update_level<l1::L1Side::Buy>(1000, 300);
    book.update_level<l1::L1Side::Sell>(1002, 100);
    book.update_level<l1::L1Side::Sell>(1005, 400);

    auto snap = book.get_snapshot();
    ASSERT_TRUE(snap.is_valid());
    EXPECT_EQ(snap.best_bid_price, 1000);
    EXPECT_EQ(snap.best_bid_qty, 300);
    EXPECT_EQ(snap.best_ask_price, 1002);
    EXPECT_EQ(snap.best_ask_qty, 100);
    EXPECT_EQ(snap.spread(), 2);
    EXPECT_DOUBLE_EQ(snap.mid_price(), 1001.0);
    EXPECT_DOUBLE_EQ(snap.micro_price(), 1001.5);
    EXPECT_DOUBLE_EQ(snap.imbalance(), 0.5);
}

TEST_F(L1OrderBookTest, DeltaLevelAndCancelLevel) {
    book.update_level<l1::L1Side::Buy>(1000, 200);
    book.update_level<l1::L1Side::Buy>(999, 150);

    book.delta_level<l1::L1Side::Buy>(1000, 50);
    EXPECT_EQ(book.get_snapshot().best_bid_qty, 250);

    book.delta_level<l1::L1Side::Buy>(1000, -100);
    EXPECT_EQ(book.get_snapshot().best_bid_qty, 150);

    book.cancel_level<l1::L1Side::Buy>(1000);
    auto snap = book.get_snapshot();
    EXPECT_EQ(snap.best_bid_price, 999);
    EXPECT_EQ(snap.best_bid_qty, 150);
}

TEST_F(L1OrderBookTest, CrossedBookSweepsOppositeLevels) {
    book.update_level<l1::L1Side::Sell>(1001, 50);
    book.update_level<l1::L1Side::Sell>(1002, 50);
    book.update_level<l1::L1Side::Sell>(1005, 100);

    book.update_level<l1::L1Side::Buy>(1003, 200);

    auto snap = book.get_snapshot();
    EXPECT_EQ(snap.best_bid_price, 1003);
    EXPECT_EQ(snap.best_ask_price, 1005);
    EXPECT_EQ(snap.best_ask_qty, 100);
}

TEST_F(L1OrderBookTest, MarketOrderExecutionAndVWAP) {
    book.update_level<l1::L1Side::Sell>(1000, 100);
    book.update_level<l1::L1Side::Sell>(1002, 100);

    book.execute_market_order(l1::L1Side::Buy, 150);

    auto snap = book.get_snapshot();
    EXPECT_EQ(snap.best_ask_price, 1002);
    EXPECT_EQ(snap.best_ask_qty, 50);
    EXPECT_EQ(snap.total_volume, 150);
    EXPECT_EQ(snap.last_traded_price, 1002);
    EXPECT_EQ(snap.last_traded_qty, 50);
    EXPECT_NEAR(snap.session_vwap(), 1000.666666, 1e-5);
}

TEST_F(L1OrderBookTest, OutOfBoundsPricesAreSafelyIgnored) {
    book.update_level<l1::L1Side::Buy>(l1::MIN_PRICE - 1, 100);
    book.update_level<l1::L1Side::Sell>(l1::MAX_PRICE + 1, 100);
    EXPECT_FALSE(book.get_snapshot().is_valid());
}

// ============================================================================
// L2 ORDER BOOK TESTS
// ============================================================================

class L2OrderBookTest : public ::testing::Test {
protected:
    l2::L2OrderBook book;
};

TEST_F(L2OrderBookTest, InitialStateIsEmptyAndInvalid) {
    auto snap = book.get_snapshot<5>();
    EXPECT_FALSE(snap.is_valid());
    EXPECT_EQ(snap.bid_depth_count, 0);
    EXPECT_EQ(snap.ask_depth_count, 0);
    EXPECT_EQ(snap.spread(), -1);
    EXPECT_DOUBLE_EQ(snap.mid_price(), 0.0);
    EXPECT_DOUBLE_EQ(snap.session_vwap(), 0.0);
}

TEST_F(L2OrderBookTest, MultiLevelDepthOrderingAndAnalytics) {
    book.update_level<l2::L2Side::Buy>(995, 400);
    book.update_level<l2::L2Side::Buy>(1000, 200);
    book.update_level<l2::L2Side::Buy>(998, 300);

    book.update_level<l2::L2Side::Sell>(1005, 500);
    book.update_level<l2::L2Side::Sell>(1001, 100);
    book.update_level<l2::L2Side::Sell>(1003, 200);

    auto snap = book.get_snapshot<5>();
    ASSERT_TRUE(snap.is_valid());
    ASSERT_EQ(snap.bid_depth_count, 3);
    ASSERT_EQ(snap.ask_depth_count, 3);

    EXPECT_EQ(snap.bids[0].price, 1000);
    EXPECT_EQ(snap.bids[0].qty, 200);
    EXPECT_EQ(snap.bids[1].price, 998);
    EXPECT_EQ(snap.bids[1].qty, 300);
    EXPECT_EQ(snap.bids[2].price, 995);
    EXPECT_EQ(snap.bids[2].qty, 400);

    EXPECT_EQ(snap.asks[0].price, 1001);
    EXPECT_EQ(snap.asks[0].qty, 100);
    EXPECT_EQ(snap.asks[1].price, 1003);
    EXPECT_EQ(snap.asks[1].qty, 200);
    EXPECT_EQ(snap.asks[2].price, 1005);
    EXPECT_EQ(snap.asks[2].qty, 500);

    EXPECT_EQ(snap.spread(), 1);
    EXPECT_DOUBLE_EQ(snap.mid_price(), 1000.5);

    EXPECT_NEAR(snap.multi_level_imbalance(), 100.0 / 1700.0, 1e-6);
}

TEST_F(L2OrderBookTest, DeltaLevelAndCancelLevel) {
    book.update_level<l2::L2Side::Buy>(1000, 300);
    book.update_level<l2::L2Side::Buy>(999, 150);

    book.delta_level<l2::L2Side::Buy>(1000, 100);
    EXPECT_EQ(book.get_snapshot<5>().bids[0].qty, 400);

    book.delta_level<l2::L2Side::Buy>(1000, -400);
    auto snap = book.get_snapshot<5>();
    ASSERT_EQ(snap.bid_depth_count, 1);
    EXPECT_EQ(snap.bids[0].price, 999);
    EXPECT_EQ(snap.bids[0].qty, 150);

    book.cancel_level<l2::L2Side::Buy>(999);
    EXPECT_EQ(book.get_snapshot<5>().bid_depth_count, 0);
}

TEST_F(L2OrderBookTest, CrossedBookClearsCrossingAsksAndBids) {
    book.update_level<l2::L2Side::Sell>(1001, 100);
    book.update_level<l2::L2Side::Sell>(1002, 100);
    book.update_level<l2::L2Side::Sell>(1003, 100);
    book.update_level<l2::L2Side::Sell>(1005, 250);

    book.update_level<l2::L2Side::Buy>(1003, 500);

    auto snap = book.get_snapshot<5>();
    ASSERT_EQ(snap.bid_depth_count, 1);
    ASSERT_EQ(snap.ask_depth_count, 1);
    EXPECT_EQ(snap.bids[0].price, 1003);
    EXPECT_EQ(snap.asks[0].price, 1005);
    EXPECT_EQ(snap.asks[0].qty, 250);
}

TEST_F(L2OrderBookTest, MarketOrderSweepsMultipleLevels) {
    book.update_level<l2::L2Side::Sell>(1000, 50);
    book.update_level<l2::L2Side::Sell>(1001, 75);
    book.update_level<l2::L2Side::Sell>(1002, 100);

    book.execute_market_order(l2::L2Side::Buy, 150);

    auto snap = book.get_snapshot<5>();
    ASSERT_EQ(snap.ask_depth_count, 1);
    EXPECT_EQ(snap.asks[0].price, 1002);
    EXPECT_EQ(snap.asks[0].qty, 75);
    EXPECT_EQ(snap.total_volume, 150);
    EXPECT_EQ(snap.last_traded_price, 1002);
    EXPECT_EQ(snap.last_traded_qty, 25);

    double expected_vwap = (50.0 * 1000.0 + 75.0 * 1001.0 + 25.0 * 1002.0) / 150.0;
    EXPECT_NEAR(snap.session_vwap(), expected_vwap, 1e-6);
}

TEST_F(L2OrderBookTest, ProcessPacketDispatch) {
    l2::L2MarketPacket set_pkt{1, 1000, 200, l2::L2PacketType::SetLevel, l2::L2Side::Buy, {}};
    book.process_packet(set_pkt);
    EXPECT_EQ(book.get_snapshot<5>().bids[0].qty, 200);

    l2::L2MarketPacket delta_pkt{2, 1000, 50, l2::L2PacketType::DeltaLevel, l2::L2Side::Buy, {}};
    book.process_packet(delta_pkt);
    EXPECT_EQ(book.get_snapshot<5>().bids[0].qty, 250);

    l2::L2MarketPacket trade_pkt{3, 1000, 30, l2::L2PacketType::TradePrint, l2::L2Side::Buy, {}};
    book.process_packet(trade_pkt);
    EXPECT_EQ(book.get_snapshot<5>().total_volume, 30);

    l2::L2MarketPacket cxl_pkt{4, 1000, 0, l2::L2PacketType::CancelLevel, l2::L2Side::Buy, {}};
    book.process_packet(cxl_pkt);
    EXPECT_EQ(book.get_snapshot<5>().bid_depth_count, 0);
}

// ============================================================================
// L3 ORDER BOOK TESTS
// ============================================================================

class L3OrderBookTest : public ::testing::Test {
protected:
    using TestL3Book = l3::L3OrderBook<1024, 2048>;
    std::unique_ptr<TestL3Book> book = std::make_unique<TestL3Book>();
};

TEST_F(L3OrderBookTest, AddOrdersAndFIFOPriority) {
    book->add_order<l3::L3Side::Buy>(101, 1000, 50, 1000);
    book->add_order<l3::L3Side::Buy>(102, 1000, 75, 2000);
    book->add_order<l3::L3Side::Buy>(103, 1000, 25, 3000);
    book->add_order<l3::L3Side::Sell>(201, 1002, 100, 4000);

    auto snap = book->get_telemetry_snapshot<5, 5>();
    ASSERT_TRUE(snap.is_valid());
    EXPECT_EQ(snap.best_bid, 1000);
    EXPECT_EQ(snap.best_bid_qty, 150);
    EXPECT_EQ(snap.bids[0].total_orders, 3);

    EXPECT_EQ(snap.bids[0].orders[0].order_id, 101);
    EXPECT_EQ(snap.bids[0].orders[1].order_id, 102);
    EXPECT_EQ(snap.bids[0].orders[2].order_id, 103);

    auto qpos = book->get_order_queue_position(102);
    ASSERT_TRUE(qpos.found);
    EXPECT_EQ(qpos.queue_rank, 1);
    EXPECT_EQ(qpos.volume_ahead, 50);
    EXPECT_EQ(qpos.volume_behind, 25);
    EXPECT_DOUBLE_EQ(qpos.queue_depletion_risk, 50.0 / 150.0);
}

TEST_F(L3OrderBookTest, CancelMiddleOrderPreservesQueueIntegrity) {
    book->add_order<l3::L3Side::Buy>(1, 1000, 10, 100);
    book->add_order<l3::L3Side::Buy>(2, 1000, 20, 200);
    book->add_order<l3::L3Side::Buy>(3, 1000, 30, 300);

    book->cancel_order(2);

    EXPECT_FALSE(book->get_order_queue_position(2).found);

    auto qpos3 = book->get_order_queue_position(3);
    ASSERT_TRUE(qpos3.found);
    EXPECT_EQ(qpos3.queue_rank, 1);
    EXPECT_EQ(qpos3.volume_ahead, 10);
    EXPECT_EQ(qpos3.volume_behind, 0);

    auto snap = book->get_telemetry_snapshot<5, 5>();
    EXPECT_EQ(snap.best_bid_qty, 40);
    EXPECT_EQ(snap.bids[0].total_orders, 2);
    EXPECT_EQ(snap.total_cancels, 1);
}

TEST_F(L3OrderBookTest, ModifyOrderPriorityRules) {
    book->add_order<l3::L3Side::Buy>(1, 1000, 50, 100);
    book->add_order<l3::L3Side::Buy>(2, 1000, 50, 200);

    book->modify_order<l3::L3Side::Buy>(1, 30);
    auto qpos1 = book->get_order_queue_position(1);
    EXPECT_EQ(qpos1.queue_rank, 0);
    EXPECT_EQ(qpos1.qty, 30);

    // Increasing size loses queue priority
    book->modify_order<l3::L3Side::Buy>(1, 80);
    qpos1 = book->get_order_queue_position(1);
    EXPECT_EQ(qpos1.queue_rank, 1);
    EXPECT_EQ(qpos1.volume_ahead, 50);
    EXPECT_EQ(qpos1.qty, 80);
}

TEST_F(L3OrderBookTest, CrossMatchingSweepsAndRestsRemainder) {
    book->add_order<l3::L3Side::Sell>(10, 1000, 40, 100);
    book->add_order<l3::L3Side::Sell>(11, 1000, 60, 200);
    book->add_order<l3::L3Side::Sell>(12, 1001, 50, 300);

    book->add_order<l3::L3Side::Buy>(99, 1001, 180, 400);

    EXPECT_FALSE(book->get_order_queue_position(10).found);
    EXPECT_FALSE(book->get_order_queue_position(11).found);
    EXPECT_FALSE(book->get_order_queue_position(12).found);

    auto qpos99 = book->get_order_queue_position(99);
    ASSERT_TRUE(qpos99.found);
    EXPECT_EQ(qpos99.price, 1001);
    EXPECT_EQ(qpos99.qty, 30);

    auto snap = book->get_telemetry_snapshot<5, 5>();
    EXPECT_EQ(snap.best_bid, 1001);
    EXPECT_EQ(snap.best_bid_qty, 30);
    EXPECT_EQ(snap.best_ask, 0);
    EXPECT_EQ(snap.total_volume, 150);
    EXPECT_EQ(snap.total_execs, 3);
}

TEST_F(L3OrderBookTest, ExecuteOrderPartialAndFullFill) {
    book->add_order<l3::L3Side::Sell>(500, 1005, 100, 100);

    book->execute_order<l3::L3Side::Sell>(500, 40);
    auto qpos = book->get_order_queue_position(500);
    ASSERT_TRUE(qpos.found);
    EXPECT_EQ(qpos.qty, 60);

    book->execute_order<l3::L3Side::Sell>(500, 60);
    EXPECT_FALSE(book->get_order_queue_position(500).found);
    EXPECT_EQ((book->get_telemetry_snapshot<5, 5>().best_ask), 0);
}

TEST_F(L3OrderBookTest, ProcessPacketDispatch) {
    l3::L3MarketPacket add_pkt{1001, 995, 50, l3::L3PacketType::AddOrder, l3::L3Side::Buy, {}};
    book->process_packet(add_pkt, 10);
    EXPECT_EQ((book->get_telemetry_snapshot<5, 5>().best_bid), 995);

    l3::L3MarketPacket mod_pkt{1001, 995, 25, l3::L3PacketType::ModifyOrder, l3::L3Side::Buy, {}};
    book->process_packet(mod_pkt, 20);
    EXPECT_EQ((book->get_telemetry_snapshot<5, 5>().best_bid_qty), 25);

    l3::L3MarketPacket exec_pkt{1001, 995, 10, l3::L3PacketType::ExecuteOrder, l3::L3Side::Buy, {}};
    book->process_packet(exec_pkt, 30);
    EXPECT_EQ((book->get_telemetry_snapshot<5, 5>().best_bid_qty), 15);

    l3::L3MarketPacket cxl_pkt{1001, 995, 0, l3::L3PacketType::CancelOrder, l3::L3Side::Buy, {}};
    book->process_packet(cxl_pkt, 40);
    EXPECT_EQ((book->get_telemetry_snapshot<5, 5>().best_bid), 0);
}