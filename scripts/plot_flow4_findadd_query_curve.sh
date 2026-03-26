#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "${script_dir}/.." && pwd)

gnuplot -e "ROOT='${repo_root}'" "${script_dir}/plot_flow4_findadd_query_curve.plt"
