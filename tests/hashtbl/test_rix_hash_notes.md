# test_rix_hash.c Test Scenarios

## Common Settings

| Item | Value |
|------|-----|
| Node type | `struct mynode { uint64_t key[2]; }` (128-bit key) |
| Key assignment | `key[0] = 1-origin index, key[1] = 0xDEADC0DE00000000` (basic) / `0` (fuzz) |
| Hash function | Murmur3-finalizer-style mix (independent mix of both 64-bit halves then XOR) |
| Buckets for basic | `NB_BK_BASIC=4` (4×16=64 slots, 20 nodes) |

---

## test_init_empty

**Purpose**: Verify that the table is empty immediately after initialization.

1. After `RIX_HASH_INIT`, verify `rhh_nb == 0` and `rhh_mask == NB_BK_BASIC - 1`.
2. Verify that `find` on an empty table returns `NULL`.
3. Verify that `remove` on an empty table returns `NULL`.

---

## test_insert_find_remove

**Purpose**: Verify basic insert / find / remove behavior.

1. Insert all 20 nodes → each return value is `NULL` (success).
2. After insert, `rhh_nb == 20`.
3. `find` all nodes → each returns the correct pointer.
4. `find` a non-existent key → `NULL`.
5. `remove` nodes with even indices → returns the removed node's own pointer.
6. After remove, `rhh_nb == 10`.
7. `find` removed nodes → `NULL`. `find` remaining nodes → correct pointer.
8. `remove` a non-existent key → `NULL`.

---

## test_duplicate

**Purpose**: Verify that duplicate inserts of the same key are correctly detected.

1. Insert `g_basic[0]` → `NULL` (success).
2. Re-insert the same `g_basic[0]` → returns existing node pointer (`&g_basic[0]`). `rhh_nb` does not increase.
3. Insert a separate object outside the pool (`dup`) with the same key value →
   - Duplicate check is performed by key comparison, which occurs before index calculation, so `dup`'s out-of-pool address is not a problem.
   - Return value is the existing `&g_basic[0]`. `rhh_nb` does not increase.

---

## test_staged_find

**Purpose**: Verify that the 3-stage decomposed API (hash_key / scan_bk / cmp_key) in x1/x2/x4 variants returns the same result as `find`.

### x1 staged
- For all 20 nodes, execute `hash_key` → `scan_bk` → `cmp_key` in sequence and verify correct pointers are returned.
- Verify that `cmp_key` returns `NULL` for a non-existent key.

### x2 bulk
- Run `hash_key2` → `scan_bk2` → `cmp_key2` with `keys2 = { g_basic[0].key, bad_key }`.
- Verify `res2[0] == &g_basic[0]`, `res2[1] == NULL`.

### x4 bulk
- Run `hash_key4` → `scan_bk4` → `cmp_key4` with `keys4 = { g_basic[3].key, bad_key, g_basic[7].key, g_basic[11].key }`.
- Verify `res4[0..3]` matches expected pointers (`&g_basic[3]`, `NULL`, `&g_basic[7]`, `&g_basic[11]`).

---

## test_walk

**Purpose**: Verify walk callback traversal and early termination.

1. After inserting all 20 nodes, count with `walk_count_cb` → `20`.
2. After removing 5 nodes, walk again → `15`.
3. Early termination test: callback returns `99` after being called 3 times.
   - Verify the return value of `walk` is `99`.
   - Verify the callback was called exactly 3 times.

---

## test_fuzz

**Purpose**: Verify consistency by comparing random operations against a model (`present[]` array).

### Parameters (defaults)

| Argument | Default | Description |
|------|-----------|------|
| seed | `0xC0FFEE11` | Random seed |
| N | 512 | Node pool size |
| nb_bk | 64 | Number of buckets (64×16=1024 slots, ~50% fill) |
| ops | 200000 | Number of operations |

Command line: `./hash_test [seed [N [nb_bk [ops]]]]`

### Operation ratios

| Operation | Probability | Verification |
|------|------|---------|
| insert | 60% | If `present[idx]` exists: returns existing pointer; if not: `NULL` (success) or `elm` (table full, may skip) |
| remove | 20% | If `present[idx]` exists: returns removed node; if not: `NULL` |
| find   | 20% | If `present[idx]` exists: returns correct pointer; if not: `NULL` |

### Periodic check
- Every 1024 steps, verify `head.rhh_nb == in_table` (model count).

### Final checks
- Final verification that `head.rhh_nb == in_table`.
- Verify that the number of nodes reachable via `walk` matches `in_table`.

---

## Build

```sh
# from the hashtbl/ directory
make hash_test          # build
make hash_test_run      # build + run
./hash_test             # run with default parameters
./hash_test 42 1024 128 500000  # seed=42, N=1024, nb_bk=128, ops=500000
```

Build flags: `-std=gnu11 -g -O2 -mavx2 -Wall -Wextra`
(AVX-512 is skipped; AVX2 only)
