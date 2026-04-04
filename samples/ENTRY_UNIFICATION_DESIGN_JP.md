# entry 共通化設計ノート

この文書は、`samples/fcache/` の現行実装を出発点として、
将来の flow cache / flow table で entry 構造と entry 操作を共通化するための
設計変更メモである。

現行 `samples/ftable/` 実装は前提にしない。必要なら破棄し、新規設計とする。

## 1. 背景

現行 `fcache` は以下の性質を持つ。

- hot path は `find` / `findadd` / `findadd_burst32` であり、毎キーごとに
  entry metadata を直接読む
- `last_ts` 更新、free-list 操作、reclaim 判定が lookup path と強く結合している
- `init_ex()` により caller-owned record へ intrusive に埋め込める
- `record_ptr()` / `entry_ptr()` / `RECORD_FROM_ENTRY()` など、
  record と entry の相互変換 API はすでに持っている

一方で、将来は flow cache と flow table の record 管理を統一し、
entry metadata 操作を共通部品として使い回したい。

## 2. 目的

この設計変更の目的は以下とする。

- caller-owned record 上に 1 つの統一 entry 契約を定義する
- timestamp / free-list node / record-entry 相互変換などの entry 操作を共通化する
- single-key の `add` / `remove` / `find` を共通ライブラリとして切り出せるようにする
- `fcache` 固有の検索、findadd、timeout reclaim、pressure relief は個別実装に残す
- 将来の `ftable` 固有の探索、追加、拡張、寿命管理も個別実装に残す
- runtime callback を使わず、compile-time に展開される形で hot path 性能を維持する

## 3. 非目標

この段階では以下を行わない。

- 現行 `ftable` 実装の互換維持
- 既存 source code の即時変更
- `fcache` と `ftable` のアルゴリズム統合
- function pointer callback による抽象化

## 4. 現行 `fcache` から引き継ぐ制約

`fcache` の設計を崩さずに共通化するには、以下を守る必要がある。

### 4.1 hot metadata は inline 展開で触れること

`last_ts` の read/write、free-list next の read/write、entry から record への変換は、
`find` / `findadd` の hot path から毎回呼ばれる。
ここを function pointer callback にすると、

- 間接 call が増える
- inlining が消える
- alias 解析が弱くなる
- prefetch の位置決定が難しくなる

ため、採用しない。

### 4.2 free-list head は owner object 側に残す

free-list の head は現状どおり cache/table オブジェクト側に持つ前提とする。
共通化対象は各 record に埋め込まれた metadata であり、
head まで record 側へ逃がす必要はない。

この前提により、

- free-list head の管理責務は owner 側に残る
- 共通 entry 操作は `next` の load/store のみ知ればよい
- `fcache` の alloc/free fast path を壊さない

### 4.3 metadata の物理配置は user record 側責務とする

`last_ts` と free-list node は、論理的には `fcache` の private struct から外す。
ただし物理アドレス上でどこに置くかは caller の record layout に委ねる。

性能上の前提は以下とする。

- hot metadata は entry 本体に近い位置へ置くことを推奨する
- 遠い位置に置いた場合の性能劣化は caller 側責務とする
- `fcache` / `ftable` は correctness のみを保証し、性能配置は保証しない

## 5. 目標アーキテクチャ

将来構成は以下の 3 層に分ける。

### 5.1 共通 entry 契約層

caller-owned record が満たすべき共通 metadata 契約を定義する。
ここで統一する対象は、

- timestamp
- free-list next
- record-entry 相互変換
- metadata clear/init
- optional prefetch helper

である。

### 5.2 共通 entry core 層

共通 entry 契約の上に、single-key 操作を提供する小さな共通 core を置く。
ここで共通化する対象は以下とする。

- `init`
- `find`
- `add`
- `remove_by_key`
- `remove_by_idx`
- free-list alloc / free
- bucket link / unlink
- duplicate check の基本動作

この層は caller-owned stable record と intrusive metadata を前提とする。

### 5.3 `fcache` 固有層

`fcache` 側には以下を残す。

- hash bucket 構造
- key compare / hash / dispatch
- `find_bulk` / `findadd_bulk` / `findadd_burst32`
- timeout reclaim / pressure relief / maintain
- event callback / stats

つまり、共通化するのは metadata 操作であり、
lookup pipeline そのものではない。

### 5.4 将来 `ftable` 固有層

将来の `ftable` 側には以下を残す。

- table 探索方式
- add / remove / resize 方針
- stable entry の寿命モデル
- listing / traversal / ordering policy

`ftable` の詳細は現時点では未定であり、この文書では拘束しない。

## 6. 共通化単位

「entry 構造の統一」は、単一の完全同型 C struct を強制する意味ではなく、
共通 metadata 契約を持つ intrusive record を全実装が共有する、という意味で扱う。

最小単位としては以下を共通化対象とする。

| 項目 | 役割 | 備考 |
|---|---|---|
| `last_ts` | 最終アクセス時刻 | `fcache` では timeout / hit update に使用 |
| `free_next` | free-list 連結 | head は owner object 側 |
| `record <-> entry` 変換 | intrusive record 対応 | `init_ex()` 系の自然拡張 |
| `meta clear/init` | free 化、新規化 | sentinel 契約を統一する |
| `prefetch helper` | 次使用 record の先読み | compile-time 展開前提 |

## 7. 推奨インタフェース形態

runtime callback は使わず、macro か `static inline` で表現する。

方向性としては、現行 `fc_cache_generate.h` の `FCG_LAYOUT_*` /
`FCG_FREE_LIST_*` の拡張で扱うのが最小変更である。

たとえば以下のような customization point を想定する。

```c
#define FCX_RECORD_FROM_ENTRY(type, member, entry_ptr) ...
#define FCX_ENTRY_FROM_RECORD(record_ptr, member) ...

#define FCX_META_TS_LOAD(owner, entry) ...
#define FCX_META_TS_STORE(owner, entry, now) ...
#define FCX_META_TS_CLEAR(owner, entry) ...

#define FCX_META_FREE_NEXT_LOAD(owner, entry) ...
#define FCX_META_FREE_NEXT_STORE(owner, entry, idx) ...

#define FCX_META_PREFETCH_RECORD(owner, entry) ...
```

ここで重要なのは以下である。

- compile-time に展開されること
- member 名や offset が静的に確定すること
- owner object 側の head 管理と独立していること

## 8. API 変更の方向性

既存 `fcache` は `init()` / `init_ex()` / `FC_*_CACHE_INIT_TYPED()` を持つが、
将来は現在の `init_ex()` 契約を正式な `init()` に昇格させ、
旧 `init()` は廃止する方向とする。

つまり API は以下へ整理する。

- 本体 API は caller-owned record を受ける新 `init()`
- 利便性のための typed macro は残してよい
- 旧 `init()` 互換は維持しない

この方針により、record intrusive モデルを API 上の標準形とできる。

候補は 2 案ある。

### 案 A: 現行 `init_ex()` 契約を新 `init()` に改名して拡張する

- 現行の `array + stride + entry_offset` 契約を維持する
- entry 内 metadata の member 配置だけを traits macro で差し替える
- 既存利用者への影響が最小

### 案 B: 共通 record traits を別ヘッダで宣言させる

- caller が record type と member 名を 1 箇所で宣言する
- `fcache` / 将来 `ftable` は同じ traits 宣言を読む
- API は整理しやすいが、導入時の差分はやや大きい

現時点では案 A を第一候補とする。
`fcache` の現行 intrusive API と整合しやすく、移行段階を刻みやすいためである。

## 9. 共通 entry core API draft

先に定義すべきなのは、`fcache` / 将来 `ftable` の両方が依存する
小さな共通 entry core API である。

### 9.1 共通 core が扱う state

```c
struct fc_record_node {
    u32 entry_idx;
    RIX_SLIST_ENTRY(fc_record_node) next;
};

struct fc_record_allocator {
    RIX_SLIST_HEAD(fc_free_head, fc_record_node) free_head;
    unsigned free_count;
    unsigned capacity;
};

struct fc_record_layout {
    unsigned record_stride;
    unsigned entry_offset;
    unsigned last_ts_offset;
    unsigned free_next_offset;
};

struct fcore_state {
    struct rix_hash_bucket_s *buckets;
    unsigned nb_bk;
    struct flow_entry_hdr *records;
    unsigned nb_records;
    struct fc_record_layout layout;
    struct fc_record_allocator *alloc;
    unsigned user_area_size;
    u8 user_area[FCORE_USER_AREA_SIZE];
};

ここで `records` は `struct flow_entry_hdr *` 型にしておき、`flowX_entry` / `flowX_entry_hdr` の定義と一貫性を保ちながら offset で body 部分へアクセスする設計にする。
```

ここでの `allocator` は heap allocator ではなく、
caller-owned record array の free slot を index で管理する
`RIX_SLIST` ベースの free-index allocator である。`layout` はポインタではなく
構造体そのものを保持し、`fcore_state` が内容を所有する形で構築する。
`user_area` は allocator/free/prefetch state を inline で置けるように予約しておき、
将来的なカスタムルーチンがこの領域を reinterpret して使えるようにするためのもの。

allocator skeleton 例:

```c
struct fc_alloc_state {
    u32 head;
    u32 tail;
    unsigned count;
};

static struct fc_alloc_state *
alloc_state_from_user_area(struct fcore_state *state)
{
    return (struct fc_alloc_state *)state->user_area;
}
```

`alloc_idx`/`free_idx` はこの skeleton を使って head/tail を管理し、
prefetch を併せて `user_area` で静的に定義される必要な state を読み書きする。

### 9.2 共通 core の関数案

```c
int fcore_init(struct fcore_state *ec,
               struct rix_hash_bucket_s *buckets,
               unsigned nb_bk,
               void *records,
               unsigned nb_records,
               struct fc_record_layout layout,
               struct fc_record_allocator *alloc);

void fcore_find_bulk(struct fcore_state *ec,
                             const void *keys,
                             unsigned nb_keys,
                             u64 now,
                             u32 *entry_idxs);

void fcore_add_bulk(struct fcore_state *ec,
                            const void *keys,
                            const u32 *entry_idxs,
                            unsigned nb_keys,
                            u64 now,
                            u32 *out_entry_idxs);

void fcore_del_bulk(struct fcore_state *ec,
                            const void *keys,
                            const u32 *entry_idxs,
                            unsigned nb_keys,
                            u32 *removed);

void *fcore_record_ptr(struct fcore_state *ec,
                               u32 entry_idx);

void *fcore_state_ptr(struct fcore_state *ec,
                              u32 entry_idx);

u32 fcore_alloc_idx(struct fcore_state *ec);

void fcore_free_idx(struct fcore_state *ec,
                    u32 entry_idx);
```

### 9.2a 必要な hash/cmp/store trait

`fcore_*` は key の hash/compare/書き込みなど variant 固有の処理を取り込む必要がある。
最小構成は以下の trait macro/inline 関数と考える。

- `FC_FCORE_HASH_KEY(ec, key, ctx)` : key から bucket/entry に必要な情報（hash, fingerprint）を ctx に詰める
- `FC_FCORE_MATCH_ENTRY(ec, ctx, entry)` : ctx と entry を比較して一致するかを判定
- `FC_FCORE_STORE_KEY(ec, ctx, entry)` : miss 時に entry へ key/fingerprint を書き込む
- `FC_FCORE_UPDATE_TS(ec, entry, now)` : hit 時に timestamp を更新する（共通化済みでも macro で書き換え可）

これらは現在 `samples/fcache/src/fc_cache_generate.h` にある `FCG_HT(p, …)` macros を通じて `flow4/flow6/flowu` ごとに `#define` される形にすれば hot path 性能を維持できる。
将来的に public API 化する場合は `struct fc_fcore_traits` で trait 関数列をまとめて渡す選択肢もあるが、最初は internal layer 内で macro による compile-time disagreements として扱い、実装上問題が出た時点で拡張する。

これは概念 API であり、実際には variant 固有の hash/cmp traits を受ける必要がある。
ただし責務はここまでに限定する。

ここでの `find` / `add` / `del` は、bulk 操作の最小核として以下の契約を想定する。

- `find_bulk(keys, n)`:
  hit なら entry idx を返し、miss なら 0
- `add_bulk(keys, idxs, n)`:
  caller が与えた各 `idx` の record/entry を table に link する
- `del_bulk(keys, idxs, n)`:
  `idxs[i] != RIX_NIL` なら、その idx を前提に key 整合を確認して削除する
- `del_bulk(keys, idxs, n)`:
  `idxs[i] == RIX_NIL` なら key から find して削除する

この形にすると、

- `fcache`: 内部で `alloc_idx()` してから `add_bulk(..., idxs, n)` を呼ぶ
- 将来 `ftable`: user が slot を確保してから `add_bulk(..., idxs, n)` を呼ぶ

という使い分けができる。

oneshot API はこの共通 core には持たず、
各利用側が `nb_keys == 1` の wrapper として提供すればよい。

### 9.3 共通 core はまず internal common layer として扱う

この共通 core は、第一段階では public API ではなく
internal common layer として扱うのが妥当である。

理由は以下である。

- `fcache` と将来 `ftable` の内部共有だけで目的を達成できる
- traits 契約や不変条件を先に外部公開すると、設計自由度が落ちる
- hot path 寄りの実装都合を残したまま内部で育てられる
- 実利用が 2 実装以上で安定してから public 化を判断できる

したがって、初期方針は

- まず internal common layer として実装する
- `fcache` と新 `ftable` の両方で使ってみる
- その後、利用実績と需要が見えたら public API 化を検討する

とする。

### 9.4 共通 core の意味論

この API では、意味論を先に固定する。

- record は caller-owned stable storage
- entry は record に intrusive に埋め込まれる
- free record 管理は `RIX_SLIST` ベースの free-index list
- `find_bulk` は hit なら `entry_idx`、miss なら 0 を返す
- `add_bulk(keys, idxs, n)` は duplicate 非許容、各要素ごとに成功時 `idx`、
  失敗時 0 を返す
- `del_bulk(keys, idxs, n)` は各要素ごとに成功時 1、失敗時 0 を返す
- `del_bulk` では `idx == RIX_NIL` を find-and-delete として扱える
- free / invalid 判定は第1段階では `last_ts == 0`

### 9.5 共通 core に入れないもの

以下は最初から共通 core に含めない。

- oneshot API
- `findadd`
- `findadd_bulk`
- `findadd_burst32`
- timeout reclaim
- pressure relief
- maintain
- live entry listing policy
- resize policy

## 10. `fcache` は共通 core をどう使うか

`fcache` は共通 core の上に specialized datapath を積む。

### 10.1 `fcache` が共通 core から使うもの

- `init`
- `alloc_idx` / `free_idx`
- `record_ptr` / `entry_ptr`
- `find_bulk`
- `add_bulk(keys, idxs, n)`
- `del_bulk(keys, idxs, n)`
- bucket link / unlink 相当の内部操作

### 10.2 `fcache` 側に残すもの

- `find_bulk`
- `findadd_bulk`
- `findadd_burst32`
- `add_bulk`
- `del_bulk`
- `del_idx_bulk`
- timeout reclaim
- pressure relief
- maintain
- stats / event callback

`fcache` の bulk / pipeline 最適化は、
共通 core の薄い単機能 API を直接ループ展開して使うか、
あるいは同じ traits を使う別実装として持てばよい。

### 10.3 `fcache` の設計上の利点

この分割にすると、

- `fcache` の hot path を callback 化せずに保てる
- free-index `RIX_SLIST` 実装を使い回せる
- intrusive record モデルを崩さずに済む
- `findadd_burst32` のような optional fast path を個別に持てる

## 11. 将来 `ftable` は共通 core をどう使うか

将来の `ftable` は、現行 `ftable` 実装に縛られず、
同じ共通 core を別の ownership モデルで使えばよい。

### 11.1 `ftable` 側で共通化できるもの

- `init`
- `alloc_idx` / `free_idx`
- `find_bulk`
- `add_bulk(keys, idxs, n)`
- `del_bulk(keys, idxs, n)`
- record/entry 変換

### 11.2 `ftable` 側で user 主体にするもの

- いつ `alloc/free` を呼ぶか
- resize のタイミング
- listing / traversal policy
- stable entry の寿命管理

つまり、allocator 実装は共有できても、
「誰がどのタイミングでそれを使うか」は別にしてよい。

## 12. `fcache` API 具体案

この節では、共通 core を踏まえた上で、
`fcache` の公開 API をどの形へ寄せるかを具体化する。

### 12.1 新しい初期化 API

現行の `init_ex()` 契約を、新しい正式 `init()` とする。

概念シグネチャ:

```c
void fc_flow4_cache_init(struct fc_flow4_cache *fc,
                         struct rix_hash_bucket_s *buckets,
                         unsigned nb_bk,
                         void *records,
                         unsigned max_entries,
                         const struct fc_record_layout *layout,
                         struct fc_record_allocator *alloc,
                         const struct fc_flow4_config *cfg);
```

意図は以下である。

- `records + stride + entry_offset` は現行 `init_ex()` と同じ intrusive 契約
- `last_ts` と free-next は entry private field ではなく layout で示す
- `alloc` は free-index `RIX_SLIST` の state
- `fcache` が内部で alloc/free を呼ぶ

### 12.2 typed convenience macro

使い勝手のため、typed macro は残す。

概念例:

```c
#define FC_FLOW4_CACHE_INIT_TYPED(fc, buckets, nb_bk, array, max_entries,     \
                                  type, entry_member, last_ts_member,         \
                                  free_next_member, alloc_ptr, cfg)           \
    do {                                                                      \
        const struct fc_record_layout _layout = {                             \
            .record_stride    = sizeof(type),                                 \
            .entry_offset     = offsetof(type, entry_member),                 \
            .last_ts_offset   = offsetof(type, last_ts_member),               \
            .free_next_offset = offsetof(type, free_next_member),             \
        };                                                                    \
        fc_flow4_cache_init((fc), (buckets), (nb_bk), (array),                \
                            (max_entries), &_layout, (alloc_ptr), (cfg));     \
    } while (0)
```

### 12.3 allocator の受け渡し方

allocator の受け渡しは 2 形態を許してよい。

1. external allocator state を明示的に渡す
2. `struct fc_flowX_cache` 内の inline allocator state を使う

公開 API は前者を基本とし、後者は convenience macro で吸収するのが素直である。

### 12.4 single-key API

single-key API は共通 core と整合する形へ整理する。

残すもの:

- `fc_flow4_cache_find()`
- `fc_flow4_cache_add()`
- `fc_flow4_cache_del()`
- `fc_flow4_cache_del_idx()`

`fc_flow4_cache_findadd()` は `fcache` 固有 API として残すが、
共通 core には入れない。

内部実装としては、

- `fc_flow4_cache_find()` は `find_bulk(..., n=1)`
- `fc_flow4_cache_add()` は `alloc_idx()` + `add_bulk(..., n=1)`
- `fc_flow4_cache_del()` は `del_bulk(..., idx=RIX_NIL, n=1)`
- `fc_flow4_cache_del_idx()` は `del_bulk(..., idx, n=1)`

へ寄せる形が自然である。

### 12.5 batch API

batch API は 2 層に分ける。

共通寄り:

- `find_bulk`
- `add_bulk`
- `del_bulk`
- `del_idx_bulk`

`fcache` 固有:

- `findadd_bulk`
- `findadd_burst32`

現在の `fcache` の pipelined datapath は、この共通 bulk API を呼ぶ形で構築できる。
oneshot 用の `find`/`add`/`del` は `nb_keys == 1` の thin wrapper であり、
共有 layer との整合性が保たれる。

### 12.6 参照 API

record/entry 変換 API は維持する。

残すもの:

- `record_ptr(entry_idx)`
- `entry_ptr(entry_idx)`
- `record_stride()`
- `entry_offset()`

追加候補:

- `fc_flow4_cache_last_ts_ptr(fc, entry_idx)`
- `fc_flow4_cache_free_next_ptr(fc, entry_idx)`

### 12.7 削除・非推奨候補

削除する:

- 旧 `fc_flowX_cache_init()`
- 旧 `fc_flowX_cache_init_ex()`

残すが立場を明確化する:

- `FC_FLOWX_CACHE_INIT_TYPED()`
- `fc_flowX_cache_findadd()`
- `fc_flowX_cache_findadd_bulk()`
- `fc_flowX_cache_findadd_burst32()`

### 12.8 新旧対応表

| 現行 API | 変更案 |
|---|---|
| `fc_flowX_cache_init()` | 廃止 |
| `fc_flowX_cache_init_ex()` | 新 `fc_flowX_cache_init()` に改名 |
| `FC_FLOWX_CACHE_INIT_TYPED()` | 維持。ただし metadata member を追加指定 |
| `fc_flowX_cache_find()` | 維持 |
| `fc_flowX_cache_add()` | 維持 |
| `fc_flowX_cache_del()` | 維持 |
| `fc_flowX_cache_del_idx()` | 維持 |
| `fc_flowX_cache_findadd()` | 維持。`fcache` 固有 |
| `fc_flowX_cache_find_bulk()` | 維持 |
| `fc_flowX_cache_add_bulk()` | 維持 |
| `fc_flowX_cache_del_bulk()` | 維持 |
| `fc_flowX_cache_del_idx_bulk()` | 維持 |
| `fc_flowX_cache_findadd_bulk()` | 維持。`fcache` 固有 |
| `fc_flowX_cache_findadd_burst32()` | 維持。optional specialized path |

## 13. 共通ライブラリ化できる範囲

共通ライブラリ化の第一対象は single-key entry 操作である。

### 13.1 共通化しやすい操作

- `init`
- `find_bulk`
- `add_bulk(keys, idxs, n)`
- `del_bulk(keys, idxs, n)`
- free-list alloc / free
- metadata clear/init
- hash bucket への link / unlink

これらは「caller-owned stable record」と「intrusive metadata 契約」があれば
実装を共有しやすい。

### 13.2 共通化しにくい操作

- `findadd_bulk`
- `findadd_burst32`
- timeout reclaim
- pressure relief
- maintain
- listing / ordering policy
- resize policy

これらは datapath、寿命管理、探索方針、所有モデルに強く依存するため、
共通 core に入れない方がよい。

### 13.3 共通 core の意味論として先に固定すべき点

bulk core API を本当に共通化するには、最低でも以下の意味論を固定する必要がある。

- record は caller-owned stable storage か
- key は record 内に埋め込まれるか
- duplicate add は reject か replace か
- full 時は fail か reclaim か
- `add_bulk` は caller が idx を渡すか、内部 alloc を含むか
- `del_bulk` は key ベースか idx ベースか両方か
- free / invalid 判定は `last_ts == 0` か別 valid bit か

この段階では以下を暫定前提とする。

- record は caller-owned stable storage
- duplicate add は reject
- full 時は fail
- `add_bulk` は caller が idx 配列を渡す
- `del_bulk` は key / idx の両方を許し、`idx == RIX_NIL` で find-and-delete
- free / invalid 判定は `last_ts == 0`

## 14. record 形状の考え方

共通化対象は「record の mandatory metadata」であり、
`fcache` / `ftable` 固有 state は別領域に残してよい。

概念例:

```c
struct flow_record {
    struct flow_entry_common meta;
    struct flow_cache_state  fc;
    struct flow_table_state  ft;
    struct flow_payload      payload;
};
```

ここで重要なのは、

- `meta` は両者で共通に使う
- `fc` は `fcache` 専用
- `ft` は将来 `ftable` 専用

という分離である。

実装上、必ずしも上記の struct 分割そのものを採用する必要はない。
重要なのは、共通部と個別部の責務境界を固定することである。

`fcache` における `struct fc_flow4_entry` は、`flow4_entry` を含む entry metadata を包む intrusively embedded な部分で、`user_record` がその外側に body を足す構造だった。今後の共通化では `flowX_entry` 自体を metadata の代表とし、`user_record` はその entry に任意の body（cookie/timestamp/callback data）を重ねた構造体と見ると分かりやすい。つまり `fc_flow4_entry` をそのまま保持するのではなく、metadata は `flowX_entry` に、body は `user_record` に移して `flowX_entry` を共通化の中心に据える方向へ移行する。

## 15. 検討すべき論点

### 15.1 `last_ts == 0` を free sentinel に使い続けるか

現行 `fcache` は `last_ts == 0` を free / invalid 判定に使っている。
この契約を維持すれば移行は楽だが、

- `ftable` が別の valid bit を欲しがる可能性
- timestamp 未使用モードを設けたくなる可能性

がある。

第一段階では互換性を優先し、
`last_ts == 0` 維持を推奨する。

### 15.2 free-list next の表現

現行 `fcache` は intrusive singly-linked list を使う。
共通 metadata として next index を持つ設計は自然だが、
将来は

- index stack 方式
- bitmap + cursor 方式

も候補になり得る。

そのため、共通化対象は「next の load/store 契約」であり、
内部表現を完全固定しすぎない方がよい。

### 15.3 prefetch の責務

`findadd` の高速化には、次に使う予定の record を事前に prefetch できることが重要である。

ただし必要なのは runtime callback ではなく、

- `entry -> record`
- `free_head -> next candidate`

が compile-time に辿れることだと考える。

したがって prefetch は「共通 metadata API の副産物」であり、
独立 callback としては設計しない。

### 15.4 可読性と generator の境界

現行 generator はすでに複雑である。
全面的な code generator 拡張は避け、

- layout 差分
- metadata access 差分
- free-list 差分

だけを traits 化するのが望ましい。

アルゴリズム本体まで抽象化すると、`fcache` 側の可読性がさらに落ちる。

### 15.5 hash/cmp traits をどこで受けるか

ここでいう `hash/cmp traits` とは、
共通 core が key をどう扱うかを variant ごとに差し替えるための契約である。

必要になるのは典型的には以下である。

- key から 2 bucket を引く hash 処理
- entry と key の一致判定
- entry への key 書き込み
- 必要なら hash fingerprint の計算や保持

つまり、共通 core は

- free-index allocator
- record/entry 変換
- bucket link/unlink

だけでは完結せず、
「この key をどう hash し、どう compare するか」を
variant ごとに知らないと `find/add/remove` を実装できない。

ここで気にしているのは、
その差し替えを

- runtime callback にするか
- function table にするか
- macro / `static inline` traits にするか

という点である。

懸念は以下である。

- runtime callback や function pointer だと hot path で不利
- public API にすると traits 契約まで固定され、将来の変更が難しくなる
- internal layer なら compile-time traits で多少実装寄りでも許容しやすい

したがって第一段階では、

- traits は internal compile-time 契約として扱う
- public API にはしない
- `fcache` / 新 `ftable` の 2 実装で共通に使える最小集合に絞る

のがよい。

### 15.6 共通 `find/add/remove` の範囲を広げすぎないこと

共通ライブラリ化を進める際は、`findadd` や reclaim まで一度に共通化しない。

第一段階の共通化対象は、

- stable record
- intrusive metadata
- bulk `find/add/remove`

までに限定するのが安全である。

`fcache` の bulk/pipeline 最適化まで共通 core に押し込むと、
設計が再び `fcache` 専用になりやすい。

### 15.7 `findadd_burst32` は specialized API として扱う

`findadd_burst32` は `findadd_bulk` と意味論は同じだが、
`nb_keys <= 32` に特化した別パイプラインであり、
性能は workload により改善にも回帰にも振れる。

現時点の bench では、

- hit-heavy な fixed-stream 条件では、`q=1` や `q=32` で数 % 以上の改善が見える場合がある
- 一方で miss 比率を上げると、`q=32` でも改善が消えるか、逆に悪化する場合がある
- `q>32` では bulk fallback なので、差はほぼ測定ノイズ帯である

したがって `findadd_burst32` は、

- 共通 entry core に含める対象ではない
- `fcache` 固有の optional fast path として保持する
- default policy は bench に基づいて variant / workload ごとに決める

のが妥当である。

### 15.8 record allocator 実装共有と所有境界

ここでいう record allocator は heap allocator ではない。
caller-owned record array の未使用 slot を管理し、
free entry を index で払い出し、index で返却する
`RIX_SLIST` ベースの free-index allocator を指す。

したがって、この設計で前提とする allocator の役割は以下である。

- record array 全体は caller が所有する
- live/free の判定対象は record array 内の各 slot である
- allocator は未使用 slot の index を払い出す
- `fcache` / `ftable` はその index から entry/record を復元する

record allocator の呼び出し主体としては、

- `fcache` では cache 側が内部で `alloc/free` を呼ぶ
- 将来 `ftable` では user 側が `alloc/free` を呼ぶ

という呼び出し主体の違いがあり得る。

ただし、allocator の内部実装そのものは共通で構わない。
重要なのは、

- allocator implementation
- allocator ownership
- allocator caller

を分離して設計することである。

推奨方針は以下とする。

- 共通部として `record_pool` あるいは `record_allocator` 相当の state を定義する
- free-list head はこの allocator state に持たせる
- `fcache` はその allocator state を内部メンバとして持つか、明示的に参照する
- 将来 `ftable` は同じ allocator state を user が直接使えるようにする

この形なら、

- 実装は共通化できる
- `fcache` の hot path は callback なしで allocator state を直接触れる
- `ftable` は caller-driven allocation にできる

### 15.9 `HEAD` を init 時に裸 pointer で受けない

free-list の `HEAD` を `fcache` に置くか、外から受け取るか、という論点では、
`HEAD` 単体を init 時に裸 pointer で渡す設計は勧めない。

理由は以下である。

- `HEAD` 単体では allocator の不変条件が表現できない
- next 管理、capacity、count、entry 変換情報が別管理になりやすい
- 所有権と lifetime が曖昧になる
- hot path で使う状態が分散する

必要なら init で受けるのは `HEAD` ではなく、
allocator state 全体であるべきである。

### 15.10 `RIX_SLIST` の適用範囲

librix sample である以上、free-list には `RIX_SLIST` を積極的に使う前提でよい。
ここでいう free allocator は抽象 list ではなく、
実質的には `RIX_SLIST` による free-index list である。

`RIX_SLIST` は free-list には適している。

- push/pop が head 側だけで完結する
- `fcache` の insert fast path と相性が良い
- prefetch 先も head から辿りやすい

一方、live entry の listing にも `RIX_SLIST` を使うかは別問題である。

`RIX_SLIST` で問題ないのは、以下の条件を満たす場合である。

- head insertion でよい
- 任意ノード削除が頻繁でない
- unlink に O(n) か補助情報が必要でも許容できる

もし listing 側で、

- 任意削除が多い
- stable ordering が重要
- tail append を安くしたい

なら、`RIX_SLIST` ではなく別の list policy を owner 側で持つ方がよい。

### 15.11 cache object 内の inline 拡張領域

`struct fc_flowX_cache` の中に user-defined 領域を設け、
その領域へ free-list `HEAD` や allocator state を置く案は成立する。

この案の利点は以下である。

- cache object と allocator state の距離を現状同等に保ちやすい
- `fcache` 側は callback なしで近い位置の state を参照できる
- 将来 `ftable` でも同じ allocator 実装を inline 配置または外出し配置で再利用できる

ただし、「自由に使える生バイト列」をそのまま公開するのは勧めない。

問題は以下である。

- size と alignment の契約が API 上で見えにくい
- user ごとの異なる用途が入り、責務境界が曖昧になる
- accessor がマクロ頼みになり、型安全性が落ちる
- 将来 ABI を触るときに reserved bytes の意味づけが重くなる

したがって、採るなら

- arbitrary user area

ではなく、

- inline allocator state
- inline owner-private extension state

として型を持って定義する方がよい。

概念的には以下のような形を推奨する。

```c
struct fc_record_allocator {
    u32 free_head;
    u32 free_count;
    u32 capacity;
    /* free-index allocator local state */
};

struct fc_flowX_cache {
    ...
    struct fc_record_allocator alloc;
};
```

必要ならさらに、

- inline member として埋め込む
- 外部 state を pointer で参照する

の両方を許せばよい。

### 15.12 `HEAD` だけ user 設定可能にする案

user が init 時に `HEAD` だけ設定し、現状と同じ距離感を保つ、という考え方は
意図としては妥当である。

ただし実際には、`HEAD` 単体では不足する。
`fcache` が hot path で必要とするのは、

- `HEAD`
- next の表現
- index -> entry/record 変換前提
- capacity / empty 判定
- allocator 初期化済みかどうか

といった allocator state 一式だからである。

したがって API として受ける単位は、

- `HEAD` 単体

ではなく、

- allocator state

にすべきである。

## 16. 想定する移行段階

### Phase 0: 文書化

- 本文書で責務境界と用語を固定する

### Phase 1: 共通 traits 定義

- metadata access point を列挙する
- `fcache` の現行フィールド直接アクセスを traits 経由へ整理する
- 新 `init()` 契約を確定する

### Phase 2: 共通 entry core を切り出す

- single-key `find/add/remove` を共通 core 化する
- bucket link / unlink と alloc/free を共通部へ寄せる

### Phase 3: `fcache` を共通 traits / core 上へ移行

- 振る舞いを変えずに metadata access だけ差し替える
- bench / test で regressions を確認する

### Phase 4: 新しい `ftable` を別実装として追加

- 現行 `ftable` は参照しない
- 共通 traits と共通 record 契約の上に新規実装する

## 17. 暫定結論

現時点の結論は以下である。

- 現行 `ftable` 実装は設計前提から外す
- 現在の `init_ex()` 契約を正式な `init()` に昇格させ、旧 `init()` は廃止する
- `fcache` の hot path を守るため、runtime callback は採用しない
- free-list head は owner object 側に残す
- `last_ts` と free-list node は caller-owned record 側 metadata として扱う
- single-key の `find/add/remove` は共通ライブラリ化の対象にできる
- `findadd` / reclaim / bulk pipeline は `fcache` 固有に残す
- `findadd_burst32` は条件付きで有効な specialized API として扱い、
  共通 core には入れない
- 共通化は generator 全面再設計ではなく、layout / metadata / free-list access の
  traits 化で進める

この方針なら、`fcache` の性能要件と intrusive record モデルを保ったまま、
将来の flow table を新規に設計できる。
