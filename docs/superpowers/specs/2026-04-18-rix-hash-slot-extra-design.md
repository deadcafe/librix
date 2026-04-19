# rix_hash slot-extra variant — Design Spec

- Status: Draft
- Date: 2026-04-18
- Owner: deadcafe.beef@gmail.com
- Scope: `include/rix/rix_hash_slot_extra.h` (new) と対応 test / bench のみ.
  flowtable への適用は本 spec の完了後に別途判断する.

## 1. 背景と目的

flowtable の `maintain` は slot ごとに `flow_entry_meta.timestamp` を pool から
読み込むため, bucket 1 つの scan で最大 16 回の pool 間接参照が発生する.
bucket 側に per-slot の u32 を持たせて TS を配置すれば, `hash[] / idx[] /
extra[]` の 3 cache line だけで生死判定ができる.

`rix_hash_slot` の bucket (128B) は librix 公開の汎用構造なので直接拡張しない.
代わりに **192B 拡張 bucket を扱う parallel variant** を追加し, 生成マクロで
切り替え可能にする.

本 spec は rix_hash レイヤーに extension を追加することだけを扱う. flowtable
統合は本 variant を汎用拡張として test / bench で validate した上で, 別 spec
で判断する.

## 2. 非ゴール

- flowtable 側の API 変更.
- `rix_hash_fp` / `rix_hash_keyonly` variant への拡張 (パターン確立後に検討).
- `rix_hash.h` umbrella への自動取り込み (オプトイン方針のため意図的に除外).
- 既存 `rix_hash_slot` の動作 / 性能 / レイアウト変更.
- 既存 `struct rix_hash_bucket_s` (128B) の再定義.

## 3. 設計

### 3.1 ファイル構成

| 種別 | パス | 役割 |
|------|------|------|
| 公開ヘッダ | `include/rix/rix_hash_slot_extra.h` | 型 + 生成マクロ |
| 機能テスト | `tests/hashtbl_extra/test_rix_hash_extra.c` | 動作検証 |
| ベンチ | `tests/hashtbl_extra/bench_rix_hash_extra.c` | classic 比較 |
| ビルド | `tests/hashtbl_extra/Makefile` | make target |

`include/rix/rix_hash.h` (umbrella) は変更しない. 本 variant を使う caller は
`#include <rix/rix_hash_slot_extra.h>` を明示的に記述する.

### 3.2 型定義

`include/rix/rix_hash_slot_extra.h` で新規定義:

```c
struct rix_hash_bucket_extra_s {
    u32 hash [RIX_HASH_BUCKET_ENTRY_SZ];  /* cl0 (64B): fingerprints   */
    u32 idx  [RIX_HASH_BUCKET_ENTRY_SZ];  /* cl1 (64B): 1-origin idx   */
    u32 extra[RIX_HASH_BUCKET_ENTRY_SZ];  /* cl2 (64B): user u32       */
} __attribute__((aligned(64)));

struct rix_hash_find_ctx_extra_s {
    union rix_hash_hash_u            hash;
    struct rix_hash_bucket_extra_s  *bk[2];
    const void                      *key;
    u32  fp;
    u32  fp_hits[2];
    u32  empties[2];
};
```

- `RIX_HASH_BUCKET_ENTRY_SZ = 16` (共通定数を再利用).
- bucket の前半 2 cache line は classic と完全同一レイアウト. `extra[]` は
  `cl2` (オフセット 128, 64B アラインされた第 3 キャッシュライン) に配置.
- `RIX_HASH_HEAD(name)` struct は classic と共通で再利用する (bucket 型に
  依存しない).

### 3.3 公開マクロ

以下を `rix_hash_slot_extra.h` で提供する:

```c
RIX_HASH_PROTOTYPE_SLOT_EXTRA        (name, type, key_field, hash_field,
                                      slot_field, cmp_fn)
RIX_HASH_PROTOTYPE_SLOT_EXTRA_EX     (name, type, key_field, hash_field,
                                      slot_field, cmp_fn, hash_fn)
RIX_HASH_PROTOTYPE_STATIC_SLOT_EXTRA (...)       /* static 版 */
RIX_HASH_PROTOTYPE_STATIC_SLOT_EXTRA_EX (...)    /* static 版 */
RIX_HASH_GENERATE_SLOT_EXTRA         (name, type, key_field, hash_field,
                                      slot_field, cmp_fn)
RIX_HASH_GENERATE_SLOT_EXTRA_EX      (name, type, key_field, hash_field,
                                      slot_field, cmp_fn, hash_fn)
```

便利マクロとして:

```c
RIX_HASH_INSERT_EXTRA (name, head, buckets, base, elm, extra)
```

は新規追加. 既存の `RIX_HASH_FIND`, `RIX_HASH_REMOVE`, `RIX_HASH_WALK`,
`RIX_HASH_HASH_KEY` 系, `RIX_HASH_SCAN_BK` 系, `RIX_HASH_PREFETCH_NODE` 系,
`RIX_HASH_CMP_KEY` 系は `name##_xxx(...)` への token paste のみを行う
ラッパなので, 生成関数が `rix_hash_bucket_extra_s` を受け取るようになっても
caller コードは同一マクロを引数を変えずに使える (再定義不要). 同一 TU 内で
classic と extra の両 variant を併用する場合は `name` prefix を別にする
(`RIX_HASH_GENERATE_SLOT(foo_c, ...)` と
`RIX_HASH_GENERATE_SLOT_EXTRA(foo_x, ...)`) ことで symbol 衝突を避ける.

### 3.4 生成関数シグネチャ

classic (`rix_hash_slot.h`) との差分を太字で示す:

```c
void name##_init(struct name *head, unsigned nb_bk);

struct type *name##_insert(
    struct name *head,
    struct rix_hash_bucket_extra_s *buckets,         /* type 変更 */
    struct type *base,
    struct type *elm,
    u32 extra);                                       /* NEW 引数 */

unsigned name##_remove_at(
    struct name *head,
    struct rix_hash_bucket_extra_s *buckets,
    unsigned bk, unsigned slot);

struct type *name##_remove(
    struct name *head,
    struct rix_hash_bucket_extra_s *buckets,
    struct type *base,
    struct type *elm);

struct type *name##_find(
    struct name *head,
    struct rix_hash_bucket_extra_s *buckets,
    struct type *base,
    const key_type *key);

int name##_walk(
    struct name *head,
    struct rix_hash_bucket_extra_s *buckets,
    struct type *base,
    int (*cb)(struct type *, void *), void *arg);

/* staged find x1 */
void          name##_hash_key      (struct rix_hash_find_ctx_extra_s *ctx,
                                    struct name *head,
                                    struct rix_hash_bucket_extra_s *buckets,
                                    const key_type *key);
void          name##_scan_bk       (struct rix_hash_find_ctx_extra_s *ctx,
                                    struct name *head,
                                    struct rix_hash_bucket_extra_s *buckets);
void          name##_prefetch_node (struct rix_hash_find_ctx_extra_s *ctx,
                                    struct type *base);
struct type  *name##_cmp_key       (struct rix_hash_find_ctx_extra_s *ctx,
                                    struct type *base);

/* staged find xN (bulk) — 同様に ctx 型のみ差し替え */
```

### 3.5 動作契約

- **insert**
  - 空き slot を見つけて `hash / idx / extra` を一括書き込み.
  - 同時に `elm->hash_field`, `elm->slot_field` を書き換える (classic と同じ).
  - 必要なら kickout で経路確保してから配置する.
- **kickout (`flipflop`)**
  - 既存 entry を現在の slot から alt bucket の空き slot に移す.
  - `alt->hash[new]  = cur->hash[old]`
  - `alt->idx [new]  = cur->idx [old]`
  - `alt->extra[new] = cur->extra[old]`
  - `cur->hash[old] = 0; cur->idx[old] = RIX_NIL; cur->extra[old] = 0`
  - `node->hash_field / slot_field` は既存契約通り更新.
- **remove / remove_at**
  - `hash[slot] = 0; idx[slot] = RIX_NIL; extra[slot] = 0`.
- **find / walk / staged find**
  - `extra[]` を読まない / 書かない. 返り値や挙動は classic と同一.
- **update (post-insert で extra を書き換え)**
  - caller が `buckets[bk].extra[node->slot_field] = new_val` を直接実行.
  - rix_hash は helper を提供しない (冗長化を避ける).
- **fingerprint scan (SIMD)**
  - `hash[]` のみを対象とする classic と同じ実装を使う.
  - `extra[]` は fingerprint 判定に関与しない.

### 3.6 prefetch / accessor helpers

classic で定義されている helper を `_extra_bucket_` を名前中に挟む形で並列
定義する (型による overload ではなく名前で明示的に区別する):

```c
/* idx から bucket ポインタを取得 */
static inline struct rix_hash_bucket_extra_s *
rix_hash_extra_bucket_of_idx(struct rix_hash_bucket_extra_s *buckets,
                             unsigned bk_idx);

/* cl0 (hash[]) のみ prefetch */
static inline void
rix_hash_prefetch_extra_bucket_hashes_of(
    const struct rix_hash_bucket_extra_s *bucket);

/* cl1 (idx[]) のみ prefetch */
static inline void
rix_hash_prefetch_extra_bucket_indices_of(
    const struct rix_hash_bucket_extra_s *bucket);

/* cl2 (extra[]) のみ prefetch — 拡張で新規追加される用途 */
static inline void
rix_hash_prefetch_extra_bucket_extras_of(
    const struct rix_hash_bucket_extra_s *bucket);

/* cl0 + cl1 (通常 find 相当) */
static inline void
rix_hash_prefetch_extra_bucket_of(
    const struct rix_hash_bucket_extra_s *bucket);

/* cl0 + cl1 + cl2 (全 3 cache line) */
static inline void
rix_hash_prefetch_extra_bucket_full_of(
    const struct rix_hash_bucket_extra_s *bucket);
```

`_extra_bucket_` という中間 token によって classic の同機能 helper
(`rix_hash_prefetch_bucket_hashes_of` 等) と名前空間が自然に分離される.

### 3.7 動作原理の整合性

- `rix_hash_fp(...)` による bk0/bk1/fp の導出ロジックは変更しない.
- `insert` の検索順序 (bk0 優先 -> bk1 -> kickout) は classic と同一.
- `remove` は `hash_field & mask` から bk, `slot_field` から slot を O(1) で
  特定する既存ロジックを変更しない.
- `RIX_HASH_GENERATE_SLOT_EXTRA_EX` は `hash_fn` を明示指定でき, `EX` なしは
  `rix_hash_bytes_fast` ベースの default hash を使う (classic と同じ規約).

### 3.8 非互換性

- 既存 classic variant の API/ABI/レイアウトは一切変更しない.
- `rix_hash_slot_extra.h` は新規ヘッダなので, 取り込まない TU には影響しない.
- `struct rix_hash_bucket_extra_s` は `struct rix_hash_bucket_s` と binary
  互換でない (cl2 が付く). 相互キャストは行わない.

## 4. テスト戦略

### 4.1 機能テスト (`test_rix_hash_extra.c`)

1. **基本**: init → insert → find → remove → find(miss) の 1-shot 動作.
2. **extra 書き込み**: `insert(elm, V)` 後, 該当 bk/slot に V が書かれている.
3. **extra 非干渉**: 他 slot への insert が既存 slot の `extra` を変更しない.
4. **kickout 追従**: 16 slot 満杯 + kickout を誘発する insert 列で, walk に
   よる確認で全 elm の `extra` が挿入時の値を保持する (per-elm random 値).
5. **remove clear**: remove 後 `extra[slot] == 0` を bucket 走査で検証.
6. **staged find x1/xN**: ctx API 経由の find が `name##_find` と同一結果.
7. **classic 非干渉**: 同一 TU で `RIX_HASH_GENERATE_SLOT(classic_name, ...)`
   と `RIX_HASH_GENERATE_SLOT_EXTRA(extra_name, ...)` を同時生成し, 相互独立
   に動作する (symbol 衝突なし, head struct 共有しても動作).
8. **fuzz**: ランダム insert/remove/extra-update 混在で一貫性 invariant を
   全 slot で継続チェック (`walk` で elm->expected_extra vs bucket.extra
   を比較).
9. **gcc / clang 両方でビルド & pass**.

### 4.2 ベンチ (`bench_rix_hash_extra.c`)

- `insert`, `find`, `remove` の per-op コストを classic variant と比較.
- 期待: classic + O(1cycle) 程度. `insert` での u32 store 1 個と, bucket
  サイズ差 (192B) に起因する TLB / cache 挙動の差異を計測.
- arch variant は gen/sse/avx2/avx512 全て (hugepage 環境で評価).

### 4.3 既存回帰

- `tests/hashtbl/test_rix_hash.c`, `bench_rix_hash.c` は変更せず green を保つ.

## 5. 作業順序の提案

1. `rix_hash_slot_extra.h` 骨格 (型 + PROTOTYPE / GENERATE マクロ空実装).
2. insert / remove / find の基礎 path を `rix_hash_slot.h` からコピーして
   `_extra` 拡張 (extra[] 書き込み / clear / flipflop 拡張).
3. staged find (x1 / xN) を移植.
4. prefetch / accessor helpers.
5. 機能テスト (test_rix_hash_extra.c).
6. ベンチ (bench_rix_hash_extra.c) + Makefile.
7. gcc + clang, gen / sse / avx2 / avx512 全組合せで test.

## 6. 合否判定基準

本 variant の "汎用拡張として妥当" とみなす条件:

- 機能テスト 4.1 の全項目 green (gcc / clang, gen / sse / avx2 / avx512).
- ベンチ結果で classic との差異が insert +数% 以下, find / remove はほぼ
  同等 (hugepage 環境).
- 既存 classic ベンチ / テストに回帰がない.

ここまで通過したら flowtable 側への適用を別 spec として起案する.

## 7. 未解決事項

- (将来) fp / keyonly variant への同パターン展開の必要性は flowtable 適用
  判断後に検討する.
- (将来) `extra[]` のサイズを u32 以外 (u8 / u16 / u64) に parametric 化
  するかは, 具体的ユースケースが出てから検討する.
