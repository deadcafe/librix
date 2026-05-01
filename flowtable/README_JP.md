# Flow Table

`flowtable/` は librix 上に構築した flow-table ライブラリである。

- `findadd` は持たない
- add datapath 上での暗黙 reclaim や timeout eviction を持たない
- caller-owned の record array を固定配置で使う
- `find`、`add`、`del` を明示的に分ける
- resize では bucket hash table だけを変更し、record は移動しない

3 種の flow key family を実装している。

- `flow4` (IPv4 5-tuple)
- `flow6` (IPv6 5-tuple)
- `flowu` (統合 / protocol 非依存)

ランタイム arch dispatch（`gen`, `sse4.2`, `avx2`, `avx512f`）対応。

## クイックスタート

```sh
# flow table library をビルド
make -C flowtable static

# 正当性テスト
make -C flowtable/test test

# 仕様表と test 対応表
sed -n '1,220p' flowtable/TEST_SPEC_JP.md

# benchmark
make -C flowtable/test bench

# top-level の補助ターゲット
make flowtable
make flowtable-test
make flowtable-bench
```

build 生成物は `flowtable/build/` 配下に出力される。

- `build/lib/libftable.a`
- `build/obj/*.o`
- `build/bin/ft_test`
- `build/bin/ft_bench`

## 1. 設計目標

test の基準表は [TEST_SPEC_JP.md](TEST_SPEC_JP.md) を参照。

この実装の狙いは cache ではなく permanent-style flow table である。
entry は caller が delete するまで生存する前提で扱う。

そのため、重視するのは以下である。

- 安定した `entry_idx`
- 明示的な resize タイミング
- 暗黙 victim eviction を行わないこと
- record storage と bucket storage の双方を caller が制御できること

これは reclaim と `findadd` を中心に設計された `fcache` とは異なる。
ただし `ftable` は、caller 明示呼び出しの maintain API により
timeout expire 自体は持つ。

## 2. 現在の実装範囲

現時点の実装は以下を含む。

- `ft_flow4_table`、`ft_flow6_table`、`ft_flowu_table`
- `ft_table_extra` と `flow4_extra` / `flow6_extra` / `flowu_extra` の
  slot-extra table API
- scalar / bulk の `find`、`add_idx`、`del_idx`、`del_key`
- caller-defined record layout を扱う intrusive `init()`（stride + offset）
- サフィックスなし公開 API + runtime arch dispatch
- `migrate()` による grow / shrink / rehash
- 明示 maintenance (`maintain`, `maintain_idx_bulk`)
- bucket-table-only resize（record は移動しない）

### 2.1 timeout maintenance

`ftable` は `add` の中で reclaim しない。expire は明示 maintenance として
分離されている。

- `maintain(...)`
  - bucket sweep ベースの incremental maintain
  - caller が `start_bk`, `expire_tsc`, `max_expired`, `min_bk_entries` を制御する
  - `expired_idxv[]` と `next_bk` を返す
- `maintain_idx_bulk(...)`
  - 直近に hit した `entry_idx[]` から bucket を引き、局所的に expire する
  - `find/add` 直後の hot bucket を使う用途を想定する
  - call ごとに `expire_tsc` override ができる

どちらも expired idx の回収と free list への接続は caller responsibility
である。expire 時間は API call ごとに caller が指定し、table 状態は参照しない。

timestamp が 0 の entry は permanent として扱う。

- `ft_flowX_table_set_permanent_idx(ft, idx)`
  - linked 済み entry を permanent 化する
  - `maintain` / `maintain_idx_bulk` では expire しない
  - `find` や duplicate `add` でも timestamp 0 を保つ

### 2.2 add と maintenance の統合

`add_idx_bulk_maint(...)` は `add_idx_bulk` の上位互換である。通常の add
フェーズの後、各結果 entry の登録先 bucket を走査して expired entry を除去する。
これにより、別途 `maintain_idx_bulk` を呼ぶ必要がなくなり、meta-prefetch や
bucket 解決のステージも省略できる。

ただし、`add_idx_bulk_maint()` は add 先 bucket を起点にした局所 reclaim
である。新規 flow が入る bucket と stale entry の分布が重なる場合は
Green 維持に有効だが、stale entry が別 bucket に偏る場合や、DoS 後の
recovery で新規 add が少ない場合は単独では不足する。

fill を安定して Green に保つには、`add_idx_bulk_maint()` と bucket sweep
型の `maintain()` を併用する。`add_idx_bulk_maint()` は add 周辺を安く掃除し、
`maintain()` は table 全体を少量ずつ巡回して分布依存を下げる役割を持つ。

```c
unsigned n = ft_flow4_table_add_idx_bulk_maint(
    &ft, entry_idxv, nb_keys, policy,
    now, timeout,
    unused_idxv, max_unused, min_bk_used);
```

maint フェーズのコストを制御する 2 つのパラメータ:

- **スキャン予算 (α)** = `max_unused - nb_keys`
  `unused_idxv` の先頭 `nb_keys` スロットは add フェーズの結果
  （duplicate で返却される index）用に予約される。残りの α スロットが
  maint-expired entry の回収予算となる。α == 0 または `timeout` == 0 の場合、
  maint フェーズは完全にスキップされ、`add_idx_bulk` と同一動作になる。

- **Bucket 使用率閾値** (`min_bk_used`, 範囲 0–16)
  bucket を expired entry 走査する前に、占有スロット数をカウントする。
  カウントが `min_bk_used` 未満の場合、その bucket はスキップされる。
  これにより、sparse な bucket での高コストな per-entry meta read を回避できる。
  0 を渡すとフィルタを無効化する。

**チューニング指針**（fill-target auto-timeout 制御との併用）:

α と `min_bk_used` の組み合わせでバッチあたりの expire 能力が決まる。
α を増やすか `min_bk_used` を下げると expire rate が上がり（定常 fill rate
が低下する）、per-key コストが増加する。

`maintain()` sweep は常時・少量・定期的に回す。`next_bk` を保持し、毎回
続きの bucket から再開すること。毎回 0 から開始すると sweep が偏る。

fill zone ごとの初期目安:

| fill zone | timeout | `add_idx_bulk_maint()` | `maintain()` sweep |
|-----------|---------|------------------------|--------------------|
| Green `<75%` | 通常 `t_normal` | 小さい α | 小さい budget (`16`-`64`) |
| Yellow `75%`-`85%` | 短縮 | α を増やす | budget 増 (`128`-`512`) |
| Red `85%+` | `t_min` 近くまで短縮 | α 最大 | budget 最大 |
| Critical `95%+` | `t_min` | add 前 reclaim も検討 | emergency sweep |

`batch=256`、目標 fill ≤ 75% での推奨初期値:

| パラメータ     | 値 | 備考                         |
|---------------|------:|------------------------------|
| `max_unused`  | `nb_keys + 64` | α = 64              |
| `min_bk_used` |    10 | 10/16 未満の bucket をスキップ |
| fill-target   |   65% | auto-timeout 制御器          |

2 Mpps / 3% miss rate の条件で、add + maint 合計 ~115 cy/key となる。
分離呼び出し（`add_idx_bulk` + `maintain_idx_bulk`）の ~200 cy/key と比較して
約 42% の削減である。

例: auto-timeout、統合 maint、常時 sweep を用いた flow cache

```c
/* バッチごとに fill rate から α を算出 */
unsigned alpha = 0;
if (fill_ratio > fill_target) {
    double excess = (fill_ratio - fill_target) / (1.0 - fill_target);
    alpha = (unsigned)(64.0 * excess);   /* 最大 α = 64 */
}
unsigned max_unused = nb_keys + alpha;

unsigned n = ft_flow4_table_add_idx_bulk_maint(
    &ft, entry_idxv, nb_keys, FT_ADD_IGNORE_FORCE_EXPIRE,
    now_tsc, timeout_tsc,
    unused_idxv, max_unused,
    10u /* min_bk_used */);

/* unused_idxv[0..n-1] に add-unused と maint-expired の両方が含まれる */
for (unsigned i = 0; i < n; i++)
    reclaim(unused_idxv[i]);

/* table 全体を定期 sweep する。next_bk は caller が保持する。 */
if (sweep_budget > 0) {
    unsigned n_expired = ft_flow4_table_maintain(
        &ft, next_bk, now_tsc, timeout_tsc,
        expired_idxv, sweep_budget, min_bk_used, &next_bk);

    for (unsigned i = 0; i < n_expired; i++)
        reclaim(expired_idxv[i]);
}
```

### 2.3 slot-extra table variant

slot-extra variant は `RIX_HASH_GENERATE_SLOT_EXTRA_EX` の bucket layout
（192 B bucket）を使う。per-entry expire timestamp を
`flow_entry_meta.timestamp` ではなく bucket 側の
`rix_hash_bucket_extra_s::extra[slot]` に置く。これにより extra
maintenance は、occupied slot ごとに entry record を触る前に bucket
memory だけで expire 判定できる。

用語: **pure** は base flowtable model を指す。bucket は 128 B で、
timestamp は caller-owned flow entry metadata に保存する。**slot-extra**
は 192 B bucket と bucket-side `extra[]` timestamp slot を持つ model である。

公開 symbol は pure と共存する。

- pure: `ft_flow4_table_*`, `struct flow4_entry`
- slot-extra: `ft_table_extra_*`, `flow4_extra_table_*`,
  `flow6_extra_table_*`, `flowu_extra_table_*`,
  `struct flow4_extra_entry`, `struct flow6_extra_entry`,
  `struct flowu_extra_entry`

slot-extra family は `flow4_extra`, `flow6_extra`, `flowu_extra` を
提供する。これらは同じ `ft_table_extra` control plane と runtime arch
dispatch を共有し、family-specific wrapper が protocol layout ごとの
key / entry 型を提供する。

## 3. ストレージモデル

record array は caller が所有する。table は record を移動しない。

resize で変わるのは bucket hash table だけである。

これにより:

- `entry_idx` は `migrate()` 後も不変
- intrusive record layout と相性がよい
- resize cost は record 移動ではなく bucket 再構築に集中する

## 4. hash と resize のモデル

`add()` 時に、実装は size 非依存の hash pair を保存する。

- `hash0`
- `hash1`

この `hash0/hash1` が `ftable` の中核契約である。

`hash0/hash1` は size 非依存であり、table 作成時に固定された
`start_mask` を使って `add()` 時に 1 回だけ計算して entry に保存する。
resize 時に key から再 hash はしない。

resize で変わるのは bucket 選択だけである。

- `bk0 = hash0 & mask`
- `bk1 = hash1 & mask`

ここで `mask = nb_bk - 1` であり、`nb_bk` は常に `2^n` とする。

言い換えると、

- `key -> hash0/hash1` は安定状態
- `hash0/hash1 -> bk0/bk1` は導出状態

である。旧 `bk0/bk1` を resize 後も保持し続ける設計は誤りになる。

`hash1` の制約は current mask に対してではなく、table 作成時に固定された
`start_mask` に対して課す。これが resize-safe な契約である。

- `key -> hash0/hash1` は固定 `start_mask` を使う
- `hash0/hash1 -> bk0/bk1` は current `mask` を使う

### 4.1 必要な不変条件

保存される hash pair には、少なくとも次の条件が必要である。

- 固定 `start_mask` に対して
  `(hash0 & start_mask) != (hash1 & start_mask)`
- したがって、その後の任意の有効 table mask `mask = 2^n - 1` に対しても
  `(hash0 & mask) != (hash1 & mask)`
- したがって `fp = hash0 ^ hash1` も各 active mask に対して non-zero になる

`hash0` と `hash1` 自体は zero を取り得る。重要なのは、mask 後の bucket
pair が常に異なり、その結果 bucket 配列に保存される fingerprint が zero
にならないことである。

最後の条件により、bucket-table-only resize が可能になる。entry が
旧/new table の両方で有効な candidate bucket pair を導出できるだけの
情報を保持している、ということになる。

### 4.2 なぜ `hash0/hash1` を保存するのか

ここが `ftable` と `fcache` の本質的な違いである。

`fcache` は stable resize を中心に設計していないため、bucket 配置は
cache 内部の一時的 detail として扱える。`ftable` はそうはいかない。
entry ごとに resize-safe な hash identity を canonical state として
持つ必要がある。

そのため `ftable` は

- key
- `hash0`
- `hash1`

を保存し、bucket index は一時的な導出値としてのみ扱う。

### 4.3 resize 時の動作

resize では、old mask や old bucket、old slot から `hash1` を再構築しては
ならない。また、hash algorithm や seed を変更しない限り、key から再 hash
もしない。

通常の resize 手順は以下である。

1. 保存済み `hash0/hash1` は変更しない
2. 新しい bucket array を確保
3. `new_bk0 = hash0 & new_mask` を計算
4. `new_bk1 = hash1 & new_mask` を計算
5. live entry を新 table に再挿入
6. 旧 bucket array を解放

この性質により、record を動かさず bucket hash table だけを変更できる。

### 4.4 resize ポリシー

- bucket 数は常に `2^n`
- 最小 bucket 数は `4096`（`FT_TABLE_MIN_NB_BK`）
- `migrate()` は grow / shrink / same-size rehash のいずれも受け付ける
- 制約: 新 bucket 数は init 時の bucket 数以上であること
  （`new_nb_bk > start_mask`）
- init 時より小さい bucket 数への shrink は拒否される
- resize のタイミングと規模は caller が決める

### 4.5 現在の `migrate()` ロジック

現在の `migrate()` は、単純な逐次再挿入ではなく staged rebuild
pipeline として構成している。grow は memory latency に強く支配される
ためである。再構築では、少なくとも次の 3 種類の object に触れる必要がある。

- old bucket table
- stable な entry array
- new bucket table

そのため現行実装は、小さな ring と短い ahead を使って latency を重ねる。

1. old bucket table を bucket 順に走査する
2. old bucket では `idx[]` cache line だけを prefetch する
3. occupied slot に対応する entry line を prefetch する
4. entry に保存されている `hash0/hash1` を読み、新しい mask から
   `new_bk0/new_bk1` を導出する
5. eager prefetch するのは `new_bk0.hash[]` だけにする
6. 実際の insert は小さい staging ring に積んで遅延実行する
7. 再挿入は duplicate check を行わない専用 path で行う

### 4.6 設計上の含意

`ftable` の大半は意図的に `fcache` と似せてある。

- fixed-width flow key
- caller-defined stride/offset による intrusive record layout
- stable `entry_idx`
- scalar / bulk API

中核の設計課題は、ここまで述べた resize-safe hash 契約であり、migrate
path はその契約が性能上もっとも強く現れる箇所である。

## 5. allocator モデル

table は `malloc`、`realloc`、特定 allocator を仮定しない。
すべてのメモリは caller が供給する。

- **record array**: caller が確保・所有・解放する。table は stride と
  offset で index するだけ
- **bucket memory**: caller が raw buffer を確保し（alignment 不要）、
  `init()` または `migrate()` に渡す。ライブラリが内部で
  pure table では `ft_table_bucket_carve()`、slot-extra table では
  `ft_table_extra_bucket_carve()` により最大の power-of-2 aligned 領域を
  切り出す

helper 関数:

- `ft_table_bucket_size(max_entries)` — init 用の推奨 bucket 確保サイズ
  を計算する（最小 4096 buckets = 512 KiB）
- `ft_table_bucket_mem_size(nb_bk)` — bucket 数から bucket メモリサイズを
  計算する（grow/shrink 用: 2 倍や 1/2 に使う）
- `ft_table_extra_bucket_size(max_entries)` — slot-extra init 用の推奨
  bucket 確保サイズを計算する
- `ft_table_extra_bucket_mem_size(nb_bk)` — bucket 数から slot-extra bucket
  メモリサイズを計算する

この形は hugepage、NUMA-aware、custom control-plane allocator を想定して
いる。ライブラリ内部で `malloc` / `free` は一切呼ばない。

## 6. API 概要

公開ヘッダ:

- `flowtable/include/flow_table.h` — primary umbrella header
- `flowtable/include/ft_fill_ctrl.h` — optional fill-rate controller
- `flowtable/include/flowtable/*.h` — advanced family/common headers

通常の利用では `flow_table.h` だけを include する。これは pure API と
slot-extra API の両方を include する。slot-extra 実装は通常の
`flowtable/src/` build に含まれ、`libftable.a` へ link される。
分離された `extra/` source tree や include path は持たない。
`flowtable/*.h` は、`flowtable/flow4_table.h` や
`flowtable/flow6_extra_table.h` のように、意図的に family-specific な
低レベル API だけを参照したい code のために残している。
ここでいう pure は bucket-side `extra[]` slot を持たない base table model
である。

`flow_table.h` は、`struct ft_table` と `struct ft_table_extra` で共通な
操作を `FT_TABLE_*` generic facade として提供する。dispatch は table
pointer 型に対する compile-time `_Generic` で行う。

例:

```c
struct ft_table ft_c;
struct ft_table_extra ft_e;

FT_TABLE_INIT_TYPED(&ft_c, FT_TABLE_VARIANT_FLOW4,
                    records_c, max_entries,
                    struct my_flow4_record, entry,
                    buckets_c, bucket_size_c, &cfg_c);

FT_TABLE_INIT_TYPED(&ft_e, FT_TABLE_VARIANT_FLOW4,
                    records_e, max_entries,
                    struct my_flow4_extra_record, entry,
                    buckets_e, bucket_size_e, &cfg_e);

FT_TABLE_ADD_IDX(&ft_c, idx, now);
FT_TABLE_ADD_IDX(&ft_e, idx, now);

if (FT_TABLE_NB_ENTRIES(&ft_e) > high_watermark)
    FT_TABLE_ADD_IDX_BULK_MAINT(&ft_e, idxv, nb_idx,
                                FT_ADD_IGNORE_FORCE_EXPIRE,
                                now, timeout,
                                unused_idxv, max_unused, min_bk_used);

FT_TABLE_DESTROY(&ft_c);
FT_TABLE_DESTROY(&ft_e);
```

generic facade の対象は `INIT_TYPED`, `DESTROY`, `FLUSH`,
`NB_ENTRIES`, `NB_BK`, `STATS`, `STATUS`, `ADD_IDX`,
`ADD_IDX_BULK`, `ADD_IDX_BULK_MAINT`, `DEL_IDX`, `DEL_IDX_BULK`,
`WALK`, `MIGRATE`, `MAINT_CTX_INIT`, `MAINTAIN`,
`MAINTAIN_IDX_BULK` である。

maintenance 例:

```c
struct ft_maint_ctx ctx_c;
struct ft_maint_extra_ctx ctx_e;

FT_TABLE_MAINT_CTX_INIT(&ft_c, &ctx_c);
FT_TABLE_MAINT_CTX_INIT(&ft_e, &ctx_e);

n = FT_TABLE_MAINTAIN(&ctx_c, start_bk, now, timeout,
                      expired_idxv, max_expired, min_bk_used, &next_bk);
n = FT_TABLE_MAINTAIN(&ctx_e, start_bk, now, timeout,
                      expired_idxv, max_expired, min_bk_used, &next_bk);
```

例外:

- key-specific operation は family-specific API を使う:
  `ft_flow4_table_find()`, `ft_flow4_table_find_bulk()`,
  `ft_flow4_table_del_key_bulk()` と、対応する
  `flow4_extra_table_*`, `flow6_extra_table_*`, `flowu_extra_table_*`
  の find/delete API を使う。pure と slot-extra で key/entry 型が
  異なるためである。
- maintenance context struct は variant-specific のままである。
  pure は `struct ft_maint_ctx`、slot-extra は
  `struct ft_maint_extra_ctx` を使う。`FT_TABLE_MAINT_*` macro で関数名は
  隠蔽できるが、caller は正しい context 型を確保する。
- `ft_table_extra_touch()` / `ft_table_extra_touch_checked()` のような
  extra-only timestamp helper は extra-specific のままとする。
- bucket sizing は variant-specific である。pure は
  `ft_table_bucket_size()`、slot-extra は `ft_table_extra_bucket_size()` /
  `flow4_extra_table_bucket_size()` / `flow6_extra_table_bucket_size()` /
  `flowu_extra_table_bucket_size()` を使う。

private 実装 header は `flowtable/src/`、bench 専用 helper は
`flowtable/test/` に置く。

主要 API（`flow4` で示す。`flow6`、`flowu` も同一）:

- `ft_flow4_table_init()` — caller 供給の record array と bucket memory で初期化
- `ft_flow4_table_destroy()` — table struct をゼロクリア（メモリ解放はしない）
- `ft_flow4_table_flush()` — 全 entry を除去
- `ft_flow4_table_find()` / `ft_flow4_table_find_bulk()`
- `ft_flow4_table_add_idx()` / `ft_flow4_table_add_idx_bulk()`
- `ft_flow4_table_del_idx()` / `ft_flow4_table_del_idx_bulk()`
- `ft_flow4_table_del_key_bulk()`
- `ft_flow4_table_migrate()` — bucket resize（grow / shrink / rehash）
- `ft_flow4_table_maintain()` / `ft_flow4_table_maintain_idx_bulk()`
- `ft_flow4_table_walk()`
- `ft_arch_init()` — 起動時 1 回の CPU 検出と SIMD dispatch 選択

architecture 選択は `fcache` と同じ考え方で行う。

- `FT_ARCH_GEN`
- `FT_ARCH_SSE`
- `FT_ARCH_AVX2`
- `FT_ARCH_AVX512`
- `FT_ARCH_AUTO`

典型的には起動時に 1 回だけ:

```c
#include "flow_table.h"

ft_arch_init(FT_ARCH_AUTO);
```

想定している登録モデル:

- record は caller-owned の stable storage
- table はそれを `entry_idx` で index する
- 主 API は `add_idx()`
- `find()` は miss 時に `0`
- `add_idx()` は duplicate insert 時に既存 `entry_idx` を返す
- `add_idx()` は失敗時に `0`
- `del_idx()` / `del_key_bulk()` は miss 時に `0`

caller-owned record を登録する典型例:

```c
struct my_flow4_record {
    struct flow4_entry entry;
    unsigned char body[128];
};

u32 idx = 17;

records[idx - 1].entry.key = key;
ft_flow4_table_add_idx(&ft, idx, now);
```

## 7. intrusive layout

`init()` は caller-defined fixed-stride record を `stride` と
`entry_offset` で扱う。convenience macro `FT_FLOW4_TABLE_INIT_TYPED`
は struct type と member name からこれらを計算する。

例:

```c
struct my_flow4_record {
    unsigned char pad[64];
    struct flow4_entry entry;
    unsigned char body[128];
};

size_t bk_size = ft_table_bucket_size(max_entries);
void *bk_mem = mmap(NULL, bk_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

FT_FLOW4_TABLE_INIT_TYPED(&ft, records, max_entries,
                          struct my_flow4_record, entry,
                          bk_mem, bk_size, NULL);
```

この方式により、caller は record layout を自由に決めつつ、table は
以下を相互変換できる。

- `entry_idx -> record`
- `entry_idx -> embedded entry`
- `entry -> containing record`

`flush()` と `del()` が消すのは table metadata だけであり、caller-owned
record payload 自体は破壊しない。同じ record を再び `add_idx()` で登録できる。

helper API / macro:

- `ft_flow4_table_record_ptr()` / `ft_flow4_table_record_cptr()`
- `ft_flow4_table_entry_ptr()` / `ft_flow4_table_entry_cptr()`
- `FT_FLOW4_TABLE_RECORD_PTR_AS(...)`
- `FT_FLOW4_TABLE_RECORD_FROM_ENTRY(...)`
- `FT_FLOW4_TABLE_ENTRY_FROM_RECORD(...)`

## 8. レイアウト指針

`init()` は任意 layout を許すが、性能面では layout 品質が重要である。

推奨:

- `entry` は cache-line 境界に置く
- flow entry を先頭 member に置く hot record は
  `FT_TABLE_CACHE_LINE_SIZE` で align する
- record stride を大きくしすぎない
- body は必要以上に大きくしない
- public な `flow*_key`, `flow*_entry`, `flow*_extra_entry` の
  size/offset は固定 ABI として扱う。header 側で static assertion
  により検証する

確認できている性能リスク:

- body が大きいと cache/TLB 圧が増える
- embedded entry が misaligned だと lookup 性能が大きく悪化する
- 性能重視の用途では layout も API 契約の一部と考えるべきである

## 9. resize ポリシー

想定する運用は以下である。

- `50%`〜`60%` 程度の conservative fill を保つ
- `migrate()` を実行するかどうかは caller が stats と運用都合で決める
- grow サイズは `ft_table_bucket_mem_size(ft->nb_bk) * 2` で算出する

現行実装では incremental migration は持たない。resize は一括 rebuild である。

これは意図的な tradeoff である。

- semantics が単純
- lookup path を汚さない
- resize cost を払うタイミングを caller が決められる

shrink も可能: caller が小さい bucket 領域を確保して `migrate()` を呼ぶ。
制約は新 bucket 数が init 時の bucket 数以上であること。

## 10. 現在の test coverage

現行 test program は `flowtable/test/test_flow_table.c` である。

3 variant（flow4, flow6, flowu）すべてについて試験している項目:

- 基本 `add/find/del`
- duplicate `add` が既存 entry を返すこと
- delete miss の accounting
- intrusive record mapping
- `entry -> record` / `record -> entry` の往復 helper
- bulk `find/add/del`
- `add_idx_bulk` の duplicate ignore / update policy
- `walk()` の全走査と early stop
- resize に使う hash pair invariants
- `migrate()` 後も既存 entry が保たれること
- `migrate()` による bucket 倍増
- find/add での timestamp 更新
- permanent timestamp entry
- `maintain_idx_bulk` による expiry
- ランダム操作列による fuzz testing

slot-extra の test program は `flowtable/test/test_flow4_extra.c` であり、
`test-extra` / `test-extra-arch` から実行される。`flow4_extra`,
`flow6_extra`, `flowu_extra` について同じ密度で以下を評価する。

- add/find/delete と bulk find
- bucket-side timestamp 保存
- `find_touch` と明示 `ft_table_extra_touch()`
- stale/deleted idx に対する touch reject
- bucket sweep maintenance
- migrate 後に current bucket mask で touch できること
- maintenance facade を含む `FT_TABLE_*` generic macro

実行:

```sh
make -C flowtable/test test
```

### 10.1 API usage sample

`flowtable/test/usage_flowtable.c` は、機能 test や benchmark ではなく、
API の使い方を示すための読み物 sample code である。以下を単純な流れで
示している。

- primary API header として `flow_table.h` を include する
- caller-owned record と bucket memory を確保する
- `FT_TABLE_INIT_TYPED()` で pure / slot-extra table を初期化する
- key lookup は family-specific API を使う
- add/delete/stats/walk/migrate は `FT_TABLE_*` facade を使う
- `FT_TABLE_MAINT_CTX_INIT()` と `FT_TABLE_MAINTAIN()` で明示 maintenance
  を回す
- optional な `ft_fill_ctrl` controller を使う

build:

```sh
make -C flowtable/test sample
```

## 11. benchmark

benchmark program は `flowtable/test/bench_flow_table.c` であり、
生成される binary は `ft_bench` である。

```sh
make -C flowtable/test bench
```

測定方針と各 target がどこまで信用できるかは
[BENCHMARKING.md](BENCHMARKING.md) に記述している。

- `bench` / `bench-light`: `flow4` のみ、`q=1/8/32/256`、fill `60%`、
  `--arch auto`（runtime で一番有利な supported variant）
- `bench-dev`: 開発途中用の短い profile (`auto`, `flow4` と
  `flow4_extra`, pure `find_hit/add_idx`, `q=256`, fill `75%`,
  256K entries)。pure maintain は perf counter access が必要なため
  既定では skip する
- `bench-release`: 標準 release profile。pure と slot-extra family を
  `auto` arch, fill `75/95%`, query `32/256` で評価する。job は
  physical core に分散する。pure maintain は host perf counter 権限への
  依存を避けるため既定では skip する
- `bench-release-full`: `gen/sse/avx2/avx512`, query `1/32/256`,
  より多い repeat count と pure maintain を含む exhaustive release sweep
- `bench-full`: release 既定値を使う互換 target
- `bench-full-serial`: release profile を単一 pinned core で実行し、
  より静かな数値を取る
- `bench-extra`: `bench_flow4_vs_extra.c`。`flow4` pure と
  `flow4_extra` を同一条件で比較する microbench
  (75% active fill での insert/find/miss/touch/delete/maintain)
- `bench-extra-full`: `bench_flow_extra_table.c`。`flow4_extra`,
  `flow6_extra`, `flowu_extra` について datapath / maintain / grow、
  既定 fill `75/95%`、query size、CPU-supported arch variant を評価する
  full-family slot-extra sweep
- `bench-sweep`, `bench-zoned`, `bench-ctrl`: maintenance と
  fill-controller に焦点を置いた bench

網羅性の注意: `ft_bench` は pure の `flow4/flow6/flowu` と supported
arch variant を広く評価する full benchmark である。`bench-extra-full`
はそれに対応する slot-extra family の full sweep である。`bench-extra`
は `flow4` pure-vs-extra の focused comparison として残す。

### 11.1 benchmark mode

- **datapath**（既定）: cold bulk 操作（ラウンド間に cache flush）
- **grow**（`--grow`）: bucket-table-only `migrate()`（2x）+ mmap 確保
- **maintain**（`--maint`）: bucket sweep と index-based maintenance

オプション:

- `--arch gen|sse|avx2|avx512|auto`
- `--raw-repeat N`, `--keep-n N` — sampling 制御
- `--pin-core CPU` — core affinity
- `--no-hugepage` — 2 MiB hugepage を無効化（既定は有効）
- `--op OP` — 特定操作のみ実行
- `--query N` — バッチサイズ（既定 256）

`make bench` 既定値: `--pin-core 2 --raw-repeat 3 --keep-n 1`。
`make bench-dev` 既定値: `--raw-repeat 3 --keep-n 1`, 6 jobs。
`make bench-release` / `make bench-full` 既定値:
`--arch auto`, query `32/256`, `--raw-repeat 5 --keep-n 3`,
`BENCH_FULL_CORES=auto`。auto mode は `lscpu` から physical core ごとに
1 logical CPU を選ぶ。明示する場合は `BENCH_FULL_CORES=2,4,6,8` のように
指定する。per-arch の release evidence が必要な場合は
`make bench-release-full`、標準 release profile を単一 core で測る場合は
`make bench-full-serial` を使う (`BENCH_FULL_SERIAL_PIN` 既定値は 2)。

full sweep の `95%` は guardrail pressure data であり、通常運用性能ではない。

注記:

- cold datapath の `add_*` 計測では、測定直前に query entry 自体を warm
  する。入力 record は hot のままにしつつ、bucket と既存 table state は
  cold のまま測る。
- `add_idx_bulk()` は `--query < 4` では small scalar add path を使い、
  それ以上では pipelined bulk path を使う。

### 11.2 代表性能値

x86-64 単一コアで `--pin-core 2 --raw-repeat 3 --keep-n 1`、
hugepage 有効、1M entries、60% fill で計測。

**datapath（cold bulk、256 key バッチ）:**

| 操作           | flow4 (cy/key) | flow6 (cy/key) |
|----------------|---------------:|---------------:|
| find_hit       |          72.66 |          79.84 |
| find_miss      |          60.47 |          67.19 |
| add_idx        |          75.39 |          87.27 |
| add_ignore     |          90.62 |         101.33 |
| add_update     |          94.22 |         107.34 |
| del_idx        |          51.02 |          55.62 |
| del_key        |          71.88 |          87.97 |
| find_del_idx   |          92.73 |         111.72 |

**grow（migrate 2x）:**

| フェーズ       | flow4 (cy/live-entry) |
|----------------|----------------------:|
| alloc (mmap)   |                 68.42 |
| migrate        |                227.33 |
| total          |                295.75 |

**maintain:**

| モード              | flow4 (cy/entry) |  IPC | cache-hit |
|---------------------|------------------:|-----:|----------:|
| maint_expire_dense  |             84.50 | 1.09 |    70.18% |
| maint_nohit_dense   |             86.49 | 1.05 |    70.21% |
| maint_idx_expire    |             22.62 | 0.44 |    43.74% |
| maint_idx_filtered  |             27.50 | 0.32 |    65.70% |

注:

- datapath 操作は cold 計測（ラウンド間に cache flush）であり、
  現実的なパケット処理シナリオを反映する
- `del_idx` は entry への書き込みを行わない; 登録は bucket のみ
- grow path は主に memory-bound; staged prefetch pipeline で
  大部分の latency を隠蔽している
- `maint_idx_expire` が ~23 cy/entry であり、post-hit の
  局所 maintenance として効率的

## 12. ft_fill_ctrl — 適応型 fill-rate コントローラ

`flowtable/include/ft_fill_ctrl.h` は header-only の適応型コントローラである。
バッチごとに sweep 予算と expire タイムアウトを自動計算し、
fill を setpoint 付近に維持しながら DoS バースト時の急増を抑制する。

### 12.1 設計概要（二重ループ）

```
 Inner loop (毎バッチ): fill_delta EWMA → sweep_budget
   fill が setpoint を超えた分 + 上昇トレンドを予算に変換する。
   ceiling を超えた場合は budget_max を強制する。

 Outer loop (毎バッチ): miss-rate EWMA + fill zone → expire_tsc
   新規フロー到着率の増加（DoS）を miss-rate の上昇として検出し、
   expire_tsc を比例短縮することで fill の上限を抑える。
   さらに Yellow/Red では fill 圧だけでも timeout を短縮する。
```

### 12.2 初期化

```c
#include <ft_fill_ctrl.h>

struct ft_fill_ctrl ctrl;
ft_fill_ctrl_init(&ctrl,
    N,                        /* table 容量 */
    68,                       /* setpoint (%) */
    74,                       /* ceiling (%) */
    FT_MISS_X1024(3),         /* 通常 miss rate 3% */
    23ULL * tsc_hz,           /* t_normal: 通常 timeout (tsc ticks) */
    3ULL  * tsc_hz);          /* t_min: DoS 時の最短 timeout */
```

`t_normal` の目安:

```
t_normal = setpoint_entries / add_rate [flows/s] × tsc_hz
例) N=1M, setpoint=68%, add_rate=30000 flows/s:
    t_normal = 680000 / 30000 × 2e9 ≈ 22.7s × 2 GHz = 4.5e10 ticks
```

### 12.3 バッチループへの組み込み

```c
unsigned prev_added = 0, prev_hits = 0, prev_next_bk = 0;
for (;;) {
    unsigned budget, start_bk;
    u64      tmo;

    ft_fill_ctrl_compute(&ctrl,
        ft_flow4_table_nb_entries(&ft),  /* 現在の fill */
        prev_added, prev_hits,           /* 前バッチの結果 */
        prev_next_bk,
        &budget, &start_bk, &tmo);      /* 今バッチの制御パラメータ */

    /* add path + local reclaim around add buckets */
    unsigned n_unused = ft_flow4_table_add_idx_bulk_maint(
        &ft, entry_idxv, nb_add, FT_ADD_IGNORE,
        now_tsc, tmo, unused_idxv, max_unused, min_bk_used);

    for (unsigned i = 0; i < n_unused; i++)
        reclaim(unused_idxv[i]);

    /* hit path: application-specific find/touch work */
    prev_hits = do_hit_path(&ft, now_tsc, tmo);

    /* global sweep; prev_next_bk keeps the cursor across batches */
    if (budget > 0) {
        unsigned n_expired = ft_flow4_table_maintain(
            &ft, start_bk, now_tsc, tmo,
            expired_idxv, budget, min_bk_used, &prev_next_bk);

        for (unsigned i = 0; i < n_expired; i++)
            reclaim(expired_idxv[i]);
    }

    prev_added = nb_add;
}
```

### 12.4 パラメータ選択の指針

| 状況                       | 対処                                      |
|----------------------------|-------------------------------------------|
| steady-state fill が高い   | setpoint_pct を下げるか t_normal を短縮    |
| DoS 時に fill が上限を超える | t_min を短縮するか ceiling_pct を上げる    |
| sweep が追いつかない        | budget_max を増やす（既定 512）            |
| miss-rate 検出が遅い        | outer_shift を下げる（既定 4 → 3）        |

### 12.5 bench-ctrl — コントローラ動作検証ベンチ

`bench_fill_ctrl.c` は 3 フェーズのシミュレーションで制御動作を検証する。

```sh
make -C flowtable/test bench-ctrl          # auto (AVX2)
make -C flowtable/test bench-ctrl-gen      # GEN 最適化 binary
make -C flowtable/test bench-ctrl-sse      # SSE 最適化 binary
make -C flowtable/test bench-ctrl-avx2     # AVX2 最適化 binary
make -C flowtable/test bench-ctrl-avx512   # AVX512 最適化 binary (未対応 CPU では実行 skip)
make -C flowtable/test bench-ctrl-build-all
```

| フェーズ     | 内容                                              |
|-------------|---------------------------------------------------|
| Phase1:normal   | 定常: Q_add=64, Q_hit=2048, fill ≈ setpoint を維持 |
| Phase2:DoS      | バースト: Q_add=512, miss_rate=20% → tmo 短縮、fill 抑制 |
| Phase3:recovery | 復帰: Yellow/Red で timeout を短縮し Green 上限へ戻す |

AVX2 での代表値（N=1M, setpoint=68%, 1.996 GHz TSC）:

| フェーズ        | cy/pkt |
|----------------|-------:|
| Phase1:normal  |    84.9 |
| Phase2:DoS     |   111.4 |
| Phase3:recovery|    98.0 |

## 13. ファイル構成

```text
flowtable/
  README.md
  README_JP.md
  TEST_SPEC_JP.md
  Makefile
  include/
    flow_table.h
    ft_fill_ctrl.h        (適応型 fill-rate コントローラ)
    flowtable/
      flow4_table.h
      flow6_table.h
      flowu_table.h
      flow_extra_table.h
      flow4_extra_table.h
      flow6_extra_table.h
      flowu_extra_table.h
      flow_common.h
      flow_key.h
      flow_extra_common.h
      flow_extra_key.h
  src/
    flow4.c
    flow6.c
    flowu.c
    flow4_extra.c
    flow6_extra.c
    flowu_extra.c
    flow_core.h
    flow_core_extra.h
    flow_hash.h
    flow_hash_extra.h
    ft_dispatch.c
    ft_dispatch_extra.c
    ft_maintain.c
    ft_maintain_extra.c
    flow_dispatch.h
    flow_dispatch_extra.h
    flow_table_generate.h
    flow_table_generate_extra.h
  test/
    Makefile
    bench_flow_table.c
    bench_flow4_vs_extra.c
    bench_flow_extra_table.c
    bench_scope.h
    usage_flowtable.c      (API usage sample; test/bench ではない)
    test_flow_table.c
    test_flow4_extra.c
    bench_fill_ctrl.c     (ft_fill_ctrl 動作検証ベンチ)
```
