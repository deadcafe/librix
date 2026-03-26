#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BENCH="$SCRIPT_DIR/fc_bench"
BENCH_DIR="$SCRIPT_DIR"
VARIANT="${1:-flow4}"
BENCH_SAMPLES="${BENCH_SAMPLES:-3}"
BENCH_FC_OPTS="${BENCH_FC_OPTS:---pin-core 0 --raw-repeat 7 --keep-n 5}"

if [ ! -x "$BENCH" ]; then
    make -C "$BENCH_DIR" fc_bench
fi

usage() {
    cat <<EOF
usage: $0 [variant] [profile]

variants:
  flow4   fc flow4 matrix
  flow6   fc flow6 matrix
  flowu   fc flowu matrix
  all     run all 3 variants

profiles:
  quick   representative open-set window matrix (default)
  full    full trace matrix including long-running trace_open_custom cases

environment:
  BENCH_FC_OPTS  extra fc_bench options (default: "$BENCH_FC_OPTS")
  BENCH_SAMPLES  outer repeated-run count (default: $BENCH_SAMPLES)
EOF
}

run_bench() {
    echo
    echo ">>> $VARIANT $*  [opts: $BENCH_FC_OPTS]"
    # shellcheck disable=SC2086
    "$BENCH" $BENCH_FC_OPTS "$VARIANT" "$@"
}

run_bench_median() {
    tmp_file=$(mktemp)
    run_idx=1

    echo
    echo ">>> $VARIANT $*  (median of ${BENCH_SAMPLES} runs)"

    while [ "$run_idx" -le "$BENCH_SAMPLES" ]; do
        # shellcheck disable=SC2086
        output=$("$BENCH" $BENCH_FC_OPTS "$VARIANT" "$@")
        summary=$(printf '%s\n' "$output" | awk '/^(fc :|summary:)/{line=$0} END{print line}')
        cycles=$(printf '%s\n' "$summary" \
            | sed -n 's/.*cycles\/key= *\([0-9.][0-9.]*\).*/\1/p')
        if [ -z "$summary" ] || [ -z "$cycles" ]; then
            printf '%s\n' "$output"
            rm -f "$tmp_file"
            echo "failed to extract summary line for median run" >&2
            exit 2
        fi
        printf '%s\t%s\n' "$cycles" "$summary" >> "$tmp_file"
        run_idx=$((run_idx + 1))
    done

    sort -n "$tmp_file" | awk -F '\t' '
        { rows[++n] = $0 }
        END {
            mid = int((n + 1) / 2)
            split(rows[mid], a, "\t")
            printf("median[%d]: %s\n", n, a[2])
        }'
    rm -f "$tmp_file"
}

# Accepted batch-maint policy:
#   thresholds: 70/73/75/77
#   kicks:      0/0/1/2
TRACE_POLICY_FILL0=70
TRACE_POLICY_FILL1=73
TRACE_POLICY_FILL2=75
TRACE_POLICY_FILL3=77
TRACE_POLICY_KICK0=0
TRACE_POLICY_KICK1=0
TRACE_POLICY_KICK2=1
TRACE_POLICY_KICK3=2
TRACE_POLICY_SCALE=1

run_trace_case() {
    desired_entries="$1"
    start_fill_pct="$2"
    hit_pct="$3"
    keys_per_sec="$4"

    run_bench_median trace_open_custom \
        "$desired_entries" "$start_fill_pct" "$hit_pct" "$keys_per_sec" \
        8000 3 2000 \
        "$TRACE_POLICY_FILL0" "$TRACE_POLICY_FILL1" \
        "$TRACE_POLICY_FILL2" "$TRACE_POLICY_FILL3" \
        "$TRACE_POLICY_KICK0" "$TRACE_POLICY_KICK1" \
        "$TRACE_POLICY_KICK2" "$TRACE_POLICY_KICK3" \
        "$TRACE_POLICY_SCALE"
}

PROFILE="${2:-quick}"

run_variant_quick() {
    echo
    echo "====== $VARIANT matrix ======"

    echo
    echo "== Windowed Open-set: 1M / 500k keys/s / fill 60-75% =="
    run_bench_median findadd_window 1000000 60 75 95 500000 1000 1000
    run_bench_median findadd_window 1000000 60 75 90 500000 1000 1000
    run_bench_median findadd_window 1000000 60 75 80 500000 1000 1000

}

run_variant_full() {
    echo
    echo "====== $VARIANT matrix ======"

    echo
    echo "== Windowed Open-set: 1M / 500k keys/s / fill 60-75% =="
    run_bench_median findadd_window 1000000 60 75 95 500000 1000 1000
    run_bench_median findadd_window 1000000 60 75 90 500000 1000 1000
    run_bench_median findadd_window 1000000 60 75 80 500000 1000 1000

    echo
    echo "== Trace Policy: 1M / 500k keys/s =="
    run_trace_case 1000000 60 95 500000
    run_trace_case 1000000 60 90 500000
    run_trace_case 1000000 60 80 500000
    run_trace_case 1000000 60 70 500000
    run_trace_case 1000000 75 95 500000
    run_trace_case 1000000 75 90 500000
    run_trace_case 1000000 85 95 500000
    run_trace_case 1000000 85 90 500000

    echo
    echo "== Trace Policy: 2M / 500k keys/s =="
    run_trace_case 2000000 75 95 500000
    run_trace_case 2000000 85 90 500000

    echo
    echo "== Trace Policy: 2M / 1M keys/s =="
    run_trace_case 2000000 75 95 1000000
    run_trace_case 2000000 75 90 1000000
    run_trace_case 2000000 75 80 1000000
}

if [ "$VARIANT" = "-h" ] || [ "$VARIANT" = "--help" ]; then
    usage
    exit 0
fi

case "$VARIANT" in
    flow4|flow6|flowu)
        case "$PROFILE" in
            quick) run_variant_quick ;;
            full)  run_variant_full ;;
            *)
                echo "unknown profile: $PROFILE" >&2
                usage >&2
                exit 2
                ;;
        esac
        ;;
    all)
        for v in flow4 flow6 flowu; do
            VARIANT="$v"
            case "$PROFILE" in
                quick) run_variant_quick ;;
                full)  run_variant_full ;;
                *)
                    echo "unknown profile: $PROFILE" >&2
                    usage >&2
                    exit 2
                    ;;
            esac
        done
        ;;
    *)
        echo "unknown variant: $VARIANT" >&2
        usage >&2
        exit 2
        ;;
esac
