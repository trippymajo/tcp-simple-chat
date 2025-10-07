Lock-free Data Structures — Overview
1. SPSC Ring Buffer (Single-Producer / Single-Consumer)

Use case: Passing data between two threads (network RX→parser, stdin→TX, driver→user mode, audio/video pipelines).

Pros: Ultra-low latency, no locks, cache-friendly (array), very simple.

Cons: Only 1 producer and 1 consumer, usually fixed capacity.

2. MPSC Queue (Multi-Producer / Single-Consumer)

Use case: Many worker threads enqueue logs/events → one thread flushes to disk/network.

Pros: Multiple writers, one reader keeps order.

Cons: Writers contend on a single index, bounded capacity.

3. MPMC Queue (Multi-Producer / Multi-Consumer)

Variants: Michael–Scott linked-list queue, bounded array-based queues (LCRQ, Vyukov).

Use case: General-purpose work-queues for thread pools, message brokers.

Pros: Any number of writers and readers.

Cons: Complex, higher contention, expensive memory reclamation if linked-list based.

4. Treiber Stack (Lock-free stack / freelist)

Use case: Memory/buffer pools (recycle objects, buffers for RX/TX).

Pros: Very simple (CAS on top pointer), fast.

Cons: ABA problem, safe reclamation required (hazard pointers/epochs).

5. Work-Stealing Deque (Chase–Lev)

Use case: Task schedulers, work-stealing runtimes.

Pros: Efficient parallel scheduling, reduces contention.

Cons: More complex, narrow use case.

6. Lock-free Hash Map / Set

Use case: High-throughput lookups (connection tables, telemetry indices, caches).

Pros: Readers don’t block writers, can scale with RCU or epochs.

Cons: Very complex implementation, rehashing/resizing is hard.

7. Lock-free Skip List

Use case: Ordered storage, range queries (time-ordered logs, LSM-tree components).

Pros: Easier to implement lock-free than trees, supports ordered scans.

Cons: Higher overhead than arrays, complex reclamation.

8. Atomic Counters, Bitsets, Flags

Use case: Statistics, semaphores, signaling between threads.

Pros: Very simple, universal.

Cons: Easy to waste CPU with busy-wait if misused.

9. RCU (Read-Copy-Update, concept)

Use case: Read-mostly data structures (policies, large read-only maps).

Pros: Reads are completely lock-free; updates create new version.

Cons: Requires epochs / grace periods; non-trivial for general apps.

General Pros of Lock-free Structures

Lower latency and higher throughput (no mutexes, fewer syscalls).

No priority inversion (no thread stuck holding a lock).

Cache-friendly (especially ring buffers).

General Cons

Complexity: Hard to implement and verify correctness.

Memory reclamation is tricky (Hazard Pointers, Epoch, RCU).

Potential starvation under contention.

Specialized: e.g., SPSC cannot be reused as MPSC/MPMC.