#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bench="${script_dir}/fc_bench"
out_dir="${script_dir}/out"

desired="${DESIRED:-8192}"
fill_pct="${FILL_PCT:-50}"
hit_pct="${HIT_PCT:-95}"
total_keys="${TOTAL_KEYS:-262144}"
repeats="${REPEATS:-7}"
core="${CORE:-0}"

mkdir -p "${out_dir}"

tsv="${out_dir}/flow4_findadd_query_curve_median.tsv"
svg="${out_dir}/flow4_findadd_query_curve_median.gnuplot.svg"
png="${out_dir}/flow4_findadd_query_curve_median.gnuplot.png"
raw="${out_dir}/flow4_findadd_query_curve_median_samples.tsv"

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

if [[ "${repeats}" -lt 1 ]]; then
    echo "REPEATS must be >= 1" >&2
    exit 1
fi

if [[ "${total_keys}" -lt 1 ]]; then
    echo "TOTAL_KEYS must be >= 1" >&2
    exit 1
fi

extract_cycles() {
    sed -n 's/.*cycles\/key= *\([0-9.]*\).*/\1/p' | tail -n 1
}

median_from_values() {
    awk '
        { vals[NR] = $1 }
        END {
            if (NR == 0) exit 1;
            if (NR % 2 == 1) {
                printf "%.6f\n", vals[(NR + 1) / 2];
            } else {
                printf "%.6f\n", (vals[NR / 2] + vals[NR / 2 + 1]) / 2.0;
            }
        }'
}

printf 'query\tbulk\tburst32\tgain_pct\n' > "${tsv}"
printf 'query\tapi\trep\tcycles_per_key\n' > "${raw}"

for query in $(seq 1 256); do
    bulk_samples=()
    burst_samples=()
    rounds=$(( (total_keys + query - 1) / query ))
    for rep in $(seq 1 "${repeats}"); do
        if (( rep % 2 == 1 )); then
            bulk_value="$("${bench_prefix[@]}" "${bench}" --query "${query}" --findadd-api bulk \
                flow4 findadd_closed "${desired}" "${fill_pct}" "${hit_pct}" "${rounds}" \
                | extract_cycles)"
            burst_value="$("${bench_prefix[@]}" "${bench}" --query "${query}" --findadd-api burst32 \
                flow4 findadd_closed "${desired}" "${fill_pct}" "${hit_pct}" "${rounds}" \
                | extract_cycles)"
        else
            burst_value="$("${bench_prefix[@]}" "${bench}" --query "${query}" --findadd-api burst32 \
                flow4 findadd_closed "${desired}" "${fill_pct}" "${hit_pct}" "${rounds}" \
                | extract_cycles)"
            bulk_value="$("${bench_prefix[@]}" "${bench}" --query "${query}" --findadd-api bulk \
                flow4 findadd_closed "${desired}" "${fill_pct}" "${hit_pct}" "${rounds}" \
                | extract_cycles)"
        fi
        bulk_samples+=("${bulk_value}")
        burst_samples+=("${burst_value}")
        printf '%s\tbulk\t%s\t%s\n' "${query}" "${rep}" "${bulk_value}" >> "${raw}"
        printf '%s\tburst32\t%s\t%s\n' "${query}" "${rep}" "${burst_value}" >> "${raw}"
    done

    bulk="$(printf '%s\n' "${bulk_samples[@]}" | sort -g | median_from_values)"
    burst32="$(printf '%s\n' "${burst_samples[@]}" | sort -g | median_from_values)"
    gain="$(awk -v bulk="${bulk}" -v burst="${burst32}" \
        'BEGIN { if (burst > 0.0) printf "%.2f", ((bulk / burst) - 1.0) * 100.0; else printf "0.00"; }')"
    printf '%s\t%s\t%s\t%s\n' "${query}" "${bulk}" "${burst32}" "${gain}" >> "${tsv}"
    printf 'query=%3u  bulk_med=%8s  burst32_med=%8s  gain=%6s%%\n' \
        "${query}" "${bulk}" "${burst32}" "${gain}" >&2
done

gnuplot <<GPLOT
set datafile separator '\t'
set key left top
set grid xtics ytics lc rgb '#d8dde6'
set border lc rgb '#666666'
set xlabel 'query width'
set ylabel 'cycles/key (median of ${repeats} runs)'
set xrange [1:256]
set xtics 1,31,256
set title 'flow4 findadd_closed query sweep (median, repeats=${repeats}, total_keys=${total_keys}, core=${core})'
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
echo "wrote ${raw}"
echo "wrote ${svg}"
echo "wrote ${png}"
