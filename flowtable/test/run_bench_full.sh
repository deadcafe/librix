#!/usr/bin/env bash

set -u
set -o pipefail

bench=${BENCH:?}
bench_extra_full=${BENCH_EXTRA_FULL:?}
bench_full_name=${BENCH_FULL_NAME:-bench-full}
bench_ft_full_opts=${BENCH_FT_FULL_OPTS:-"--raw-repeat 7 --keep-n 5"}
bench_full_arches=${BENCH_FULL_ARCHES:-"gen sse avx2 avx512"}
bench_full_cores=${BENCH_FULL_CORES:-auto}
bench_full_cat_logs=${BENCH_FULL_CAT_LOGS:-1}
bench_full_entries=${BENCH_FULL_ENTRIES:-1048576}
bench_full_families=${BENCH_FULL_FAMILIES:-"flow4 flow6 flowu"}
bench_full_extra_families=${BENCH_FULL_EXTRA_FAMILIES:-"flow4_extra flow6_extra flowu_extra"}
bench_full_fills=${BENCH_FULL_FILLS:-"75 95"}
bench_full_ops=${BENCH_FULL_OPS:-}
bench_full_logroot=${BENCH_FULL_LOGDIR:-"../build/bench-full-logs"}
bench_full_logdir=$(printf '%s/%s-%s' "$bench_full_logroot" \
    "$(date +%Y%m%d-%H%M%S)" "$$")
bench_grow_fill=${BENCH_GROW_FILL:-60}
bench_maint_fill=${BENCH_MAINT_FILL:-50}
bench_queries=${BENCH_QUERIES:-"1 32 256"}
bench_extra_full_entries=${BENCH_EXTRA_FULL_ENTRIES:-$bench_full_entries}
bench_extra_full_reps=${BENCH_EXTRA_FULL_REPS:-7}
bench_full_run_extra=${BENCH_FULL_RUN_EXTRA:-1}
bench_full_run_maint=${BENCH_FULL_RUN_MAINT:-1}
bench_full_run_pure_maint=${BENCH_FULL_RUN_PURE_MAINT:-$bench_full_run_maint}
bench_full_run_extra_maint=${BENCH_FULL_RUN_EXTRA_MAINT:-$bench_full_run_maint}
bench_full_run_grow=${BENCH_FULL_RUN_GROW:-1}

list_from_var()
{
    local text=${1//,/ }
    printf '%s\n' $text
}

detect_physical_cores()
{
    if command -v lscpu >/dev/null 2>&1; then
        lscpu -p=CPU,CORE,SOCKET,ONLINE 2>/dev/null |
            awk -F, '!/^#/ && $4 == "Y" && !seen[$2 "," $3]++ { print $1 }'
        return
    fi

    local n
    n=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n')
    awk -v n="$n" 'BEGIN { for (i = 0; i < n; i++) print i }'
}

arch_supported()
{
    case "$1" in
    auto)
        return 0
        ;;
    gen)
        return 0
        ;;
    sse)
        grep -qw sse4_2 /proc/cpuinfo
        ;;
    avx2)
        grep -qw avx2 /proc/cpuinfo
        ;;
    avx512)
        grep -qw avx512f /proc/cpuinfo
        ;;
    *)
        printf 'unknown arch: %s\n' "$1" >&2
        return 1
        ;;
    esac
}

sanitize_label()
{
    printf '%s\n' "$1" | tr -c 'A-Za-z0-9_.=-' '_'
}

enabled()
{
    case "$1" in
    0|false|FALSE|no|NO|off|OFF)
        return 1
        ;;
    *)
        return 0
        ;;
    esac
}

readarray -t cores < <(
    if [ "$bench_full_cores" = auto ]; then
        detect_physical_cores
    else
        list_from_var "$bench_full_cores"
    fi
)

if [ "${#cores[@]}" -eq 0 ]; then
    printf '%s: no CPU cores selected\n' "$bench_full_name" >&2
    exit 1
fi

mkdir -p "$bench_full_logdir"

read -r -a ft_opts <<< "$bench_ft_full_opts"
read -r -a arches <<< "${bench_full_arches//,/ }"
read -r -a families <<< "${bench_full_families//,/ }"
read -r -a extra_families <<< "${bench_full_extra_families//,/ }"
read -r -a fills <<< "${bench_full_fills//,/ }"
read -r -a queries <<< "${bench_queries//,/ }"
read -r -a ops <<< "${bench_full_ops//,/ }"

if [ "${#cores[@]}" -eq 1 ]; then
    printf '== %s: pinned serial sweep ==\n' "$bench_full_name"
else
    printf '== %s: parallel physical-core sweep ==\n' "$bench_full_name"
fi
printf 'cores: %s\n' "${cores[*]}"
printf 'logs: %s\n' "$bench_full_logdir"
printf 'pure families: %s\n' "${families[*]:-(none)}"
if enabled "$bench_full_run_extra"; then
    printf 'extra families: %s\n' "${extra_families[*]:-(none)}"
else
    printf 'extra families: disabled\n'
fi
printf 'queries: %s\n' "${queries[*]:-(none)}"
printf 'ops: %s\n' "${ops[*]:-all}"
printf 'fills: %s\n' "${fills[*]:-(none)}"
printf 'maint: pure=%s extra=%s  grow: %s\n' \
    "$bench_full_run_pure_maint" "$bench_full_run_extra_maint" "$bench_full_run_grow"

active=0
job_seq=0
failed=0
taskset_bin=
free_cores=("${cores[@]}")
declare -A pid_core

cleanup_children()
{
    local pid

    for pid in "${!pid_core[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
}

trap cleanup_children EXIT
trap 'cleanup_children; exit 130' INT
trap 'cleanup_children; exit 143' TERM HUP

if command -v taskset >/dev/null 2>&1; then
    taskset_bin=$(command -v taskset)
fi

reap_one()
{
    local pid
    local status
    local core

    wait -n -p pid
    status=$?
    core=${pid_core[$pid]:-}
    if [ -n "$core" ]; then
        free_cores+=("$core")
        unset "pid_core[$pid]"
    fi
    active=$((active - 1))
    if [ "$status" -ne 0 ]; then
        failed=1
    fi
}

alloc_core()
{
    while [ "${#free_cores[@]}" -eq 0 ]; do
        reap_one
    done

    allocated_core=${free_cores[0]}
    free_cores=("${free_cores[@]:1}")
}

start_job()
{
    local core=$1
    local label=$2
    shift 2
    local slug
    local log
    local pid

    job_seq=$((job_seq + 1))
    slug=$(sanitize_label "$label")
    log=$(printf '%s/%04d-%s.log' "$bench_full_logdir" "$job_seq" "$slug")

    printf '[%04d] core=%s %s\n' "$job_seq" "$core" "$label"
    (
        printf '== %s ==\n' "$label"
        printf 'core: %s\n' "$core"
        "$@"
    ) >"$log" 2>&1 &
    pid=$!
    pid_core[$pid]=$core

    active=$((active + 1))
}

for arch in "${arches[@]}"; do
    if ! arch_supported "$arch"; then
        printf 'skip: CPU does not support %s\n' "$arch"
        continue
    fi

    for family in "${families[@]}"; do
        for fill in "${fills[@]}"; do
            for q in "${queries[@]}"; do
                if [ "${#ops[@]}" -eq 0 ]; then
                    label="ft_bench arch=$arch family=$family q=$q entries=$bench_full_entries fill=$fill"
                    alloc_core
                    start_job "$allocated_core" "$label" \
                        "$bench" "${ft_opts[@]}" --pin-core "$allocated_core" \
                        --arch "$arch" --query "$q" "$family" "$bench_full_entries" "$fill"
                else
                    for op in "${ops[@]}"; do
                        label="ft_bench arch=$arch family=$family op=$op q=$q entries=$bench_full_entries fill=$fill"
                        alloc_core
                        start_job "$allocated_core" "$label" \
                            "$bench" "${ft_opts[@]}" --pin-core "$allocated_core" \
                            --arch "$arch" --op "$op" --query "$q" \
                            "$family" "$bench_full_entries" "$fill"
                    done
                fi
            done
        done

        if enabled "$bench_full_run_pure_maint"; then
            label="ft_bench maint arch=$arch family=$family entries=$bench_full_entries fill=$bench_maint_fill"
            alloc_core
            start_job "$allocated_core" "$label" \
                "$bench" "${ft_opts[@]}" --pin-core "$allocated_core" \
                --arch "$arch" --maint "$family" "$bench_full_entries" "$bench_maint_fill"
        fi

        if enabled "$bench_full_run_grow"; then
            label="ft_bench grow arch=$arch family=$family entries=$bench_full_entries fill=$bench_grow_fill"
            alloc_core
            start_job "$allocated_core" "$label" \
                "$bench" "${ft_opts[@]}" --pin-core "$allocated_core" \
                --arch "$arch" --grow "$family" "$bench_full_entries" "$bench_grow_fill"
        fi
    done

    if ! enabled "$bench_full_run_extra"; then
        continue
    fi

    for family in "${extra_families[@]}"; do
        for fill in "${fills[@]}"; do
            for q in "${queries[@]}"; do
                label="ft_bench_extra_full arch=$arch family=$family q=$q entries=$bench_extra_full_entries fill=$fill"
                alloc_core
                if [ -n "$taskset_bin" ]; then
                    start_job "$allocated_core" "$label" \
                        "$taskset_bin" -c "$allocated_core" \
                        "$bench_extra_full" --arch "$arch" --query "$q" \
                        --reps "$bench_extra_full_reps" "$family" \
                        "$bench_extra_full_entries" "$fill"
                else
                    start_job "$allocated_core" "$label" \
                        "$bench_extra_full" --arch "$arch" --query "$q" \
                        --reps "$bench_extra_full_reps" "$family" \
                        "$bench_extra_full_entries" "$fill"
                fi
            done
        done

        if enabled "$bench_full_run_extra_maint"; then
            label="ft_bench_extra_full maint arch=$arch family=$family entries=$bench_extra_full_entries fill=$bench_maint_fill"
            alloc_core
            if [ -n "$taskset_bin" ]; then
                start_job "$allocated_core" "$label" \
                    "$taskset_bin" -c "$allocated_core" \
                    "$bench_extra_full" --arch "$arch" --maint \
                    --reps "$bench_extra_full_reps" "$family" \
                    "$bench_extra_full_entries" "$bench_maint_fill"
            else
                start_job "$allocated_core" "$label" \
                    "$bench_extra_full" --arch "$arch" --maint \
                    --reps "$bench_extra_full_reps" "$family" \
                    "$bench_extra_full_entries" "$bench_maint_fill"
            fi
        fi

        if enabled "$bench_full_run_grow"; then
            label="ft_bench_extra_full grow arch=$arch family=$family entries=$bench_extra_full_entries fill=$bench_grow_fill"
            alloc_core
            if [ -n "$taskset_bin" ]; then
                start_job "$allocated_core" "$label" \
                    "$taskset_bin" -c "$allocated_core" \
                    "$bench_extra_full" --arch "$arch" --grow \
                    --reps "$bench_extra_full_reps" "$family" \
                    "$bench_extra_full_entries" "$bench_grow_fill"
            else
                start_job "$allocated_core" "$label" \
                    "$bench_extra_full" --arch "$arch" --grow \
                    --reps "$bench_extra_full_reps" "$family" \
                    "$bench_extra_full_entries" "$bench_grow_fill"
            fi
        fi
    done
done

while [ "$active" -gt 0 ]; do
    reap_one
done

if [ "$bench_full_cat_logs" != 0 ]; then
    for log in "$bench_full_logdir"/*.log; do
        [ -e "$log" ] || continue
        printf '\n'
        awk 'NF { print }' "$log"
    done
fi

if [ "$failed" -ne 0 ]; then
    printf '%s: one or more jobs failed; see %s\n' "$bench_full_name" "$bench_full_logdir" >&2
    exit 1
fi

printf '%s: completed %u jobs\n' "$bench_full_name" "$job_seq"
