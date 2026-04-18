# librix v0.3.0

## Suggested GitHub repository description

Index-based shared-memory data structures in C, with reusable index rings and
an IPv4/IPv6 flow-table library.

## Suggested GitHub topics

`c`, `shared-memory`, `mmap`, `data-structures`, `ring-buffer`, `hash-table`,
`cuckoo-hash`, `simd`, `avx2`, `networking`, `flow-table`

## Release summary

`v0.3.0` adds a reusable `rix_ring` container for `u32` index FIFO/LIFO use
and moves the flowtable support allocator paths onto that ring-backed model.
The release also exposes `add_idx_bulk_maint` for combined add-plus-maintain
work, which lets the flow-cache simulator exercise hot-bucket expiration while
new flows are being inserted.

The simulator side is the other major change in this release. Generator and
receiver responsibilities are now separated more cleanly, attack traffic is
carried explicitly as batch metadata instead of by positional convention, and
the timeout controller now distinguishes normal target tracking from
high-fill emergency reclaim. In tested hugepage scenarios at `1M entry /
1M pps / batch=256`, normal load settles near the configured target while
burst load is allowed to rise toward the aggressive band and is then pulled
back without crossing the `95%` guardrail.

## Highlights

- new public `rix_ring` API for reusable index FIFO/LIFO handling
- test/bench/sim flow allocators now use a ring-backed LIFO instead of
  intrusive free-list links
- `flow4/6/u add_idx_bulk_maint` combines add and maintenance work in one path
- flow-cache simulator separates generator and receiver concerns more clearly
- burst controller now applies aggressive reclaim in `85%/90%/95%` fill bands

## Validation status

- `make test -C flowtable/test`: passed
- `make -C flowtable/test bench`: passed
- `make -C flowtable/test /home/tokuzo/work/deadcafe/librix/flowtable/build/bin/ft_sim_cache`: passed
- Hugepage `ft_sim_cache` scenarios validated at `1M entry / 1M pps / batch=256`

## Notes

- The simulator controller is tuned to stay just below target in normal load,
  then spend headroom more gradually under burst before entering aggressive
  reclaim at high fill.
- AVX-512 support remains workload- and CPU-dependent rather than universally
  faster than AVX2.
- Most datapath tuning still focuses on `flow4`. `flow6` and `flowu` remain
  functionally covered, but have seen less tuning.
