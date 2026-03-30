# librix -- Relative Index Library

共有メモリ向けインデックスベースデータ構造ライブラリ。

librix は、BSD 標準データ構造 (SLIST, LIST, STAILQ, TAILQ, CIRCLEQ,
Red-Black ツリー) と高性能カッコーハッシュテーブル 3 種を、生ポインタを
一切埋め込まない**相対インデックス**で実装します。共有メモリや mmap
領域にそのまま格納できます。

## 目次

- なぜインデックスか?
- 設計
- リポジトリ構成
- クイックスタート
- 共通ヘルパー
- キュー構造体
  - RIX_SLIST
  - RIX_LIST
  - RIX_STAILQ
  - RIX_TAILQ
  - RIX_CIRCLEQ
- Red-Black ツリー -- RIX_RB
- カッコーハッシュテーブル
  - バリアント比較
  - RIX_HASH (フィンガープリント、可変長キー)
  - RIX_HASH32 (uint32_t キー)
  - RIX_HASH64 (uint64_t キー)
- サンプル
- ビルド
- 並行性
- テスト
- ライセンス

---

## なぜインデックスか?

生ポインタはプロセスローカルであり再配置不可能です。代わりに**符号なし
インデックス**を格納することで:

- プロセスをまたいで、またリマップ後も構造体を再配置できる。
- 共有メモリやファイルバック領域に自然に収まる。
- ゼロ初期化でヘッド/ノードが自動的に空状態になる。
- 同一領域をマップする 32/64 ビット混在プロセス間でポインタサイズの
  不一致を回避できる。

---

## 設計

| 概念 | 詳細 |
|------|------|
| NIL 番兵 | `RIX_NIL = 0` -- ゼロ値が「要素なし」を意味する |
| 1-origin マッピング | 有効インデックスは `1 ... UINT_MAX-1`; `pool[i]` <-> インデックス `i+1` |
| ポインタを格納しない | ヘッドとリンクフィールドはインデックスのみ |
| 一時ポインタ | 変換はマクロ呼び出し時に `base` (要素配列) を渡すことで行う |
| 規格 | C11; 外部依存なし; サブシステムごとに単一ヘッダ |

インデックス <-> ポインタ変換マクロ:

```c
RIX_IDX_FROM_PTR(base, p)   /* (p - base) + 1  (NULL -> RIX_NIL) */
RIX_PTR_FROM_IDX(base, i)   /* base + (i-1)    (0 -> NULL)        */
```

---

## リポジトリ構成

```
include/
  librix.h          傘ヘッダ (全サブシステムをインクルード)
  rix/
    rix_defs_private.h  共通マクロ、インデックスヘルパー  (内部用; 自動インクルード)
    rix_hash_arch.h     アーキテクチャ依存ディスパッチ・SIMD ヘルパー (内部用; 自動インクルード)
    rix_queue.h     SLIST / LIST / STAILQ / TAILQ / CIRCLEQ
    rix_tree.h      Red-Black ツリー
    rix_hash_common.h  カッコーハッシュ -- 共通型・ヘルパー (内部用; 自動インクルード)
    rix_hash_fp.h      カッコーハッシュ -- フィンガープリント版
    rix_hash_slot.h    カッコーハッシュ -- スロット版 (hash_field + slot_field)
    rix_hash_keyonly.h カッコーハッシュ -- キーオンリー版 (補助フィールドなし)
    rix_hash.h      カッコーハッシュ傘ヘッダ (fp, slot, keyonly, hash32, hash64 を全てインクルード)
    rix_hash32.h    カッコーハッシュ -- uint32_t キー版
    rix_hash64.h    カッコーハッシュ -- uint64_t キー版
    rix_hash_key.h  カッコーハッシュ -- uint32_t / uint64_t キー版 (統合)
samples/              フローキャッシュサンプル (samples/README.md 参照)
```

---

## クイックスタート

```c
#include "librix.h"      /* キュー + ツリー + 全ハッシュバリアント */
```

必要なものだけインクルードすることも可能:

```c
#include "rix/rix_queue.h"   /* キュー構造体のみ                      */
#include "rix/rix_tree.h"    /* Red-Black ツリーのみ                  */
#include "rix/rix_hash.h"    /* カッコーハッシュ (fp 版)              */
#include "rix/rix_hash32.h"  /* カッコーハッシュ (u32 キー版)         */
#include "rix/rix_hash64.h"  /* カッコーハッシュ (u64 キー版)         */
#include "rix/rix_hash_key.h"/* カッコーハッシュ (u32 + u64 統合版)   */
```

### キューの最小サンプル

```c
struct node {
    int value;
    RIX_TAILQ_ENTRY(node) link;
};

RIX_TAILQ_HEAD(qhead);
struct qhead h;
struct node *base;   /* 共有メモリ上の要素配列 */

RIX_TAILQ_INIT(&h);

RIX_TAILQ_INSERT_TAIL(&h, base, &base[0], link);
RIX_TAILQ_INSERT_TAIL(&h, base, &base[1], link);

struct node *it;
RIX_TAILQ_FOREACH(it, &h, base, link) {
    /* it->value を使用 */
}

RIX_TAILQ_REMOVE(&h, base, &base[0], link);
```

---

## 共通ヘルパー

任意の rix 公開ヘッダ経由で利用可能 (内部では `rix_defs_private.h` で定義):

```c
RIX_NIL                          /* 0 -- ヌルインデックス */
RIX_IDX_FROM_PTR(base, p)        /* ポインタ -> インデックス */
RIX_PTR_FROM_IDX(base, i)        /* インデックス -> ポインタ (i==0 なら NULL) */
RIX_IDX_IS_NIL(i)                /* i == RIX_NIL */
RIX_IDX_IS_VALID(i, cap)         /* 1 <= i <= cap */

RIX_MIN(a, b)
RIX_MAX(a, b)
RIX_COUNT_OF(arr)
RIX_OFFSET_OF(type, field)
RIX_CONTAINER_OF(ptr, type, field)
RIX_SWAP(a, b)
RIX_CLAMP(v, lo, hi)
RIX_ASSERT(expr)
RIX_STATIC_ASSERT(expr, msg)
```

---

## キュー構造体

以下の全マクロで共通の引数:
- `type` -- 要素の構造体名 (`struct` キーワードなしの裸の識別子)
- `field` -- `type` 内に埋め込んだリンクフィールド名
- `base` -- 要素配列への `type *` ポインタ
- `head` -- コンテナヘッドへのポインタ

### RIX_SLIST

単方向リスト。先頭挿入 O(1)、削除 O(n)。

```c
/* 宣言 */
RIX_SLIST_ENTRY(type)                   /* 構造体内リンクフィールド */
RIX_SLIST_HEAD(name)                    /* ヘッド型の宣言 */
RIX_SLIST_HEAD_INITIALIZER(var)         /* 静的初期化子 */

/* 初期化・参照 */
RIX_SLIST_INIT(head)
RIX_SLIST_EMPTY(head)                   /* 空なら 1 */
RIX_SLIST_FIRST(head, base)             /* 先頭要素または NULL */
RIX_SLIST_NEXT(elm, base, field)        /* 次要素または NULL */

/* 変更 */
RIX_SLIST_INSERT_HEAD(head, base, elm, field)
RIX_SLIST_INSERT_AFTER(base, slistelm, elm, field)
RIX_SLIST_REMOVE_HEAD(head, base, field)
RIX_SLIST_REMOVE_AFTER(base, elm, field)
RIX_SLIST_REMOVE(head, base, elm, type, field)   /* O(n) 線形探索 */

/* 反復 */
RIX_SLIST_FOREACH(var, head, base, field)
RIX_SLIST_FOREACH_SAFE(var, head, base, field, tvar)
RIX_SLIST_FOREACH_PREVINDEX(var, varidxp, head, base, field)
```

### RIX_LIST

双方向リスト。任意位置での挿入・削除が O(1)。

```c
/* 宣言 */
RIX_LIST_ENTRY(type)
RIX_LIST_HEAD(name)
RIX_LIST_HEAD_INITIALIZER(var)

/* 初期化・参照 */
RIX_LIST_INIT(head)
RIX_LIST_EMPTY(head)
RIX_LIST_FIRST(head, base)
RIX_LIST_NEXT(elm, base, field)

/* 変更 */
RIX_LIST_INSERT_HEAD(head, base, elm, field)
RIX_LIST_INSERT_AFTER(head, base, listelm, elm, field)
RIX_LIST_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_LIST_REMOVE(head, base, elm, field)
RIX_LIST_SWAP(head1, head2, base, type, field)

/* 反復 */
RIX_LIST_FOREACH(var, head, base, field)
RIX_LIST_FOREACH_SAFE(var, head, base, field, tvar)
```

### RIX_STAILQ

単方向テールキュー。先頭・末尾挿入がともに O(1)。

```c
/* 宣言 */
RIX_STAILQ_ENTRY(type)
RIX_STAILQ_HEAD(name)
RIX_STAILQ_HEAD_INITIALIZER(var)

/* 初期化・参照 */
RIX_STAILQ_INIT(head)
RIX_STAILQ_EMPTY(head)
RIX_STAILQ_FIRST(head, base)
RIX_STAILQ_LAST(head, base)
RIX_STAILQ_NEXT(head, base, elm, field)

/* 変更 */
RIX_STAILQ_INSERT_HEAD(head, base, elm, field)
RIX_STAILQ_INSERT_TAIL(head, base, elm, field)
RIX_STAILQ_INSERT_AFTER(head, base, tqelm, elm, field)
RIX_STAILQ_REMOVE_HEAD(head, base, field)
RIX_STAILQ_REMOVE_AFTER(head, base, elm, field)
RIX_STAILQ_REMOVE(head, base, elm, type, field)   /* O(n) 線形探索 */
RIX_STAILQ_REMOVE_HEAD_UNTIL(head, base, elm, field)
RIX_STAILQ_CONCAT(head1, head2, base, field)
RIX_STAILQ_SWAP(head1, head2, base)

/* 反復 */
RIX_STAILQ_FOREACH(var, head, base, field)
RIX_STAILQ_FOREACH_SAFE(var, head, base, field, tvar)
```

### RIX_TAILQ

双方向テールキュー。先頭・末尾・任意位置での挿入・削除がすべて O(1)。

```c
/* 宣言 */
RIX_TAILQ_ENTRY(type)
RIX_TAILQ_HEAD(name)
RIX_TAILQ_HEAD_INITIALIZER(var)

/* 初期化・参照 */
RIX_TAILQ_INIT(head)
RIX_TAILQ_RESET(head)           /* INIT の別名 */
RIX_TAILQ_EMPTY(head)
RIX_TAILQ_FIRST(head, base)
RIX_TAILQ_LAST(head, base)
RIX_TAILQ_NEXT(head, base, elm, field)
RIX_TAILQ_PREV(head, base, elm, field)

/* 変更 */
RIX_TAILQ_INSERT_HEAD(head, base, elm, field)
RIX_TAILQ_INSERT_TAIL(head, base, elm, field)
RIX_TAILQ_INSERT_AFTER(head, base, listelm, elm, field)
RIX_TAILQ_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_TAILQ_REMOVE(head, base, elm, field)
RIX_TAILQ_CONCAT(head1, head2, base, field)
RIX_TAILQ_SWAP(head1, head2, base)

/* 反復 */
RIX_TAILQ_FOREACH(var, head, base, field)
RIX_TAILQ_FOREACH_SAFE(var, head, base, field, tvar)
RIX_TAILQ_FOREACH_REVERSE(var, head, base, field)
```

### RIX_CIRCLEQ

循環双方向リスト。FIRST が LAST に戻る循環走査が可能。

```c
/* 宣言 */
RIX_CIRCLEQ_ENTRY(type)
RIX_CIRCLEQ_HEAD(name)
RIX_CIRCLEQ_HEAD_INITIALIZER(var)

/* 初期化・参照 */
RIX_CIRCLEQ_INIT(head)
RIX_CIRCLEQ_EMPTY(head)
RIX_CIRCLEQ_FIRST(head, base)
RIX_CIRCLEQ_LAST(head, base, field)
RIX_CIRCLEQ_NEXT(elm, base, field)
RIX_CIRCLEQ_PREV(elm, base, field)

/* 変更 */
RIX_CIRCLEQ_INSERT_HEAD(head, base, elm, field)
RIX_CIRCLEQ_INSERT_TAIL(head, base, elm, field)
RIX_CIRCLEQ_INSERT_AFTER(head, base, listelm, elm, field)
RIX_CIRCLEQ_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_CIRCLEQ_REMOVE(head, base, elm, field)

/* 反復 (1 周分) */
RIX_CIRCLEQ_FOREACH(var, head, base, field)
RIX_CIRCLEQ_FOREACH_REVERSE(var, head, base, field)
RIX_CIRCLEQ_FOREACH_SAFE(var, head, base, field, tvar)
RIX_CIRCLEQ_FOREACH_REVERSE_SAFE(var, head, base, field, tvar)
```

---

## Red-Black ツリー -- RIX_RB

自己平衡 BST。挿入・削除・検索がすべて O(log n)。

### 最小サンプル

```c
struct rbnode {
    int key;
    RIX_RB_ENTRY(rbnode) rb;
};

static int rb_cmp(const struct rbnode *a, const struct rbnode *b) {
    return (a->key > b->key) - (a->key < b->key);
}

RIX_RB_HEAD(rbtree);
RIX_RB_PROTOTYPE(rbt, rbnode, rb, rb_cmp)
RIX_RB_GENERATE (rbt, rbnode, rb, rb_cmp)

void demo(struct rbtree *rh, struct rbnode *base) {
    RIX_RB_INIT(rh);

    base[0].key = 42;
    RIX_RB_INSERT(rbt, rh, base, &base[0]);

    struct rbnode probe = { .key = 42 };
    struct rbnode *hit = RIX_RB_FIND(rbt, rh, base, &probe);

    struct rbnode *it;
    RIX_RB_FOREACH(it, rbt, rh, base) { /* 昇順走査 */ }
}
```

### API リファレンス

```c
/* 宣言・コード生成 */
RIX_RB_ENTRY(type)
RIX_RB_HEAD(name)
RIX_RB_HEAD_INITIALIZER(var)
RIX_RB_INIT(head)
RIX_RB_PROTOTYPE(name, type, field, cmp)     /* extern 宣言 */
RIX_RB_GENERATE (name, type, field, cmp)     /* 実装生成 */

/* 操作 */
RIX_RB_INSERT(name, head, base, elm)   /* NULL -> 挿入成功; 非 NULL -> 重複 */
RIX_RB_REMOVE(name, head, base, elm)   /* 削除した elm を返す */
RIX_RB_FIND  (name, head, base, key)   /* 完全一致または NULL */
RIX_RB_NFIND (name, head, base, key)   /* 下界 (key 以上の最小ノード) */
RIX_RB_MIN   (name, head, base)
RIX_RB_MAX   (name, head, base)
RIX_RB_NEXT  (name, base, elm)
RIX_RB_PREV  (name, base, elm)

/* 反復 */
RIX_RB_FOREACH        (var, name, head, base)   /* 昇順 */
RIX_RB_FOREACH_REVERSE(var, name, head, base)   /* 降順 */
```

比較関数シグネチャ: `int cmp(const type *a, const type *b)` -- 狭義弱順序。

---

## カッコーハッシュテーブル

インデックスベースのカッコーハッシュ 5 バリアント。共通特性:

- **バケットあたり 16 スロット** (SIMD 並列スキャン)
- **実行時 SIMD ディスパッチ** -- `rix_hash_arch_init(enable)` でソースファイルごとに Generic / SSE4.2 / AVX2 / AVX-512 を選択
- **XOR 対称性による 2 候補バケット** -- 削除 O(1)、リハッシュ不要
- **N 段先行パイプラインルックアップ** -- DRAM レイテンシを複数リクエスト間で隠蔽
- **1-origin インデックス格納** -- `RIX_NIL = 0` が空スロットを示す; 生ポインタなし

### バリアント比較

| バリアント | ヘッダ | キー格納場所 | ノード補助フィールド | バケットサイズ | 適したキー |
|-----------|--------|------------|-------------------|--------------|-----------|
| fp      | `rix_hash_fp.h`      | フィンガープリント→バケット、フルキー→ノード | `hash_field`                | 128 B (2 CL) | 可変長キー、汎用 |
| slot    | `rix_hash_slot.h`    | フィンガープリント→バケット、フルキー→ノード | `hash_field` + `slot_field` | 128 B (2 CL) | 可変長キー、最速 remove |
| keyonly | `rix_hash_keyonly.h` | フィンガープリント→バケット、フルキー→ノード | (なし)                      | 128 B (2 CL) | 可変長キー、最小ノード |
| hash32  | `rix_hash32.h`       | `uint32_t` キーをバケットに直接格納          | (なし)                      | 128 B (2 CL) | 32 ビット整数キー |
| hash64  | `rix_hash64.h`       | `uint64_t` キーをバケットに直接格納          | (なし)                      | 192 B (3 CL) | 64 ビット整数キー |

fp/slot/keyonly の 3 バリアントは同じバケットレイアウトと staged-find パイプラインを共有します。
`rix_hash.h` は 5 バリアント全てをインクルードする傘ヘッダです。

#### Find 性能 (DRAM コールド、パイプライン、平均 cycles/op)

| パターン | fp | keyonly | slot | hash32 | hash64 |
|---------|---:|-------:|-----:|-------:|-------:|
| single (パイプラインなし) | 377 | 406 | 458 | 152 | 141 |
| x1 staged               | 266 | 220 | 291 |  75 | 122 |
| x4 staged               | 231 | 249 | 248 |  90 | 137 |
| x6 pipeline             | 153 | 113 | 153 |  75 | 117 |
| x8 pipeline             | 159 | 122 | 142 |  73 | 107 |

条件: 1 億エントリ (fp/slot/keyonly)、1000 万エントリ (hash32/hash64)、
充填率 74.5%/59.6%、シーケンシャルキー、AVX2、バケットメモリ >> L3 キャッシュ。
MSHR 理論下限: 45 cycles/op (20 MSHRs, 300 cy レイテンシ)。

#### Insert / Remove 性能 (平均 cycles/op)

| 操作 | fp | keyonly | slot | hash32 | hash64 |
|------|---:|-------:|-----:|-------:|-------:|
| insert | 80 |  57 |  51 |  37 |  51 |
| remove | 31 |  37 |  12 |  27 |  27 |

slot の remove が最速 (12 cy/op)。`slot_field` による O(1) スロット特定が寄与。
keyonly の remove が最遅 (37 cy/op)。キーの再ハッシュが必要なため。

#### 最大充填率

| バリアント | max_fill |
|-----------|----------|
| fp / slot / keyonly | 95.0% |
| hash32 / hash64     | 100%  |

hash32/hash64 がより高い充填率を達成できるのは、キー比較がバケット内で
完結しノードアクセスが不要なため。

#### バケットスキャン性能 (`find_u32x16`、16 スロット/バケット、L2 ウォーム)

最内ホットパスはバケット内 16 スロットのフィンガープリント / キースキャンです。
128 バケットが L2 キャッシュに収まった状態でシングルコア測定:

| ビルドフラグ | 実行時レベル | cy/バケット | 備考 |
|------------|------------|-----------|------|
| `-msse4.2` のみ       | GEN (enable=0) | 36.0 | 純スカラーループ |
| `-msse4.2` のみ       | **SSE4.2**     |  6.3 | XMM 128-bit — GEN 比 ×5.7 |
| `-mavx2 -msse4.2`    | SSE4.2         |  6.0 | XMM 128-bit |
| `-mavx2 -msse4.2`    | **AVX2**       |  3.3 | YMM 256-bit — SSE4.2 比 ×1.8 |
| `-mavx2 -msse4.2`    | GEN (enable=0) |  3.8 | コンパイラが AVX2 で自動ベクトル化 * |

\* `-mavx2` ビルドではコンパイラが GEN スカラーループを AVX2 で自動ベクトル化するため、
`enable=0` (GEN 強制) でも実際には AVX2 命令が実行されます。

この表は現状 GEN / SSE4.2 / AVX2 までを掲載しています。AVX-512 の参考値は
[samples/README_JP.md](samples/README_JP.md) に分けて記載しています。
公開している AVX-512 再計測は `AMD Ryzen 9 8945HS`（Zen 4）上で後から取得した値で、
CPU 依存の参考データとして扱うためです。

**まとめ:** SSE4.2 tier が最も有効なのは SSE4.2 はあるが AVX2 がない CPU
(Sandy Bridge / Ivy Bridge、2011〜2012 年世代) です。
AVX2 以降の CPU では AVX2 パスが最適です。

---

### RIX_HASH (フィンガープリント、可変長キー)

ノード構造体には、現在所属している bucket に対応する hash を保持する
`hash_field` 整数 field が必要です。`SLOT` variant では、さらに現在の
slot を保持する呼び出し側定義の整数 `slot_field` を持ちます。

```c
#include "rix/rix_hash.h"

/* 1. typedef 必須 -- マクロは裸の識別子を使用する */
typedef struct mynode mynode;
struct mynode {
    uint8_t  key[16];    /* キーフィールド (可変長) */
    uint32_t cur_hash;   /* current-bucket hash (名前は任意) */
    uint32_t value;
};

/* 2. ヘッドの宣言と API 生成 */
RIX_HASH_HEAD(myht);
RIX_HASH_GENERATE(myht, mynode, key, cur_hash, my_cmp_fn)
/* 型付き hash hook を渡す場合:
 * RIX_HASH_GENERATE_EX(myht, mynode, key, cur_hash,
 *                      my_cmp_fn, my_hash_fn)
 */

typedef struct mynode_slot mynode_slot;
struct mynode_slot {
    uint8_t  key[16];
    uint32_t cur_hash;
    uint16_t slot;
    uint16_t value;
};

RIX_HASH_HEAD(myht_slot);
RIX_HASH_GENERATE_SLOT(myht_slot, mynode_slot, key, cur_hash, slot, my_cmp_fn)

/* 3. 任意: このソースファイルで SIMD を有効化 */
rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

/* 4. 64 バイトアライメントのバケット配列を確保 */
struct rix_hash_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));
memset(buckets, 0, NB_BK * sizeof(*buckets));   /* 0 = 全スロット空 */
mynode *pool = calloc(N, sizeof(*pool));         /* 1-origin: pool[0] = インデックス 1 */

struct myht head;
RIX_HASH_INIT(&head, NB_BK);   /* NB_BK は 2 の冪乗であること */
```

#### 単発操作

```c
/* 挿入: NULL -> 成功; 別ポインタ -> 重複; elm 自身 -> テーブル満杯 */
mynode *dup = myht_insert(&head, buckets, pool, &pool[i]);

/* キーポインタで検索 */
mynode *hit = myht_find(&head, buckets, pool, key_ptr);

/* ノードポインタで削除 */
mynode *rem = myht_remove(&head, buckets, pool, &pool[i]);

/* 全エントリ巡回: cb が 0 を返す間継続、非 0 で停止 */
myht_walk(&head, buckets, pool, cb, arg);
```

#### パイプライン (ステージ型) 検索

複数のルックアップを同時進行させて DRAM レイテンシを隠蔽する:

```c
struct rix_hash_find_ctx_s ctx[4];
const void *keys[4] = { k0, k1, k2, k3 };
mynode *results[4];

/* ステージ 1: ハッシュ計算 + バケットプリフェッチ */
myht_hash_key4(ctx, &head, buckets, keys);
/* ステージ 2: フィンガープリントスキャン */
myht_scan_bk4 (ctx, &head, buckets);
/* ステージ 3: フルキー比較 */
myht_cmp_key4 (ctx, pool, results);
```

バルクバリアント: `_key1` / `_key2` / `_key4` / `_key8` (サフィックスが件数)。

#### `RIX_HASH_GENERATE` オプション

| バリアント | マクロ |
|-----------|-------|
| 外部リンケージ | `RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)` |
| 外部リンケージ + custom hash | `RIX_HASH_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)` |
| 外部リンケージ + slot-aware remove | `RIX_HASH_GENERATE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn)` |
| 外部リンケージ + slot-aware remove + custom hash | `RIX_HASH_GENERATE_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn)` |
| `static` リンケージ | `RIX_HASH_GENERATE_STATIC(name, type, key_field, hash_field, cmp_fn)` |
| `static` リンケージ + custom hash | `RIX_HASH_GENERATE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)` |
| `static` リンケージ + slot-aware remove | `RIX_HASH_GENERATE_STATIC_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn)` |
| `static` リンケージ + slot-aware remove + custom hash | `RIX_HASH_GENERATE_STATIC_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn)` |

`cmp_fn` シグネチャ: `int cmp_fn(const type *a, const type *b)` -- 等しければ 0 を返す。
`hash_fn` シグネチャ: `union rix_hash_hash_u hash_fn(const key_type *key, uint32_t mask)`。
`slot_field` は `[0, RIX_HASH_BUCKET_ENTRY_SZ - 1]` を表現できる
呼び出し側定義の整数型であれば十分です。

`SLOT` variant は次を維持します。

- `node->hash_field & mask == current_bucket`
- `buckets[current_bucket].idx[node->slot_field] == node_idx`

これにより `remove()` は direct-slot になり、非 `SLOT` variant のような
`idx[16]` scan を行いません。

---

### RIX_HASH32 (uint32_t キー)

ノード構造体に `hash_field` 不要。キー自体をバケットに直接格納するため、
`scan_bk` が 32 ビット完全一致比較を行う。

```c
#include "rix/rix_hash32.h"

typedef struct entry32 entry32;
struct entry32 {
    uint32_t key;    /* キーフィールド (名前は任意) */
    uint32_t value;
};

RIX_HASH32_HEAD(ht32);
RIX_HASH32_GENERATE(ht32, entry32, key)

rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

struct rix_hash32_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));
memset(buckets, 0, NB_BK * sizeof(*buckets));
entry32 *pool = calloc(N, sizeof(*pool));

struct ht32 head;
RIX_HASH32_INIT(&head, NB_BK);
```

#### API

```c
entry32 *ht32_insert(&head, buckets, pool, &pool[i]);
entry32 *ht32_find  (&head, buckets, pool, key_value);   /* 値渡しでキー指定 */
entry32 *ht32_remove(&head, buckets, pool, key_value);
int      ht32_walk  (&head, buckets, pool, cb, arg);

/* パイプライン検索 (rix_hash と同じステージ構成) */
struct rix_hash32_find_ctx_s ctx[4];
uint32_t keys[4] = { k0, k1, k2, k3 };
entry32 *results[4];

ht32_hash_key4(ctx, &head, buckets, keys);
ht32_scan_bk4 (ctx, &head, buckets);
ht32_cmp_key4 (ctx, pool, results);
```

---

### RIX_HASH64 (uint64_t キー)

RIX_HASH32 と同じインターフェースで `uint64_t` キーに対応。
バケットは 128 B ではなく 192 B (3 キャッシュライン)。

```c
#include "rix/rix_hash64.h"

typedef struct entry64 entry64;
struct entry64 {
    uint64_t key;
    uint32_t value;
};

RIX_HASH64_HEAD(ht64);
RIX_HASH64_GENERATE(ht64, entry64, key)

struct rix_hash64_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));

struct ht64 head;
RIX_HASH64_INIT(&head, NB_BK);

entry64 *ht64_insert(&head, buckets, pool, &pool[i]);
entry64 *ht64_find  (&head, buckets, pool, key_value);
entry64 *ht64_remove(&head, buckets, pool, key_value);
```

パイプラインステージは同じパターン: `ht64_hash_key4`, `ht64_scan_bk4`,
`ht64_cmp_key4`。

#### 全ハッシュバリアント共通の注意事項

- `rix_hash_arch_init(enable)` は任意。呼ばない場合、各ソースファイルは
  デフォルトで Generic パスのまま動作する。
- SIMD を有効にする場合は、ハッシュ操作を行う各ソースファイルで
  `rix_hash_arch_init(enable)` を呼ぶこと。
  `RIX_HASH_ARCH_AUTO` を渡すと利用可能な最良の SIMD を自動選択（推奨）。
  `RIX_HASH_ARCH_SSE` を渡すと SSE4.2 XMM tier 止まり（AVX2 不使用）。
  `RIX_HASH_ARCH_AVX2` を渡すと AVX-512 が存在しても AVX2 に制限。
  `0` を渡すと Generic（スカラー）強制 — ベンチマーク比較用。
- バケット配列は**64 バイトアライメント**必須 (`aligned_alloc(64, ...)` または `posix_memalign`)。
- `NB_BK` は**2 の冪乗**かつ 2 以上であること。
- `insert` の戻り値:
  - `NULL` -- 挿入成功
  - 別のポインタ -- 重複 (同じキーがすでに存在)
  - `elm` 自身 -- テーブル満杯 (キックアウト深度上限に達した)
- 運用指針として、`100%` 充填を前提にしてはいけません。
  定常運用の上限は全スロット (`NB_BK * 16`) の **90% 以下**に抑え、
  データパス性能を重視する場合は **75% 以下**を優先します。
  sample flow-cache ではそのために maintenance / reclaim を定期実行します。

---

## サンプル

`samples/` には librix を基盤としたフローキャッシュ実装が含まれます。
現在のツリーには次の 3 バリアントがあります。

- `flow4`  : IPv4 5-tuple キャッシュ
- `flow6`  : IPv6 5-tuple キャッシュ
- `flowu`  : IPv4/IPv6 統合キャッシュ

設計・API・性能データは [samples/README.md](samples/README.md) を参照してください。
公開向けの要約文書としては
[CHANGELOG.md](CHANGELOG.md)、
[RELEASE_NOTES_v0.1.1.md](RELEASE_NOTES_v0.1.1.md)、
[RELEASE_NOTES_v0.1.1_SHORT.md](RELEASE_NOTES_v0.1.1_SHORT.md)、
[GITHUB_ABOUT.md](GITHUB_ABOUT.md)、
[PUBLIC_RELEASE_CHECKLIST.md](PUBLIC_RELEASE_CHECKLIST.md)
も用意しています。

flow cache バリアント (`flow4`, `flow6`, `flowu`) では、allocator 非依存で
hugepage 配置にも向く size-query ベースの初期化 API を追加しています。

さらに intrusive な統合向けに、`*_cache_init_ex()` は単純な
`fc_flow*_entry[]` ではなく、呼び出し側が持つ固定 stride の record 配列を
受けられます。library が必要とするのは次の 2 つだけです。

- `stride`: record 間のバイト間隔
- `entry_offset`: 各 record 内で embedded `fc_flow*_entry` が始まる位置

record の残りの領域は librix にとって opaque であり、user が自由に payload
として使えます。record base / entry 取得には次を使います。

- `fc_flow*_cache_record_ptr()`
- `fc_flow*_cache_entry_ptr()`

典型的には次のような形です。

```c
struct my_flow_rec {
    struct fc_flow4_entry entry;   /* 先頭でなくてもよい */
    uint8_t body[];
};
```

あるいは raw byte record として

```c
unsigned char *rec =
    FC_FLOW4_CACHE_RECORD_PTR_AS(&fc, unsigned char, entry_idx);
void *my_body = rec + my_body_offset;
```

`*_init_ex()` の性能上の指針:

- `entry_offset` は `FC_CACHE_LINE_SIZE` 境界に合わせる
- embedded entry が cache line を跨がないようにする
- `entry` は先頭 member でなくてもよいが、datapath 性能を重視するなら
  cache-line 境界で始める
- record を大きくしすぎると footprint が増え、cache/TLB 圧で lookup/add/remove
  が遅くなる。`*_init_ex()` 自体のコストは小さいが、巨大な user record は
  別問題として性能低下要因になる
- `fc_flow*_cache_set_event_cb()` を有効にすると alloc/free path で caller code
  が走る。軽い counter 更新程度なら許容されやすいが、大きい payload や cold
  な領域へ触る callback は throughput をかなり落とし得る

この record/entry accessor は `flow4`, `flow6`, `flowu` のすべてで利用できます。
一方で、従来の `fc_flow*_cache_init()` API もそのまま維持しています。

この flow cache は単なるサンプルではなく、このリポジトリにおける主要な
性能アピール対象です。直近の再計測では `AMD Ryzen 9 8945HS`
（`avx512`, 2026-03-23）上で、`flow4` の `findadd_hit` が `32K` entries で
`25.94 cyc/key`、`1024K` entries で `26.60 cyc/key` を記録し、大きい
`1024K findadd_miss` では `flowu` が `341.42 cyc/key` でした。

`avx512` dispatch tier 自体は存在し、実機でも確認済みです。現行の
`samples/fcache` では `avx512` tier が AVX-512 bucket-scan helper を使い、
`avx2` tier は AVX2 helper を使います。ただし AVX2 と AVX-512 の優劣は
CPU 依存で、この Zen 4 実測でも強制 `avx512` は一部の datapath では改善し、
別の datapath では悪化しました。README に載せる数値は Zen 4 上の参考値です。

helper 名の詳しい説明や `fcache` 固有の API、性能表、backend 別検証手順、
設計ノートは
[samples/README_JP.md](samples/README_JP.md) を参照してください。

---

## ビルド

C11 必須。推奨フラグ:

```sh
# AVX2（推奨デフォルト）
cc -std=gnu11 -O3 -mavx2 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror  \
   -I/path/to/librix/include       \
   your_sources.c

# AVX-512
cc -std=gnu11 -O3 -mavx512f -mavx2 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror             \
   -I/path/to/librix/include                  \
   your_sources.c

# Generic スカラーのみ（SIMD サーチなし、CRC32C ハッシュは維持）
cc -std=gnu11 -O3 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror \
   -I/path/to/librix/include      \
   your_sources.c
```

付属のテスト・ベンチマークスイートでは `SIMD=` 変数で一括切り替え可能:

```sh
make SIMD=avx2    # デフォルト
make SIMD=avx512
make SIMD=gen
```

コンパイラと最適化レベルも上書き可能:

```sh
make CC=gcc   OPTLEVEL=3
make CC=clang OPTLEVEL=3
```

現状ツリーはこの条件で GCC / Clang の両方でビルドできる前提です。

`samples/fcache` では、`avx512` tier が AVX-512 bucket-scan helper を使います。
AVX2 と AVX-512 の実測比較は
[samples/README_JP.md](samples/README_JP.md) に記載しています。
掲載値は `AMD Ryzen 9 8945HS`（Zen 4）上の参考値です。

`samples/fcache` の lookup pipeline 定数の既定値は
`FLOW_CACHE_LOOKUP_STEP_KEYS=8`,
`FLOW_CACHE_LOOKUP_AHEAD_STEPS=4`,
`FLOW_CACHE_LOOKUP_AHEAD_KEYS=32` です。
`AHEAD_KEYS` は hardware prefetch 回数ではなく、
software pipeline の段間距離です。
tuning 比較では `EXTRA_CFLAGS` で上書きできます:

```sh
make -C samples/fcache static CC=gcc OPTLEVEL=3 \
     EXTRA_CFLAGS='-DFLOW_CACHE_LOOKUP_STEP_KEYS=8 -DFLOW_CACHE_LOOKUP_AHEAD_KEYS=64'
make -C samples/fcache/test all CC=gcc OPTLEVEL=3 \
     EXTRA_CFLAGS='-DFLOW_CACHE_LOOKUP_STEP_KEYS=8 -DFLOW_CACHE_LOOKUP_AHEAD_KEYS=64'
```

開発時のサニタイザ:

```sh
-fsanitize=address,undefined -fno-omit-frame-pointer
```

---

## 並行性

librix は内部同期を持ちません。全構造体はシングルスレッド、または独自ロック
(futex, pthread mutex, プロセス共有プリミティブ等) 下での
マルチリーダ/シングルライタに適しています。

ロックフリー / RCU 動作はスコープ外であり、追加設計が必要です。

---

## テスト

```sh
# ライブラリ全体のテスト
make test

# トップレベルの代表 bench 再実行
make bench

# トップレベルの full 長時間 bench 再実行
make bench-full
```

flow-cache 固有の検証・ベンチマーク手順は
[samples/README_JP.md](samples/README_JP.md) を参照してください。

テストカバレッジ:

- 空 / シングルトン / 複数要素の各操作の遷移
- 全挿入・削除バリアント
- 削除しながらの安全な反復
- Red-Black 不変条件 (ルートが黒、赤-赤なし、黒高さ一致)
- ハッシュテーブル: 重複検出、パイプラインステージ正確性、walk カウント
- ファジング: ランダム insert/find/remove をリファレンスモデルと照合
- フローキャッシュ: find, remove, flush, expire, batch lookup, insert 枯渇

### ハッシュテーブル テストカバレッジ一覧

カッコーハッシュ全 5 バリアントがフルテストスイートを通過:

| テスト項目 | fp | slot | keyonly | hash32 | hash64 |
|-----------|:--:|:----:|:------:|:------:|:------:|
| init/empty           | PASS | PASS | PASS | PASS | PASS |
| insert/find/remove   | PASS | PASS | PASS | PASS | PASS |
| 2重登録 (duplicate insert) | PASS | PASS | PASS | PASS | PASS |
| 存在しないキーの削除   | PASS | PASS | PASS | PASS | PASS |
| staged find (x1/x2/x4) | PASS | - | - | PASS | PASS |
| walk                 | PASS | -    | -    | PASS | PASS |
| high_fill (90%+)     | 93.8% | 93.8% | 93.8% | 93.8% | 93.8% |
| max_fill             | 95.0% | 95.0% | 95.0% | 100%  | 100%  |
| kickout_safety       | PASS | PASS | PASS | PASS | PASS |
| fuzz (200K ops)      | PASS | PASS | PASS | PASS | PASS |
| fuzz (500K ops)      | PASS | PASS | PASS | PASS | PASS |

主な確認結果:

- **Kickout 安全性**: 全バリアントでカッコーキックアウト連鎖中のエレメント
  消失ゼロを確認。非破壊的再帰キックアウト実装の正しさを実証。
- **充填率**: fp/slot/keyonly は全スロットの 95% まで到達。hash32/hash64 は
  キー比較がバケット内で完結するため 100% 到達可能。
- **ファジングテスト**: ランダムな insert/find/remove シーケンス (最大 50 万 ops) を
  メモリ上のリファレンスセットと照合し、全バリアントで整合性を確認。

---

## ライセンス

BSD 3-Clause。[LICENSE](LICENSE) を参照。

---

## 設計の系譜

キューおよびツリー API は BSD `sys/queue.h` / `sys/tree.h` インターフェースを
踏襲し、生ポインタをインデックスに置き換えることで堅牢な共有メモリ展開を
実現しています。

カッコーハッシュテーブルは XOR ベースの 2 バケット方式を採用し、
プリフェッチ駆動のステージ型ルックアップにより O(1) 償却挿入を達成しています。
