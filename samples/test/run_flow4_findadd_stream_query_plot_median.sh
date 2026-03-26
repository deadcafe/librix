#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bench="${script_dir}/fc_bench"
out_dir="${script_dir}/out"

desired="${DESIRED:-8192}"
fill_pct="${FILL_PCT:-50}"
hit_pct="${HIT_PCT:-95}"
total_keys="${TOTAL_KEYS:-262144}"
keep_n="${KEEP_N:-21}"
raw_repeats="${RAW_REPEATS:-31}"
core="${CORE:-0}"

mkdir -p "${out_dir}"

tsv="${out_dir}/flow4_findadd_stream_query_curve_median.tsv"
svg="${out_dir}/flow4_findadd_stream_query_curve_median.gnuplot.svg"
png="${out_dir}/flow4_findadd_stream_query_curve_median.gnuplot.png"

if command -v taskset >/dev/null 2>&1; then
    bench_prefix=(taskset -c "${core}")
else
    bench_prefix=()
fi

if [[ ! -x "${bench}" ]]; then
    echo "missing benchmark binary: ${bench}" >&2
    echo "run: make -C samples/test fc_bench" >&2
    exit 1
fi

"${bench_prefix[@]}" "${bench}" flow4 findadd_closed_stream_sweep \
    "${desired}" "${fill_pct}" "${hit_pct}" "${total_keys}" "${keep_n}" "${raw_repeats}" \
    > "${tsv}"

gnuplot <<GPLOT
set datafile separator '\t'
set key left top
set grid xtics ytics lc rgb '#d8dde6'
set border lc rgb '#666666'
set xlabel 'query width'
set ylabel 'cycles/key (tightest ${keep_n} of ${raw_repeats} runs)'
set xrange [1:256]
set xtics 1,31,256
set title 'flow4 findadd_closed_stream query sweep (keep_n=${keep_n}, raw=${raw_repeats}, total_keys=${total_keys}, core=${core})'
set style line 1 lc rgb '#1f77b4' lw 2
set style line 2 lc rgb '#d62728' lw 2
set terminal svg size 1200,640 dynamic background rgb 'white'
set output '${svg}'
plot '${tsv}' using 1:2 with lines ls 1 title 'bulk median', \
     '${tsv}' using 1:3 with lines ls 2 title 'burst32 median'
set terminal pngcairo size 1200,640 enhanced background rgb 'white'
set output '${png}'
replot
GPLOT

echo "wrote ${tsv}"
echo "wrote ${svg}"
echo "wrote ${png}"
