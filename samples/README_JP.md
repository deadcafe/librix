# Samples

このディレクトリには、librix 上の 2 つの sample family がある。

- `samples/fcache/`
  `findadd`、reclaim、pressure relief、timeout 処理、
  SIMD dispatch datapath を備えた高性能 flow cache。
- `samples/ftable/`
  permanent-style entry を想定した flow table 試作。
  `find` と `add` は分離され、record は caller-owned の安定配置で、
  resize では bucket hash table だけを拡大する。

## 構成

```text
samples/
  README.md
  README_JP.md
  Makefile
  fcache/
  ftable/
  test/
```

## 共通コマンド

```sh
# flow cache library と sample test binary をビルド
make -C samples all

# flow cache の正当性テスト
make -C samples test

# flow cache の代表ベンチ
make -C samples bench

# flow table library をビルド
make -C samples ftable

# flow table の正当性テスト
make -C samples ftable-test
```

## 文書

- flow cache の設計、API、test、benchmark メモ:
  `samples/fcache/README_JP.md`
- flow table の設計、API、test、benchmark 計画:
  `samples/ftable/README_JP.md`
- flow cache / flow table の将来 entry 共通化に向けた設計ノート:
  `samples/ENTRY_UNIFICATION_DESIGN_JP.md`

`samples/` 直下の README は索引のみとし、詳細な設計ノートは
`fcache` / `ftable` ごとの README に分離して管理する。
