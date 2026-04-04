# フローキャッシュ

librix（`rix_hash.h`）を基盤とした本番品質のフローキャッシュ実装。
高性能パケット処理向けにパイプライン方式のバッチ検索で
DRAM レイテンシを隠蔽する。

## クイックスタート

```sh
# キャッシュライブラリをビルドし、test と bench-short を実行
make -C samples/fcache all

# 正当性テスト
make -C samples/fcache/test test

# 代表的な short ベンチ
make -C samples/fcache/test bench

# commit 前の full ベンチ
make -C samples/fcache/test bench-full

# top-level の補助ターゲット一覧
make help
```


## 1. 概要

`samples/fcache/` はこの repo の現行フローキャッシュ実装である。
librix のインデックスベースハッシュテーブル (`rix_hash.h`) を使う、
64 バイト 1 キャッシュライン設計。

### 利用シナリオ

- L3/L4 妥当性チェック直後に呼び出し
- パケットはベクタ処理（バッチあたり約256パケット）
- DRAMレイテンシを隠蔽するパイプライン方式
- スレッドごとのインスタンス（ロックフリー、同期不要）
- 呼び出し元が cache/buckets/pool/scratch を所有
- `fc_arch_init(FC_ARCH_...)` によるランタイム SIMD dispatch

### 性能目標と実測結果

| 操作 | 条件 | 目標 (cy/op) | 実測 (cy/op) |
|---|---|---|---|
| `find_bulk` | DRAM コールドバケット | ~100 | 40-143 |
| `findadd_bulk` | ミス + inline insert | ~60-120 | 59-470 |
| `maintain_step` | 定期 reclaim | ~20-70 | 19-67 |

現行の詳細なベンチテーブルと計測条件は §15/§16 を参照。

## 2. 3つのバリアント

フローキャッシュは 3 つのバリアントを提供し、生成実装と dispatch 層を共有する。

### 2.1 分離テーブル（IPv4専用またはIPv6専用）

| | `fc_flow4_cache` | `fc_flow6_cache` |
|---|---|---|
| ヘッダ | `flow4_cache.h` | `flow6_cache.h` |
| キー構造体 | `fc_flow4_key` (24B fixed-width) | `fc_flow6_key` (44B fixed-width) |
| エントリサイズ | 64B | 64B |
| 用途 | IPv4専用環境 | IPv6専用環境 |

分離テーブルの根拠:
- バリアントごとに固定キーサイズなので specialized hash/cmp を使える
- ホットパスにv4/v6分岐なし
- 均一な pool / bucket レイアウト

### 2.2 統合テーブル（デュアルスタック）

| | `fc_flowu_cache` |
|---|---|
| ヘッダ | `flowu_cache.h` |
| キー構造体 | `fc_flowu_key` (44B) |
| エントリサイズ | 64B |
| 用途 | デュアルスタック（IPv4 + IPv6を単一テーブルで） |

特性:
- `family` がキーの一部なので v4/v6 は alias しない
- `fc_flow4_key_make()`, `fc_flow6_key_make()`,
  `fc_flowu_key_v4()`, `fc_flowu_key_v6()` で zero-fixed key を構築
- pool 容量は IPv4/IPv6 で共有

### 2.3 性能比較

現行の datapath 計測は §15 にまとめている。概ね:

- `flow4` が steady-state hit path 最速
- `flow6` はキーが大きいぶんコスト高
- `flowu` は dual-stack の簡潔さと引き換えに一部の hit path で不利だが、
  miss-heavy path では十分競争力がある

### 2.4 バリアントの選択

- **IPv4専用** (`fc_flow4_cache`): 最小キーで IPv4 datapath 最速
- **IPv6専用** (`fc_flow6_cache`): IPv6限定セグメント
- **統合** (`fc_flowu_cache`): デュアルスタック。単一プールによりメモリ管理が
  簡素化され、v4/v6個別のキャパシティプランニングが不要。
  新規デプロイメントに推奨。

## 3. キー構造体

### 3.1 IPv4 フローキー（24B fixed-width object）

```c
struct flow4_key {
    uint8_t  family;   /* 常に 4 */
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;      /* 常に 0 */
    uint32_t vrfid;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t zero;     /* 常に 0 */
};
```

`vrfid` までの共通 prefix は `fc_flowu_key` にできるだけ寄せてある。
`family=4`, `pad=0`, `zero=0` を含む public key object は固定 24B なので、
flow4 のホットパスは 3 x 8-byte load の hash/compare を維持できる。
固定値を保つため、`fc_flow4_key_make()` を使う。

### 3.2 IPv6 フローキー（44バイト）

```c
struct flow6_key {
    uint8_t  family;   /* 常に 6 */
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;      /* 常に 0 */
    uint32_t vrfid;
    uint8_t  src_ip[16];
    uint8_t  dst_ip[16];
};
```

`family=6` と `pad=0` を固定に保つため、`fc_flow6_key_make()` を使う。

### 3.3 統合フローキー（44バイト）

上記 §2.2 を参照。

## 4. エントリレイアウト

3 つのバリアントはすべて 64 バイト、1 キャッシュラインのエントリを使う。

### 4.1 設計原則

- 1 エントリ = 1 キャッシュライン
- 右シフトした 32 bit timestamp と free-list metadata はキーと同じラインに置く
- 組み込みのユーザ payload は持たない
- エントリは 64 バイト境界にアラインする
- 呼び出し元の per-flow state はエントリ外に置き、`entry_idx` で参照する

### 4.2 エントリ設計（全て 64B / 1 CL）

```
flow4 entry (64B):
  fc_flow4_key    24B   family/proto/ports/pad/vrfid/src_ip/dst_ip/zero
  cur_hash          4B   O(1) remove 用 hash_field
  timestamp         4B   右シフトした 32 bit のアクセス時刻（0 = free）
  free_link         4B   SLIST entry（フリーリストインデックス）
  slot              2B   現在のバケット内スロット
  reserved1         2B
  reserved0        24B   64B へのパディング

flow6 entry (64B):
  fc_flow6_key    44B   family/proto/ports/pad/vrfid/src_ip[16]/dst_ip[16]
  cur_hash          4B
  timestamp         4B
  free_link         4B
  slot              2B
  reserved1         6B

flowu entry (64B):
  fc_flowu_key    44B   family/proto/ports/vrfid/addr union
  cur_hash          4B
  timestamp         4B
  free_link         4B
  slot              2B
  reserved1         6B
```

### 4.3 この配置の理由

- **キーと管理メタデータを 1 行に集約**:
  lookup、比較、タイムスタンプ更新、free-list 遷移が 1 キャッシュライン内で完結する
- **shifted timestamp も同じ行**:
  maintenance 側は追加の payload 参照なしに期限切れ判定できる。保存値は
  `now >> ts_shift` の 32 bit 符号化値で、`ts_shift` は init 時に指定し、
  未指定時は `FLOW_TIMESTAMP_DEFAULT_SHIFT` を使う
- **状態は外部配列へ**:
  payload を内包しないので、エントリを小さく保ちつつ移設もしやすい

### 4.4 flowu 統合キー

統合キーは `family` をキーの一部として持ち、IPv4/IPv6 が alias しない。

```c
struct flowu_key {
    uint8_t  family;
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;
    uint32_t vrfid;
    union {
        struct {
            uint32_t src;
            uint32_t dst;
            uint8_t  _pad[24];
        } v4;
        struct {
            uint8_t  src[16];
            uint8_t  dst[16];
        } v6;
    } addr;
} __attribute__((packed));
```

IPv4 側は未使用の 24 バイトをゼロ埋めし、`memcmp` ベースの比較を壊さない。
キー構築には `fc_flowu_key_v4()` / `fc_flowu_key_v6()` を使う。

### 4.5 公開 API パターン

各バリアントは同じ関数セットを公開する（`PREFIX` = `flow4` / `flow6` / `flowu`）。

| 関数 | 目的 |
|---|---|
| `fc_PREFIX_cache_init()` | バケット、プール、設定でキャッシュを初期化 |
| `fc_PREFIX_cache_flush()` | 全エントリをフリーリストに返却 |
| `fc_PREFIX_cache_nb_entries()` | 現在のエントリ数 |
| `fc_PREFIX_cache_stats()` | カウンタのスナップショット |
| **Bulk（ホットパス、パイプライン）** | |
| `fc_PREFIX_cache_find_bulk()` | パイプライン検索（ミス時挿入なし） |
| `fc_PREFIX_cache_findadd_bulk()` | パイプライン検索 + ミス時挿入 |
| `fc_PREFIX_cache_add_bulk()` | バッチ挿入（重複チェックなし） |
| `fc_PREFIX_cache_del_bulk()` | キー指定バッチ削除 |
| `fc_PREFIX_cache_del_idx_bulk()` | インデックス指定バッチ削除 |
| **Single-key（便利関数）** | |
| `fc_PREFIX_cache_find()` | 単一キー検索（`find_bulk` の `n=1` wrapper） |
| `fc_PREFIX_cache_findadd()` | 単一キー検索+挿入（`findadd_bulk` の `n=1` wrapper） |
| `fc_PREFIX_cache_add()` | 単一キー挿入（`add_bulk` の `n=1` wrapper） |
| `fc_PREFIX_cache_del()` | 単一キー削除 |
| `fc_PREFIX_cache_del_idx()` | インデックス指定単一削除 |
| **Maintenance** | |
| `fc_PREFIX_cache_maintain()` | 指定 bucket 範囲の reclaim |
| `fc_PREFIX_cache_maintain_step_ex()` | スキップ閾値付き部分スイープ |
| `fc_PREFIX_cache_maintain_step()` | throttle 付き単ステップ maintenance |
| **Query（コールドパス）** | |
| `fc_PREFIX_cache_walk()` | 全アクティブエントリをコールバックで走査 |

### 4.6 実装メモ

- ルックアップは `hash_key_2bk -> scan_bk_empties -> prefetch_node -> cmp_key_empties`
  の 4 段パイプライン
- `findadd_bulk` では miss を compare 段で inline insert する
- entry 自体に payload は持たず、返される `entry_idx` を side array のキーにする
- aging は `maintain` / `maintain_step_ex` / `maintain_step` で明示的に進める

## 5. ハッシュテーブル設定

- ハッシュバリアント: `rix_hash.h` フィンガープリント（任意キーサイズ、cmp_fn）
- `RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)`
- 型付き hash hook が必要なら:
  `RIX_HASH_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)`
- `hash_field`（`cur_hash`）によりO(1)除去が可能（再ハッシュ不要）
- キックアウトはXORトリック使用: `alt_bk = (fp ^ hash_field) & mask`
- バケット: 128バイト（2 CL）、16スロット/バケット
- ランタイムSIMDディスパッチ: Generic / SSE4.2 / AVX2 / AVX-512

### 5.1 テーブルサイジング

正確な丸め結果は `fc_PREFIX_cache_size_query()` を使う。
この helper は:

- `requested_entries` を内部プール粒度に切り上げる
- `rix_hash_nb_bk_hint()` から `nb_bk` を導く
- cache、bucket、pool、scratch の必要サイズをまとめて返す

エントリは 64 バイトなので、旧 128 バイト設計より総メモリフットプリントを抑えやすい。

### 5.2 bk[0] 配置率 vs 充填率

充填率75%以下では、98%以上のエントリがプライマリバケット（bk[0]）に
配置される。`scan_bk` はこれらのエントリに対して1 CLのみアクセスし、
DRAMアクセスを最小化する。

| 充填率 | bk[0] % | bk[1] % |
|---|---|---|
| 10-50 | 99.5-100 | 0-0.5 |
| 60 | 99.7 | 0.3 |
| 70 | 99.2 | 0.8 |
| 75 | 98+ | ~2 |
| 80 | 96 | 4 |
| 90 | 91 | 9 |

これが75%閾値が重要な理由である（§8参照）。実運用でも flow-cache は
`100%` 近傍を前提にすべきではなく、上限としては `90%` 以下、
データパス性能を重視する定常運用では `75%` 以下を維持するのが望ましい。

## 6. パイプライン設計

### 6.1 ルックアップパイプライン

`rix_hash.h` のステージドfindは4段構成:

```
Stage 0: hash_key_2bk       ハッシュ計算、bucket[0] と bucket[1] をプリフェッチ
Stage 1: scan_bk_empties    SIMDフィンガープリントスキャン + 空きスロット収集
Stage 2: prefetch_node      候補ノードの flow_entry CL0をプリフェッチ
Stage 3: cmp_key_empties    完全キー比較、miss時は即座にinline insert（rehashなし）
```

### 6.2 N-aheadソフトウェアパイプライン

`nb_pkts` パケットを `STEP_KEYS` 幅のステップで処理し、
各段を `AHEAD_KEYS` キーぶん先行させる:

```
STEP_KEYS   = 8    (ステップあたりの処理キー数)
AHEAD_STEPS = 4    (各段が何ステップ先行するか)
AHEAD_KEYS  = 32   (= STEP_KEYS * AHEAD_STEPS)
```

```c
for (i = 0; i < nb_pkts + 3 * AHEAD_KEYS; i += STEP_KEYS) {
    if (i                        < nb_pkts) hash_key_n      (i,              STEP_KEYS);
    if (i >= AHEAD_KEYS       && ...)       scan_bk_n       (i-AHEAD_KEYS,   STEP_KEYS);
    if (i >= 2*AHEAD_KEYS     && ...)       prefetch_node_n (i-2*AHEAD_KEYS, STEP_KEYS);
    if (i >= 3*AHEAD_KEYS     && ...)       cmp_key_n       (i-3*AHEAD_KEYS, STEP_KEYS);
}
```

`AHEAD_KEYS` は software pipeline の段間距離であり、
一度に 128 回 hardware prefetch を発行する意味ではない。

現行の 64 バイト設計では、hit 後に 2 本目の payload line を触ることはない。
呼び出し元は通常、返された `entry_idx` を使って side data を更新する。

### 6.3 タイムスタンプ

- TSC（`rdtsc`）はルックアップループ**前に1回だけ**読み取り
- ベクタ内の全パケットに同じ `now` 値を使用
- パケットごとのTSC読み取りなし
- 初期化時に50 msスリープで校正

## 7. ヒット / ミス処理

### 7.1 データパスの選択肢

現在のキャッシュには 2 つの主要 datapath がある。

- `find_bulk`: 検索のみ、ミス時の挿入なし
- `findadd_bulk`: 検索 + ミス時の inline insert

単一キー用 helper (`find`, `findadd`, `add`, `del`, `del_idx`) は bulk API を
`n=1` で呼ぶ薄い wrapper である。

### 7.2 処理フロー

```c
process_vector(pkts[256]):

  now = rdtsc()                                  // ベクタあたり1回のTSC読み取り
  extract_keys(keys, pkts, 256)

  fc_flow4_cache_findadd_bulk(&fc, keys, 256, now, results)
  // 挿入を別経路で扱う場合は fc_flow4_cache_find_bulk(&fc, ...) を使う

  for each pkt:
      if (results[i].entry_idx != 0) {
          // hit、または miss 後の挿入成功
      } else {
          // find_bulk の miss、または insert 系 API での full
      }

  fc_flow4_cache_maintain_step(&fc, now, idle)
```

ホットパスは `findadd_bulk`、定期エージングは `maintain` /
`maintain_step_ex` / `maintain_step` で扱う。

### 7.3 エントリに紐づく呼び出し元状態

現在の sample cache entry は key/hash/shifted-timestamp/free-list metadata のみを
保持する。

追加の per-flow state が必要な場合は、`entry_idx`（1-origin の pool index）
をキーに side array や外部構造体を持つのが基本パターンである。

## 8. エージングと回収

エントリの寿命管理は右シフトした 32 bit `timestamp` で行う。hit と挿入成功時に
`now >> ts_shift` が更新される。
期限切れエントリの削除は明示的で、maintenance API を使う。

- `maintain(start_bk, bucket_count, now)`: 明示した bucket 範囲を走査
- `maintain_step_ex(start_bk, bucket_count, skip_threshold, now)`: 低レベルの
  cursor 管理 sweep
- `maintain_step(now, idle)`: poll-loop 向けに throttle された helper

実効タイムアウトは adaptive で、充填率が上がると stale entry を早めに
回収するよう timeout window を短縮する。insert 圧力も局所的で、inline miss
insert の前に候補 bucket が密なら、その bucket から期限切れ entry を回収する。

回収後も free entry が無い場合、insert 系 API は `entry_idx = 0` を返し、
`fill_full` が増える。

運用指針:

- `100%` 充填を通常運用点としてはいけない。
- `90%` 以下は上限寄りの安全側目標であり、性能目標ではない。
- 定常データパス性能を重視するなら live fill は `75%` 以下に保つ。
- そのために `maintain` / `maintain_step_ex` / `maintain_step` を定期実行し、
  stale entry を高充填域に入る前に回収するのが intended use である。

### 8.1 ランタイム設定

ランタイム挙動は per-cache config で制御する。

```c
struct fc_flow4_config cfg = {
    .timeout_tsc = ...,
    .pressure_empty_slots = ...,
    .maint_interval_tsc = ...,
    .maint_base_bk = ...,
    .maint_fill_threshold = ...,
};
```

SIMD/backend 選択は per-cache parameter ではない。`fc_arch_init(FC_ARCH_...)`
でグローバルに選び、公開 API はその tier へランタイム dispatch する。

## 9. フリーリストとメモリ配置

- 呼び出し元が cache 本体、bucket 配列、entry pool、任意の bulk scratch を提供する
- `fc_PREFIX_cache_size_query()` が丸め後のフットプリントと配置を返す
- `fc_cache_size_bind()` が 1 つの連続メモリ領域を cache/buckets/pool/scratch に束縛する
- `fc_PREFIX_cache_init_attr()` がその packed layout から初期化する
- ライブラリ内部で heap は使わない

未使用 entry は SLIST フリーリストに保持される。flush、delete、maintenance で
解放された entry はフリーリストに戻り、insert 系 API はそこから取り出す。

## 10. API

3 つのバリアントは同じ API 形状を共有する（`flow4` / `flow6` / `flowu`）。

```c
int  fc_PREFIX_cache_size_query(unsigned nb_entries,
                                struct fc_cache_size_attr *attr);
int  fc_cache_size_bind(void *base, struct fc_cache_size_attr *attr);
int  fc_PREFIX_cache_init_attr(struct fc_cache_size_attr *attr,
                               const struct fc_PREFIX_config *cfg);

void fc_PREFIX_cache_init(struct fc_PREFIX_cache *fc,
                          struct rix_hash_bucket_s *buckets,
                          unsigned nb_bk,
                          struct fc_PREFIX_entry *pool,
                          unsigned max_entries,
                          const struct fc_PREFIX_config *cfg);
void     fc_PREFIX_cache_flush(struct fc_PREFIX_cache *fc);
unsigned fc_PREFIX_cache_nb_entries(const struct fc_PREFIX_cache *fc);
void     fc_PREFIX_cache_stats(const struct fc_PREFIX_cache *fc,
                               struct fc_PREFIX_stats *out);
int      fc_PREFIX_cache_walk(struct fc_PREFIX_cache *fc,
                              int (*cb)(uint32_t entry_idx, void *arg),
                              void *arg);

void     fc_PREFIX_cache_find_bulk(...);
void     fc_PREFIX_cache_findadd_bulk(...);
void     fc_PREFIX_cache_add_bulk(...);
void     fc_PREFIX_cache_del_bulk(...);
void     fc_PREFIX_cache_del_idx_bulk(...);

uint32_t fc_PREFIX_cache_find(...);
uint32_t fc_PREFIX_cache_findadd(...);
uint32_t fc_PREFIX_cache_add(...);
void     fc_PREFIX_cache_del(...);
int      fc_PREFIX_cache_del_idx(...);

unsigned fc_PREFIX_cache_maintain(...);
unsigned fc_PREFIX_cache_maintain_step_ex(...);
unsigned fc_PREFIX_cache_maintain_step(...);
```

`flowu` にはキー構築 helper もある:

```c
struct flowu_key fc_flowu_key_v4(...);
struct flowu_key fc_flowu_key_v6(...);
```

## 11. テンプレートとディスパッチ構成

現在の実装は 1 つの active template と 1 つの dispatch 層で構成される。

- `src/fc_cache_generate.h`: バリアント別の `static` 実装を生成
- `src/fc_ops.h`: SIMD tier ごとの ops table を定義
- `src/fc_dispatch.c`: サフィックスなし公開 API とランタイム tier 選択
- `src/flow4.c`, `src/flow6.c`, `src/flowu.c`: バリアント固有 hash/cmp を定義し、
  `FC_CACHE_GENERATE(...)` を instantiate

active build に separate legacy body template は存在しない。

### 11.1 新バリアントの追加

新しいバリアントの追加手順:

1. `flow4_cache.h` / `flow6_cache.h` / `flowu_cache.h` を手本に公開ヘッダを追加
2. key/result/entry/cache/config/stats 型と size-query helper を定義
3. `src/<variant>.c` にバリアント固有 hash/cmp を実装
4. `FC_CACHE_GENERATE(prefix, pressure, hash_fn, cmp_fn)` を instantiate
5. `FC_OPS_TABLE(prefix, FC_ARCH_SUFFIX)` を追加し、`fc_dispatch.c`、
   `fc_ops.h`、build/test ルールへ接続

## 12. ファイル構成

```
samples/
  README.md
  README_JP.md
  Makefile

  fcache/
    README.md
    README_JP.md
    Makefile
    include/
      fc_cache_common.h
      flow4_cache.h
      flow6_cache.h
      flowu_cache.h
    src/
      fc_cache_generate.h
      fc_dispatch.c
      fc_ops.h
      flow4.c
      flow6.c
      flowu.c
    lib/

  ftable/
    README.md
    README_JP.md
    Makefile
    include/
    src/
    test/

  test/
    Makefile
    test_flow_cache.c
    bench_fc.c
    bench_fc_body.h
    bench_fc_common.h
    run_fc_bench_all.sh
    run_fc_bench_matrix.sh
    bench_results/
```

## 13. ビルド依存関係

```
rix/rix_defs_private.h  インデックスマクロ、ユーティリティ（内部専用）
rix/rix_queue.h         SLIST（フリーリスト）
rix/rix_hash.h          フィンガープリントハッシュテーブル（SIMDディスパッチ）
```

TAILQ依存なし（LRUをタイムスタンプのみのアプローチに置換）。
SIMD高速化ハッシュ操作には `-mavx2` または `-mavx512f` が必要。
サンプル Makefile はデフォルトで `OPTLEVEL=3` を使い、`CC=gcc` と
`CC=clang` の両方でビルドできる前提とする。

## 14. スレッド安全性

フローキャッシュは **per-thread（スレッド専有）、lock-free** モデルで設計されている。

### モデル

| 項目 | 詳細 |
|---|---|
| インスタンス所有 | 1スレッドにつき1つの `fc_flow4_cache` / `fc_flow6_cache` / `fc_flowu_cache` |
| 同期 | なし — ロック・アトミック・RCU 不使用 |
| スレッド間共有状態 | なし |
| `maint_cursor` | キャッシュごと; 所有スレッドのみが更新 |

### 設計根拠

対象環境（VPPプラグイン、DPDKポールモード）では:

- 各ワーカースレッドは専用のパケットキューを持ち、排他的に処理する。
- フローキャッシュはスレッドローカル状態（フローごとのカウンタ、アクションキャッシュ）を保持する。
- スレッド間の共有がないため、フォルス・シェアリング・コンテンション・キャッシュライン競合が発生しない。
- VPPの「グローバル可変状態なし」アーキテクチャに適合する。

### ユーザーへの指針

- 1つのキャッシュインスタンスを複数スレッドで **共有しない**。
- 同一インスタンスに対して2スレッドから同時に **APIを呼び出さない**。
- 複数コアでトラフィックを処理する場合は、ワーカースレッドごとに1つのキャッシュを確保する。
- スレッドをまたいだフロー統計アクセスが必要な場合は、静止点（例: コントロールプレーンスレッドがquiescent期間後に読み取る）か、適切なメモリ順序付きのコピーで対応する。

### 共有メモリでの利用

librixデータ構造（ハッシュテーブル、フリーリスト）は **インデックス（ポインタなし）** を格納する。
これにより、異なる仮想アドレスに同じ共有メモリ領域をマップする複数プロセス間で
データを再配置できる。ただし、同一の flow cache インスタンスを複数プロセスが同時に変更する場合は、
外部での調停（シャードごとのロック・RCUなど）が必要になる。
前述のシングルライタモデルではこの要件を完全に排除できる。

## 15. 競合分析と総合評価

### 14.1 機能比較

| 特性 | **librix** | **DPDK rte_hash** | **OVS EMC** | **VPP bihash** | **nf_conntrack** |
|---|---|---|---|---|---|
| データ構造 | 16-way cuckoo, FP | 8-way cuckoo | direct-mapped | 4/8-way cuckoo | chained hash |
| キー格納 | ノード（FPバケット） | バケット内 | ノード内 | バケット内 | ノード内 |
| ルックアップ | 4段パイプライン+SIMD | パイプラインなし | 1CL直接参照 | パイプラインなし | チェーン走査 |
| 除去 | O(1) cur_hash | O(1) position | O(1) | O(n) 再ハッシュ | O(1) hlist |
| 共有メモリ | **ネイティブ対応** | 不可（ポインタ） | 不可 | 不可 | 不可 |
| エビクション | 適応的スキャン+強制 | なし（手動） | タイムスタンプ | なし | conntrack GC |
| SIMD | AVX2/512 FPスキャン | CRC32のみ | なし | なし | なし |
| バッチ処理 | ネイティブ対応 | bulk lookup あり | なし | なし | なし |
| スレッドモデル | per-thread, lock-free | RCU or lock | per-thread | per-thread | RCU + spinlock |

### 14.2 個別比較

#### vs DPDK rte_hash

rte_hash はデータプレーン向けハッシュテーブルの事実上の標準。8-way バケットで
高い充填率を達成するが、パイプライン化されたバッチルックアップは提供しない。
`rte_hash_lookup_bulk` はキーごとに独立してDRAMアクセスするため、コールド
キャッシュではレイテンシが積み上がる。librix の4段パイプラインは DRAMアクセスを
重畳するため、大規模プールで5-8倍の優位性がある。

ただし rte_hash はDPDKエコシステムとの統合（mempool, ring, EAL）が完成しており、
単体のハッシュ性能以外の運用面では成熟度が高い。

#### vs OVS EMC（Exact Match Cache）

EMC は direct-mapped（1-way）で、ヒット時は 1CL アクセスのみ。極めて高速だが
ミス率が高い（衝突時に必ずミス）。EMC は SMC（Signature Match Cache = cuckoo）の
前段キャッシュとして機能する2層構造。

librix は 16-way cuckoo 単層で、EMC の高速ヒットパスは持たないが、ミス率は
桁違いに低い。パケット処理全体のスループットでは librix が有利な場面が多い。

#### vs VPP bihash

bihash は VPP の標準ハッシュテーブル。除去に O(n) 再ハッシュが必要で、
エビクション不向き。フローキャッシュ用途では除去頻度が高いため、cur_hash
による O(1) 除去が決定的な優位性。bihash はルックアップもパイプライン化されて
おらず、大規模テーブルでの性能差は大きい。

#### vs nf_conntrack

Linuxカーネルのコネクショントラッカー。chained hash でスケーラビリティに
限界があり、GC は RCU + タイマーベース。データプレーン高速化を目的とした設計
ではなく、機能の豊富さ（NAT, helper, expectation）が強み。比較対象としては
異なる設計目標。

### 14.3 アーキテクチャ上の強み

**インデックスベース設計（最大の差別化要因）**

ポインタを一切格納しないため、共有メモリ・mmap・プロセス間共有がそのまま動作する。
これは他の高性能フローテーブル実装にはない特性。VPPプラグインや複数プロセスからの
参照が必要な環境では決定的な優位性がある。

**メモリアクセスパターンの最適化**

現行エントリは 64 バイト 1 ラインで、キー、`cur_hash`、32 bit `timestamp`、free-list
metadata を同じラインに集約している。これにより lookup、ヒット判定、
タイムスタンプ更新、maintenance の期限判定が最小限のキャッシュラインアクセスで
完結する。

**4段パイプラインの効果**

実測で 2.8x（L2）→ 8.1x（DRAM-cold）のスピードアップ。プールが大きくなるほど
効果が顕著で、DRAMレイテンシ隠蔽という本来の目的を達成している。40-143 cy/key は
DRAM-cold条件として優秀。

### 14.4 エビクション戦略の評価

**適応的タイムアウトの設計は堅実**

- 減衰/回復の平衡による定常状態収束は制御理論的に安定
- シフト演算のみで除算なし — データプレーンに適している
- 最小タイムアウト1.0秒の下限はキャッシュ効果を保証する妥当な選択

**3段フォールバックによる挿入保証**

キャッシュとして insert が失敗しないのは正しい設計判断。evict_bucket_oldest が
プールエントリとバケットスロットを同時解放するため、後続の ht_insert が必ず
ファストパスで完了する点は巧妙。

**evict_one の 1/8 バウンドに関する考察**

1/8 バウンドは最悪ケースを抑制するが、全エントリが存命で期限切れが見つからない
場合、毎回 1/8 スキャン → evict_bucket_oldest という高コストパスに入る。
攻撃トラフィック下でこのパスが連続すると、insert あたり数千サイクルのコストが
発生しうる。ただし、適応的タイムアウトが先に効いてタイムアウトを短縮するため、
実運用では evict_bucket_oldest に到達する頻度は極めて低いと考えられる。

### 14.5 改善の余地

  expire との比較ベンチマークがあると判断材料になる
- バッチ insert（ミスが複数ある場合のパイプライン化）は未実装。
  ミス率が高い場合の改善余地がある

**機能面**

- フローごとのコールバック（expiry notification）がない。エントリ除去時に
  カウンタの収集や統計の更新が必要な場合、現状では対応できない
- per-flow タイムアウト（例: TCP established vs SYN）は未対応。
  全エントリが同一タイムアウト

**運用面**

- ランタイムでのタイムアウト変更 API がない（init 時のみ）
- プールの動的リサイズは不可（事前割り当て固定）

### 14.6 総合判定

データプレーンのフローキャッシュとしては非常に完成度が高い設計。

1. **パイプライン + SIMD + CL配置最適化**の3つが一貫して設計されている
2. **適応的エビクション**により手動チューニング不要で安定動作する
3. **挿入保証**によりキャッシュとしての信頼性が確保されている
4. **インデックスベース**により共有メモリ展開が可能

150-215 cy/pkt（lookup + insert + expire 込み）は、2 GHz Xeon で 10 Mpps/core
に相当し、実用上十分な性能。テンプレートアーキテクチャによる3バリアント対応も、
コード重複なしで実現しており保守性が高い。

---

## 15. ベンチマーク結果

最新の再計測は 2026-03-24、`AMD Ryzen 9 8945HS`（Zen 4, AVX-512 対応）、
`cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` で実施。

- `./samples/fcache/test/fc_test`: PASS
- `taskset -c 2 ./samples/fcache/test/fc_bench datapath`: 自動選択 backend は `avx512`
- `taskset -c 2 ./samples/fcache/test/fc_bench maint_partial`: 自動選択 backend は `avx512`

以下の代表値は、既定の `make bench` 経路で使う
`datapath`, `maint_partial`, quick `findadd_window` matrix を基準にした。
query 幅は 256。

ベンチモードと目的:

- `datapath`: cold な reset-per-round microbench。各 timed round は fresh で
  uniq な key batch を使い、必要な resident set を作り直し、timer の外で
  cache-cold touch を行ってから、bulk API call だけを計測する。
  `findadd_hit`, `find_hit`, `findadd_miss`, `add_only`, `add+del`,
  `del_bulk`, reset-per-round の `90/10` mixed を比較する。
- `datapath` は raw な `fc_flowX_entry[]` 配列ではなく、typed な
  user-record layout を使って測る。これは実運用で caller-owned payload が
  embedded entry の隣に置かれる前提を反映するためであり、その配置は実際に
  datapath 値へ影響する。特に `flow4` は body が追加の cache line へ
  spill するかどうかに敏感である。
- `maint`: 全 entry 期限切れ前提の full-table sweep。fill 依存性や飽和時コストを見る補助 bench で、既定の representative path には含めない。
- `maint_partial`: fill 75%、全 entry 期限切れ条件で `maintain_step()` の部分 sweep コストを測る。
- `findadd_closed`: 固定集合ベンチ。固定 hit 集合と固定 miss 集合で `findadd_bulk()` を測り、miss 集合は round ごとに削除して fill を一定に保つ。
- `findadd_window`: 定常 open-set ベンチ。fresh miss を流し続けながら maintenance で fill を `60-75%` のような window に保つ。
- `trace_open_custom`: batch-maint policy を明示した open-set trace。長時間の fill drift、relief、maintenance 効果を観測する。

既定の bench 実行経路:

- `make all`: ライブラリをビルドし、test と `bench-short` を実行する。
- `make bench`: `bench-short` の alias。
- `make bench-short`: 代表 short セット。現在は `datapath` の `1M`
  比較だけを回し、最後の出力を短く比較しやすく保つ。
- `make bench-full`: full 長時間セット。full `datapath`, `maint_partial`,
  長時間 trace を含む full matrix を実行する。
- `./run_fc_bench_matrix.sh <variant> quick`: quick な windowed open-set matrix。
- `./run_fc_bench_matrix.sh <variant> full`: 長時間の trace case を含む full matrix。

`fc_bench` には sampling profile も 2 種類ある。

- `--bench full`: 指定した `--raw-repeat` / `--keep-n` を全 mode でそのまま使う。
  cold `datapath` の current 既定 internal round 数は
  `hit/find=96`, `miss/add/del=32`, `mixed=32`。
- `--bench short`: cold mode (`datapath`, `maint`, `maint_partial`) だけ sampling を下げる。
  `findadd_closed`, `findadd_open`, `findadd_window` のような hot mode は、
  user 指定の sampling をそのまま維持する。cold `datapath` の current
  既定 internal round 数は `hit/find=48`, `miss/add/del=16`, `mixed=16`。

current の `avx2` build で `1M` entries を測ると:

- `datapath` (`flow4`, `1M`, `query=256`): `short` は metric によって
  おおむね `1-9%` 程度の差に収まる。
- `maint` (`flow4`, `1M`): `short` は `full` に対しておおむね `2%` 以内。

したがって、`short` は cold path の quick screening に向き、cold bench の
比較基準としては `full` を使うのがよい。

実務上重要なのは、benchmark 値を user-record layout と切り離して読めない
ことだ。直近の `flow4`, `1M`, `avx2`, `--bench short`,
`--raw-repeat 1`, `--keep-n 1` では、bench を raw-entry harness から
typed user-record harness に切り替えたことで、次のように値が動いた。

- `findadd_hit`: `141.80 -> 130.23`
- `find_hit`: `147.19 -> 125.39`
- `findadd_miss`: `202.73 -> 192.19`
- `add_only`: `150.55 -> 130.08`
- `add+del`: `210.23 -> 199.30`
- `del_bulk`: `156.41 -> 147.27`
- `mixed_90_10_reset`: `156.02 -> 131.02`

つまり user data の配置は benchmark に無関係な detail ではなく、性能契約の
一部として扱うべきである。

修正後の cold `datapath` での current 基準値
(`flow4`, `1M`, `avx2`, `--bench full`) は次の通り。

- `findadd_hit`: `150.47 cy/key`
- `find_hit`: `143.59 cy/key`
- `findadd_miss`: `208.05 cy/key`
- `add_only`: `153.91 cy/key`
- `add+del`: `202.66 cy/key`
- `del_bulk`: `150.08 cy/key`
- `mixed_90_10_reset`: `151.48 cy/key`

### 15.1 データパス性能（cycles/key、expire なし）

#### 32K entries（`entries=32768`, `nb_bk=2048`）

| 操作 | flow4 | flow6 | flowu | 6/4 | u/4 |
|------|------:|------:|------:|----:|----:|
| findadd_hit | **25.78** | 37.50 | 39.53 | +45.5% | +53.3% |
| find_hit | **24.84** | 36.33 | 37.65 | +46.2% | +51.6% |
| findadd_miss | **52.27** | 61.72 | 61.25 | +18.1% | +17.2% |
| add_only | **37.03** | 46.41 | 46.88 | +25.3% | +26.6% |
| add+del | **68.98** | 89.53 | 90.94 | +29.8% | +31.8% |
| del_bulk | **31.56** | 42.97 | 44.22 | +36.1% | +40.1% |
| mixed 90/10 | **29.22** | 42.81 | 41.72 | +46.5% | +42.8% |

flow4 はインライン CRC32C（`__builtin_ia32_crc32di` x3）と XOR ベース 24B
キー比較を使用し、`rix_hash_arch->hash_bytes` の関数ポインタディスパッチと
`memcmp` を回避。flow6/flowu との 26〜42% の差は主に**関数ポインタ排除**に
よるもので、キーサイズの差ではない。

2.0 GHz 換算では、`flow4 findadd_hit=25.78 cy/key` は
`2.0e9 / 25.78 ~= 77.6 Mpps/core` に相当する。

#### 1024K entries（`entries=1048576`, `nb_bk=65536`）

| 操作 | flow4 | flow6 | flowu | 6/4 | u/4 |
|------|------:|------:|------:|----:|----:|
| findadd_hit | **29.53** | 41.56 | 40.62 | +40.7% | +37.6% |
| find_hit | **28.12** | 39.69 | 39.06 | +41.1% | +38.9% |
| findadd_miss | **174.84** | 213.12 | 216.41 | +21.9% | +23.8% |
| add_only | **289.61** | 301.09 | 307.73 | +4.0% | +6.3% |
| add+del | **330.39** | 353.44 | 358.36 | +7.0% | +8.5% |
| del_bulk | **41.41** | 51.72 | 51.88 | +24.9% | +25.3% |
| mixed 90/10 | **50.39** | 63.83 | 62.50 | +26.7% | +24.0% |

#### 4096K entries（`entries=4194304`, `nb_bk=262144`）

| 操作 | flow4 | flow6 | flowu | 6/4 | u/4 |
|------|------:|------:|------:|----:|----:|
| findadd_hit | **28.91** | 40.94 | 40.62 | +41.6% | +40.5% |
| find_hit | **27.81** | 38.91 | 38.91 | +39.9% | +39.9% |
| findadd_miss | **237.97** | 278.05 | 274.06 | +16.8% | +15.2% |
| add_only | **358.67** | 370.23 | 374.22 | +3.2% | +4.3% |
| add+del | **401.41** | 421.41 | 431.64 | +5.0% | +7.5% |
| del_bulk | **42.66** | 54.30 | 54.84 | +27.3% | +28.6% |
| mixed 90/10 | **57.89** | 70.62 | 60.47 | +22.0% | +4.5% |

3 つのテーブルサイズ全体で、定常 hit-path は引き続き `flow4` が最速。
テーブルが大きくなるほど miss-heavy path の差は縮み、メモリレイテンシ律速へ寄る。

### 15.2 メンテナンス：分割スイープ（fill=75%、全エントリ期限切れ）

| テーブル | step | flow4 cy/entry | flow6 cy/entry | flowu cy/entry |
|---------|-----:|------:|------:|------:|
| 0.6 MB | 64-1024 | 11.7-11.9 | 12.0-12.1 | 12.0-12.2 |
| 5.0 MB | 64-1024 | 15.8-16.2 | 15.7-16.4 | 15.3-15.6 |
| 72 MB | 64-1024 | 122.2-124.3 | 122.9-124.2 | 122.7-123.0 |

同一の residency 帯では、step サイズによる per-entry コスト差は小さい。
支配的なのはカーソルではなく、ノード配列が cache resident か DRAM resident かである。

- **L2 / LLC resident（0.6-5.0 MB）**: おおむね 12-16 cy/entry
- **DRAM resident（72 MB）**: おおむね 123 cy/entry

72 MB / step=64 の flow4 は約 `94.5K cy/call`、3 GHz 換算で約 `31.5 us`。
パケットバッチ間に償却する background maintenance としてはまだ実用的。

`maint` mode は full-table sweep や fill 依存性の調査用に残してあるが、
ここでは既定の `make bench` に一致する `maint_partial` を代表値として載せている。

### 15.3 総合

| 指標 | 値 | 備考 |
|------|-----|------|
| flow4 findadd hit | 26-29 cy | 2 GHz で約 69-78 Mpps |
| flow4 mixed 90/10 | 29-58 cy | 代表的な steady-state 範囲 |
| flow4 findadd miss | 52-238 cy | hash 計算 + insert 込み |
| partial maintenance (L2/LLC) | 12-16 cy/entry | バリアント差は小さい |
| partial maintenance (DRAM) | 123-124 cy/entry | メモリレイテンシ律速 |
| step サイズ影響 | 小さい | カーソルより residency が支配的 |

- **flow4** はデータパス最速 — インライン CRC32C + XOR 比較によるディスパッチ排除が効果的
- **flow6/flowu** はデータパスで約 15〜53% 劣るが、node array がメモリ律速になるとメンテナンス差はほぼ消える
- **メンテナンスは residency に強く依存**し、分割スイープはパケットバッチ間に素直に償却できる
- **大テーブルでは DRAM がボトルネック** — 全バリアントが収束

---

## 16. アーキテクチャ別ディスパッチ

### 16.1 設計概要

同一ソースを 4 つのアーキテクチャレベルでコンパイルし、実行時に関数ポインタ
テーブル（ops テーブル）で最適な実装を選択する。コンパイラが arch ごとに
命令選択・スケジューリング・auto-vectorize を最適化するため、アプリケーション
コードに手書き SIMD は不要。

```
                      fc_arch_init(FC_ARCH_AUTO)
                              │
                              ▼
┌─────────────────────────────────────────────────┐
│  fc_dispatch.o  (arch フラグなし)               │
│  - rix_hash_arch_init() を本 TU 用に呼び出し    │
│  - FC_OPS_SELECT() でバリアント毎に ops 選択    │
│  - サフィックスなし API ラッパー                │
│    hot-path  → _fc_flow4_active->findadd_bulk() │
│    cold-path → fc_flow4_ops_gen.init()          │
└────────────────────┬────────────────────────────┘
                     │ ops テーブル選択
      ┌──────────────┼──────────────┐
      ▼              ▼              ▼
 ops_gen        ops_avx2       ops_avx512 ...
 (flow4/6/u)   (flow4/6/u)   (flow4/6/u)
```

**設計ポイント**:

- 公開 API は**サフィックスなし**の関数名
  （`fc_flow4_cache_findadd_bulk` 等）。`fc_dispatch.c` の薄いラッパーが
  選択された ops テーブルに転送する。
- **ホットパス**（find/findadd/add/del bulk, maintain）はランタイム選択された ops ポインタ経由で
  ディスパッチ。**コールドパス**（init, flush, stats, remove_idx, nb_entries, walk）は
  `ops_gen` 経由で転送。
- 生成関数（`_FCG_API`）はすべて **static** — arch TU 内でのみ可視。
  ops テーブルがアドレスを取得し、TU 間ディスパッチに使う。
- 各 arch TU は `__attribute__((constructor))` で
  `rix_hash_arch_init(RIX_HASH_ARCH_AUTO)` を自動呼び出し。
  `rix_hash_hash_bytes_fast()` が正しい SIMD パスを使うことを保証。

### 16.2 アーキテクチャ階層

| 階層 | コンパイルフラグ | 主要機能 | 対象 |
|------|-----------------|---------|------|
| GEN | (なし) | スカラーのみ、CRC32C なし | ARM 移植性ベースライン |
| SSE4.2 | `-msse4.2` | CRC32C、128bit SIMD | 旧世代 x86_64 サーバ |
| AVX2 | `-mavx2 -msse4.2` | 256bit SIMD | 一般的な Xeon |
| AVX-512 | `-mavx512f -mavx2 -msse4.2` | 512bit SIMD | Xeon Scalable（主要ターゲット） |

GEN は将来の ARM 対応のための移植性ベースライン。x86_64 では SSE2 が ABI
保証されるが、SSE4.2 は保証外（2008 年以前の Core 2 には非搭載）。
実用上の Xeon ターゲットでは全て SSE4.2 以上が利用可能。
AVX-512 path は Xeon 想定に加えて、AMD Zen 4
（`Ryzen 9 8945HS`）実機でも動作確認済み。

### 16.3 ビルドマトリクス

各バリアントのソース（例: `flow4.c`）を `-DFC_ARCH_SUFFIX=_<tier>` と
対応する `-m` フラグで 4 回コンパイル。ディスパッチラッパーは arch フラグ
なしで 1 回。

```makefile
VARIANTS = flow4 flow6 flowu

# GEN（ポータブル、SIMD フラグなし）
$(LIBDIR)/%_gen.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -DFC_ARCH_SUFFIX=_gen -c -o $@ $<

# SSE4.2
$(LIBDIR)/%_sse.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -msse4.2 -DFC_ARCH_SUFFIX=_sse -c -o $@ $<

# AVX2
$(LIBDIR)/%_avx2.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -mavx2 -msse4.2 -DFC_ARCH_SUFFIX=_avx2 -c -o $@ $<

# AVX-512
$(LIBDIR)/%_avx512.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -mavx512f -mavx2 -msse4.2 -DFC_ARCH_SUFFIX=_avx512 -c -o $@ $<

# ディスパッチラッパー（arch フラグなし、FC_ARCH_SUFFIX なし）
$(LIBDIR)/fc_dispatch.o: $(SRCDIR)/fc_dispatch.c
	$(CC) $(CFLAGS) -c -o $@ $<
```

合計オブジェクト: 3 バリアント x 4 階層 + 1 ディスパッチ = **13 個**。

`FC_ARCH_SUFFIX` により `FC_CACHE_GENERATE` が生成する全関数名にサフィックスが
付加される（例: `fc_flow4_cache_findadd_bulk_avx2`）。マクロ未定義時は
元のサフィックスなし名が生成される。

### 16.4 Zen 4 における AVX2 と AVX-512 の実測

`avx512` tier 自体は存在し、`fc_arch_init(FC_ARCH_AVX512)` で
ランタイム選択できる。現行の `fcache` generator では、
`__AVX512F__` 有効時に bucket scan が専用の AVX-512 helper を使い、
`avx2` tier では AVX2 helper へフォールバックする。

つまり現状は次の通り:

- `avx512` tier: 実装あり、dispatch 可能
- ホットパスの bucket scan 幅: `avx512` tier では AVX-512、`avx2` tier では AVX2
- 含意: 強制 `avx512` で AVX-512 scan helper を直接計測できるが、
  結果は CPU に依存する

`AMD Ryzen 9 8945HS` 上で 2026-03-23 に次を実行:

```sh
./samples/fcache/test/fc_bench --arch avx2 datapath
./samples/fcache/test/fc_bench --arch avx512 datapath
```

代表値（cycles/key）:

| テーブル / 操作 | AVX2 | AVX-512 |
|---|---:|---:|
| 32K `flow4 findadd_hit` | 27.27 | 26.27 |
| 32K `flow4 findadd_miss` | 56.42 | 60.20 |
| 1024K `flow4 findadd_hit` | 28.46 | 26.73 |
| 1024K `flow4 findadd_miss` | 1331.24 | 908.07 |
| 32K `flow6 findadd_hit` | 38.36 | 38.99 |
| 1024K `flow6 findadd_hit` | 39.43 | 39.16 |

これらはこの Zen 4 実機での参考値であり、Xeon など他の AVX-512 CPU での
一律な優劣を示すものではない。この環境では、強制 `avx512` は改善するケースと
悪化するケースが混在した。

### 16.5 ハッシュ関数の方針

ハッシュ関数は**インライン・マクロ制御**のまま維持し、関数ポインタで
ディスパッチしない。パイプライン性能にはインライン展開が必須。

```c
static inline union rix_hash_hash_u
fc_flow4_hash_fn(const struct flow4_key *key, uint32_t mask)
{
#if defined(__x86_64__) && defined(__SSE4_2__)
    /* CRC32C 直接展開 — 24B キー = 3 x crc32q */
    ...
#else
    /* GEN フォールバック — rix_hash_hash_bytes_fast() */
    return rix_hash_hash_bytes_fast(key, sizeof(*key), mask);
#endif
}
```

`-msse4.2` 以上でコンパイルすると `__SSE4_2__` が定義され CRC32C が
使用される。GEN ビルドは乗算ハッシュにフォールバック。ソース変更不要 —
コンパイルフラグがコードパスを制御する。

SIMD find 操作（`_RIX_HASH_FIND_U32X16_2`）も同様で、
`fc_cache_generate.h` は `__AVX512F__` 定義時に AVX-512 helper を優先し、
`__AVX2__` 定義時に AVX2 helper へフォールバックし、それ以外では
既定の dispatch 経路を使う。

`rix_hash_hash_bytes_fast()`（flow6/flowu の GEN フォールバックで使用）は
各 arch TU の per-TU `rix_hash_arch` ポインタを経由する。
`FC_CACHE_GENERATE` が生成する `__attribute__((constructor))` により
各 TU の `rix_hash_arch` が自動初期化される。

### 16.6 ops テーブル構造

```c
/* src/fc_ops.h の FC_OPS_DEFINE(flow4) が生成（private ヘッダ） */
struct fc_flow4_ops {
    /* コールドパス */
    void (*init)(struct fc_flow4_cache *fc,
                 struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                 struct fc_flow4_entry *pool, unsigned max_entries,
                 const struct fc_flow4_config *cfg);
    void (*flush)(struct fc_flow4_cache *fc);
    unsigned (*nb_entries)(const struct fc_flow4_cache *fc);
    int (*remove_idx)(struct fc_flow4_cache *fc, uint32_t entry_idx);
    void (*stats)(const struct fc_flow4_cache *fc,
                  struct fc_flow4_stats *out);
    int (*walk)(struct fc_flow4_cache *fc,
                int (*cb)(uint32_t entry_idx, void *arg), void *arg);
    /* ホットパス */
    void (*find_bulk)(struct fc_flow4_cache *fc,
                      const struct flow4_key *keys,
                      unsigned nb_keys, uint64_t now,
                      struct fc_flow4_result *results);
    void (*findadd_bulk)(struct fc_flow4_cache *fc,
                         const struct flow4_key *keys,
                         unsigned nb_keys, uint64_t now,
                         struct fc_flow4_result *results);
    void (*add_bulk)(struct fc_flow4_cache *fc,
                     const struct flow4_key *keys,
                     unsigned nb_keys, uint64_t now,
                     struct fc_flow4_result *results);
    void (*del_bulk)(struct fc_flow4_cache *fc,
                     const struct flow4_key *keys,
                     unsigned nb_keys);
    void (*del_idx_bulk)(struct fc_flow4_cache *fc,
                         const uint32_t *idxs, unsigned nb_idxs);
    unsigned (*maintain)(struct fc_flow4_cache *fc,
                         unsigned start_bk, unsigned bucket_count,
                         uint64_t now);
    unsigned (*maintain_step_ex)(struct fc_flow4_cache *fc,
                                 unsigned start_bk, unsigned bucket_count,
                                 unsigned skip_threshold, uint64_t now);
    unsigned (*maintain_step)(struct fc_flow4_cache *fc,
                              uint64_t now, int idle);
};
```

バリアントごと・arch 階層ごとに 1 つの ops テーブルインスタンス（計 12 個）。
各 arch `.c` ファイル内の `FC_OPS_TABLE(prefix, FC_ARCH_SUFFIX)` で生成。
生成関数はすべて `static` — ops テーブルがアドレスを取得し TU 間ディスパッチに使う。

### 16.7 ランタイム選択

```c
/* fc_dispatch.c */
#include <rix/rix_hash_arch.h>
#include "fc_ops.h"

static const struct fc_flow4_ops *_fc_flow4_active = &fc_flow4_ops_gen;
static const struct fc_flow6_ops *_fc_flow6_active = &fc_flow6_ops_gen;
static const struct fc_flowu_ops *_fc_flowu_active = &fc_flowu_ops_gen;

void
fc_arch_init(unsigned arch_enable)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);   /* 本 TU 用 */
    FC_OPS_SELECT(flow4, arch_enable, &_fc_flow4_active);
    FC_OPS_SELECT(flow6, arch_enable, &_fc_flow6_active);
    FC_OPS_SELECT(flowu, arch_enable, &_fc_flowu_active);
}

/* ホットパスラッパー — ランタイム選択された ops 経由 */
void
fc_flow4_cache_findadd_bulk(struct fc_flow4_cache *fc,
                             const struct flow4_key *keys,
                             unsigned nb_keys, uint64_t now,
                             struct fc_flow4_result *results)
{
    _fc_flow4_active->findadd_bulk(fc, keys, nb_keys, now, results);
}

/* コールドパスラッパー — 常に ops_gen 経由 */
void
fc_flow4_cache_init(struct fc_flow4_cache *fc, ...)
{
    fc_flow4_cache_init_ex_gen(fc, ..., sizeof(struct fc_flow4_entry), 0u, ...);
}
```

`FC_OPS_SELECT` は `__builtin_cpu_supports()` で CPU を検出し、
最適な ops テーブルを選択（AVX-512 > AVX2 > SSE4.2 > GEN）。
ディスパッチコストはバッチあたり 1 回の間接呼び出し — 無視できる。

**呼び出し側** — 1 回の init で `rix_hash_arch_init()` と
バリアント別 ops 選択の両方を実行:

```c
int main(void) {
    fc_arch_init(FC_ARCH_AUTO);   /* これだけで全初期化完了 */
    /* ... fc_flow4_cache_init(), fc_flow4_cache_findadd_bulk() 等を使用 */
}
```

intrusive な layout では `fc_flow4_cache_init_ex()` に固定 stride の record
配列を渡せます。library が使うのは embedded `fc_flow4_entry` だけで、
record の残りは user payload のままです。record base / entry は
`fc_flow4_cache_record_ptr()` / `fc_flow4_cache_entry_ptr()` で取得できます。

性能上の注意:

- embedded entry は cache-line 境界に置く。local bench では misaligned entry
  が明確に悪化した
- user record が大きいほど callback 無効時でも cache/TLB 圧で遅くなる
- `flow4` では小さな user-body 配置の差でも datapath 値が実際に動く。
  bench は本番で使う record layout に合わせて取るべきである。
- `fc_flow4_cache_set_event_cb()` は alloc/free path で呼ばれるため、cold な
  payload や大きい payload に触る callback は miss-heavy workload で支配的に
  なり得る

### 16.8 `init_ex()` helper 名の意味

intrusive な `init_ex()` 経路では、`fc_cache_common.h` にある汎用
address helper を使う。

- `BYTE`: 対象を byte-addressable pointer として扱う
- `PTR`: mutable pointer を返す
- `CPTR`: const pointer を返す。意味は "const pointer"
- `RECORD`: caller-owned の fixed-stride record 1 件
- `MEMBER`: その record 内に埋め込まれた member

例:

- `FC_RECORD_PTR(base, stride, idx)`
  `idx` 番目 record の先頭 address を返す
- `FC_RECORD_MEMBER_PTR(base, stride, idx, entry_offset, type)`
  その record 内の embedded member を返す
- `fc_record_index_from_member_ptr(base, stride, entry_offset, member_ptr)`
  member pointer から 1-origin index を逆算する

helper を置く理由:

- `init_ex()` は通常の連続 `entry[]` ではなく
  `(base, stride, entry_offset)` を受け取る
- したがって実装は index, record 先頭, embedded entry/member の相互変換を
  繰り返し行う
- `base + (idx - 1) * stride + entry_offset` のような式を各所に直書きすると
  読みにくく、監査しにくく、誤りも入りやすい
- そのため byte-address 演算と alignment 契約を helper に集約している

### 16.9 移植性に関する注意

ランタイム検出は `__builtin_cpu_supports()` を使用（GCC ≥ 4.8、Clang ≥ 3.7）。
x86_64 では内部で `cpuid` を呼び出す。AArch64 では（GCC ≥ 14）
`getauxval(AT_HWCAP)` を読む。

**`__builtin_cpu_supports()` が利用できない・不十分な場合の代替方法：**

| 方法 | 移植性 | 備考 |
|------|--------|------|
| `getauxval(AT_HWCAP)` | Linux (glibc/musl) | `<sys/auxv.h>`、x86_64 と ARM の両方で動作 |
| `elf_aux_info()` | FreeBSD | getauxval の BSD 相当 |
| 直接 `cpuid` | x86 のみ | `<cpuid.h>`、OS 非依存 |
| `/proc/cpuinfo` | Linux のみ | 遅い（ファイル I/O + 文字列パース）、init ホットパス非推奨 |
| `IsProcessorFeaturePresent()` | Windows | Win32 API |

**新プラットフォームへの移植手順：**

1. 新しい階層定数を追加（例: `FC_ARCH_NEON`）
2. バリアントソースを適切な `-march` フラグと
   `-DFC_ARCH_SUFFIX=_neon` でコンパイル
3. `_FC_OPS_SELECT_BODY` にプラットフォームの機能検出 API を使った
   判定ロジックを追加
4. `src/fc_ops.h` に `FC_OPS_DECLARE(flow4, _neon)` 等を追加
5. パイプラインコードや公開 API の変更は不要

### 16.10 ファイル構成

| ファイル | 役割 |
|----------|------|
| `include/flow4_cache.h` | 公開 API: IPv4 flow cache の型 + 関数プロトタイプ |
| `include/flow6_cache.h` | 公開 API: IPv6 flow cache の型 + 関数プロトタイプ |
| `include/flowu_cache.h` | 公開 API: 統合 flow cache の型 + 関数プロトタイプ |
| `src/fc_cache_generate.h` | `FC_CACHE_GENERATE` — 全 `static` 関数をサフィックス付きで生成; per-TU `rix_hash_arch` constructor 出力; `FC_OPS_TABLE` で ops テーブルインスタンス生成 |
| `src/fc_ops.h` | Private: `FC_OPS_DEFINE` — ops 構造体定義; `FC_OPS_DECLARE` — extern 宣言; `FC_OPS_SELECT` — ランタイム CPU 検出 |
| `src/fc_dispatch.c` | サフィックスなし公開 API ラッパー; `fc_arch_init()` 実装 |
| `src/flow4.c`, `flow6.c`, `flowu.c` | バリアント固有ハッシュ/比較 + `FC_CACHE_GENERATE` + `FC_OPS_TABLE` |

### 16.9 テストスクリプト

ベンチマーク実行と結果のファイル保存（エビデンス用）：

```sh
./fc_test
./fc_bench datapath
./fc_bench maint_partial
./fc_bench flow4 findadd_closed 32768 75 90 200000
./fc_bench flow4 findadd_window 32768 60 75 90 500000 1000 1000
./run_fc_bench_matrix.sh flow4 quick
./run_fc_bench_matrix.sh flow4 full
./run_fc_bench_all.sh              # current build の結果を bench_results/ に保存
./run_fc_bench_all.sh -o /tmp/out  # 出力ディレクトリ指定
```

結果は `bench_results/<hostname>_<timestamp>.txt` に保存される。
`fc_test_avx2` / `fc_bench_avx2` のような tier 別バイナリが存在すればそれを実行し、
無ければ現在の single-build `fc_test` / `fc_bench` にフォールバックする。
