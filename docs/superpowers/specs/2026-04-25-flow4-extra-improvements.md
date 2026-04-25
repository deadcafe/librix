# flow4 slot_extra improvement spec

Date: 2026-04-25

## Goals

- Keep the existing experimental `flowtable/extra` ABI source-compatible.
- Fix stale bucket-mask use after `migrate()`.
- Make bucket `extra[]` timestamp access harder to misuse.
- Add touch-capable find API parity with classic flow4.
- Reduce sparse-bucket maintain cost by avoiding `extra[]` loads when the
  occupancy threshold already rejects the bucket.
- Add regression tests for migrate/touch and key-domain behavior.

## API changes

- Add generic `rix_hash_slot_extra_get()` / `rix_hash_slot_extra_set()` helpers.
  They validate slot range and expected index before reading or writing
  `bucket->extra[slot]`.  `expected_idx == RIX_NIL` (the empty-slot sentinel)
  is rejected so that callers cannot accidentally match an unoccupied slot.
- Add `rix_hash_slot_extra_touch_2bk()` — given `bk[0]/bk[1]` from a
  `rix_hash_find_ctx_extra_s`, the expected `entry_idx` and a slot, write the
  encoded timestamp to whichever of the two buckets actually holds the entry.
  Returns 0 on success, non-zero if neither bucket matches.
  `expected_idx == RIX_NIL` is rejected on the same grounds as `_get/_set`.
- Add `int ft_table_extra_touch_checked(...)`. The existing
  `ft_table_extra_touch(...)` remains and delegates to the checked function.
- Add `int ft_table_extra_maint_ctx_init(...)` to fill
  `struct ft_maint_extra_ctx` from `struct ft_table_extra`.
- Add `u32 flow4_extra_table_find_touch(..., u64 now)`. Existing
  `flow4_extra_table_find(...)` remains a non-touching wrapper.

## DRY consolidation

- Replace inline `bk->extra[slot] = flow_extra_timestamp_encode(...)` writes in
  the find paths (`flow_table_generate_extra.h::find_small_`,
  `flow_core_extra.h::find_bulk_`) with calls to
  `rix_hash_slot_extra_touch_2bk()`.
- Replace the "touch existing duplicate via meta-derived bk" pattern in
  `flow_core_extra.h::add_idx_small_` and `add_idx_bulk_` with
  `rix_hash_slot_extra_set()` (mask + cur_hash + expected_idx).  Wraps both
  add-with-touch-policy call sites.
- All call sites that route through the helpers now `RIX_ASSERT(rc == 0)`
  because the surrounding scan has already proven the (bk, slot, expected_idx)
  triple.
- Direct `ctx.bk[N]->extra[bit] = encoded` writes in the *insertion* paths
  (empty-slot fill, fp-hit duplicate replace, victim eviction) are kept as
  primitives — bucket and slot are decided locally in those blocks, so adding
  another wrapper around them is just renaming.
- The find-time bucket-side timestamp update is treated as a single concern
  with one helper. Per-entry timestamp updates remain on the existing
  `FCORE_EXTRA_TOUCH_TIMESTAMP(owner, entry, now)` hook (default no-op,
  overridable by owners that store TS per entry).
- Legacy `flow_extra_ts_get()/_set()` (in `flow_extra_key.h`) are retained for
  bench/test setup where (bk, slot) are already locally known; the header
  comments now point users to the validating helpers for higher-level code.

## Variant resolution

- `struct ft_table_extra` gains a `size_t meta_offset` (record-base ->
  flow_entry_meta_extra) that `ft_table_extra_init()` resolves via a
  per-variant switch.  `ft_table_extra_maint_ctx_init()` reads this field
  instead of hard-coding `offsetof(struct flow4_extra_entry, meta)`.  When
  flow6/flowu_extra are added, extend the switch in `ft_table_extra_init()`
  only; downstream helpers stay variant-agnostic.

## Behavior changes

- Touch uses `ft->ht_head.rhh_mask`, not `ft->start_mask`, so it remains valid
  after `migrate()`.
- Touch is a no-op when the entry index, slot, or live bucket mapping is stale.
- `flow4_extra_table_find_touch(..., now=0)` preserves existing non-touching
  behavior.

## Test coverage

- Existing extra and parity tests must continue to pass.
- Add regression coverage for:
  - touch after `migrate()`;
  - checked touch rejecting stale/deleted indices;
  - `flow4_extra_table_find_touch()` updating `extra[]`;
  - VRF/domain caveat for the compact extra key shape.
