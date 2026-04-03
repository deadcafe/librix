# ftable Test Spec

この文書は `samples/ftable/test/test_flow_table.c` の test を、
実装都合ではなく **API 仕様** を起点に整理するための基準表である。

目的は 2 つある。

- 最適化の前に、どの振る舞いを保証するのかを固定する
- 各 test 項目が、どの仕様を確認しているかを 1:1 で追えるようにする

## 1. 前提

- 接続点は `flowX_entry`
- caller は stable な record array を所有する
- `idx` は 1-origin の stable identifier
- `add_idx*` は caller が key を record に書いた後で呼ぶ
- `del_entry_idx*` は caller が valid idx だけを渡す
- `del_key*` は convenience path であり、core の主 API ではない

## 2. API 仕様表

| ID | API | 入力 | 期待動作 | 戻り/結果 |
|---|---|---|---|---|
| A1 | `find` | key | hit なら既存 idx、miss なら 0 | `idx or 0` |
| A2 | `find_bulk` | key 配列 | 各 key を独立 lookup | `results[i].entry_idx` |
| A3 | `add_idx` | idx | key が未登録なら追加 | `inserted idx` |
| A4 | `add_idx` | idx | 同一 key が既登録なら ignore | `existing idx` |
| A5 | `add_idx_bulk` | idx 配列 | 各 idx を独立 add | `results[i].entry_idx` |
| A6 | `add_idx_bulk` | 同一 key, 別 idx | ignore | `results[i]=existing idx` |
| A7 | `add_idx_bulk` | self duplicate | ignore | `results[i]=request idx` |
| A8 | `add_idx_bulk2(ignore)` | idx 配列 | duplicate を ignore | `results[i]=free すべき idx or NIL` |
| A9 | `add_idx_bulk2(update)` | idx 配列 | duplicate を置換 | `results[i]=free すべき old idx or NIL` |
| A10 | `del_entry_idx` | valid idx | 既登録 entry を削除 | `deleted idx` |
| A11 | `del_entry_idx_bulk` | valid idx 配列 | 各 idx を独立 delete | return なし |
| A12 | `del_key` | key | hit なら削除、miss なら 0 | `deleted idx or 0` |
| A13 | `walk` | callback | live entry を巡回 | callback 契約 |
| A14 | `flush` | - | 全 entry を table から外す | void |
| A15 | `grow_2x` | - | live entry を保持したまま bucket のみ拡張 | `0/-1` |
| A16 | `reserve` | min_entries | 必要 bucket 数まで拡張 | `0/-1` |

## 3. add 系の仕様詳細

### 3.1 `add_idx`

- insert:
  - key 未登録なら request idx を採用する
- duplicate ignore:
  - 同一 key が既に table にある場合、その existing idx を返す
- self duplicate:
  - request idx が既に table に入っている場合、request idx を返す

### 3.2 `add_idx_bulk`

`add_idx_bulk` は `add_idx` を bulk 化したものであり、意味論は変えてはならない。

- 各 input idx は独立に評価される
- duplicate 判定は `bk0` / `bk1` の両方を確認してから insert 判定する
- 同一 batch 内でも、先行要素が既に採用されていれば後続 duplicate は existing として扱う

### 3.3 `add_idx_bulk2`

`add_idx_bulk2` は `add_idx_bulk` に duplicate policy を追加した派生 API である。

- inserted:
  - `results[i].entry_idx = RIX_NIL`
- duplicate + `FT_ADD_IGNORE`:
  - request idx を caller が free する
  - `results[i].entry_idx = request idx`
- duplicate + `FT_ADD_UPDATE`:
  - old idx を caller が free する
  - `results[i].entry_idx = old idx`
- return value:
  - `results[i].entry_idx != RIX_NIL` の件数

## 4. test 対応表

| Spec ID | 現在の test | 状態 |
|---|---|---|
| A1 | `test_basic_add_find_del`, `testv_basic_add_find_del` | covered |
| A2 | `test_bulk_ops_and_stats`, `testv_bulk_ops_and_stats` | covered |
| A3 | `test_basic_add_find_del`, `testv_basic_add_find_del` | covered |
| A4 | `test_duplicate_and_delete_miss_stats`, `test_basic_add_find_del`, `testv_basic_add_find_del` | covered |
| A5 | `test_bulk_ops_and_stats`, `testv_bulk_ops_and_stats` | covered |
| A6 | `testv_add_idx_bulk_duplicate_ignore` | covered |
| A7 | `test_bulk_ops_and_stats`, `testv_bulk_ops_and_stats` | covered |
| A8 | `testv_add_idx_bulk2_policy` | covered |
| A9 | `testv_add_idx_bulk2_policy` | covered |
| A10 | `test_del_idx`, `testv_walk_flush_and_del_idx` | covered |
| A11 | `testv_walk_flush_and_del_idx` | covered |
| A12 | `test_duplicate_and_delete_miss_stats`, `test_basic_add_find_del`, `testv_basic_add_find_del` | covered |
| A13 | `test_walk_early_stop`, `test_walk_flush_and_stats_reset`, `testv_walk_flush_and_del_idx` | covered |
| A14 | `test_walk_flush_and_stats_reset`, `testv_walk_flush_and_del_idx` | covered |
| A15 | `test_manual_grow_preserves_entries`, `test_grow_failure_preserves_table`, `testv_manual_grow_preserves_entries` | covered |
| A16 | `test_reserve`, `test_allocator_failure_and_max_bucket_limit`, `testv_reserve` | covered |

## 5. 現時点の不足

最適化前に最低限埋めるべき不足は以下である。

1. `add_idx_bulk` の **bk0 / bk1 両側 duplicate**

## 6. test 配置方針

`test_flow_table.c` の test は、次の順に並べる。

1. lifecycle / mapping
2. find
3. add / add_bulk
4. add_idx_bulk2
5. del / del_bulk
6. walk / flush / stats
7. grow / reserve
8. fill / kickout / fuzz

この順序を変えると、「何の仕様を見ている test か」が読みにくくなる。
