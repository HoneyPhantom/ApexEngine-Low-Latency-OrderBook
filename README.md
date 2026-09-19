# ApexEngine: Low-Latency C++20 Order Matching Pipeline

[![Build](https://github.com/honeyphantom/apexengine-low-latency-orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/honeyphantom/apexengine-low-latency-orderbook/actions)
[![Language](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg?style=flat-square)](https://en.cppreference.com/w/cpp/20)

`ApexEngine` is a personal project exploring how order-matching engines and
market-data pipelines are built for low latency: lock-free queues, zero-copy
data paths, cache-aware memory layout, and compile-time validation instead of
runtime checks. It's a learning and portfolio project, not a production
trading system — it isn't connected to any real exchange or live market
data, and hasn't been hardened for the edge cases a real venue would throw
at it.

I built it to understand *why* these techniques matter, not just to use
them: every design choice below exists because I first tried the naive
version, measured it, and then looked at what was actually slow.

---

## Benchmarks

All numbers below are from local runs on my own development machine, not
production hardware or a live trading environment. Treat them as a relative
measure of "did this optimization help," not as latency guarantees.

### Order book mutation cost (Google Benchmark)

`BM_OrderBook_AddLimit`, 100 repetitions, aggregates only:

| Metric | Value |
|---|---|
| Mean | 4.78 ns |
| Median | 4.77 ns |
| Std Dev | 0.069 ns |
| Coefficient of Variation | 1.44% |
| Implied throughput | ~209M updates/sec (single core) |
| Build | Release |
| Repetitions | 100 |

The low CV (1.44%) across 100 repetitions means this number is stable, not
a lucky single run — it's the measurement I'd stand behind if asked about
it directly.

### Benchmark environment

- Machine: MacBook Air, Apple Silicon, 8-core CPU, Apple M1
- CPU caches: L1 Data 64 KiB, L1 Instruction 128 KiB, L2 Unified 4096 KiB (×8)
- OS: macOS
- Compiler / flags: clang 16.0.6, -O3 -march=native

> **Note on the clock-rate warning:** running this on Apple Silicon prints
> `Unable to determine clock rate from sysctl: hw.cpufrequency: No such
> file or directory` and a thread-affinity warning. Both are expected and
> cosmetic — Apple Silicon doesn't expose `hw.cpufrequency` or support
> `pthread_setaffinity_np` the way Linux/Intel do, so Google Benchmark
> can't report estimated CPU frequency or pin the benchmark thread. This
> does not affect the actual timing measurements above, which come from a
> high-resolution clock, not frequency estimation.

### End-to-end pipeline throughput

Simulated 5,000,000 synthetic market data packets through the full
producer → matching engine pipeline:

| Metric | Value |
|---|---|
| Packet size | 24 bytes |
| Total wall time | 611,298 µs |
| Mean latency per packet | ~122 ns |

---

## Reproducing these benchmarks

```bash
cd build
./apex_benchmark \
  --benchmark_repetitions=100 \
  --benchmark_report_aggregates_only=true
```

This runs each benchmark 100 times and reports mean, median, stddev, and
coefficient of variation instead of a single run — a single run can be
skewed by a background process, thermal throttling, or scheduler noise, so
repetitions are what make the number trustworthy rather than lucky.

To filter to a specific benchmark:
```bash
./apex_benchmark --benchmark_filter=BM_OrderBook_AddLimit --benchmark_repetitions=100 --benchmark_report_aggregates_only=true
```

To also export machine-readable results (useful if you want to track
numbers over time as you change the code):
```bash
./apex_benchmark --benchmark_repetitions=100 --benchmark_report_aggregates_only=true \
  --benchmark_format=json --benchmark_out=results.json
```

---

## Design notes

### Zero-copy SPSC queue

A single-producer/single-consumer ring buffer that avoids copying packets
between threads:

- **Direct write slots** — `get_write_slot()` hands the producer a pointer
  into the ring buffer itself, so incoming packets are parsed directly into
  place instead of being copied in afterward.
- **Cache-line-aligned indices** — `write_idx` and `read_idx` are padded to
  64 bytes (`alignas(64)`) so producer and consumer updates don't invalidate
  each other's cache lines (false sharing).
- **Acquire/release atomics** — uses `memory_order_acquire` /
  `memory_order_release` instead of full sequential consistency, since the
  queue only needs to guarantee ordering between the two threads that
  actually touch it.

### O(1) price-level lookups

Best-bid/best-ask lookups use a bitmask representation instead of
`std::map` or `std::unordered_map`:

- Price levels are packed into 64-bit words; `__builtin_clzll` /
  `__builtin_ctzll` find the best active price in a single instruction
  rather than walking a tree.
- Trade-off worth being upfront about: this bounds the representable price
  range to what fits in the bitmask tiers, which is a real constraint, not
  a free win — it's a reasonable trade for a fixed instrument's tick range,
  less so for something with a huge or sparse price domain.

### Pre-allocated memory pools

Avoids `malloc`/`new` on the hot path:

- A fixed-size pool is allocated once at startup.
- Objects are constructed in place with placement `new` and manually
  destructed on release, with freed slots tracked via a simple free-list
  stack.

### Core affinity pinning

Pins the packet generator, network/IO handling, and matching engine to
separate logical cores (`pthread_setaffinity_np` on Linux,
`thread_policy_set` via Mach on macOS) to reduce scheduler-induced jitter
between threads.

> Note: as the benchmark warning above shows, `pthread_setaffinity_np`
> pinning isn't actually supported on Apple Silicon the way it is on
> Linux/Intel — this affects the *benchmark's own* thread, not necessarily
> the engine's pipeline threads, but it's worth being aware of when
> interpreting results measured on Apple Silicon versus Linux.

### Compile-time validation

Uses C++20 concepts and `consteval` to catch structural problems at compile
time instead of runtime:

- `ValidMarketPacket` constrains packet types to be trivially copyable,
  standard-layout, ≤128 bytes, and aligned to at least 4 bytes.
- `StaticConfigManager` uses `consteval` constructors to catch duplicate or
  malformed feed schema entries during compilation rather than at startup.

---

## Repository layout

```text
├── .github/workflows/
│   └── ci.yml              # Build verification on every push
├── .vscode/
│   └── tasks.json           # Editor build/run tasks
├── apps/
│   └── main.cpp             # End-to-end pipeline harness
├── benchmarks/
│   └── book_benchmark.cpp   # Google Benchmark micro-benchmarks
├── include/low_latency/
│   ├── concepts.hpp         # C++20 concepts / compile-time constraints
│   ├── engine.hpp           # Memory pool, SPSC queue, matching logic
│   └── thread_utils.hpp     # Core affinity helpers
├── tests/
│   └── orderbook_test.cpp   # Correctness tests (data integrity, partial fills)
├── .gitignore
└── CMakeLists.txt
```

---

## Setup and build instructions

This project uses CMake, targets C++20, and depends on
[Google Benchmark](https://github.com/google/benchmark) and
[GoogleTest](https://github.com/google/googletest). The steps below assume
those are either installed system-wide or pulled in automatically via
CMake's `FetchContent` in `CMakeLists.txt` — check your `CMakeLists.txt` to
see which approach it uses, and drop the manual install steps below if it's
already fetching dependencies itself.

### Prerequisites (both platforms)

- CMake 3.20+
- A C++20 compiler: GCC 11+ or Clang 14+
- Git

### macOS

```bash
# Xcode command line tools give you clang + make
xcode-select --install

# Install CMake and dependencies via Homebrew
brew install cmake googletest google-benchmark

# Clone and build
git clone https://github.com/honeyphantom/apexengine-low-latency-orderbook.git
cd apexengine-low-latency-orderbook
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(sysctl -n hw.ncpu)"
```

### Linux (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install -y build-essential cmake git libgtest-dev libbenchmark-dev

# libgtest-dev on Debian/Ubuntu ships sources rather than a prebuilt lib —
# if CMake can't find GTest, build it once:
cd /usr/src/googletest && sudo cmake . && sudo cmake --build . --target install

# Clone and build
git clone https://github.com/honeyphantom/apexengine-low-latency-orderbook.git
cd apexengine-low-latency-orderbook
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(nproc)"
```

### Running things

From the `build/` directory after building:

```bash
# Run the end-to-end pipeline harness
./apex_runner

# Run the Google Benchmark micro-benchmarks (see "Reproducing these
# benchmarks" above for the recommended flags)
./apex_benchmark

# Run the correctness test suite
ctest --output-on-failure
# or directly:
./apex_test
```

---

## Testing

Correctness tests live in `tests/orderbook_test.cpp` and build to the
`apex_test` binary. Coverage includes:

- Order add / cancel / modify
- Partial fills
- Price-time priority ordering under simultaneous same-price orders
- Empty book edge cases

<!-- Update this list to match what orderbook_test.cpp actually covers,
     and add the real test count once you've counted them, e.g.
     "14 tests, all passing — see the Build badge above." -->

**Not currently tested:**
- Concurrent multi-book access
- Behavior under sustained high-contention load
- Fuzzing / property-based testing

Run the suite with:
```bash
cd build
ctest --output-on-failure
```

---

## Limitations

Being upfront about scope:

- Single-machine, synthetic-data benchmarks only — no live exchange
  connectivity, no real market data feed, no network layer beyond the
  simulated packet generator.
- Not tested under real contention scenarios (multiple books, cross-core
  traffic beyond the fixed pinning above).
- No persistence, recovery, or crash-safety story — none is needed for a
  benchmarking/learning project, but it would be a hard requirement before
  any of this touched a real system.

## Possible next steps

- A simple exchange simulator (replay historical or synthetic order flow
  against the matching engine) to test behavior under more realistic,
  bursty load rather than a uniform synthetic stream.
- Basic post-trade analysis tooling on top of the matching engine's output
  (fill rates, latency distribution rather than just the mean, slippage
  under load).
- Re-run the end-to-end pipeline benchmark with `--benchmark_repetitions`
  the same way `BM_OrderBook_AddLimit` was, to get a mean ± stddev instead
  of a single-run number.
