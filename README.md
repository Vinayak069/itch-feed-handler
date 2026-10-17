# itch-feed-handler

A low-latency market-data feed handler for NASDAQ TotalView-ITCH 5.0, written in C++20.

It decodes the binary ITCH protocol and receives it the way exchanges publish it: MoldUDP64 packets over UDP multicast. It detects lost packets from sequence numbers and passes packets between pinned threads through a lock-free ring. From the result it rebuilds the full-depth order book of every stock on the feed.

```
sender ──UDP multicast (MoldUDP64)──▶ receive thread ──lock-free SPSC ring──▶ book thread
                                      recv() straight into                     gap check, parse,
                                      a ring slot (no copy)                    prefetch, apply
```

## Results on a real NASDAQ trading day

This is NASDAQ's full TotalView-ITCH 5.0 capture for 30 Dec 2019: 268,744,780 messages. It is downloaded and run by the [`Real NASDAQ data`](.github/workflows/real-data.yml) workflow on a GitHub-hosted runner (4 vCPUs, Intel Xeon 6973P-C). The raw output is in [`docs/results/real/`](docs/results/real).

**Full-day replay: file → parser → order books for every stock on the feed.**

| | |
|---|---:|
| Messages | 268,744,780 |
| Time (parsing + book updates, prefetch on) | **25 s** |
| Throughput | **10.7M messages/s, 93 ns/message** |
| Same, without prefetching | 111 ns/message |
| Peak resting orders | 1.92M |
| Order messages for unknown or duplicate references | **0 / 0** |
| Resting orders left after the close | 0 |

The book reconciles: every execute, cancel, delete and replace in the day matched an order the handler had seen added. At the end of the day no resting orders are left.

Timed individually, per-message cost is p50 81 ns, p99 465 ns and p99.9 719 ns. Prefetching makes the replay 16% faster on real data. Huge pages made no measurable difference on this runner, unlike the cloud VM below.

**Live path on real data.** The first 20M messages of the day are replayed through MoldUDP64 → UDP multicast → SPSC ring → book, paced at 1M msg/s, with the threads pinned. That is 473,899 packets of about 42 messages each, with no packets lost. The table gives per-packet latency (median of 3 runs).

| Hop | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Send → receive (kernel loopback) | 2.9 µs | 3.3 µs | 4.2 µs | 20 µs |
| Waiting in the ring | 0.18 µs | 0.23 µs | 2.3 µs | 14 µs |
| Parse + apply the packet | 3.8 µs | 5.6 µs | 8.6 µs | 22 µs |
| **End to end** | **7.0 µs** | **9.0 µs** | **15.4 µs** | **39 µs** |

## Results on synthetic data (cloud VM)

Measured on a 4-vCPU cloud VM (Intel Xeon @ 2.8 GHz, 4 MB L2 per core, 33 MB L3), g++ 13.3, `-O3`. Each figure is the median of 3 to 5 runs; the raw output of every run is in [`docs/results/`](docs/results). To reproduce, run `scripts/run_benchmarks.sh`.

**Replay: file → parser → order books.** 50M messages, 3,000 stocks, about 1.05M resting orders at any time.

| Configuration | ns / message | messages / s |
|---|---:|---:|
| Parse and frame only (no book) | 6.4 | 157M |
| Book, 4 KB pages, no prefetch | 259 | 3.9M |
| + huge pages for the order table | 236 | 4.2M |
| + prefetch 8 messages ahead | **161** | **6.2M** |

Timing each message individually (which adds clock overhead): p50 114 ns, p99 551 ns, p99.9 915 ns.

**Live path over UDP multicast on loopback.** 5M messages in 116,865 packets of about 43 messages each. Sender, receiver and book threads are pinned to separate cores. Latencies are per packet.

| Paced at | Send → receive p50 | Time in ring p50 | Parse + apply p50 | End to end p50 / p90 | Lost |
|---|---:|---:|---:|---:|---:|
| 1M msg/s, no prefetch | 5.3 µs | 0.2 µs | 10.0 µs | 16.1 / 33 µs | 0 |
| 1M msg/s | 5.3 µs | 0.3 µs | **7.5 µs** | **13.6 / 25 µs** | 0 |
| 3M msg/s | 5.1 µs | 0.3 µs | 6.4 µs | 12.9 / 57 µs | 0 |
| Unthrottled (6–7M msg/s) | n/a | n/a | 5.0 µs | n/a | 1.1M–2.0M msgs, 45–60 gaps, all detected |

When unthrottled, the sender outruns the book thread. The ring fills, the kernel's socket buffer overflows, and packets are dropped. The gap detector reports every one of those drops. Prefetching a packet's orders before applying it cuts parse-and-apply time by about 25%.

**Hand-off between two pinned threads** (`queue_bench`):

| Queue | Throughput | One-way latency p50 / p99 |
|---|---:|---:|
| Lock-free SPSC ring, indices on separate cache lines | **46M items/s** | 265 / 955 ns |
| Same ring, indices sharing a cache line | 24M items/s | 259 / 903 ns |
| Mutex-guarded ring | 5.2M items/s | 971 / 12,223 ns |

Removing false sharing doubles throughput. It does not change ping-pong latency, because there is only one item in flight.

**About the tails.** The p99 and p99.9 figures on the live path run from hundreds of microseconds to milliseconds, and they move a lot between runs. The cause is the shared VM rather than the code: the large values come in bursts of consecutive packets, which is what a descheduled vCPU looks like, and the kernel reports steal time on this machine. For comparison, the same live path on the less noisy GitHub runner has a real-data p99 of 15 µs (above). Clean tails need isolated cores (`isolcpus`, `nohz_full`) on bare metal.

## Design decisions

- **Parsing.** ITCH fields are big-endian and packed, so every field is read with `memcpy` plus a byte swap. Casting the buffer to a struct would be an unaligned read and undefined behaviour. A 256-entry table gives the expected length of each message type, so a corrupt length is caught before any field is read. A message cut off at the end of a read buffer is carried over to the next read rather than treated as an error. ([`messages.hpp`](include/itch/messages.hpp), [`framing.hpp`](include/itch/framing.hpp))
- **Order table.** Executions, cancels and deletes carry only an order reference number, so every one of them starts with a lookup. The table is a flat open-addressing hash with 24-byte slots, so one probe touches one cache line ([`order_store.hpp`](include/book/order_store.hpp)).
  - It uses a Fibonacci (multiplicative) hash, because NASDAQ assigns references almost sequentially and an identity hash would cluster them.
  - Deletion is by backward shift, so there are no tombstones and probe runs stay short even though most orders die young.
  - The slot array is 2 MB aligned and `madvise(MADV_HUGEPAGE)`, because at about 100 MB, 4 KB pages make nearly every lookup a TLB miss.
- **Prefetching.** Each message's order slot is prefetched a few messages before the message is applied. In replay this is a sliding window; on the live path it is a first pass over each packet. The result is that independent cache misses overlap instead of happening one after another.
- **Price levels.** Each side of each book is a vector of aggregated levels sorted worst to best, so the best price is the last element. Most adds and removals happen near the touch, so inserts and erases move few elements. This beats a `std::map` on cache behaviour. ([`order_book.hpp`](include/book/order_book.hpp))
- **SPSC ring.** The ring has power-of-two capacity, with acquire/release ordering on the head and tail ([`spsc_queue.hpp`](include/feed/spsc_queue.hpp)).
  - The producer and consumer indices sit on separate 64-byte lines to avoid false sharing; the `Padded = false` variant exists only to measure that.
  - Each side caches the other's index and re-reads the shared atomic only when the ring looks full or empty.
  - The receive thread `recv()`s directly into a reserved slot, so a packet is never copied.
- **Threads.** Socket draining is separate from book building. The receive thread does nothing but empty the socket into the ring, so a slow burst on the book thread is absorbed by the ring rather than by the kernel buffer. Threads busy-poll on pinned cores, because a blocking wake-up costs microseconds.
- **Gap detection.** MoldUDP64 gives each packet the sequence number of its first message, and [`GapDetector`](include/itch/framing.hpp) counts the messages missing between packets. A production handler would then request a retransmission or rebuild from a snapshot. Here the gap is counted, and later messages for orders it never saw are counted as `unknown_ref`.

## Data

NASDAQ publishes sample days of real ITCH data. The real-data results above come from the 30 Dec 2019 file, run in GitHub Actions (start it from the Actions tab). `scripts/fetch_itch_day.sh` does the same on your own machine.

The synthetic stream from `itch_gen` is for the tests and for machines that cannot reach NASDAQ's server. It is built as follows:
- The messages are byte-exact ITCH 5.0.
- The message mix follows the published counts for that 2019 day: adds 44%, deletes 43%, replaces 8%, executions 2%, partial cancels 1%.
- About one million orders rest at once, and activity is spread across 3,000 stocks by a Zipf law.
- New orders are placed against the generator's own copy of the book, so the book is never crossed. Every execute, cancel, delete and replace refers to a live order.

It is still a model. Price paths and order lifetimes are simple random processes, so numbers on real data will differ.

## Testing

26 GoogleTest tests run in CI in three builds: Release, AddressSanitizer + UndefinedBehaviorSanitizer, and ThreadSanitizer.

- **Protocol:** spec sizes for every message type; a hand-assembled Add Order checked byte by byte; encode/decode round trips; 6-byte timestamps.
- **Framing:** truncated tails at every cut point, wrong lengths, unknown types, and MoldUDP64 count mismatches; a full packetize/unpacketize round trip.
- **Order table:** 400,000 random inserts and erases checked against `std::unordered_map`.
- **Order book:** a differential test against a plain `std::map` reference book. It checks best bid and offer after every order message in a 300,000-message feed, and every price level at the end. It also checks that the generated book is never crossed.
- **SPSC ring:** a two-thread stress test with 1M items through a 16-slot ring, run under ThreadSanitizer.
- **Live path:** real UDP multicast on loopback.
  - All 200,000 messages arrive and build the same book as the reference.
  - With every 50th packet deliberately dropped, the detector reports exactly those gaps.

## Build and run

Needs Linux, CMake 3.20+, a C++20 compiler and GoogleTest (`libgtest-dev`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build                                # or build with -DSANITIZE=address / thread

./build/itch_gen --messages 50000000 --out feed.itch  # synthetic day
./build/itch_replay --file feed.itch                  # throughput; --latency, --prefetch 0, --no-huge-pages
./build/feed_bench --rate 1000000 --pin               # live UDP path; --rate 0, --drop-every 50
./build/queue_bench --pin                             # SPSC vs mutex
```

## Layout

```
include/itch/   messages (decode/encode), framing (file + MoldUDP64, gap detector), generator
include/book/   order_store (hash table), order_book (levels, per-stock books)
include/feed/   spsc_queue, mutex_queue, udp sockets, pipeline (threads)
include/util/   clock and CPU pinning, latency histogram (log-linear, ~0.8% precision)
src/            implementations
tools/          itch_gen, itch_replay, feed_bench, queue_bench
tests/          GoogleTest suites + a std::map reference book
scripts/        run_benchmarks.sh, fetch_itch_day.sh
```

## Limitations

- **No recovery after a gap.** Gaps are detected and counted, but nothing requests a retransmission (MoldUDP64 re-request) or rebuilds from a snapshot.
- **Kernel networking only.** Sockets go through the standard kernel network stack. Production systems use kernel bypass (DPDK, ef_vi or Onload), which removes most of the 5 µs send → receive time.
- **One book thread.** A single thread applies messages for all stocks. Sharding stocks across book threads by locate code is the next step for higher rates.
