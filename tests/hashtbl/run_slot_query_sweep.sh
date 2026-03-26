#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out_dir="${script_dir}/out"
core="${CORE:-0}"
total_keys="${TOTAL_KEYS:-262144}"
repeat="${REPEATS:-7}"
hotset="${HOTSET:-8192}"
table_n="${TABLE_N:-1048576}"
nb_bk="${NB_BK:-131072}"

mkdir -p "${out_dir}"
make -C "${script_dir}" slot_query_sweep >/dev/null

bench_cmd=(./slot_query_sweep "${total_keys}" "${repeat}" "${hotset}" "${table_n}" "${nb_bk}")
if command -v taskset >/dev/null 2>&1; then
    bench_cmd=(taskset -c "${core}" "${bench_cmd[@]}")
fi

(
    cd "${script_dir}"
    "${bench_cmd[@]}"
) > "${out_dir}/slot_query_sweep.tsv"

printf 'wrote %s\n' "${out_dir}/slot_query_sweep.tsv"
