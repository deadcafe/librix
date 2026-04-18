# librix v0.3.0

Release focused on reusable index rings, ring-backed flow allocators, and
more realistic burst handling in the flow-cache simulator.

`v0.3.0` adds the public `rix_ring` API for `u32` index FIFO/LIFO operation,
moves the test/bench/sim flow-entry allocators onto that ring-backed model,
and exposes `add_idx_bulk_maint` so flow insertion and maintenance can run in
one path. The flow-cache simulator is also restructured so generator and
receiver responsibilities are clearer and burst control now reacts in
`85%/90%/95%` fill bands instead of pinning the table far below target.

## Validation status

- `make test -C flowtable/test`: passed
- `make -C flowtable/test bench`: passed
- `make -C flowtable/test /home/tokuzo/work/deadcafe/librix/flowtable/build/bin/ft_sim_cache`: passed
- Hugepage `ft_sim_cache` scenarios validated at `1M entry / 1M pps / batch=256`

## Notes

- Normal load is tuned to settle just below target fill, while burst load is
  allowed to rise into the aggressive band before reclaim tightens.
- AVX-512 remains workload- and CPU-dependent rather than universally faster
  than AVX2.
