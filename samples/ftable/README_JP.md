# Flow Table

`samples/ftable/` は librix 上に構築した flow-table ライブラリである。
`samples/fcache/` と近縁だが、目的は異なる。

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
make -C samples/ftable static

# 正当性テスト
make -C samples/ftable/test test

# 仕様表と test 対応表
sed -n '1,220p' samples/ftable/TEST_SPEC_JP.md

# benchmark
make -C samples/ftable/test bench

# top-level の補助ターゲット
make -C samples ftable
make -C samples ftable-test
make -C samples ftable-bench
```

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

- `hash0 != 0`
- `hash1 != 0`
- 固定 `start_mask` に対して
  `(hash0 & start_mask) != (hash1 & start_mask)`
- したがって、その後の任意の有効 table mask `mask = 2^n - 1` に対しても
  `(hash0 & mask) != (hash1 & mask)`

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
  `ft_table_bucket_carve()` により最大の power-of-2 aligned 領域を切り出す

helper 関数:

- `ft_table_bucket_size(max_entries)` — init 用の推奨 bucket 確保サイズ
  を計算する（最小 4096 buckets = 512 KiB）
- `ft_table_bucket_mem_size(nb_bk)` — bucket 数から bucket メモリサイズを
  計算する（grow/shrink 用: 2 倍や 1/2 に使う）

この形は hugepage、NUMA-aware、custom control-plane allocator を想定して
いる。ライブラリ内部で `malloc` / `free` は一切呼ばない。

## 6. API 概要

公開ヘッダ:

- `samples/ftable/include/flow_table.h` — umbrella header
- `samples/ftable/include/flow4_table.h`
- `samples/ftable/include/flow6_table.h`
- `samples/ftable/include/flowu_table.h`
- `samples/ftable/include/ft_table_common.h` — 共通型と helper

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
- record stride を大きくしすぎない
- body は必要以上に大きくしない

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

現行 test program は `samples/ftable/test/test_flow_table.c` である。

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

実行:

```sh
make -C samples/ftable/test test
```

## 11. benchmark

benchmark program は `samples/ftable/test/bench_flow_table.c` であり、
生成される binary は `ft_bench` である。

```sh
make -C samples/ftable/test bench
```

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

`make bench` 既定値: `--pin-core 2 --raw-repeat 11 --keep-n 7`

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

## 12. ファイル構成

```text
samples/ftable/
  README.md
  README_JP.md
  TEST_SPEC_JP.md
  Makefile
  include/
    flow_table.h
    flow4_table.h
    flow6_table.h
    flowu_table.h
    ft_table_common.h
  src/
    flow4.c
    flow6.c
    flowu.c
    ft_dispatch.c
    ft_maintain.c
    ft_ops.h
    ft_table_generate.h
  test/
    Makefile
    bench_flow_table.c
    test_flow_table.c
```
