#pragma once

#include <cstdint>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include "low_latency/thread_utils.hpp"

#if defined(__APPLE__) && defined(__arm64__)
constexpr size_t CACHE_LINE_SIZE = 128;
#else
constexpr size_t CACHE_LINE_SIZE = 64;
#endif

#pragma once

template<typename T>
class SeqLockCell {
private:
    alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> seq{0};
    alignas(CACHE_LINE_SIZE) T value{};

public:
    template<typename F>
    inline void write(F&& update_fn) {
        const uint32_t cur = seq.load(std::memory_order_relaxed);
        seq.store(cur + 1, std::memory_order_release);

        update_fn(value);

        seq.store(cur + 2, std::memory_order_release);
    }

    inline T read() const {
        T copy;
        uint32_t s0, s1;
        do {
            s0 = seq.load(std::memory_order_acquire);
            while (s0 & 1) [[unlikely]] {
                hardware_spin_relax();
                s0 = seq.load(std::memory_order_acquire);
            }
            copy = value;
            std::atomic_thread_fence(std::memory_order_acquire);
            s1 = seq.load(std::memory_order_relaxed);
        } while (s0 != s1);
        return copy;
    }
};

enum class MsgType : uint8_t { QuoteUpdate = 'Q', TradePrint = 'T' };
enum class Side    : uint8_t { Sell = 'S', Buy = 'B' };

#pragma pack(push, 1)
struct alignas(8) MarketPacket {
    uint32_t  order_id;       
    int32_t   price;        
    uint32_t  qty;          
    MsgType type;         
    Side    side;         
    uint8_t   _pad[10];     
};
#pragma pack(pop)

static_assert(sizeof(MarketPacket) == 24, "MarketPacket size must be 24 bytes");

class alignas(CACHE_LINE_SIZE) BBOBook {
private:
    int32_t  best_bid_price = -1;
    uint32_t best_bid_qty   = 0;
    int32_t  best_ask_price = -1;
    uint32_t best_ask_qty   = 0;

    int32_t  last_trade_price = -1;
    uint32_t last_trade_qty   = 0;
    uint64_t total_volume     = 0;
    double   cumulative_turnover = 0.0;

public:
    BBOBook() = default;

    [[gnu::always_inline]] inline void on_quote_update(Side side, int32_t price, uint32_t qty) {
        if (side == Side::Buy) {
            if (qty > 0) {
                best_bid_price = price;
                best_bid_qty   = qty;
            } else if (price == best_bid_price) {
                best_bid_price = -1;
                best_bid_qty   = 0;
            }
        } else {
            if (qty > 0) {
                best_ask_price = price;
                best_ask_qty   = qty;
            } else if (price == best_ask_price) {
                best_ask_price = -1;
                best_ask_qty   = 0;
            }
        }
    }

    [[gnu::always_inline]] inline void on_trade_print(int32_t price, uint32_t qty) {
        last_trade_price = price;
        last_trade_qty   = qty;
        total_volume    += qty;
        cumulative_turnover += (static_cast<double>(price) * qty);
    }

    template<typename PacketT>
    [[gnu::always_inline]] inline void process_packet(const PacketT& packet) {
        const char msg_type = static_cast<char>(packet.type);
        if (msg_type == 'Q') [[likely]] {
            const Side side = (packet.side == 'B' || static_cast<uint8_t>(packet.side) == static_cast<uint8_t>(Side::Buy))
                                ? Side::Buy 
                                : Side::Sell;
            on_quote_update(side, packet.price, packet.qty);
        } else if (msg_type == 'T') {
            on_trade_print(packet.price, packet.qty);
        }
    }

    [[gnu::always_inline]] inline int32_t get_bid_price() const { return best_bid_price; }
    [[gnu::always_inline]] inline uint32_t get_bid_qty() const { return best_bid_qty; }
    [[gnu::always_inline]] inline int32_t get_ask_price() const { return best_ask_price; }
    [[gnu::always_inline]] inline uint32_t get_ask_qty() const { return best_ask_qty; }
    [[gnu::always_inline]] inline int32_t get_last_price() const { return last_trade_price; }
    [[gnu::always_inline]] inline uint32_t get_last_qty() const { return last_trade_qty; }
    [[gnu::always_inline]] inline uint64_t get_total_volume() const { return total_volume; }

    [[gnu::always_inline]] inline bool is_valid() const {
        return (best_bid_price > 0) && (best_ask_price > 0) && (best_ask_price >= best_bid_price);
    }

    [[gnu::always_inline]] inline int32_t spread() const {
        return is_valid() ? (best_ask_price - best_bid_price) : -1;
    }

    [[gnu::always_inline]] inline double mid_price() const {
        return is_valid() ? (best_bid_price + best_ask_price) * 0.5 : 0.0;
    }

    [[gnu::always_inline]] inline double micro_price() const {
        const uint64_t sum_qty = static_cast<uint64_t>(best_bid_qty) + best_ask_qty;
        if (!is_valid() || sum_qty == 0) [[unlikely]] return mid_price();
        return (static_cast<double>(best_bid_qty) * best_ask_price +
                static_cast<double>(best_ask_qty) * best_bid_price) / static_cast<double>(sum_qty);
    }

    [[gnu::always_inline]] inline double imbalance() const {
        const uint64_t sum_qty = static_cast<uint64_t>(best_bid_qty) + best_ask_qty;
        if (sum_qty == 0) [[unlikely]] return 0.0;
        const int64_t diff = static_cast<int64_t>(best_bid_qty) - static_cast<int64_t>(best_ask_qty);
        return static_cast<double>(diff) / static_cast<double>(sum_qty);
    }

    [[gnu::always_inline]] inline double session_vwap() const {
        return (total_volume > 0) ? (cumulative_turnover / static_cast<double>(total_volume)) : 0.0;
    }
};

static_assert(sizeof(BBOBook) <= CACHE_LINE_SIZE, "PureL1BBOBook must fit in one cache line!");