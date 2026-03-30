#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/../.." && pwd)"
bench="${script_dir}/fc_bench"
out_dir="${script_dir}/out"

desired="${DESIRED:-8192}"
fill_pct="${FILL_PCT:-50}"
hit_pct="${HIT_PCT:-95}"
rounds="${ROUNDS:-1000}"

mkdir -p "${out_dir}"

tsv="${out_dir}/flow4_findadd_query_curve.tsv"
svg="${out_dir}/flow4_findadd_query_curve.svg"
txt="${out_dir}/flow4_findadd_query_curve.txt"

if [[ ! -x "${bench}" ]]; then
    echo "missing benchmark binary: ${bench}" >&2
    echo "run: make -C samples/fcache/test fc_bench" >&2
    exit 1
fi

run_one() {
    local api="$1"
    local query="$2"
    local output
    output="$("${bench}" --query "${query}" --findadd-api "${api}" \
        flow4 findadd_closed "${desired}" "${fill_pct}" "${hit_pct}" "${rounds}")"
    printf '%s\n' "${output}" \
        | sed -n 's/.*cycles\/key= *\([0-9.]*\).*/\1/p' \
        | tail -n 1
}

printf 'query\tbulk\tburst32\tgain_pct\n' > "${tsv}"

for query in $(seq 1 256); do
    bulk="$(run_one bulk "${query}")"
    burst32="$(run_one burst32 "${query}")"
    gain="$(awk -v bulk="${bulk}" -v burst="${burst32}" \
        'BEGIN { if (burst > 0.0) printf "%.2f", ((bulk / burst) - 1.0) * 100.0; else printf "0.00"; }')"
    printf '%s\t%s\t%s\t%s\n' "${query}" "${bulk}" "${burst32}" "${gain}" >> "${tsv}"
    printf 'query=%3u  bulk=%8s  burst32=%8s  gain=%6s%%\n' \
        "${query}" "${bulk}" "${burst32}" "${gain}" >&2
done

python3 - "${tsv}" "${svg}" "${txt}" <<'PY'
import math
import sys

tsv_path, svg_path, txt_path = sys.argv[1:4]
rows = []
with open(tsv_path, "r", encoding="utf-8") as fh:
    next(fh)
    for line in fh:
        q, bulk, burst, gain = line.rstrip("\n").split("\t")
        rows.append((int(q), float(bulk), float(burst), float(gain)))

if not rows:
    raise SystemExit("no data")

queries = [r[0] for r in rows]
bulk_vals = [r[1] for r in rows]
burst_vals = [r[2] for r in rows]
all_vals = bulk_vals + burst_vals
y_min = min(all_vals)
y_max = max(all_vals)
if y_max <= y_min:
    y_max = y_min + 1.0

width = 1100
height = 560
left = 72
right = 32
top = 28
bottom = 56
plot_w = width - left - right
plot_h = height - top - bottom

def x_px(query: int) -> float:
    return left + (query - 1) * plot_w / 255.0

def y_px(value: float) -> float:
    return top + (y_max - value) * plot_h / (y_max - y_min)

def polyline(values, color):
    pts = " ".join(f"{x_px(q):.2f},{y_px(v):.2f}" for q, v in zip(queries, values))
    return f'<polyline fill="none" stroke="{color}" stroke-width="2.0" points="{pts}" />'

grid = []
for tick in range(6):
    y_val = y_min + (y_max - y_min) * tick / 5.0
    y = y_px(y_val)
    grid.append(f'<line x1="{left}" y1="{y:.2f}" x2="{width-right}" y2="{y:.2f}" stroke="#d8dde6" stroke-width="1" />')
    grid.append(f'<text x="{left-10}" y="{y+4:.2f}" font-size="12" text-anchor="end" fill="#333">{y_val:.1f}</text>')

x_ticks = []
for q in [1, 32, 64, 96, 128, 160, 192, 224, 256]:
    x = x_px(q)
    x_ticks.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{height-bottom}" stroke="#eef2f7" stroke-width="1" />')
    x_ticks.append(f'<text x="{x:.2f}" y="{height-bottom+22}" font-size="12" text-anchor="middle" fill="#333">{q}</text>')

svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">
<rect width="100%" height="100%" fill="#ffffff" />
<text x="{left}" y="18" font-size="20" font-weight="700" fill="#111">flow4 findadd_closed query sweep (1..256)</text>
<text x="{left}" y="{height-12}" font-size="12" fill="#444">query width</text>
<text x="18" y="{top-8}" font-size="12" fill="#444">cycles/key</text>
<rect x="{left}" y="{top}" width="{plot_w}" height="{plot_h}" fill="#fbfcfe" stroke="#c9d2de" />
{''.join(grid)}
{''.join(x_ticks)}
{polyline(bulk_vals, "#1f77b4")}
{polyline(burst_vals, "#d62728")}
<line x1="{left+10}" y1="{top+16}" x2="{left+42}" y2="{top+16}" stroke="#1f77b4" stroke-width="2.0" />
<text x="{left+50}" y="{top+20}" font-size="12" fill="#111">bulk</text>
<line x1="{left+110}" y1="{top+16}" x2="{left+142}" y2="{top+16}" stroke="#d62728" stroke-width="2.0" />
<text x="{left+150}" y="{top+20}" font-size="12" fill="#111">burst32</text>
</svg>
'''

with open(svg_path, "w", encoding="utf-8") as fh:
    fh.write(svg)

grid_w = 96
grid_h = 24
canvas = [[" " for _ in range(grid_w)] for _ in range(grid_h)]

def plot_ascii(values, mark):
    for q, v in zip(queries, values):
        x = round((q - 1) * (grid_w - 1) / 255.0)
        y = round((y_max - v) * (grid_h - 1) / (y_max - y_min))
        prev = canvas[y][x]
        canvas[y][x] = mark if prev == " " else "#"

plot_ascii(bulk_vals, "*")
plot_ascii(burst_vals, "+")

lines = []
for idx, row in enumerate(canvas):
    y_val = y_max - (y_max - y_min) * idx / (grid_h - 1)
    lines.append(f"{y_val:7.1f} |{''.join(row)}")
lines.append("        +" + "-" * grid_w)
lines.append("         1" + " " * (grid_w - 8) + "256")
lines.append("legend: * bulk, + burst32, # overlap")

with open(txt_path, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines) + "\n")
PY

echo "wrote ${tsv}"
echo "wrote ${svg}"
echo "wrote ${txt}"
cat "${txt}"
