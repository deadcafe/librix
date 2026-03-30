#!/bin/sh
# run_fc_bench_all.sh — run tests and benchmarks for all arch tiers,
#                        save results to file as evidence.
#
# Usage:
#   ./run_fc_bench_all.sh              # auto-detect, run all available
#   ./run_fc_bench_all.sh gen sse      # run only specified tiers
#   ./run_fc_bench_all.sh -o outdir    # change output directory
#
# Output: bench_results/<hostname>_<timestamp>.txt
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUTDIR="$SCRIPT_DIR/bench_results"
ALL_ARCHS="gen sse avx2 avx512"
ARCHS=""
TASKSET=""

# Parse options
while [ $# -gt 0 ]; do
    case "$1" in
        -o)
            OUTDIR="$2"; shift 2 ;;
        -h|--help)
            cat <<EOF
usage: $0 [-o outdir] [arch ...]

arch: gen sse avx2 avx512 (default: all available)

Runs functional tests and datapath/maint benchmarks for each arch tier.
Results are saved to <outdir>/<hostname>_<timestamp>.txt

Options:
  -o outdir   Output directory (default: bench_results/)
  -h          Show this help
EOF
            exit 0 ;;
        gen|sse|avx2|avx512)
            ARCHS="$ARCHS $1"; shift ;;
        *)
            echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# Default: all archs
ARCHS="${ARCHS:-$ALL_ARCHS}"

# Use taskset if available
if command -v taskset >/dev/null 2>&1; then
    TASKSET="taskset -c 0"
fi

mkdir -p "$OUTDIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
HOST=$(hostname)
OUTFILE="${OUTDIR}/${HOST}_${TIMESTAMP}.txt"

# Collect system info
collect_sysinfo() {
    echo "=== Flowcache Benchmark Results ==="
    echo "Host:     $HOST"
    echo "Date:     $(date -Iseconds)"
    if [ -f /proc/cpuinfo ]; then
        echo "CPU:     $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | sed 's/^ //')"
    fi
    echo "Compiler: $(cc --version | head -1)"
    echo "Archs:   $ARCHS"
    echo ""
}

# Run functional tests for one arch
run_tests() {
    _arch="$1"
    _test="$SCRIPT_DIR/fc_test_${_arch}"

    if [ ! -x "$_test" ]; then
        echo "  (fc_test_${_arch} not found — skipped)"
        return
    fi

    echo "  [${_arch}] functional test"
    if "$_test" 2>&1; then
        echo "  [${_arch}] PASS"
    else
        echo "  [${_arch}] FAIL (exit=$?)"
    fi
}

# Run benchmarks for one arch
run_bench() {
    _arch="$1"
    _bench="$SCRIPT_DIR/fc_bench_${_arch}"

    if [ ! -x "$_bench" ]; then
        echo "  (fc_bench_${_arch} not found — skipped)"
        return
    fi

    echo "  [${_arch}] datapath"
    $TASKSET "$_bench" datapath 2>&1
    echo ""

    echo "  [${_arch}] maint"
    $TASKSET "$_bench" maint 2>&1
    echo ""

    echo "  [${_arch}] maint_partial"
    $TASKSET "$_bench" maint_partial 2>&1
}

# Also support current single-arch binaries (fc_test / fc_bench)
run_current() {
    _test="$SCRIPT_DIR/fc_test"
    _bench="$SCRIPT_DIR/fc_bench"

    if [ -x "$_test" ] || [ -x "$_bench" ]; then
        echo "--- current (single-arch) build ---"
        if [ -x "$_test" ]; then
            echo "  functional test"
            if "$_test" 2>&1; then
                echo "  PASS"
            else
                echo "  FAIL (exit=$?)"
            fi
        fi
        if [ -x "$_bench" ]; then
            echo ""
            echo "  datapath"
            $TASKSET "$_bench" datapath 2>&1
            echo ""
            echo "  maint"
            $TASKSET "$_bench" maint 2>&1
            echo ""
            echo "  maint_partial"
            $TASKSET "$_bench" maint_partial 2>&1
        fi
        echo ""
    fi
}

# Main
{
    collect_sysinfo

    # Run per-arch binaries if they exist
    _found_arch=false
    for arch in $ARCHS; do
        if [ -x "$SCRIPT_DIR/fc_test_${arch}" ] || \
           [ -x "$SCRIPT_DIR/fc_bench_${arch}" ]; then
            _found_arch=true
            echo "--- arch: ${arch} ---"
            run_tests "$arch"
            echo ""
            run_bench "$arch"
            echo ""
        fi
    done

    # Fallback: run current single-arch binaries
    if [ "$_found_arch" = false ]; then
        run_current
    fi

    echo "=== Done ==="
} | tee "$OUTFILE"

echo ""
echo "Results saved to: $OUTFILE"
