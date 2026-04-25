# flowtable/extra/

Parallel implementation of the flow cache using the
`RIX_HASH_GENERATE_SLOT_EXTRA_EX` (192 B bucket) rix_hash variant.

The per-entry expiry timestamp is relocated from
`flow_entry_meta.timestamp` into the bucket's third cacheline
(`rix_hash_bucket_extra_s::extra[slot]`), which lets
`ft_table_extra_maintain` decide expiry without touching entry
memory.

Status: experimental. API parallels `flowtable/src/` (classic) but
uses `ft_table_extra_*`, `flow4_extra_table_*` symbol names so both
variants coexist in one binary for AB benchmarking.

Consumers that want to try extra include
`<rix/flow_extra_table.h>` explicitly and add
`-I<repo>/flowtable/extra/include` to their build. Not part of
`make install` until classic is retired.

See `docs/superpowers/specs/2026-04-22-flow4-slot-extra-design.md`
for design rationale and success criteria.
