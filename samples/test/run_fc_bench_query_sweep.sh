#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
bench="${script_dir}/fc_bench"

if [[ ! -x "${bench}" ]]; then
    echo "fc_bench not found: ${bench}" >&2
    echo "build it first with: make -C samples/test fc_bench" >&2
    exit 1
fi

if [[ $# -eq 0 ]]; then
    cat >&2 <<'EOF'
usage:
  run_fc_bench_query_sweep.sh <fc_bench args...>

examples:
  run_fc_bench_query_sweep.sh datapath
  run_fc_bench_query_sweep.sh archcmp flow4 findadd_closed 8192 50 95 1000
  run_fc_bench_query_sweep.sh archcmp flow4 findadd_window 1000000 60 75 95 500000 1000 1000
EOF
    exit 2
fi

for query in 32 64 128 256; do
    printf '\n===== query=%u =====\n\n' "${query}"
    "${bench}" --query "${query}" "$@"
done
