# ApexEngine: Low-Latency C++20 Order Matching Pipeline

[![Language](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg?style=flat-square)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux-lightgrey.svg?style=flat-square)]()
[![Build Status](https://img.shields.io/badge/Build-Passing-brightgreen.svg?style=flat-square)]()
[![Tests](https://img.shields.io/badge/Tests-18%2F18%20Passed-brightgreen.svg?style=flat-square)]()

`ApexEngine` is a C++20 portfolio project built to explore the mechanical sympathies and data-oriented design patterns required in modern high-frequency trading (HFT) systems. 

It implements four tiers of market-data / order-matching engines—**BBO (Top of Book), L1, L2 (Price Depth), and L3 (Market-by-Order)**—connected via a lock-free, zero-copy pipeline. 

**Note on scope:** This is a learning vehicle. It is not connected to a live exchange, does not utilize kernel bypass (e.g., Solarflare/ef_vi or DPDK), and hasn't been hardened against the edge cases of a real venue. I built it to measure *why* certain low-latency techniques matter (e.g., watching L3 latency drop from 1,000 ns to 2 ns by ditching open-addressed hash tables for flat direct arrays and compressing structs to fit exactly two per cache line).

## 🧠 Core Engineering Principles

*   **Zero-Copy SPSC Ring Buffer:** A Single-Producer/Single-Consumer queue where the producer writes directly into cache-aligned memory slots. Uses strictly bounded `memory_order_acquire` / `memory_order_release` atomics.
*   **O(1) Bitmask Price Routing:** Price ladders avoid `std::map` entirely. Active price levels are mapped to 64-bit integers; finding the best bid/ask is a single hardware instruction (`__builtin_clzll` / `__builtin_ctzll`).
*   **Static Flat Memory Architecture (L3):** Zero heap allocations on the hot path. Order IDs are routed via a zero-collision flat static array (`OrderDirectMap`).
*   **Cache-Line Density:** The L3 `Order` struct is explicitly compressed and padded to exactly **32 bytes**, allowing exactly two orders per 64-byte L1 cache line, halving cache misses during aggressive FIFO queue sweeps.
*   **Compile-Time Validation:** Utilizes C++20 `concepts` and `consteval` to validate packet layouts, alignment, and schema mappings during compilation rather than startup.

---

## ⏱️ Micro-Benchmarks (Google Benchmark)

Benchmarked over **20 repetitions** to guarantee statistical stability (minimizing scheduler noise/jitter). 
*Hardware: Apple M1 (8-Core), 8GB RAM, Apple Clang 21.0.0.*

### BBO Engine (Top of Book)
| Operation | Mean Latency | Throughput | Note |
| :--- | :--- | :--- | :--- |
| `OnQuoteUpdate` | **1.02 ns** | 1.04 G ops/s | Scalar assignment + minimal branching. |
| `ProcessPacket` | **0.94 ns** | 1.06 G ops/s | Full dispatch pipeline. |
| `ComputeAnalytics` | **1.55 ns** | 643 M ops/s | Mid-price, Micro-price, and OBI math. |

### L1 / L2 Engine (Price Depth)
| Operation | Mean Latency | Throughput | Note |
| :--- | :--- | :--- | :--- |
| `UpdateLevel` (L1/L2) | **~1.30 ns** | 1.54 G ops/s | Direct array write + bitmask `set()`. |
| `ExecuteMarketOrder` (L2) | **1.67 ns** | 597 M ops/s | Sweeps crossing levels in < 2 ns. |
| `DeltaLevel_FastPath` (L2) | **2.07 ns** | 966 M ops/s | In-place volume deduction. |
| `ProcessPacket` (L2) | **0.97 ns** | 1.02 G ops/s | End-to-end packet routing. |

### L3 Engine (Market-by-Order)
The L3 engine maintains discrete order identities and strict Price-Time (FIFO) priority.
| Operation | Mean Latency | Throughput | Note |
| :--- | :--- | :--- | :--- |
| `AddOrder_Passive` | **1.93 ns** | 1.03 G ops/s | Slab alloc + tail pointer append + direct array write. |
| `AddAndCancel_Cycle` | **1.43 ns** | 1.39 G ops/s | Doubly-linked list link/unlink (fits entirely in store buffer). |
| `ModifyOrder_ReduceQty`| **2.39 ns** | 418 M ops/s | Direct ID lookup + in-place deduction without priority loss. |
| `CrossMatching_Sweep` | **6.40 ns** | 156 M ops/s | Sweeping an aggressive taker across multiple resting maker orders. |
| `ProcessPacket` | **2.45 ns** | 408 M ops/s | Full decode, dispatch, and state mutation. |
| `QueuePosition_Lookup` | **233 ns** | 4.29 M ops/s | *Out-of-band monitoring path.* Linear traversal of 50 orders. |

---

## 🚀 End-to-End Pipeline Performance

Synthetic benchmark injecting **5,000,000 packets** (24 bytes each) over localhost UDP into the receiver thread (Core 3), parsed via the SPSC zero-copy queue, and matched by the engine thread (Core 4).

Because the internal engines process packets in ~1.5 - 2.5 ns, the end-to-end pipeline latency is heavily bounded by the L2 cache-coherency (store-forwarding) limits of the Apple M1 silicon across physical cores.

| Engine Tier | Total Run Time (5M Packets) | Avg Latency Per Packet (Wire-to-Engine) |
| :--- | :--- | :--- |
| **L1 BBO** | 741,961 µs | **148.39 ns** |
| **L2 Depth** | 793,251 µs | **158.65 ns** |
| **L3 MBO** | 740,801 µs | **148.16 ns** |

*Note: The L3 pipeline actually edges out L2 throughput slightly due to the O(1) zero-collision flat index map preventing tree/hash lookups.*

---

## 🧪 Testing & Reliability

The engine logic is verified by a strict GoogleTest suite (`tests/orderbook_test.cpp`). All **18/18 tests** pass.
Coverage includes:
* Price-time FIFO queue integrity during middle-order cancellations.
* Order modification priority rules (size reduction retains priority; size increase loses priority).
* Crossed-book resolution (sweeping aggressive takers against multiple resting price levels).
* Edge cases (out-of-bounds prices, partial fills, empty book state).

---

## 🛠️ Build & Run Instructions

### Prerequisites
* CMake 3.22+
* C++20 compliant compiler (Apple Clang 21.0.0+ or GCC 11+)
* Git

### Compiling
Dependencies (`gtest`, `benchmark`) are fetched automatically by CMake.

```bash
git clone [https://github.com/honeyphantom/apexengine-low-latency-orderbook.git](https://github.com/honeyphantom/apexengine-low-latency-orderbook.git)
cd apexengine-low-latency-orderbook
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"

```

### Running the End-to-End Pipelines

```bash
./build/apexBBO_runner
./build/apexL1_runner
./build/apexL2_runner
./build/apexL3_runner

```

### Running Micro-Benchmarks & Unit Tests

```bash
# Run unit tests
./build/apex_tests

# Run benchmarks (Google Benchmark will output full stats)
./build/apexL3_benchmark --benchmark_repetitions=20 --benchmark_report_aggregates_only=true

```

---

## ⚠️ Environmental Limitations (macOS / Apple Silicon)

* **Thread Affinity:** You will see a warning: `Failed to set thread affinity`. Apple Silicon/macOS Mach threads do not support standard Linux `pthread_setaffinity_np` or strict core isolation. Pipeline thread pinning is "best effort" via Mach thread policies, meaning context-switch jitter is slightly higher here than it would be on an isolated Linux `PREEMPT_RT` kernel.
* **CPU Frequency Scaling:** Apple M1 dynamically scales frequencies. Benchmarks are run without disabling CPU throttling (unlike production Linux servers where C-states are locked).

---

## 🔮 Future Roadmap

* **Kernel Bypass Simulation:** Replace the standard POSIX UDP `recv()` sockets with an `io_uring` implementation for Linux to simulate bypass polling.
* **SIMD Parsing:** Implement AVX-512 / ARM NEON vectorized packet parsing for the ingress feed handler.
* **TCP Outbound:** Add a lightweight Order Entry (OE) TCP session handler to emit trade fills and acknowledgments back to the client.

- A simple exchange simulator (replay historical or synthetic order flow
  against the matching engine) to test behavior under more realistic,
  bursty load rather than a uniform synthetic stream.
