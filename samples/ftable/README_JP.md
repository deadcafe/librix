# Flow Table

`samples/ftable/` は librix 上に構築した flow-table 試作である。
`samples/fcache/` と近縁だが、目的は異なる。

- `findadd` は持たない
- reclaim や timeout 起因の eviction を持たない
- caller-owned の record array を固定配置で使う
- `find`、`add`、`del` を明示的に分ける
- resize では bucket hash table だけを拡大する

現時点では意図的に scope を絞っている。

- `flow4` のみ
- ランタイム arch dispatch（`gen`, `sse4.2`, `avx2`, `avx512f`）
- bucket hash table は `2x` 拡大
- automatic shrink は未実装

## クイックスタート

```sh
# flow table library をビルド
make -C samples/ftable static

# 正当性テスト
make -C samples/ftable/test test

# 現在の benchmark 一式
make -C samples/ftable/test bench

# top-level の補助ターゲット
make -C samples ftable
make -C samples ftable-test
make -C samples ftable-bench
```

## 1. 設計目標

この実装の狙いは cache ではなく permanent-style flow table である。
entry は caller が delete するまで生存する前提で扱う。

そのため、重視するのは以下である。

- 安定した `entry_idx`
- 明示的な grow タイミング
- 暗黙 victim eviction を行わないこと
- bucket storage の allocator を caller が制御できること

これは reclaim と `findadd` を中心に設計された `fcache` とは異なる。

## 2. 現在の実装範囲

現時点の実装は以下を含む。

- `ft_flow4_table`
- scalar / bulk の `find`、`add`、`del`
- caller-defined record layout を扱う intrusive `init_ex()`
- サフィックスなし公開 API + runtime arch dispatch
- `need_grow`、`grow_2x`、`reserve`
- bucket-table-only resize

まだ未実装のもの:

- `flow6`
- `flowu`
- shrink
- background / incremental resize
- benchmark 本文と性能表

## 3. ストレージモデル

record array は caller が所有する。table は record を移動しない。

resize で変わるのは bucket hash table だけである。

これにより:

- `entry_idx` は `grow_2x()` 後も不変
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

この性質により、record を動かさず bucket hash table だけを拡大できる。

bucket table 拡大時は:

1. 新しい bucket array を確保
2. live entry ごとに保存済み `hash0/hash1` と新しい mask から
   `bk0/bk1` を再計算
3. 新 table へ再挿入
4. 旧 bucket array を解放

現行ポリシー:

- bucket 数は常に `2^n`
- 既定の最小 bucket 数は `16384`
- 既定の最大 bucket 数は `1048576`
- 既定 grow watermark は `60%`
- grow factor は `2x`
- shrink は行わない

### 4.4 現在の `grow_2x()` ロジック

現在の `grow_2x()` は、単純な

1. old entry を 1 件見る
2. new bucket pair を 1 件計算する
3. その場で insert する

という逐次処理にはしていない。grow は memory latency に強く支配される
ためである。再構築では、少なくとも次の 3 種類の object に触れる必要が
ある。

- old bucket table
- stable な entry array
- new bucket table

そのため現行実装は、小さな ring と短い ahead を使った staged rebuild
pipeline として構成している。

現在の grow path は概ね次の手順で動く。

1. old bucket table を bucket 順に走査する
2. old bucket では `idx[]` cache line だけを prefetch する
   grow 中に old table から必要なのは live entry index だけであり、
   old `hash[]` line は参照しない
3. occupied slot に対応する entry line を prefetch する
4. entry に保存されている `hash0/hash1` を読み、新しい mask から
   `new_bk0/new_bk1` を導出する
5. eager prefetch するのは `new_bk0.hash[]` だけにする
   `grow_2x()` 後の table は概ね半分の fill になるため、多くの insert は
   `bk0` だけで完結すると期待できる
6. 実際の insert は小さい staging ring に積んで遅延実行する
7. 再挿入は duplicate check を行わない専用 path で行う

rehash insert path は通常の `add()` と意図的に異なる。

- duplicate-key check を行わない
- old table を lookup path の一部として扱わない
- 必要なのは空き slot を見つけることと、必要なら kickout することだけ

これは、一意性が old table 側で既に保証されているからである。grow 中に
移動しているのは、新規 key ではなく「既知の live かつ unique な entry」
である。

また、次の 2 点は意図的な設計である。

- `bk1` は grow 中に eager prefetch しない
- new bucket の `idx[]` line も eager prefetch しない

これらは insert path が実際に必要になった時だけ lazily 触る。現在の grow
watermark は `60%`、拡大率は `2x` なので、rebuild 後の fill は概ね `30%`
である。したがって `bk1` を投機的に温めても、多くの場合は無駄になる。

現在の prefetch 深さは API 契約ではなく、保守的な default にすぎない。
machine ごとの tuning 余地はあるが、より重要なのは staging の構造そのもの
である。すなわち、

- old bucket `idx[]`
- entry
- new `bk0.hash[]`
- commit

の順で latency を重ねる設計である。

### 4.5 設計上の含意

`ftable` の大半は意図的に `fcache` と似せてある。

- fixed-width flow key
- intrusive `init_ex()` record layout
- stable `entry_idx`
- scalar / bulk API

新しい設計課題は周辺 API だけではない。中核は依然としてここまで述べた
resize-safe hash 契約であり、grow path はその契約が性能上もっとも強く
現れる箇所である。

## 5. allocator モデル

table は `malloc`、`realloc`、特定 allocator を仮定しない。
bucket memory は `struct ft_bucket_allocator` の callback で受け取る。

契約は単純である。

- `(size, align, arg)` で新 bucket storage を確保
- その領域へ再構築
- 旧 bucket storage を `(ptr, size, align, arg)` で解放

この形は hugepage や custom control-plane allocator を想定している。

## 6. API 概要

公開ヘッダ:

- `samples/ftable/include/flow_table.h`
- `samples/ftable/include/flow4_table.h`

主要 API:

- `ft_flow4_table_init()`
- `ft_flow4_table_init_ex()`
- `ft_flow4_table_destroy()`
- `ft_flow4_table_flush()`
- `ft_flow4_table_find()`
- `ft_flow4_table_add_idx()`
- `ft_flow4_table_add_entry()`
- `ft_flow4_table_add()`
- `ft_flow4_table_del()`
- `ft_flow4_table_find_bulk()`
- `ft_flow4_table_add_idx_bulk()`
- `ft_flow4_table_add_entry_bulk()`
- `ft_flow4_table_add_bulk()`
- `ft_flow4_table_del_bulk()`
- `ft_flow4_table_need_grow()`
- `ft_flow4_table_grow_2x()`
- `ft_flow4_table_reserve()`
- `ft_arch_init()`

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

想定している登録モデルは次の通り。

- record は caller-owned の stable storage
- table はそれを `entry_idx` で index する
- 主 API は `add_idx()`
- `add_entry()` は `add_idx()` の typed wrapper
- `add()` は内部 free-entry list を使う compatibility wrapper として残す

挙動は意図的に単純にしてある。

- `find()` は miss 時に `0`
- `add_idx()` / `add_entry()` は duplicate insert 時に既存
  `entry_idx` を返す
- `add_idx()` / `add_entry()` は失敗時に `0`
- `add()` は duplicate insert 時に既存 `entry_idx` を返す
- `add()` は allocation/full failure 時に `0`
- `del()` は miss 時に `0`

caller-owned record を登録する典型例:

```c
struct my_flow4_record {
    struct ft_flow4_entry entry;
    unsigned char body[128];
};

uint32_t idx = 17;

records[idx - 1].entry.key = key;
ft_flow4_table_add_idx(&ft, idx);
```

## 7. `init_ex()` による intrusive layout

`_ex` API は caller-defined fixed-stride record を扱える。

例:

```c
struct my_flow4_record {
    unsigned char pad[64];
    struct ft_flow4_entry entry;
    unsigned char body[128];
};

struct ft_flow4_config cfg = { 0 };

FT_FLOW4_TABLE_INIT_TYPED(&ft, records, max_entries,
                          struct my_flow4_record, entry, &cfg);
```

この方式により、caller は record layout を自由に決めつつ、table は
以下を相互変換できる。

- `entry_idx -> record`
- `entry_idx -> embedded entry`
- `entry -> containing record`

`init_ex()` 使用時でも、`flush()` と `del()` が消すのは table metadata だけ
であり、caller-owned record payload 自体は破壊しない。同じ record を
再び `add_idx()` / `add_entry()` で登録できる。

helper API / macro:

- `ft_flow4_table_record_ptr()`
- `ft_flow4_table_record_cptr()`
- `ft_flow4_table_entry_ptr()`
- `ft_flow4_table_entry_cptr()`
- `FT_FLOW4_TABLE_RECORD_PTR_AS(...)`
- `FT_FLOW4_TABLE_RECORD_FROM_ENTRY(...)`
- `FT_FLOW4_TABLE_ENTRY_FROM_RECORD(...)`

## 8. レイアウト指針

`init_ex()` は任意 layout を許すが、性能面では layout 品質が重要である。

推奨:

- `entry` は cache-line 境界に置く
- record stride を大きくしすぎない
- body は必要以上に大きくしない

確認できている性能リスク:

- body が大きいと cache/TLB 圧が増える
- embedded entry が misaligned だと lookup 性能が大きく悪化する
- 性能重視の用途では layout も API 契約の一部と考えるべきである

## 9. grow ポリシー

想定する運用は以下である。

- `50%`〜`60%` 程度の conservative fill を保つ
- watermark を超えたら `need_grow` を立てる
- 実際の `grow_2x()` は caller が都合のよいタイミングで実行する

現行試作では incremental migration は持たない。grow は一括 rebuild である。

これは意図的な tradeoff である。

- semantics が単純
- lookup path を汚さない
- resize cost を払うタイミングを caller が決められる

## 10. 現在の test coverage

現行 test program は `samples/ftable/test/test_flow_table.c` である。

試験している項目:

- 基本 `add/find/del`
- duplicate `add` が既存 entry を返すこと
- delete miss の accounting
- intrusive `init_ex()` による record mapping
- `entry -> record` / `record -> entry` の往復 helper
- bulk `find/add/del`
- `walk()` の全走査と early stop
- resize に使う hash pair invariants
- manual `grow_2x()` 後も既存 entry が保たれること
- 複数回 `grow_2x()` 後も `hash0/hash1` が不変なこと
- `grow_2x()` 失敗時に旧 table が壊れないこと
- `need_grow()` の動作
- `reserve()` による事前 grow
- 初期 bucket 確保での allocator failure
- max bucket 制約
- config の丸めと clamp

実行:

```sh
make -C samples/ftable/test test
```

## 11. 現在の benchmark coverage

現行 benchmark program は `samples/ftable/test/bench_flow_table.c` であり、
生成される binary は `ft_bench` である。

実行:

```sh
make -C samples/ftable/test bench
```

現行 mode:

- `./ft_bench datapath`
- `./ft_bench grow`

現行 datapath benchmark の計測項目:

- `find_hit`
- `find_miss`
- `add_only`
- `del_bulk`

現行の組み込み table size:

- `32768`
- `1048576`

現行 resize benchmark の計測項目:

- `grow_2x()` の live-entry あたり cycle

### 11.1 現在の grow 状態

現在の `grow_2x()` path には、すでにいくつかの構造改善が入っている。

- resize-safe な保存済み `hash0/hash1`
- scalar な逐次再挿入ではなく staged rebuild
- duplicate check を行わない rehash 専用 insert path
- old table 側 prefetch は `idx[]` line のみに限定
- new table 側 eager prefetch は `bk0.hash[]` のみに限定
- `bk1` の eager prefetch は行わない

現時点の代表値は次の通り。

- arch: `avx2`
- entries: `1048576`
- grow 前 fill: `60%`
- benchmark sampling: `raw_repeat=5`, `keep_n=3`, `pin-core=2`
- 観測された `grow_2x()`: おおむね `118 cy/live-entry`

同条件の `perf stat` の代表点は概ね次である。

- IPC: `0.91`
- cache miss rate: `13.55%`

したがって、grow path は依然として主に memory-bound だが、
不要な duplicate check や無駄な `bk1` prefetch に支配される状態では
なくなっている。

現在の grow 既定値は保守的に次としている。

- `FT_FLOW4_GROW_OLD_BK_AHEAD = 2`
- `FT_FLOW4_GROW_REINSERT_AHEAD = 8`

これらの深さは experiment である程度 tuning したが、full rebuild を含む
検証では run-to-run noise が残り、より深い設定が安定して勝つとは言えな
かった。現時点の結論は以下である。

- ahead 値の微調整より staging 構造のほうが重要
- `old idx[]`, `entry`, `new bk0.hash[]` という line 選択のほうが
  投機的な `bk1` traffic より重要
- さらなる改善余地はあるが、今後は code complexity の cost が高くなる

現行条件:

- runtime arch dispatch は `ft_arch_init()` を使う
- `--arch gen|sse|avx2|avx512|auto` を指定可能
- `--raw-repeat N`, `--keep-n N`, `--pin-core CPU` を指定可能
- `make bench` 既定値は `--pin-core 2 --raw-repeat 11 --keep-n 7`
- `raw_repeat` 個の生計測から最も幅の狭い `keep_n` 個を採用して報告する
- datapath prefill は conservative な `40%` live-entry を使う
- grow benchmark は stable record storage のまま bucket-table-only rebuild を測る

現状の benchmark は intentionally narrow である。まずは `flow4` table の
基本 datapath と grow path を確認することを目的としている。

## 12. 性能メモ

現時点では意図的に性能表は本文へ載せていない。

今はまず以下を固める段階である。

- API 形状
- grow semantics
- intrusive layout support
- allocator 契約

benchmark binary 自体は追加済みだが、公開する性能本文は以下が揃ってからにする。

- benchmark 項目がさらに広がること
- 複数 CPU 世代で結果を確認すること
- 結果が代表値として十分安定すること

## 13. ファイル構成

```text
samples/ftable/
  README.md
  README_JP.md
  Makefile
  include/
    flow_table.h
    flow4_table.h
    ft_table_common.h
  src/
    flow4.c
  test/
    Makefile
    bench_flow_table.c
    test_flow_table.c
```
