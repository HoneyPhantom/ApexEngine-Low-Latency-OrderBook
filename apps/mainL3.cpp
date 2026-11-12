#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <stdexcept>
#include <cerrno>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <iomanip>
#include <sstream>

#if defined(__ARM_NEON)
    #include <arm_neon.h>
    #define SIMD_W 4
    typedef uint32x4_t simd_reg;
    #define LOAD_SIMD(ptr) vld1q_u32(ptr)
    #define STORE_SIMD(ptr, val) vst1q_u32(ptr, val)
    #define MAC_SIMD(acc, mul1, mul2) vmlaq_u32(acc, mul1, mul2)
    #define SET1_SIMD(val) vdupq_n_u32(val)
#elif defined(__AVX512F__)
    #include <immintrin.h>
    #define SIMD_W 16
    typedef __m512i simd_reg;
    #define LOAD_SIMD(ptr) _mm512_loadu_si512(ptr)
    #define STORE_SIMD(ptr, val) _mm512_storeu_si512(ptr, val)
    #define MAC_SIMD(acc, mul1, mul2) _mm512_add_epi32(acc, _mm512_mullo_epi32(mul1, mul2))
    #define SET1_SIMD(val) _mm512_set1_epi32(val)
#else
    #define SIMD_W 4
    struct simd_reg { uint32_t lanes[4]; };
    inline simd_reg LOAD_SIMD(const uint32_t* ptr) {
        simd_reg r; for(int i = 0; i < 4; ++i) r.lanes[i] = ptr[i]; return r;
    }
    inline void STORE_SIMD(uint32_t* ptr, simd_reg val) {
        for(int i = 0; i < 4; ++i) ptr[i] = val.lanes[i];
    }
    inline simd_reg MAC_SIMD(simd_reg acc, simd_reg mul1, simd_reg mul2) {
        simd_reg r; for(int i = 0; i < 4; ++i) r.lanes[i] = acc.lanes[i] + (mul1.lanes[i] * mul2.lanes[i]); return r;
    }
    inline simd_reg SET1_SIMD(uint32_t val) {
        simd_reg r; for(int i = 0; i < 4; ++i) r.lanes[i] = val; return r;
    }
#endif

#include "low_latency/concepts.hpp"
#include "low_latency/ZeroCopySPSC.hpp"
#include "low_latency/L3_engine.hpp"
#include "low_latency/thread_utils.hpp"

using ActivePacketType = CME_FuturePacket; 
constexpr std::string_view ACTIVE_STREAM_ID = "CME_FUTURES";

constexpr std::array<SchemaConfig, 2> network_grid{{
    { "NYSE_EQUITIES", 16, FeedProtocol::UDP_MC,     4096 },
    { "CME_FUTURES",   24, FeedProtocol::DIRECT_DMA, 8192 }
}};
constexpr StaticConfigManager config_manager(network_grid);
static_assert(config_manager.validate_packet_compatibility<ActivePacketType>(ACTIVE_STREAM_ID), "Hardware configuration map variant sizing error!");
              
L3OrderBook<> book;
ZeroCopySPSC<DataChunk<ActivePacketType>, QUEUE_CAPACITY> zero_copy_queue;
std::atomic<bool> is_receiver_ready{false};
std::atomic<bool> is_sender_finished{false};
std::atomic<bool> is_receiver_done{false};
std::atomic<uint32_t> tracked_order_id{0};

template<ValidMarketPacket PacketType>
class MarketDataReceiver {
private:
    int server_fd = -1;
    ZeroCopySPSC<DataChunk<PacketType>, QUEUE_CAPACITY>& queue;
    int active_port = -1;

public:
    MarketDataReceiver(ZeroCopySPSC<DataChunk<PacketType>, QUEUE_CAPACITY>& q_ref, int base_port) : queue(q_ref) {
        server_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (server_fd < 0) throw std::runtime_error("Socket creation failed");

        int flags = fcntl(server_fd, F_GETFL, 0);
        fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

        int rcvbuf_size = 16 * 1024 * 1024;
        setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

        bool bound_successfully = false;
        for (int offset = 0; offset < 4; ++offset) {
            int current_port = base_port + offset;
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = INADDR_ANY;
            address.sin_port = htons(current_port);

            if (::bind(server_fd, (struct sockaddr*)&address, sizeof(address)) >= 0) {
                active_port = current_port;
                bound_successfully = true;
                break;
            }
        }

        if (!bound_successfully) {
            throw std::runtime_error("Socket binding failed: All ports in fallback range are locked.");
        }
    }

    ~MarketDataReceiver() { if (server_fd != -1) close(server_fd); }

    int get_active_port() const { return active_port; }

    void run_receiver_loop(size_t max_packets_to_process) {
        pin_thread_to_core(3);

        size_t packets_received = 0;
        DataChunk<PacketType>* current_slot = nullptr;
        while ((current_slot = queue.get_write_slot()) == nullptr) hardware_spin_relax();
        current_slot->valid_count = 0;

        std::cout << "Engine active. Running Zero-Copy In-Place SPSC Engine on Port " << active_port << "..." << std::endl;

        is_receiver_ready.store(true, std::memory_order_release);

        constexpr size_t MAX_PACKETS_PER_NETWORK_FRAME = 32;

        while (packets_received < max_packets_to_process) {
            size_t spaces_left = CHUNK_SIZE - current_slot->valid_count;
            if (spaces_left < MAX_PACKETS_PER_NETWORK_FRAME) [[unlikely]] {
                queue.commit_write();
                while ((current_slot = queue.get_write_slot()) == nullptr) hardware_spin_relax();
                current_slot->valid_count = 0;
                spaces_left = CHUNK_SIZE;
            }

            size_t max_bytes_to_read = spaces_left * sizeof(PacketType);
            ssize_t bytes_read = recv(server_fd, &current_slot->packets[current_slot->valid_count], max_bytes_to_read, 0);

            if (bytes_read < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNREFUSED) {
                    if (is_sender_finished.load(std::memory_order_acquire)) {
                        // Check if socket buffer has drained completely
                        static int idle_cycles = 0;
                        if (++idle_cycles > 500000) break;
                    }
                    hardware_spin_relax();
                    continue;
                }
                break;
            }

            size_t packets_in_batch = static_cast<size_t>(bytes_read) / sizeof(PacketType);
            current_slot->valid_count += packets_in_batch;
            packets_received += packets_in_batch;

            if (current_slot->valid_count == CHUNK_SIZE) {
                queue.commit_write(); 
                while ((current_slot = queue.get_write_slot()) == nullptr) hardware_spin_relax();
                current_slot->valid_count = 0;
            }
        }

        if (current_slot && current_slot->valid_count > 0) {
            queue.commit_write();
        }

        std::cout << "Target boundary satisfied. Processing halted. (Received " 
                  << packets_received << " packets)" << std::endl;
        is_receiver_done.store(true, std::memory_order_release);
    }
};

template<ValidMarketPacket PacketType>
void run_matching_engine_consumer(L3OrderBook<>& book_obj, ZeroCopySPSC<DataChunk<PacketType>, QUEUE_CAPACITY>& q_ref, size_t total_packets) {
    pin_thread_to_core(4);
    
    size_t processed = 0;
    while (true) {
        const DataChunk<PacketType>* current_slot = q_ref.peek_read_slot();
        if (current_slot != nullptr) {
            const void* next_slot = q_ref.peek_next_read_slot();
            if (next_slot != nullptr) {
                __builtin_prefetch(next_slot, 0, 3);
            }
            
            size_t count = current_slot->valid_count;
            for (size_t i = 0; i < count; ++i) {
                if (i + 4 < count) __builtin_prefetch(&current_slot->packets[i + 4], 0, 0);
                const auto& cme_pkt = current_slot->packets[i];
                
                L3MarketPacket l3_pkt;
                l3_pkt.order_id = cme_pkt.order_id;
                l3_pkt.price    = cme_pkt.price;
                l3_pkt.qty      = cme_pkt.qty;
                l3_pkt.type     = static_cast<L3PacketType>(cme_pkt.type);
                l3_pkt.side     = static_cast<L3Side>(cme_pkt.side);

                book_obj.process_packet(l3_pkt, cme_pkt.timestamp);
            }
            processed += count;
            q_ref.commit_read();

            if (processed >= total_packets) break;
        } else {
            if (is_receiver_done.load(std::memory_order_acquire) && q_ref.peek_read_slot() == nullptr) {
                break;
            }
            hardware_spin_relax();
        }
    }
}

template<ValidMarketPacket PacketType>
void run_mock_exchange_tx(int port, size_t total_packets) {
    pin_thread_to_core(2);

    std::cout << "Mock Exchange Thread waiting for Receiver to initialize..." << std::endl;
    while (!is_receiver_ready.load(std::memory_order_acquire)) hardware_spin_relax();
    std::cout << "Receiver detected! Injecting L3 packets into Port " << port << "..." << std::endl;

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    std::cout << "Mock Exchange Thread running on Core 2. Injecting " << total_packets << " packets into Port " << port << "..." << std::endl;

    const size_t packets_per_frame = 32; 
    std::vector<PacketType> frame_buffer(packets_per_frame);

    alignas(HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE) uint32_t rng[SIMD_W];
    for(int i = 0; i < SIMD_W; ++i) rng[i] = 42 + (i * 1000);

    simd_reg v_state = LOAD_SIMD(rng);
    simd_reg v_a = SET1_SIMD(1664525u);
    simd_reg v_c = SET1_SIMD(1013904223u);
    
    int mid = 1000;
    size_t packets_sent = 0;
    uint32_t next_order_id = 1;

    constexpr size_t ACTIVE_WINDOW = 4096;
    constexpr size_t WINDOW_MASK = ACTIVE_WINDOW - 1;
    std::vector<uint32_t> live_orders(ACTIVE_WINDOW, 0);

    while (packets_sent < total_packets) {
        for (size_t i = 0; i < packets_per_frame; i += SIMD_W) {
            v_state = MAC_SIMD(v_state, v_a, v_c);
            
            alignas(64) uint32_t results[SIMD_W];
            STORE_SIMD(results, v_state);

            for (size_t j = 0; j < SIMD_W; ++j) {
                size_t idx = i + j;
                auto& pkt = frame_buffer[idx];
                std::memset(&pkt, 0, sizeof(PacketType));

                int drift = (results[j] % 3) - 1;
                mid = std::clamp(mid + drift, MIN_PRICE + 15, MAX_PRICE - 15);
                
                bool is_buy = (results[j] & 1);
                pkt.side = is_buy ? 'B' : 'S';
                
                const int spread_offset = 1 + (results[j] % 10);
                pkt.price = is_buy ? (mid - spread_offset) : (mid + spread_offset);
                pkt.timestamp = static_cast<uint64_t>(packets_sent + idx);

                uint32_t selector = (results[j] >> 8) % 100;
                size_t slot = (results[j] >> 16) & WINDOW_MASK;
                uint32_t target_live_id = live_orders[slot];

                if (selector < 60 || target_live_id == 0) {
                    pkt.type = static_cast<char>(L3PacketType::AddOrder);
                    pkt.order_id = next_order_id++;
                    pkt.qty = 1 + (results[j] % 50);
                    live_orders[slot] = pkt.order_id;

                    if (packets_sent > total_packets - 1000) {
                        tracked_order_id.store(pkt.order_id, std::memory_order_relaxed);
                    }
                } else if (selector < 75) {
                    pkt.type = static_cast<char>(L3PacketType::ModifyOrder);
                    pkt.order_id = target_live_id;
                    pkt.qty = 1 + (results[j] % 25);
                } else if (selector < 90) {
                    pkt.type = static_cast<char>(L3PacketType::CancelOrder);
                    pkt.order_id = target_live_id;
                    pkt.qty = 0;
                    live_orders[slot] = 0;
                } else {
                    pkt.type = static_cast<char>(L3PacketType::ExecuteOrder);
                    pkt.order_id = target_live_id;
                    pkt.qty = 1 + (results[j] % 15);
                    live_orders[slot] = 0;
                }
            }
        }

        sendto(client_fd, frame_buffer.data(), packets_per_frame * sizeof(PacketType), 0,
               (struct sockaddr*)&serv_addr, sizeof(serv_addr));

        packets_sent += packets_per_frame;
    }

    close(client_fd);
    std::cout << "Mock Exchange finished transmission." << std::endl;
    is_sender_finished.store(true, std::memory_order_release);
}

int main() {
    const int BASE_PORT = 12345;
    const size_t TOTAL_PACKETS = 5000000;
    using clock = std::chrono::steady_clock;

    try {
        MarketDataReceiver<ActivePacketType> receiver(zero_copy_queue, BASE_PORT);
        int resolved_port = receiver.get_active_port();

        std::cout << "Starting Lock-Free ZERO-COPY SPSC Pipeline..." << std::endl;
        std::cout << "Mock Exchange Thread -> Core 2" << std::endl;
        std::cout << "IO Network Thread    -> Core 3" << std::endl;
        std::cout << "Matching Engine Core -> Core 4" << std::endl;

        std::thread exchange_thread(run_mock_exchange_tx<ActivePacketType>, resolved_port, TOTAL_PACKETS);
        
        auto start_time = clock::now();

        std::thread engine_thread(run_matching_engine_consumer<ActivePacketType>, 
                                  std::ref(book), 
                                  std::ref(zero_copy_queue), 
                                  TOTAL_PACKETS);
        
        receiver.run_receiver_loop(TOTAL_PACKETS);
        
        if (engine_thread.joinable()) engine_thread.join();
        auto end_time = clock::now();
        
        if (exchange_thread.joinable()) exchange_thread.join();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        auto snap = book.get_telemetry_snapshot<5, 3>();

        std::cout << "\n=========================================== HFT L3 MARKET-BY-ORDER TELEMETRY ===========================================\n";
        std::cout << "  BBO: [" << snap.best_bid << " x " << snap.best_ask << "]  |  Spread: " 
                  << snap.spread() << "  |  Mid: " << std::fixed << std::setprecision(2) << snap.mid_price()
                  << "  |  Micro: " << snap.micro_price() << "  |  VWAP: " << snap.vwap() << "\n";
        std::cout << "  Order Flow: Adds=" << snap.total_adds << " | Cancels=" << snap.total_cancels 
                  << " | Execs=" << snap.total_execs << " | Cancel/Fill=" << std::setprecision(2) << snap.cancel_to_fill_ratio() 
                  << " | Imbalance=" << std::setprecision(3) << snap.order_book_imbalance() << "\n";
        std::cout << "------------------------------------------------------------------------------------------------------------------------\n";
        std::cout << "               BIDS (Price-Time FIFO Ladder)               |              ASKS (Price-Time FIFO Ladder)\n";
        std::cout << "  Px   |  Total  | #Ord | Discrete Orders [ID:Qty]         |   Px   |  Total  | #Ord | Discrete Orders [ID:Qty]\n";
        std::cout << "-----------------------------------------------------------+------------------------------------------------------------\n";

        for (size_t i = 0; i < 5; ++i) {
            if (i < snap.bid_depth_count) {
                const auto& b = snap.bids[i];
                std::stringstream ss;
                for (size_t o = 0; o < b.visible_orders; ++o) {
                    ss << "#" << b.orders[o].order_id << ":" << b.orders[o].qty << " ";
                }
                std::cout << " " << std::setw(5) << b.price << " | "
                          << std::setw(7) << b.total_qty << " | "
                          << std::setw(4) << b.total_orders << " | "
                          << std::left << std::setw(30) << ss.str().substr(0, 30) << std::right;
            } else {
                std::cout << "  ---- |      -- |   -- | " << std::setw(30) << " ";
            }

            std::cout << " | ";

            if (i < snap.ask_depth_count) {
                const auto& a = snap.asks[i];
                std::stringstream ss;
                for (size_t o = 0; o < a.visible_orders; ++o) {
                    ss << "#" << a.orders[o].order_id << ":" << a.orders[o].qty << " ";
                }
                std::cout << " " << std::setw(5) << a.price << " | "
                          << std::setw(7) << a.total_qty << " | "
                          << std::setw(4) << a.total_orders << " | "
                          << std::left << std::setw(30) << ss.str().substr(0, 30) << std::right << "\n";
            } else {
                std::cout << "  ---- |      -- |   -- |\n";
            }
        }
        std::cout << "------------------------------------------------------------------------------------------------------------------------\n";

        uint32_t monitored_id = tracked_order_id.load(std::memory_order_relaxed);
        if (monitored_id != 0) {
            QueuePosition qpos = book.get_order_queue_position(monitored_id);
            if (qpos.found) {
                std::cout << "  [LIVE TRACKER] Order #" << qpos.order_id << " @ Px " << qpos.price 
                          << " | Rank: " << qpos.queue_rank 
                          << " | Vol Ahead: " << qpos.volume_ahead << " (" << std::fixed << std::setprecision(1) << (qpos.queue_depletion_risk * 100.0) << "% of level)"
                          << " | Vol Behind: " << qpos.volume_behind 
                          << " | My Size: " << qpos.qty << "\n";
            } else {
                std::cout << "  [LIVE TRACKER] Order #" << monitored_id << " not resting (Filled or Cancelled)\n";
            }
        }
        std::cout << "========================================================================================================================\n";

        std::cout << "\n================ ENGINE RUN STATS ================" << std::endl;
        std::cout << "Total Data Streamed    : " << TOTAL_PACKETS << " Packets" << std::endl;
        std::cout << "Packet Core Footprint  : " << sizeof(ActivePacketType) << " Bytes" << std::endl;
        std::cout << "Total Processing Time  : " << duration << " microseconds" << std::endl;
        std::cout << "Average Cost Per Packet: " << (double)duration * 1000.0 / TOTAL_PACKETS << " nanoseconds" << std::endl;
        std::cout << "Top-of-Book Spread     : " << snap.spread() << std::endl;
        std::cout << "Top-of-Book Mid Price  : " << snap.mid_price() << std::endl;
        std::cout << "Last Traded Price      : " << snap.last_traded_price << " (Qty: " << snap.last_traded_qty << ")" << std::endl;
        std::cout << "Total Traded Volume    : " << snap.total_volume << std::endl;
        std::cout << "==================================================" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Engine Runtime Failure Exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}