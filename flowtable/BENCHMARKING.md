# Flowtable Benchmark Methodology

This document defines what each benchmark is allowed to prove, how it measures
that claim, and where the result should not be over-interpreted.  Flowcache
performance is the project claim, so the main benchmarks avoid 100% active
fill.  Fill percentages mean active entries divided by configured entry
capacity, not hash slot saturation.

## Operating Range

The flowcache target is a controlled cache, not a packed hash-table envelope.

- Green: below 75% active fill.
- Yellow: 75% to below 85% active fill.
- Red: 85% to below 95% active fill.
- 95% and above is a guardrail/pressure region, not a normal target.

Hash capacity envelope testing is a different question: how far the underlying
hash can be pushed before insert failure or long relocation tails dominate.
That cannot be proven by a single cycles/key number because it depends on key
distribution, table size, seed, churn pattern, and tail latency.  If added, it
must be a separate `capacity envelope` benchmark reporting success rate,
relocation/failure counts, and tail latency across multiple seeds.  It must not
be mixed with flowcache performance results.

## Common Measurement Rules

- Use optimized binaries (`-O3 -DNDEBUG`) for benchmark targets.
- Prefer pinned-core runs for headline numbers.  The Makefile uses
  `--pin-core 2` for `ft_bench` targets by default.
- Hugepage-backed allocations are enabled by default where the benchmark
  supports them.  Use `--no-hugepage` in `ft_bench` only when testing the
  allocator/memory sensitivity itself.
- `ft_bench` is the headline microbenchmark because it uses repeated samples,
  cold-cache preparation for datapath operations, optional perf cycle timing,
  and kept-sample aggregation.
- The fixed-scenario helpers (`ft_bench_extra`, `ft_bench_sweep`,
  `ft_bench_zoned`, `ft_bench_ctrl`) are trusted for the narrower questions
  documented below.  They should not be used as generic headline datapath
  numbers unless their setup exactly matches the claim.

## `ft_bench`

Source: `flowtable/test/bench_flow_table.c`

Targets:

- `make -C flowtable/test bench`
- `make -C flowtable/test bench-full`

Purpose:

- Measure pure `flow4`, `flow6`, and `flowu` datapath operations under
  controlled cold-cache table conditions.
- Measure pure-table maintenance modes with cycle, IPC, and cache-hit metrics.
- Measure bucket-table migration cost for grow.

Default Makefile policy:

- `bench`: `flow4`, `q=1/8/32/256`, 1M capacity at 60% fill,
  `--pin-core 2 --raw-repeat 3 --keep-n 1`.
- `bench-full`: `flow4/flow6/flowu`, fills `40/60/75/80/90`, all supported
  arch variants, `--pin-core 2 --raw-repeat 11 --keep-n 7`.

Datapath method:

- Each operation is measured from a fresh round:
  `flush -> reset allocator -> prefill(live) -> prepare query records ->
  insert query records where needed -> 64 MiB cold touch -> icache/metadata
  warmup -> timed operation`.
- The query records are explicitly prefetched for add/delete-by-index tests so
  the input records are hot while buckets and resident table state remain cold.
  This isolates hash/table access instead of measuring unrelated record fetch
  latency.
- Each inner operation reports median cycles over repeated inner rounds, then
  the public sample layer aggregates `raw-repeat` samples and keeps the fastest
  `keep-n` window.  This reduces scheduler and interrupt noise without hiding
  algorithmic cost.

Datapath operations:

- `find_hit`: bulk lookup of existing keys.
- `find_miss`: bulk lookup of keys not present in the table.
- `add_idx`: insert currently absent pre-bound records by index.
- `add_ignore`: insert duplicate keys with `FT_ADD_IGNORE`.
- `add_update`: insert duplicate keys with `FT_ADD_UPDATE`.
- `del_idx`: delete by known entry index.
- `del_key`: delete by key.
- `find_del_idx`: lookup followed by delete by returned index.

Maintain method:

- `maint_expire_dense`: dense table where entries are expired; measures full
  bucket sweep with eviction.
- `maint_nohit_dense`: dense table where entries are fresh; measures scan cost
  when no eviction happens.
- `maint_idx_expire`: indexed maintenance seeded from hit entries; reports
  cycles per active entry represented by touched buckets.
- `maint_idx_filtered`: same indexed path with filtering enabled.
- perf counters are read in two small groups to avoid event multiplexing.

Grow method:

- Prefill to the requested fill, allocate a 2x bucket array, migrate buckets,
  and report allocation, migration, and total cycles per live entry.

Reasonableness:

- This is the main evidence for cold datapath throughput and pure-table
  maintenance behavior because it controls cache state, fill, sample count,
  arch variant, and timing source.

## `ft_bench_extra`

Source: `flowtable/test/bench_flow4_vs_extra.c`

Target: `make -C flowtable/test bench-extra`

Purpose:

- Focused pure `flow4` vs slot-extra `flow4_extra` comparison under matched
  capacity, keys, bucket count, operation count, and timestamp shift.
- Show whether slot-extra improves maintenance and find locality without
  hiding insert/touch costs.

Method:

- The initial matched block runs at 75% active fill.
- The insert sweep fills an empty table to 75% active fill for
  `q=1/32/64/128/256`.
- The combined `insert+maint` case starts at 75% active fill, marks half of
  active entries stale, times a maintenance sweep plus reinsertion of freed
  entries, and reports cycles per turned-over entry.
- The inline-maint cases start at 75% active fill with active entries stale,
  then time `add_idx_bulk_maint` for bounded new-flow windows.
- Timestamp 0 is never used as an expired timestamp because it is the permanent
  sentinel; expired entries use `1 << ts_shift`.

Reasonableness:

- This is a relative comparison benchmark.  It is useful because both variants
  see the same keys and active fill, and because the measurement stays inside
  the flowcache operating range.
- It is not a hash capacity benchmark and does not claim 100% fill behavior.

## `ft_bench_extra_full`

Source: `flowtable/test/bench_flow_extra_table.c`

Target: `make -C flowtable/test bench-extra-full`

Purpose:

- Broad slot-extra family sweep across `flow4_extra`, `flow6_extra`, and
  `flowu_extra`.
- Confirm that datapath, maintain, and grow behavior is stable across
  families, fill levels, query sizes, and arch variants.

Method:

- Datapath mode prefills to the requested active fill, measures
  `find_hit_touch`, `find_miss`, and `del_key` at that fill, then refills a
  fresh table and measures a bounded `add_idx` window from that fill.
- Maintain mode marks 50% of active entries stale and reports one
  `ft_table_extra_maintain()` pass in cycles per bucket.
- Grow mode prefills to requested active fill, allocates a 2x bucket region,
  migrates, and reports allocation/migration/total cycles per live entry.
- The Makefile sweep uses fills `40/60/75/80/90`.  The standalone binary
  rejects fills above 95.

Reasonableness:

- This is the slot-extra coverage sweep.  Use it to find regressions across
  variants and fill levels.
- For headline cold-cache datapath numbers, prefer `ft_bench`; this benchmark
  intentionally favors broad coverage and simple repeated timing.

## Fixed Reference Results

These are local reference datapath results for the 2026-04-29 brush-up pass.
They are not cross-machine claims; use them as regression anchors for the same
host and setup.

Environment:

- Host CPU: AMD Ryzen 7 5825U with Radeon Graphics, 16 logical CPUs, 8 cores,
  16 MiB shared L3.
- Kernel perf permission: `kernel.perf_event_paranoid=0`.
- `ft_bench`: `--arch avx2 --pin-core 2 --query 256 --raw-repeat 3 --keep-n 1
  FAMILY 1048576 FILL`, with default hugepage-backed allocations.
- `ft_bench_extra_full`: `taskset -c 2 ... --arch avx2 --query 256 --reps 7
  FAMILY 1048576 FILL`.
- Units are cycles per key.  Fill is active entries divided by configured
  entry capacity.  `90%` is Red-zone pressure data, not a normal operating
  target.
- In `ft_bench_extra_full`, `add_idx` is the bounded add-from-fill window
  reported by the binary; for this 1M-entry setup the window is 32768 keys.

`ft_bench` / pure table:

| family | fill | find_hit | find_miss | add_idx | add_ignore | add_update | del_idx | del_key | find_del_idx |
|--------|-----:|---------:|----------:|--------:|-----------:|-----------:|--------:|--------:|-------------:|
| flow4 | 40 | 63.75 | 69.30 | 78.36 | 94.77 | 95.43 | 30.66 | 63.71 | 82.38 |
| flow4 | 60 | 71.02 | 65.90 | 84.02 | 102.03 | 101.41 | 32.07 | 71.52 | 92.38 |
| flow4 | 75 | 83.75 | 69.10 | 86.48 | 110.70 | 110.35 | 31.64 | 84.14 | 103.05 |
| flow4 | 80 | 90.70 | 69.69 | 86.84 | 115.04 | 116.64 | 32.46 | 88.52 | 106.13 |
| flow4 | 90 | 98.83 | 70.31 | 91.56 | 120.47 | 120.08 | 33.12 | 97.77 | 117.03 |
| flow6 | 40 | 71.29 | 76.29 | 84.80 | 98.55 | 98.36 | 30.74 | 71.17 | 83.52 |
| flow6 | 60 | 70.31 | 78.16 | 86.68 | 100.94 | 103.55 | 32.62 | 79.38 | 98.09 |
| flow6 | 75 | 90.98 | 78.87 | 95.23 | 115.90 | 118.16 | 33.09 | 96.45 | 113.16 |
| flow6 | 80 | 94.06 | 76.80 | 89.10 | 112.30 | 113.09 | 23.16 | 93.44 | 103.98 |
| flow6 | 90 | 90.55 | 51.17 | 91.56 | 114.73 | 115.70 | 22.70 | 101.72 | 108.40 |
| flowu | 40 | 60.55 | 49.22 | 74.77 | 86.84 | 87.42 | 21.76 | 66.68 | 79.84 |
| flowu | 60 | 67.97 | 51.60 | 76.88 | 92.73 | 93.52 | 23.09 | 73.16 | 89.06 |
| flowu | 75 | 81.25 | 52.34 | 82.58 | 104.30 | 104.73 | 22.34 | 85.08 | 97.93 |
| flowu | 80 | 84.10 | 50.66 | 84.34 | 111.80 | 111.41 | 23.32 | 92.81 | 106.29 |
| flowu | 90 | 91.68 | 51.09 | 91.41 | 116.64 | 117.85 | 23.36 | 99.22 | 109.06 |

`ft_bench_extra_full` / slot-extra table:

| family | fill | add_idx | find_hit_touch | find_miss | del_key |
|--------|-----:|--------:|---------------:|----------:|--------:|
| flow4_extra | 40 | 67.30 | 41.02 | 31.27 | 57.68 |
| flow4_extra | 60 | 69.77 | 41.58 | 31.22 | 59.09 |
| flow4_extra | 75 | 100.24 | 43.49 | 30.85 | 60.06 |
| flow4_extra | 80 | 132.17 | 44.06 | 30.49 | 60.42 |
| flow4_extra | 90 | 269.48 | 48.97 | 30.82 | 62.23 |
| flow6_extra | 40 | 82.31 | 62.16 | 49.41 | 76.39 |
| flow6_extra | 60 | 87.07 | 63.35 | 49.58 | 78.20 |
| flow6_extra | 75 | 112.87 | 63.99 | 49.47 | 81.10 |
| flow6_extra | 80 | 146.40 | 65.90 | 49.61 | 79.93 |
| flow6_extra | 90 | 309.19 | 70.39 | 49.49 | 86.80 |
| flowu_extra | 40 | 83.00 | 62.33 | 49.50 | 77.16 |
| flowu_extra | 60 | 84.69 | 62.64 | 49.50 | 78.82 |
| flowu_extra | 75 | 114.44 | 64.31 | 49.57 | 79.76 |
| flowu_extra | 80 | 149.26 | 65.26 | 49.60 | 80.74 |
| flowu_extra | 90 | 306.71 | 69.57 | 49.57 | 86.27 |

## `ft_bench_sweep`

Source: `flowtable/test/bench_flow4_maint_sweep.c`

Target: `make -C flowtable/test bench-sweep`

Purpose:

- Compare pure vs slot-extra full maintenance sweep cost across table size,
  active fill, and expired-entry ratio.

Method:

- Matrix:
  - capacities: `16K, 64K, 256K, 1M, 4M`
  - active fills: `25%, 50%, 75%, 90%`
  - expired ratios inside active entries: `10%, 50%, 90%`
- Bucket count is selected so the requested fill is approximately the actual
  bucket load after power-of-two rounding.
- Both variants get the same flow4 keys and the same stale/fresh split.
- Reports one sweep in cycles per bucket and the slot-extra/pure ratio.

Reasonableness:

- This benchmark answers scaling and memory-locality questions for full
  maintenance sweeps.  It is a matrix/trend benchmark, not a datapath headline.
- The stale timestamp is non-zero to avoid the permanent timestamp sentinel.

## `ft_bench_zoned`

Source: `flowtable/test/bench_flow4_zoned.c`

Target: `make -C flowtable/test bench-zoned`

Purpose:

- Model the flowcache zone policy under sustained traffic and compare pure vs
  slot-extra behavior in Green/Yellow/Red regions.

Method:

- Table sizes: 1M and 2M.
- Scenarios:
  - Green: 70% active fill, 8% of active entries stale.
  - Yellow: 80% active fill, 20% stale.
  - Red: 90% active fill, 30% stale.
- Traffic batches use `Q_add=64` and `Q_add=128`; hits are `32 * Q_add`,
  approximating a 3% miss stream.
- Add timing includes add, inline maintenance, and zone-triggered sweep work.
- Hit timing measures repeated `find`/`find_touch` over fresh entries.
- Reported `cy/add`, `cy/hit`, and `cy/pkt` use the actual counted operations,
  not an assumed batch size.

Reasonableness:

- This is the workload-level evidence that the algorithms remain fast while
  fill control reacts before the Red guardrail.
- It is intentionally scenario-based and should be read alongside the colder,
  more controlled `ft_bench` datapath numbers.

## `ft_bench_ctrl`

Source: `flowtable/test/bench_fill_ctrl.c`

Target: `make -C flowtable/test bench-ctrl`

Purpose:

- Validate the adaptive fill controller on a slot-extra `flow4` table.

Method:

- Simulates three phases on a 1M table:
  - normal: `Q_add=64`, `Q_hit=2048`, about 3% miss
  - DoS: `Q_add=512`, `Q_hit=2048`, about 20% miss
  - recovery: `Q_add=64`, `Q_hit=2048`
- Simulated time controls timestamp aging; real TSC is used only for cycle
  measurement.
- Pre-fill uses a uniform age distribution so normal traffic reaches a real
  steady state rather than relying on an artificial empty table.
- Reported `cy/pkt` includes add/inline-maint, hit, and sweep cost.  Sweep cost
  is also shown per batch.

Reasonableness:

- This benchmark is not a raw hash datapath microbenchmark.  It demonstrates
  whether the controller keeps fill near target and limits DoS pressure without
  unbounded maintenance work.

## `ft_sim_cache`

Source: `flowtable/test/sim_flow_cache.c`

Target: `make -C flowtable/test sim`

Purpose:

- End-to-end flowcache simulation with configurable pps, miss rate, timeout,
  maintenance budget, inline maintenance, adaptive fill target, and optional
  DDoS burst model.

Method:

- Uses simulated time for traffic and expiration so runs are deterministic with
  respect to wall-clock duration.
- Reports fill, miss rate, adds, maintenance expirations, forced expirations,
  average add/maintenance cycles, and table stats.

Reasonableness:

- Treat this as system behavior validation, not as the primary microbenchmark.
  It is useful for confirming that the cache remains in the configured fill
  band under traffic assumptions.
