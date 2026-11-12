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
#include "low_latency/BBO_engine.hpp"
#include "low_latency/thread_utils.hpp"

using ActivePacketType = CME_FuturePacket; 
constexpr std::string_view ACTIVE_STREAM_ID = "CME_FUTURES";

constexpr std::array<SchemaConfig, 2> network_grid{{
    { "NYSE_EQUITIES", 16, FeedProtocol::UDP_MC,     4096 },
    { "CME_FUTURES",   24, FeedProtocol::DIRECT_DMA, 8192 }
}};
constexpr StaticConfigManager config_manager(network_grid);
static_assert(config_manager.validate_packet_compatibility<ActivePacketType>(ACTIVE_STREAM_ID), "Hardware configuration map variant sizing error!");
              
alignas(HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE) BBOBook bbo_book;
ZeroCopySPSC<DataChunk<ActivePacketType>, QUEUE_CAPACITY> zero_copy_queue;
std::atomic<bool> is_receiver_ready{false};

template<typename PacketType>
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

        std::cout << "[Receiver] Active on Port " << active_port << "..." << std::endl;
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
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
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
    }
};

template<typename PacketType>
void run_bbo_engine_consumer(BBOBook& book_obj, 
                             ZeroCopySPSC<DataChunk<PacketType>, QUEUE_CAPACITY>& q_ref, 
                             size_t total_packets) {
    pin_thread_to_core(4);
    
    size_t processed = 0;
    while (processed < total_packets) {
        const DataChunk<PacketType>* current_slot = q_ref.peek_read_slot();
        if (current_slot != nullptr) {
            const void* next_slot = q_ref.peek_next_read_slot();
            __builtin_prefetch(next_slot, 0, 3);

            const size_t count = current_slot->valid_count;
            for (size_t i = 0; i < count; ++i) {
                if (i + 4 < count) {
                    __builtin_prefetch(&current_slot->packets[i + 4], 0, 0);
                }
                book_obj.process_packet(current_slot->packets[i]);
            }

            processed += count;
            q_ref.commit_read();
        } else {
            hardware_spin_relax();
        }
    }
}

template<typename PacketType>
void run_mock_exchange_tx(int port, size_t total_packets) {
    pin_thread_to_core(2);

    while (!is_receiver_ready.load(std::memory_order_acquire)) hardware_spin_relax();

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    const size_t packets_per_frame = 32; 
    std::vector<PacketType> frame_buffer(packets_per_frame);

    alignas(HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE) uint32_t rng[SIMD_W];
    for(int i = 0; i < SIMD_W; ++i) rng[i] = 42 + (i * 1000);

    simd_reg v_state = LOAD_SIMD(rng);
    simd_reg v_a = SET1_SIMD(1664525u);
    simd_reg v_c = SET1_SIMD(1013904223u);
    
    int mid = 1000;
    size_t packets_sent = 0;

    while (packets_sent < total_packets) {
        for (size_t i = 0; i < packets_per_frame; i += SIMD_W) {
            v_state = MAC_SIMD(v_state, v_a, v_c);
            
            alignas(64) uint32_t results[SIMD_W];
            STORE_SIMD(results, v_state);

            for(size_t j = 0; j < SIMD_W; ++j) {
                size_t idx = i + j;
                frame_buffer[idx].timestamp = 0;
                frame_buffer[idx].order_id  = static_cast<uint32_t>(packets_sent + idx + 1);

                bool is_trade = (results[j] % 10 == 0);
                frame_buffer[idx].type = is_trade ? 'T' : 'Q';

                int drift = (results[j] % 3) - 1;
                mid = std::clamp(mid + drift, 950, 1050);

                bool is_buy = (results[j] & 1);
                frame_buffer[idx].side = is_buy ? 'B' : 'S';

                if (is_trade) {
                    frame_buffer[idx].price = mid;
                    frame_buffer[idx].qty   = 1 + (results[j] % 50);
                } else {
                    int spread_half = 1 + (results[j] % 3);
                    frame_buffer[idx].price = is_buy ? (mid - spread_half) : (mid + spread_half);
                    frame_buffer[idx].qty   = 50 + (results[j] % 500);
                }
            }
        }

        sendto(client_fd, frame_buffer.data(), packets_per_frame * sizeof(PacketType), 0,
               (struct sockaddr*)&serv_addr, sizeof(serv_addr));

        packets_sent += packets_per_frame;
    }
    close(client_fd);
}

int main() {
    const int BASE_PORT = 12345;
    const size_t TOTAL_PACKETS = 5000000;
    using clock = std::chrono::steady_clock;

    try {
        MarketDataReceiver<ActivePacketType> receiver(zero_copy_queue, BASE_PORT);
        int resolved_port = receiver.get_active_port();

        std::cout << "Starting Lock-Free ZERO-COPY L1 (BBO) Pipeline..." << std::endl;
        std::cout << "Mock Exchange Thread -> Core 2" << std::endl;
        std::cout << "IO Network Thread    -> Core 3" << std::endl;
        std::cout << "BBO Engine Core      -> Core 4" << std::endl;

        std::thread exchange_thread(run_mock_exchange_tx<ActivePacketType>, resolved_port, TOTAL_PACKETS);
        
        auto start_time = clock::now();

        std::thread engine_thread(run_bbo_engine_consumer<ActivePacketType>, 
                                  std::ref(bbo_book), 
                                  std::ref(zero_copy_queue), 
                                  TOTAL_PACKETS);

        receiver.run_receiver_loop(TOTAL_PACKETS);
        
        if (engine_thread.joinable()) engine_thread.join();
        auto end_time = clock::now();
        
        if (exchange_thread.joinable()) exchange_thread.join();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

        std::cout << "\n================ L1 BBO ENGINE RUN STATS ================" << std::endl;
        std::cout << "Total Data Streamed    : " << TOTAL_PACKETS << " Packets" << std::endl;
        std::cout << "Packet Footprint       : " << sizeof(ActivePacketType) << " Bytes" << std::endl;
        std::cout << "BBO Book Footprint     : " << sizeof(BBOBook) << " Bytes (Single Cache Line)" << std::endl;
        std::cout << "Total Processing Time  : " << duration << " microseconds" << std::endl;
        std::cout << "Average Cost Per Packet: " << (double)duration * 1000.0 / TOTAL_PACKETS << " nanoseconds" << std::endl;
        std::cout << "Best Bid (BBO)         : " << bbo_book.get_bid_price() << " (Qty: " << bbo_book.get_bid_qty() << ")" << std::endl;
        std::cout << "Best Ask (BBO)         : " << bbo_book.get_ask_price() << " (Qty: " << bbo_book.get_ask_qty() << ")" << std::endl;
        std::cout << "Top-of-Book Spread     : " << bbo_book.spread() << std::endl;
        std::cout << "Top-of-Book Mid Price  : " << bbo_book.mid_price() << std::endl;
        std::cout << "Top-of-Book Micro Price: " << bbo_book.micro_price() << std::endl;
        std::cout << "Book Imbalance (OBI)   : " << bbo_book.imbalance() << std::endl;
        std::cout << "Last Trade Price       : " << bbo_book.get_last_price() << " (Qty: " << bbo_book.get_last_qty() << ")" << std::endl;
        std::cout << "Total Traded Volume    : " << bbo_book.get_total_volume() << std::endl;
        std::cout << "Session VWAP           : " << bbo_book.session_vwap() << std::endl;
        std::cout << "=========================================================" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Engine Runtime Failure Exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}