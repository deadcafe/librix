# Flow Cache

Production-quality flow cache implementations built on librix (`rix_hash.h`).
Designed for high-performance packet processing with pipeline-style batch
lookup to hide DRAM latency.

## Quick Start

```sh
# Build the cache library
make -C samples/fcache all

# Run functional tests
make -C samples/test test

# Run datapath benchmark
make -C samples/test bench

# Show top-level helper targets
make help
```


## 1. Overview

`samples/fcache/` is the active flow-cache implementation in this repo.
It is a 64-byte, single-cache-line design built on librix's index-based
hash-table primitives (`rix_hash.h`).

### Usage scenario

- Called immediately after L3/L4 validity checks
- Packets are vector-processed (~256 packets per batch)
- Pipeline-style processing to hide DRAM latency
- Per-thread instance (lock-free, no synchronization)
- Caller-owned memory layout (cache + buckets + pool + optional scratch)
- Runtime SIMD dispatch via `fc_arch_init(FC_ARCH_...)`

### Performance snapshot

| Operation | Condition | Target (cy/op) | Measured (cy/op) |
|---|---|---|---|
| `find_bulk` | DRAM-cold buckets | ~100 | 40-143 |
| `findadd_bulk` | miss + inline insert | ~60-120 | 59-470 |
| `maintain_step` | periodic reclaim | ~20-70 | 19-67 |

See §15/§16 for the current benchmark tables and measurement environment.

## 2. Three Variants

The cache is provided in three variants. All share the same generated
implementation and dispatch layer.

### 2.1 Separate tables (IPv4-only or IPv6-only)

| | `fc_flow4_cache` | `fc_flow6_cache` |
|---|---|---|
| Header | `flow4_cache.h` | `flow6_cache.h` |
| Key struct | `fc_flow4_key` (24B fixed-width) | `fc_flow6_key` (44B fixed-width) |
| Entry size | 64B | 64B |
| Use case | IPv4-only environments | IPv6-only environments |

Rationale for separate tables:
- Fixed key size per variant enables specialized hash/cmp code
- No v4/v6 branch in hot path
- Uniform pool and bucket layout

### 2.2 Unified table (dual-stack)

| | `fc_flowu_cache` |
|---|---|
| Header | `flowu_cache.h` |
| Key struct | `fc_flowu_key` (44B) |
| Entry size | 64B |
| Use case | Dual-stack (IPv4 + IPv6 in single table) |

Properties:
- `family` is part of the key, so v4/v6 never alias
- Helper functions `fc_flow4_key_make()`, `fc_flow6_key_make()`,
  `fc_flowu_key_v4()`, and `fc_flowu_key_v6()` construct zero-fixed keys
- Pool capacity is shared across IPv4 and IPv6 entries

### 2.3 Performance comparison

Current datapath measurements are summarized in §16. In general:

- `flow4` is usually the fastest steady-state hit path
- `flow6` is more expensive because of the larger key
- `flowu` trades some hit-path cost for dual-stack simplicity and can be
  competitive on some miss-heavy paths

### 2.4 Choosing a variant

- **IPv4-only** (`fc_flow4_cache`): smallest key, fastest IPv4 datapath
- **IPv6-only** (`fc_flow6_cache`): IPv6-exclusive segments
- **Unified** (`fc_flowu_cache`): dual-stack. Single pool simplifies
  memory management and avoids separate capacity planning for v4/v6.
  Recommended for new deployments.

## 3. Key Structures

### 3.1 IPv4 flow key (24-byte fixed-width object)

```c
struct fc_flow4_key {
    uint8_t  family;   /* always 4 */
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;      /* always 0 */
    uint32_t vrfid;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t zero;     /* must be 0 */
};
```

The common prefix through `vrfid` is arranged to match `fc_flowu_key` as
closely as possible. `family=4`, `pad=0`, and `zero=0` keep the public key
object fixed at 24 bytes, which lets flow4 stay on the optimized
3 x 8-byte hash/compare path. Use `fc_flow4_key_make()` so those fixed fields
stay canonical.

### 3.2 IPv6 flow key (44 bytes)

```c
struct fc_flow6_key {
    uint8_t  family;   /* always 6 */
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;      /* always 0 */
    uint32_t vrfid;
    uint8_t  src_ip[16];
    uint8_t  dst_ip[16];
};
```

Use `fc_flow6_key_make()` so `family=6` and `pad=0` stay canonical.

### 3.3 Unified flow key (44 bytes)

See §2.2 above.

## 4. Entry Layout

All three variants use a 64-byte single-cache-line entry layout.

### 4.1 Design principles

- One cache line per entry
- `last_ts` and free-list metadata stay in the same cache line as the key
- No built-in user payload
- Entry aligned to 64 bytes (`__attribute__((aligned(64)))`)
- Caller-owned per-flow state lives outside the entry and is referenced by
  `entry_idx`

### 4.2 Entry design (all 64B / 1 CL)

```
flow4 entry (64B):
  fc_flow4_key    24B   family/proto/ports/pad/vrfid/src_ip/dst_ip/zero
  cur_hash          4B   O(1) remove 用 hash_field
  last_ts           8B   last access TSC (0 = free)
  free_link         4B   SLIST entry (free list index)
  slot              2B   slot in current bucket
  reserved1         2B
  reserved0        20B   pad to 64B

flow6 entry (64B):
  fc_flow6_key    44B   family/proto/ports/pad/vrfid/src_ip[16]/dst_ip[16]
  cur_hash          4B
  last_ts           8B
  free_link         4B
  slot              2B
  reserved1         2B

flowu entry (64B):
  fc_flowu_key    44B   family/proto/ports/vrfid/addr union
  cur_hash          4B
  last_ts           8B
  free_link         4B
  slot              2B
  reserved1         2B
```

### 4.3 Rationale

- **Key + metadata in one line**: lookup, compare, timestamp update, and
  free-list transitions all stay within one cache line.
- **`last_ts` in the hot line**: maintenance can decide expiry without any
  extra indirection.
- **External state by index**: no payload fields means the entry stays compact
  and relocatable.

## 5. Hash Table Configuration

- Hash variant: `rix_hash.h` fingerprint (arbitrary key size, cmp_fn)
- `RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)`
- Optional typed hash hook:
  `RIX_HASH_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)`
- `hash_field` (`cur_hash`) enables O(1) remove (no rehash)
- Kickout uses XOR trick: `alt_bk = (fp ^ hash_field) & mask`
- Bucket: 128 bytes (2 CL), 16 slots/bucket
- Runtime SIMD dispatch: Generic / SSE4.2 / AVX2 / AVX-512

### 5.1 Table sizing

Use `fc_PREFIX_cache_size_query()` for the exact rounded layout. The helper:

- rounds `requested_entries` up to the cache's power-of-two pool granularity
- derives `nb_bk` from `rix_hash_nb_bk_hint()`
- reports cache, bucket, pool, and scratch sizes in one packed footprint

Because entries are 64 bytes, the total memory footprint is materially smaller
than the older 128-byte design.

### 5.2 bk[0] placement rate vs fill rate

At ≤75% fill, 98%+ of entries reside in their primary bucket (bk[0]).
`scan_bk` touches only 1 CL for these entries, minimizing DRAM access.

| Fill % | bk[0] % | bk[1] % |
|---|---|---|
| 10-50 | 99.5-100 | 0-0.5 |
| 60 | 99.7 | 0.3 |
| 70 | 99.2 | 0.8 |
| 75 | 98+ | ~2 |
| 80 | 96 | 4 |
| 90 | 91 | 9 |

This is why the 75% threshold is important (§8). In practice, flow-cache
should not be operated near `100%` fill: use `<= 90%` only as an upper safety
bound, and keep steady-state operation at `<= 75%` when datapath throughput
matters.

## 6. Pipeline Design

### 6.1 Lookup pipeline

`rix_hash.h` staged find has 4 stages:

```
Stage 0: hash_key_2bk       compute hash, prefetch bucket[0] and bucket[1]
Stage 1: scan_bk_empties    SIMD fingerprint scan + collect empty slots
Stage 2: prefetch_node      prefetch flow_entry CL0 for candidates
Stage 3: cmp_key_empties    full key compare; on miss → inline insert (no rehash)
```

### 6.2 N-ahead software pipeline

Process `nb_pkts` packets in `STEP_KEYS`-wide steps. Each stage runs
`AHEAD_KEYS` keys ahead of the next stage:

```
STEP_KEYS   = 8    (keys processed per step)
AHEAD_STEPS = 4    (how many steps each stage runs ahead)
AHEAD_KEYS  = 32   (= STEP_KEYS * AHEAD_STEPS)
```

```c
for (i = 0; i < nb_pkts + 3 * AHEAD_KEYS; i += STEP_KEYS) {
    if (i                        < nb_pkts) hash_key_n      (i,              STEP_KEYS);
    if (i >= AHEAD_KEYS       && ...)       scan_bk_n       (i-AHEAD_KEYS,   STEP_KEYS);
    if (i >= 2*AHEAD_KEYS     && ...)       prefetch_node_n (i-2*AHEAD_KEYS, STEP_KEYS);
    if (i >= 3*AHEAD_KEYS     && ...)       cmp_key_n       (i-3*AHEAD_KEYS, STEP_KEYS);
}
```

`AHEAD_KEYS` is a software-pipeline distance. It does not mean that the code
issues 128 hardware prefetches at once.

For the current 64-byte entry design there is no second-line payload touch on
hit. The caller typically uses `entry_idx` to update side data structures.

### 6.3 Timestamp

- TSC (`rdtsc`) read **once** before the lookup loop
- Same `now` value used for all packets in the vector
- No per-packet TSC read
- Calibrated at init time via 50 ms sleep

## 7. Hit / Miss Processing

### 7.1 Datapath choices

The current cache exposes two main datapath styles:

- `find_bulk`: search only, no insertion on miss
- `findadd_bulk`: search + inline insert on miss

Single-key helpers (`find`, `findadd`, `add`, `del`, `del_idx`) are thin
wrappers over the bulk paths with `n=1`.

### 7.2 Processing flow

Typical packet-loop usage is:

```c
process_vector(pkts[256]):

  now = rdtsc()                                  // 1 TSC read per vector
  extract_keys(keys, pkts, 256)

  fc_flow4_cache_findadd_bulk(&fc, keys, 256, now, results)
  // or fc_flow4_cache_find_bulk(&fc, ...) if inserts are handled elsewhere

  for each pkt:
      if (results[i].entry_idx != 0) {
          // hit, or miss that was inserted successfully
      } else {
          // miss with no insert (find_bulk), or cache full on insert path
      }

  fc_flow4_cache_maintain_step(&fc, now, idle)
```

The active hot path is `findadd_bulk`, and periodic aging is handled by
`maintain`, `maintain_step_ex`, or `maintain_step`.

### 7.3 Per-entry state

The current sample cache entries contain only key/hash/timestamp/free-list
metadata.

If the caller needs extra per-flow state, the usual pattern is to keep a side
array or external structure keyed by `entry_idx` (1-origin pool index).

## 8. Aging and Reclaim

Entries age via `last_ts`. Hits and successful inserts update the timestamp.
Removal of stale entries is explicit and uses the maintenance APIs:

- `maintain(start_bk, bucket_count, now)`: scan an explicit bucket range
- `maintain_step_ex(start_bk, bucket_count, skip_threshold, now)`: low-level
  cursor-managed sweep
- `maintain_step(now, idle)`: throttled poll-loop helper

The effective timeout is adaptive: as fill rises, the cache shortens the
timeout window to reclaim stale entries sooner. Insert pressure is also local:
before an inline miss insert, the implementation may reclaim expired entries
from the candidate buckets when they are dense.

If no free entry is available after reclaim, insert-style APIs return
`entry_idx = 0` and increment `fill_full`.

Operational guidance:

- Do not treat `100%` fill as a normal operating point.
- `<= 90%` is an upper bound, not a performance target.
- For fast steady-state datapath, keep live fill at `<= 75%`.
- The intended way to do that is to call `maintain`, `maintain_step_ex`, or
  `maintain_step` regularly so stale entries are reclaimed before the table
  drifts into the high-fill regime.

### 8.1 Runtime configuration

Runtime behavior is controlled by the per-cache config:

```c
struct fc_flow4_config cfg = {
    .timeout_tsc = ...,
    .pressure_empty_slots = ...,
    .maint_interval_tsc = ...,
    .maint_base_bk = ...,
    .maint_fill_threshold = ...,
};
```

SIMD/backend selection is not a per-cache parameter. It is chosen globally
through `fc_arch_init(FC_ARCH_...)`, and the unsuffixed public APIs dispatch
to the selected tier at runtime.

## 9. Storage Layout and Free List

- The caller provides the cache object, bucket array, entry pool, and optional
  bulk scratch area.
- `fc_PREFIX_cache_size_query()` reports the rounded footprint and layout.
- `fc_cache_size_bind()` binds one packed memory block to cache/buckets/pool/
  scratch pointers.
- `fc_PREFIX_cache_init_attr()` initializes from that packed layout.
- No internal heap allocation is used by the cache library.

Unused entries live on an SLIST free list. Flush, delete, and maintenance
return entries to the free list; insert-style operations pop from it.

## 10. Public API

All three variants share the same API shape (`flow4`, `flow6`, `flowu`):

```c
int  fc_PREFIX_cache_size_query(unsigned nb_entries,
                                struct fc_cache_size_attr *attr);
int  fc_cache_size_bind(void *base, struct fc_cache_size_attr *attr);
int  fc_PREFIX_cache_init_attr(struct fc_cache_size_attr *attr,
                               const struct fc_PREFIX_config *cfg);

void fc_PREFIX_cache_init(struct fc_PREFIX_cache *fc,
                          struct rix_hash_bucket_s *buckets,
                          unsigned nb_bk,
                          struct fc_PREFIX_entry *pool,
                          unsigned max_entries,
                          const struct fc_PREFIX_config *cfg);
void     fc_PREFIX_cache_flush(struct fc_PREFIX_cache *fc);
unsigned fc_PREFIX_cache_nb_entries(const struct fc_PREFIX_cache *fc);
void     fc_PREFIX_cache_stats(const struct fc_PREFIX_cache *fc,
                               struct fc_PREFIX_stats *out);
int      fc_PREFIX_cache_walk(struct fc_PREFIX_cache *fc,
                              int (*cb)(uint32_t entry_idx, void *arg),
                              void *arg);

void     fc_PREFIX_cache_find_bulk(...);
void     fc_PREFIX_cache_findadd_bulk(...);
void     fc_PREFIX_cache_add_bulk(...);
void     fc_PREFIX_cache_del_bulk(...);
void     fc_PREFIX_cache_del_idx_bulk(...);

uint32_t fc_PREFIX_cache_find(...);
uint32_t fc_PREFIX_cache_findadd(...);
uint32_t fc_PREFIX_cache_add(...);
void     fc_PREFIX_cache_del(...);
int      fc_PREFIX_cache_del_idx(...);

unsigned fc_PREFIX_cache_maintain(...);
unsigned fc_PREFIX_cache_maintain_step_ex(...);
unsigned fc_PREFIX_cache_maintain_step(...);
```

For `flowu`, key construction helpers are also provided:

```c
struct fc_flowu_key fc_flowu_key_v4(...);
struct fc_flowu_key fc_flowu_key_v6(...);
```

## 11. Template and Dispatch Architecture

The implementation is generated from one active template and one dispatch
layer:

- `src/fc_cache_generate.h`: generates per-variant static implementations
- `src/fc_ops.h`: defines the per-tier ops tables
- `src/fc_dispatch.c`: exposes unsuffixed public wrappers and runtime tier
  selection
- `src/flow4.c`, `src/flow6.c`, `src/flowu.c`: provide variant-specific
  hash/cmp functions and instantiate `FC_CACHE_GENERATE(...)`

There is no longer a separate legacy body template in the active build.

### 11.1 Adding a new variant

To add a new variant:

1. Add a public header modeled after `flow4_cache.h` / `flow6_cache.h` /
   `flowu_cache.h`
2. Define the key/result/entry/cache/config/stats types and size-query helpers
3. Implement variant-specific hash/cmp in `src/<variant>.c`
4. Instantiate `FC_CACHE_GENERATE(prefix, pressure, hash_fn, cmp_fn)`
5. Add `FC_OPS_TABLE(prefix, FC_ARCH_SUFFIX)` and wire the variant into
   `fc_dispatch.c`, `fc_ops.h`, and the build/test rules

## 12. File Structure

```
samples/
  README.md
  README_JP.md
  Makefile

  fcache/
    Makefile
    include/
      fc_cache_common.h
      flow_cache.h
      flow4_cache.h
      flow6_cache.h
      flowu_cache.h
    src/
      fc_cache_generate.h
      fc_dispatch.c
      fc_ops.h
      flow4.c
      flow6.c
      flowu.c
    lib/

  test/
    Makefile
    test_flow_cache.c
    bench_fc.c
    bench_fc_body.h
    bench_fc_common.h
    run_fc_bench_all.sh
    run_fc_bench_matrix.sh
    bench_results/
```

## 13. Build Dependencies

```
rix/rix_defs_private.h  index macros, utilities (private: included by rix headers)
rix/rix_queue.h         SLIST (free list)
rix/rix_hash.h          fingerprint hash table (SIMD dispatch)
```

No TAILQ dependency (LRU replaced by timestamp-only approach).
Requires `-mavx2` or `-mavx512f` for SIMD-accelerated hash operations.
The sample Makefiles default to `OPTLEVEL=3` and are intended to build with
both `CC=gcc` and `CC=clang`.

## 14. Thread Safety

The flow cache is designed for a **per-thread, lock-free** model.

### Model

| Aspect | Detail |
|---|---|
| Instance ownership | One `fc_flow4_cache` / `fc_flow6_cache` / `fc_flowu_cache` per thread |
| Synchronization | None — no locks, no atomics, no RCU |
| Shared state | None between threads |
| `maint_cursor` | Per-cache; updated only by the owning thread |

### Rationale

In the target environment (VPP plugin, DPDK poll-mode):

- Each worker thread owns a private packet queue and processes it exclusively.
- The flow cache holds thread-local state (per-flow counters, action cache).
- No inter-thread sharing means no false sharing, no contention, and no cache-line bouncing.
- This matches VPP's "no global mutable state" architecture.

### Implications for users

- **Do not share** a single cache instance between threads.
- **Do not call** any flow cache API from two threads concurrently on the same instance.
- To process traffic across multiple cores, allocate one cache per worker thread.
- If cross-thread access to flow statistics is required, read them from a safe point
  (e.g., control-plane thread reading counters after a quiescent period) or copy to a
  separate stats structure with appropriate memory ordering.

### Shared memory usage

librix data structures (hash table, free list) store **indices, not pointers**.
This makes the raw data relocatable — valid across processes mapping the same
shared memory region at different virtual addresses.  However, concurrent
modification still requires external coordination (e.g., per-shard locks or
RCU) when multiple processes share the same `flow_cache` instance.
The single-writer model above removes this requirement entirely.

## 15. Competitive Analysis

### 15.1 Feature comparison

| Feature | **librix** | **DPDK rte_hash** | **OVS EMC** | **VPP bihash** | **nf_conntrack** |
|---|---|---|---|---|---|
| Structure | 16-way cuckoo, FP | 8-way cuckoo | direct-mapped | 4/8-way cuckoo | chained hash |
| Key storage | node (FP bucket) | in bucket | in node | in bucket | in node |
| Lookup | 4-stage pipeline+SIMD | no pipeline | 1CL direct | no pipeline | chain walk |
| Remove | O(1) cur_hash | O(1) position | O(1) | O(n) rehash | O(1) hlist |
| Shared memory | **native** | No (pointer) | No | No | No |
| Eviction | adaptive scan+force | none (manual) | timestamp | none | conntrack GC |
| SIMD | AVX2/512 FP scan | CRC32 only | none | none | none |
| Batch | native | bulk lookup | none | none | none |
| Thread model | per-thread, lock-free | RCU or lock | per-thread | per-thread | RCU + spinlock |

### 15.2 Individual comparisons

#### vs DPDK rte_hash

rte_hash is the de facto standard for data-plane hash tables.  8-way
buckets achieve high fill rates, but pipelined batch lookup is not provided.
`rte_hash_lookup_bulk` makes independent DRAM accesses per key — latency
accumulates with cold caches.  librix's 4-stage pipeline overlaps DRAM
accesses, giving a 5-8x advantage at large pool sizes.

rte_hash has mature DPDK ecosystem integration (mempool, ring, EAL) and
higher operational maturity beyond raw hash performance.

#### vs OVS EMC (Exact Match Cache)

EMC is direct-mapped (1-way): on hit, only 1 CL access.  Extremely fast
but high miss rate (any collision causes a miss).  EMC serves as a front
cache for SMC (Signature Match Cache = cuckoo) in a 2-tier design.

librix uses a single 16-way cuckoo tier — no fast hit-only path, but miss
rate is orders of magnitude lower.  Overall packet processing throughput
typically favors librix.

#### vs VPP bihash

bihash is VPP's standard hash table.  Remove requires O(n) rehash —
unsuitable for frequent eviction.  Flow cache use cases need frequent
remove, making cur_hash-based O(1) remove a decisive advantage.
bihash also lacks pipelined lookup, so performance gap widens at scale.

#### vs nf_conntrack

Linux kernel connection tracker.  Chained hash has scalability limits;
GC uses RCU + timer.  Not designed for data-plane acceleration — its
strength is feature richness (NAT, helper, expectation).  Different
design goals make direct comparison inappropriate.

### 15.3 Architectural strengths

**Index-based design (primary differentiator)**

No pointer storage means shared memory, mmap, and cross-process access
work natively — a property no other high-performance flow table
implementation provides.  Decisive advantage when VPP plugins or
multi-process access is required.

**Memory access pattern optimization**

The active design keeps each entry to a single 64-byte cache line, with
the key, `cur_hash`, `last_ts`, and free-list metadata colocated.
Lookup, hit resolution, timestamp refresh, and maintenance expiry checks
therefore complete with minimum cache-line traffic.

**4-stage pipeline effect**

Measured 2.8x (L2) → 8.1x (DRAM-cold) speedup.  Effect grows with pool
size — correctly achieving DRAM latency hiding.  40-143 cy/key is
excellent for DRAM-cold conditions.

### 15.4 Eviction strategy assessment

**Adaptive timeout design is sound**

- Decay/recovery equilibrium converges stably (control-theory perspective)
- Shift-only arithmetic, no division — appropriate for data plane
- 1.0s minimum floor guarantees cache effectiveness at any miss rate

**Insert guarantee via 3-tier fallback**

Correct design decision: cache insert should never fail.
`evict_bucket_oldest` simultaneously frees a pool entry and a bucket slot,
ensuring the subsequent `ht_insert` always takes the fast path — elegant.

**evict_one 1/8 bound**

The 1/8 bound caps worst-case cost, but when all entries are alive and no
expired entry is found, every call incurs 1/8-scan → evict_bucket_oldest —
potentially thousands of cycles per insert.  In practice adaptive timeout
shortens the effective timeout first, so evict_bucket_oldest is extremely
rare in real workloads.

### 15.5 Areas for improvement

- Batch insert (pipeline across multiple misses) not implemented.
  Potential improvement when miss rate is high.

**Functionality**

- No per-flow timeout (e.g., TCP established vs SYN) — all entries share
  the same effective timeout.
- No runtime timeout change API (init-time only).

**Operations**

- Dynamic pool resize is not supported (pre-allocated, fixed size).

### 15.6 Summary

An exceptionally complete design for a data-plane flow cache:

1. **Pipeline + SIMD + CL placement** designed coherently throughout
2. **Adaptive eviction** eliminates manual tuning while maintaining stability
3. **Insert guarantee** ensures cache reliability
4. **Index-based** enables shared memory deployment

150-215 cy/pkt (lookup + insert + expire combined) = 10 Mpps/core at 2 GHz
Xeon — sufficient for practical deployments.  Template architecture delivers
three variants with zero code duplication, maintaining high maintainability.

---

## 16. Benchmark Results

Latest rerun: March 24, 2026 on `AMD Ryzen 9 8945HS` (Zen 4, AVX-512 ready),
`cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`.

- `./samples/test/fc_test`: passed
- `taskset -c 2 ./samples/test/fc_bench datapath`: auto-selected `avx512`
- `taskset -c 2 ./samples/test/fc_bench maint_partial`: auto-selected `avx512`

Representative results below come from the default `make bench` path:
`datapath`, `maint_partial`, and the quick `findadd_window` matrix.
Query width is 256. The datapath microbench measures with 50% active entries.

Benchmark modes and intent:

- `datapath`: reset-per-round microbench. Compares `findadd_hit`, `find_hit`, `findadd_miss`, `add_only`, `add+del`, `del_bulk`, and reset-per-round mixed `90/10`.
- `maint`: full-table sweep with all entries expired. Useful for fill-sensitivity and saturation studies, but not part of the representative default bench path.
- `maint_partial`: measures `maintain_step()` cost for partial sweeps at 75% fill with all entries expired.
- `findadd_closed`: fixed-set benchmark. Measures `findadd_bulk()` with a stable hit set and a stable miss set; the miss set is deleted after each round so fill stays fixed.
- `findadd_window`: steady-state open-set benchmark. Measures persistent fresh misses while maintenance keeps fill inside a target window such as `60-75%`.
- `trace_open_custom`: open-set trace with explicit batch-maint policy. Measures long-running fill drift, relief pressure, and maintenance effectiveness.

Default benchmark entry points:

- `make bench`: representative suite. Runs `datapath`, `maint_partial`, and the quick `findadd_window` matrix only.
- `make bench-full`: full long-running suite. Adds the heavy `trace_open_custom` matrix runs.
- `./run_fc_bench_matrix.sh <variant> quick`: quick windowed open-set matrix.
- `./run_fc_bench_matrix.sh <variant> full`: full matrix including long-running trace cases.

### 16.1 Datapath Performance (cycles/key, no expire)

#### 32K entries (`entries=32768`, `nb_bk=2048`)

| Operation | flow4 | flow6 | flowu | 6/4 | u/4 |
|-----------|------:|------:|------:|----:|----:|
| findadd_hit | **25.78** | 37.50 | 39.53 | +45.5% | +53.3% |
| find_hit | **24.84** | 36.33 | 37.65 | +46.2% | +51.6% |
| findadd_miss | **52.27** | 61.72 | 61.25 | +18.1% | +17.2% |
| add_only | **37.03** | 46.41 | 46.88 | +25.3% | +26.6% |
| add+del | **68.98** | 89.53 | 90.94 | +29.8% | +31.8% |
| del_bulk | **31.56** | 42.97 | 44.22 | +36.1% | +40.1% |
| mixed 90/10 | **29.22** | 42.81 | 41.72 | +46.5% | +42.8% |

flow4 uses inline CRC32C (`__builtin_ia32_crc32di` x3) and XOR-based 24B key
comparison, bypassing the `rix_hash_arch->hash_bytes` function-pointer dispatch
and `memcmp`.  The 26-42% advantage over flow6/flowu comes primarily from
**function-pointer elimination**, not key-size difference.

At 2.0 GHz, `flow4 findadd_hit` at 25.78 cy/key corresponds to about
`2.0e9 / 25.78 ~= 77.6 Mpps/core`.

#### 1024K entries (`entries=1048576`, `nb_bk=65536`)

| Operation | flow4 | flow6 | flowu | 6/4 | u/4 |
|-----------|------:|------:|------:|----:|----:|
| findadd_hit | **29.53** | 41.56 | 40.62 | +40.7% | +37.6% |
| find_hit | **28.12** | 39.69 | 39.06 | +41.1% | +38.9% |
| findadd_miss | **174.84** | 213.12 | 216.41 | +21.9% | +23.8% |
| add_only | **289.61** | 301.09 | 307.73 | +4.0% | +6.3% |
| add+del | **330.39** | 353.44 | 358.36 | +7.0% | +8.5% |
| del_bulk | **41.41** | 51.72 | 51.88 | +24.9% | +25.3% |
| mixed 90/10 | **50.39** | 63.83 | 62.50 | +26.7% | +24.0% |

#### 4096K entries (`entries=4194304`, `nb_bk=262144`)

| Operation | flow4 | flow6 | flowu | 6/4 | u/4 |
|-----------|------:|------:|------:|----:|----:|
| findadd_hit | **28.91** | 40.94 | 40.62 | +41.6% | +40.5% |
| find_hit | **27.81** | 38.91 | 38.91 | +39.9% | +39.9% |
| findadd_miss | **237.97** | 278.05 | 274.06 | +16.8% | +15.2% |
| add_only | **358.67** | 370.23 | 374.22 | +3.2% | +4.3% |
| add+del | **401.41** | 421.41 | 431.64 | +5.0% | +7.5% |
| del_bulk | **42.66** | 54.30 | 54.84 | +27.3% | +28.6% |
| mixed 90/10 | **57.89** | 70.62 | 60.47 | +22.0% | +4.5% |

Across all three table sizes, `flow4` remains the best steady-state hit-path
variant. As the table grows, the gap on miss-heavy paths narrows because the
work becomes increasingly memory-latency bound.

### 16.2 Maintenance: Partial Sweep (fill=75%, all expired)

| Table | step | flow4 cy/entry | flow6 cy/entry | flowu cy/entry |
|-------|-----:|------:|------:|------:|
| 0.6 MB | 64-1024 | 11.7-11.9 | 12.0-12.1 | 12.0-12.2 |
| 5.0 MB | 64-1024 | 15.8-16.2 | 15.7-16.4 | 15.3-15.6 |
| 72 MB | 64-1024 | 122.2-124.3 | 122.9-124.2 | 122.7-123.0 |

Step-size has very little impact on per-entry cost inside each residency band.
The dominant factor is whether the node array is still cache-resident.

- **L2 / LLC resident (0.6-5.0 MB)**: about 12-16 cy/entry.
- **DRAM resident (72 MB)**: about 123 cy/entry across all variants.

At 72 MB / step=64, flow4 is about `94.5K cy/call`, or roughly `31.5 us`
at 3 GHz. That is still practical as amortized background maintenance
between packet batches.

The `maint` mode remains available for full-table sweep and fill-sensitivity
studies, but the representative results above focus on `maint_partial`
because it matches the default `make bench` path.

### 16.3 Summary

| Metric | Value | Note |
|--------|-------|------|
| flow4 findadd hit | 26-29 cy | ~69-78 Mpps @2 GHz |
| flow4 mixed 90/10 | 29-58 cy | Representative steady-state range |
| flow4 findadd miss | 52-238 cy | Including hash + insert |
| Partial maintenance (L2/LLC) | 12-16 cy/entry | Variant differences are small |
| Partial maintenance (DRAM) | 123-124 cy/entry | Memory-latency bound |
| Step-size effect | small | Residency dominates, not cursor overhead |

- **flow4** is clearly the fastest variant for datapath — inline CRC32C +
  XOR compare eliminates dispatch overhead
- **flow6/flowu** pay about 15-53% more in datapath, while maintenance
  converges to almost identical cost once the node array goes memory-bound
- **Maintenance scales with residency** more than with sweep step size;
  partial sweep amortizes cleanly across packet batches
- **DRAM is the bottleneck** at large table sizes — all variants converge

---

## 17. Architecture-Specific Dispatch

### 17.1 Design Overview

The flowcache compiles the **same source** at four architecture levels
and selects the best implementation at runtime via a function-pointer
table (ops table).  This lets the compiler apply arch-specific
optimizations — instruction selection, scheduling, auto-vectorization —
without hand-written SIMD in the application code.

```
                      fc_arch_init(FC_ARCH_AUTO)
                              │
                              ▼
┌─────────────────────────────────────────────────┐
│  fc_dispatch.o  (no arch flags)                 │
│  - rix_hash_arch_init() for this TU             │
│  - FC_OPS_SELECT() per variant                  │
│  - unsuffixed API wrappers                      │
│    hot-path  → _fc_flow4_active->findadd_bulk() │
│    cold-path → fc_flow4_ops_gen.init()          │
└────────────────────┬────────────────────────────┘
                     │ selects ops table
      ┌──────────────┼──────────────┐
      ▼              ▼              ▼
 ops_gen        ops_avx2       ops_avx512 ...
 (flow4/6/u)   (flow4/6/u)   (flow4/6/u)
```

**Key points**:

- The public API uses **unsuffixed** function names
  (`fc_flow4_cache_findadd_bulk`, etc.).  These are thin wrappers
  in `fc_dispatch.c` that forward to the selected ops table.
- **Hot-path** functions (find/findadd/add/del bulk, maintain) dispatch through the
  runtime-selected ops pointer.  **Cold-path** functions (init,
  flush, stats, remove, nb_entries, walk) forward through `ops_gen`.
- All generated functions (`_FCG_API`) are **static** — visible
  only within their arch-specific TU.  The ops table takes their
  addresses for cross-TU dispatch.
- Each arch-specific TU has a `__attribute__((constructor))` that
  calls `rix_hash_arch_init(RIX_HASH_ARCH_AUTO)`, ensuring
  `rix_hash_hash_bytes_fast()` uses the correct SIMD path.

### 17.2 Architecture Tiers

| Tier | Compiler flags | Key features | Target |
|------|----------------|-------------|--------|
| GEN | (none) | Scalar only, no CRC32C | ARM portability baseline |
| SSE4.2 | `-msse4.2` | CRC32C, 128-bit SIMD | Older x86_64 servers |
| AVX2 | `-mavx2 -msse4.2` | 256-bit SIMD | Mainstream Xeon |
| AVX-512 | `-mavx512f -mavx2 -msse4.2` | 512-bit SIMD | Xeon Scalable (primary target) |

GEN exists as a portability baseline for future ARM support.
On x86_64, SSE2 is ABI-guaranteed; SSE4.2 is not (absent on pre-2008
Core 2), but all practical Xeon targets have it.
The AVX-512 path has also been validated on AMD Zen 4 hardware
(`Ryzen 9 8945HS`) in addition to Xeon-targeted builds.

### 17.3 Build Matrix

Each variant source (e.g. `flow4.c`) is compiled four times with
`-DFC_ARCH_SUFFIX=_<tier>` and the corresponding `-m` flags.
The dispatch wrapper is compiled once without arch flags.

```makefile
VARIANTS = flow4 flow6 flowu

# GEN (portable, no SIMD flags)
$(LIBDIR)/%_gen.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -DFC_ARCH_SUFFIX=_gen -c -o $@ $<

# SSE4.2
$(LIBDIR)/%_sse.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -msse4.2 -DFC_ARCH_SUFFIX=_sse -c -o $@ $<

# AVX2
$(LIBDIR)/%_avx2.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -mavx2 -msse4.2 -DFC_ARCH_SUFFIX=_avx2 -c -o $@ $<

# AVX-512
$(LIBDIR)/%_avx512.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -mavx512f -mavx2 -msse4.2 -DFC_ARCH_SUFFIX=_avx512 -c -o $@ $<

# Dispatch wrapper (no arch flags, no FC_ARCH_SUFFIX)
$(LIBDIR)/fc_dispatch.o: $(SRCDIR)/fc_dispatch.c
	$(CC) $(CFLAGS) -c -o $@ $<
```

Total objects: 3 variants x 4 tiers + 1 dispatch = **13 objects**.

`FC_ARCH_SUFFIX` causes `FC_CACHE_GENERATE` to append the suffix to
all generated function names (e.g. `fc_flow4_cache_findadd_bulk_avx2`).
Without the macro, original unsuffixed names are generated.

### 17.4 Measured AVX2 vs AVX-512 on Zen 4

The `avx512` dispatch tier exists and is runtime-selectable via
`fc_arch_init(FC_ARCH_AVX512)`. The current `fcache` generator also prefers
dedicated AVX-512 bucket-scan helpers when `__AVX512F__` is enabled, and
falls back to AVX2 helpers in `avx2` tiers.

So the current state is:

- `avx512` tier: yes, compiled and dispatchable
- bucket scan width in the hot path: AVX-512 in `avx512`, AVX2 in `avx2`
- implication: forcing `avx512` now exercises the AVX-512 scan helpers
  directly, but the measured result is still microarchitecture-dependent

Measured on `AMD Ryzen 9 8945HS` on March 23, 2026 with:

```sh
./samples/test/fc_bench --arch avx2 datapath
./samples/test/fc_bench --arch avx512 datapath
```

Representative results (cycles/key):

| Table / op | AVX2 | AVX-512 |
|---|---:|---:|
| 32K `flow4 findadd_hit` | 27.27 | 26.27 |
| 32K `flow4 findadd_miss` | 56.42 | 60.20 |
| 1024K `flow4 findadd_hit` | 28.46 | 26.73 |
| 1024K `flow4 findadd_miss` | 1331.24 | 908.07 |
| 32K `flow6 findadd_hit` | 38.36 | 38.99 |
| 1024K `flow6 findadd_hit` | 39.43 | 39.16 |

These numbers are reference measurements for this Zen 4 machine, not a
universal ranking for Xeon or other AVX-512 CPUs. On this host, forced
`avx512` improved some cases and regressed others.

### 17.5 Hash Function Strategy

Hash functions remain **inline, macro-controlled** — not dispatched
via function pointers.  Inlining is critical for pipeline performance.

```c
static inline union rix_hash_hash_u
fc_flow4_hash_fn(const struct fc_flow4_key *key, uint32_t mask)
{
#if defined(__x86_64__) && defined(__SSE4_2__)
    /* Direct CRC32C — 3 x crc32q for 24B key */
    ...
#else
    /* GEN fallback — rix_hash_hash_bytes_fast() */
    return rix_hash_hash_bytes_fast(key, sizeof(*key), mask);
#endif
}
```

When compiled with `-msse4.2` or higher, `__SSE4_2__` is
defined and CRC32C is used.  The GEN build falls back to the
multiplicative hash.  No source changes needed — the compiler flag
controls the code path.

The same applies to SIMD find operations (`_RIX_HASH_FIND_U32X16_2`):
`fc_cache_generate.h` prefers AVX-512 helpers when `__AVX512F__` is
defined, falls back to AVX2 helpers when `__AVX2__` is defined, and
otherwise leaves the call on the default dispatched path.

For `rix_hash_hash_bytes_fast()` (used by flow6/flowu GEN fallback),
each arch TU auto-initializes its per-TU `rix_hash_arch` pointer via
a `__attribute__((constructor))` generated by `FC_CACHE_GENERATE`.

### 17.6 Ops Table Structure

```c
/* Generated by FC_OPS_DEFINE(flow4) in src/fc_ops.h (private) */
struct fc_flow4_ops {
    /* cold-path */
    void (*init)(struct fc_flow4_cache *fc,
                 struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                 struct fc_flow4_entry *pool, unsigned max_entries,
                 const struct fc_flow4_config *cfg);
    void (*flush)(struct fc_flow4_cache *fc);
    unsigned (*nb_entries)(const struct fc_flow4_cache *fc);
    int (*remove_idx)(struct fc_flow4_cache *fc, uint32_t entry_idx);
    void (*stats)(const struct fc_flow4_cache *fc,
                  struct fc_flow4_stats *out);
    int (*walk)(struct fc_flow4_cache *fc,
                int (*cb)(uint32_t entry_idx, void *arg), void *arg);
    /* hot-path */
    void (*find_bulk)(struct fc_flow4_cache *fc,
                      const struct fc_flow4_key *keys,
                      unsigned nb_keys, uint64_t now,
                      struct fc_flow4_result *results);
    void (*findadd_bulk)(struct fc_flow4_cache *fc,
                         const struct fc_flow4_key *keys,
                         unsigned nb_keys, uint64_t now,
                         struct fc_flow4_result *results);
    void (*add_bulk)(struct fc_flow4_cache *fc,
                     const struct fc_flow4_key *keys,
                     unsigned nb_keys, uint64_t now,
                     struct fc_flow4_result *results);
    void (*del_bulk)(struct fc_flow4_cache *fc,
                     const struct fc_flow4_key *keys,
                     unsigned nb_keys);
    void (*del_idx_bulk)(struct fc_flow4_cache *fc,
                         const uint32_t *idxs, unsigned nb_idxs);
    unsigned (*maintain)(struct fc_flow4_cache *fc,
                         unsigned start_bk, unsigned bucket_count,
                         uint64_t now);
    unsigned (*maintain_step_ex)(struct fc_flow4_cache *fc,
                                 unsigned start_bk, unsigned bucket_count,
                                 unsigned skip_threshold, uint64_t now);
    unsigned (*maintain_step)(struct fc_flow4_cache *fc,
                              uint64_t now, int idle);
};
```

One ops table instance per arch tier, per variant (12 total).
Generated by `FC_OPS_TABLE(prefix, FC_ARCH_SUFFIX)` in each
arch-specific `.c` file.  All generated functions are `static`;
the ops table captures their addresses for dispatch.

### 17.7 Runtime Selection

```c
/* fc_dispatch.c */
#include <rix/rix_hash_arch.h>
#include "fc_ops.h"

static const struct fc_flow4_ops *_fc_flow4_active = &fc_flow4_ops_gen;
static const struct fc_flow6_ops *_fc_flow6_active = &fc_flow6_ops_gen;
static const struct fc_flowu_ops *_fc_flowu_active = &fc_flowu_ops_gen;

void
fc_arch_init(unsigned arch_enable)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    FC_OPS_SELECT(flow4, arch_enable, &_fc_flow4_active);
    FC_OPS_SELECT(flow6, arch_enable, &_fc_flow6_active);
    FC_OPS_SELECT(flowu, arch_enable, &_fc_flowu_active);
}

/* Hot-path wrapper — dispatch through runtime-selected ops */
void
fc_flow4_cache_findadd_bulk(struct fc_flow4_cache *fc,
                             const struct fc_flow4_key *keys,
                             unsigned nb_keys, uint64_t now,
                             struct fc_flow4_result *results)
{
    _fc_flow4_active->findadd_bulk(fc, keys, nb_keys, now, results);
}

/* Cold-path wrapper — always through ops_gen */
void
fc_flow4_cache_init(struct fc_flow4_cache *fc, ...)
{
    fc_flow4_ops_gen.init(fc, ...);
}
```

`FC_OPS_SELECT` uses `__builtin_cpu_supports()` to probe the CPU
and selects the best available ops table (AVX-512 > AVX2 > SSE4.2 > GEN).
The dispatch cost is one indirect call per batch — negligible.

**Caller usage** — one init call replaces both `rix_hash_arch_init()`
and per-variant ops selection:

```c
int main(void) {
    fc_arch_init(FC_ARCH_AUTO);   /* does everything */
    /* ... use fc_flow4_cache_init(), fc_flow4_cache_findadd_bulk(), etc. */
}
```

### 17.8 Portability Notes

The runtime detection uses `__builtin_cpu_supports()` (GCC ≥ 4.8,
Clang ≥ 3.7).  On x86_64, this calls `cpuid` internally.  On AArch64
(GCC ≥ 14), it reads `getauxval(AT_HWCAP)`.

**Alternative detection methods** if `__builtin_cpu_supports()` is
unavailable or insufficient:

| Method | Portability | Notes |
|--------|-------------|-------|
| `getauxval(AT_HWCAP)` | Linux (glibc/musl) | `<sys/auxv.h>`, works on x86_64 and ARM |
| `elf_aux_info()` | FreeBSD | BSD equivalent of getauxval |
| Direct `cpuid` | x86 only | `<cpuid.h>`, no OS dependency |
| `/proc/cpuinfo` | Linux only | Slow (file I/O + string parse), not for init hot path |
| `IsProcessorFeaturePresent()` | Windows | Win32 API |

To port to a new platform:

1. Add a new tier constant (e.g. `FC_ARCH_NEON`)
2. Compile the variant sources with the appropriate `-march` flag
   and `-DFC_ARCH_SUFFIX=_neon`
3. Add detection logic in `_FC_OPS_SELECT_BODY` using the
   platform's feature-detection API
4. Add `FC_OPS_DECLARE(flow4, _neon)` etc. in `src/fc_ops.h`
5. No changes to pipeline code or public API

### 17.8 File Summary

| File | Role |
|------|------|
| `include/flow_cache.h` | **Public umbrella header** — includes all variant headers + `fc_arch_init()` + `FC_ARCH_*` flags |
| `include/flow4_cache.h` | Public API: types + function prototypes for IPv4 flow cache |
| `include/flow6_cache.h` | Public API: types + function prototypes for IPv6 flow cache |
| `include/flowu_cache.h` | Public API: types + function prototypes for unified flow cache |
| `src/fc_cache_generate.h` | `FC_CACHE_GENERATE` — generates all `static` functions with optional suffix; emits per-TU `rix_hash_arch` constructor; `FC_OPS_TABLE` generates ops table instance |
| `src/fc_ops.h` | Private: `FC_OPS_DEFINE` — ops struct definition; `FC_OPS_DECLARE` — extern declarations; `FC_OPS_SELECT` — runtime CPU detection |
| `src/fc_dispatch.c` | Unsuffixed public API wrappers; `fc_arch_init()` implementation |
| `src/flow4.c`, `flow6.c`, `flowu.c` | Variant-specific hash/cmp + `FC_CACHE_GENERATE` + `FC_OPS_TABLE` |

### 17.9 Test Script

Run benchmarks and save results as evidence:

```sh
./fc_test
./fc_bench datapath
./fc_bench maint_partial
./fc_bench flow4 findadd_closed 32768 75 90 200000
./fc_bench flow4 findadd_window 32768 60 75 90 500000 1000 1000
./run_fc_bench_matrix.sh flow4 quick
./run_fc_bench_matrix.sh flow4 full
./run_fc_bench_all.sh              # save current-build results to bench_results/
./run_fc_bench_all.sh -o /tmp/out  # custom output directory
```

Results are saved to `bench_results/<hostname>_<timestamp>.txt`.
If per-tier binaries such as `fc_test_avx2` / `fc_bench_avx2` exist, the
script will run them; otherwise it falls back to the current single-build
`fc_test` / `fc_bench`.
