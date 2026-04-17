/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

/*
 * sim_flow_cache.c — Flow cache simulation
 *
 * Simulates a flow cache backed by the flowtable library under realistic
 * packet-processing conditions.  Parameters such as PPS, batch size,
 * miss rate, timeout, and maintenance expiration capacity are adjustable via the
 * command line so that the user can find the operating point where the
 * cache stabilises at 65–75 % fill.
 *
 * Simulated (wall-clock-free) time is used so the simulation runs as
 * fast as the CPU allows.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/mman.h>

#include "flow_table.h"
#include "ft_test_record_allocator.h"

/*===========================================================================
 * Constants
 *===========================================================================*/

enum {
    SIM_DEFAULT_PPS         = 2000000u,
    SIM_DEFAULT_BATCH       = 256u,
    SIM_DEFAULT_POOL_SIZE   = 1048576u,
    SIM_DEFAULT_MISS_PCT    = 3u,
    SIM_DEFAULT_DURATION    = 60u,
    SIM_DEFAULT_MAINT_MAX_EXPIRED = 4096u,
    SIM_DEFAULT_MAINT_BUDGET      = 32u,
    SIM_MAX_BATCH           = 4096u,
    SIM_ALIGN               = 64u,

    /* --ddos-burst defaults */
    SIM_DEFAULT_LEGIT_POOL        = 600000u,
    SIM_DEFAULT_ATTACK_PCT        = 50u,
    SIM_DEFAULT_BURST_ON          = 5u,
    SIM_DEFAULT_BURST_OFF         = 15u,
    SIM_DEFAULT_DOS_THRESHOLD     = 15u,
    SIM_DEFAULT_NORMAL_THRESHOLD  = 5u,
};

#define SIM_DEFAULT_TIMEOUT_SEC      10.0
#define SIM_DEFAULT_DOS_TIMEOUT_SEC   2.0

/*===========================================================================
 * Miss-rate variation profiles
 *===========================================================================*/

enum sim_miss_vary {
    SIM_MISS_FIXED = 0,  /* constant miss rate (default) */
    SIM_MISS_STEP,       /* alternate low/high every 10s */
    SIM_MISS_SINE,       /* sine wave, period 20s */
    SIM_MISS_BURST,      /* baseline + 2s bursts every 8s */
};

/*===========================================================================
 * DDoS receiver mode (--ddos-burst)
 *===========================================================================*/

enum sim_dos_state {
    SIM_DOS_NORMAL = 0,
    SIM_DOS_ACTIVE = 1,
};

/*===========================================================================
 * Configuration
 *===========================================================================*/

struct sim_config {
    unsigned pps;
    unsigned batch_size;
    unsigned pool_size;
    unsigned miss_pct;
    double   timeout_sec;
    unsigned duration_sec;
    unsigned maint_max_expired;
    unsigned maint_budget;   /* adaptive maint work budget */
    int      pin_core;
    unsigned arch_enable;
    int      use_hugepage;
    int      use_force_expire;
    int      inline_maint;            /* 1 = combine add + maint timing */
    unsigned min_bucket_occupancy;    /* inline-maint bucket occupancy gate */
    unsigned fill_target_pct;    /* 0 = disabled (fixed TO), 1..100 = auto */
    enum sim_miss_vary miss_vary;

    /* --ddos-burst */
    int      ddos_burst;
    unsigned legit_pool_size;
    unsigned attack_pct;           /* attack flow % of batch during burst */
    unsigned burst_on_sec;
    unsigned burst_off_sec;
    unsigned dos_threshold_pct;    /* miss rate → DoS mode */
    unsigned normal_threshold_pct; /* miss rate → Normal mode */
    double   dos_timeout_sec;
};

/*===========================================================================
 * User record
 *===========================================================================*/

struct sim_record {
    struct flow4_entry entry;
    u32 active_pos;
    unsigned char pad[32];
} __attribute__((aligned(FT_TABLE_CACHE_LINE_SIZE)));

/*===========================================================================
 * Active set — O(1) add / remove / random-access
 *===========================================================================*/

struct active_set {
    u32     *indices;
    unsigned count;
    unsigned capacity;
};

static int
active_init(struct active_set *as, unsigned capacity)
{
    as->indices = calloc(capacity, sizeof(*as->indices));
    if (as->indices == NULL)
        return -1;
    as->count = 0u;
    as->capacity = capacity;
    return 0;
}

static void
active_destroy(struct active_set *as)
{
    free(as->indices);
    as->indices = NULL;
    as->count = 0u;
}

static void
active_add(struct active_set *as, u32 entry_idx, struct sim_record *records)
{
    unsigned pos = as->count;

    as->indices[pos] = entry_idx;
    records[entry_idx - 1u].active_pos = pos;
    as->count++;
}

static void
active_remove(struct active_set *as, u32 entry_idx, struct sim_record *records)
{
    unsigned pos = records[entry_idx - 1u].active_pos;
    unsigned last;

    if (pos == UINT32_MAX || pos >= as->count)
        return;
    last = as->count - 1u;
    if (pos != last) {
        u32 moved = as->indices[last];

        as->indices[pos] = moved;
        records[moved - 1u].active_pos = pos;
    }
    records[entry_idx - 1u].active_pos = UINT32_MAX;
    as->count--;
}

/*===========================================================================
 * xorshift64 PRNG
 *===========================================================================*/

static u64 rng_state = UINT64_C(0xdeadcafe12345678);

static inline u64
rng_next(void)
{
    u64 x = rng_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

/*===========================================================================
 * Key generation (same scheme as bench_flow_table.c)
 *===========================================================================*/

static struct flow4_key
sim_make_key(unsigned i)
{
    struct flow4_key k;

    memset(&k, 0, sizeof(k));
    k.family   = 2u;
    k.proto    = (u8)(6u + (i & 1u));
    k.src_port = (u16)(1024u + (i & 0x7fffu));
    k.dst_port = (u16)(2048u + ((i >> 11) & 0x7fffu));
    k.vrfid    = 1u + (i >> 24);
    k.src_ip   = UINT32_C(0x0a000000) | (i & 0x00ffffffu);
    k.dst_ip   = UINT32_C(0x14000000)
               | ((i * UINT32_C(2654435761)) & 0x00ffffffu);
    return k;
}

/*===========================================================================
 * rdtsc (cycle measurement)
 *===========================================================================*/

static inline u64
sim_rdtsc(void)
{
#if defined(__x86_64__)
    u32 lo, hi;

    __asm__ volatile("lfence\n\trdtsc\n\tlfence"
                     : "=a"(lo), "=d"(hi) : : "memory");
    return ((u64)hi << 32) | lo;
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
#endif
}

/*===========================================================================
 * Memory allocation helpers
 *===========================================================================*/

static int g_use_hugepage = 1;

static size_t
sim_round_up(size_t n, size_t align)
{
    return (n + align - 1u) & ~(align - 1u);
}

static void *
sim_mmap_huge(size_t size)
{
    size_t hp = (size_t)2u * 1024u * 1024u;
    size_t rounded = sim_round_up(size, hp);
    void *ptr;

    ptr = mmap(NULL, rounded, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (ptr == MAP_FAILED) {
        ptr = mmap(NULL, rounded, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED)
            return NULL;
        (void)madvise(ptr, rounded, MADV_HUGEPAGE);
        for (size_t off = 0; off < rounded; off += 4096u)
            ((volatile char *)ptr)[off] = 0;
    }
    return ptr;
}

static void
sim_munmap_huge(void *ptr, size_t size)
{
    size_t hp = (size_t)2u * 1024u * 1024u;

    munmap(ptr, sim_round_up(size, hp));
}

static void *
sim_alloc_zero(size_t count, size_t elem_size)
{
    size_t total = count * elem_size;

    if (g_use_hugepage) {
        void *ptr = sim_mmap_huge(total);

        if (ptr == NULL)
            exit(1);
        return ptr;
    }
    {
        size_t rounded = sim_round_up(total, SIM_ALIGN);
        void *ptr = aligned_alloc(SIM_ALIGN, rounded);

        if (ptr == NULL) {
            perror("aligned_alloc");
            exit(1);
        }
        memset(ptr, 0, rounded);
        return ptr;
    }
}

static void
sim_free_zero(void *ptr, size_t count, size_t elem_size)
{
    if (g_use_hugepage)
        sim_munmap_huge(ptr, count * elem_size);
    else
        free(ptr);
}

static void *
sim_alloc_buckets(size_t size)
{
    if (g_use_hugepage)
        return sim_mmap_huge(size);
    return aligned_alloc(SIM_ALIGN, size);
}

static void
sim_free_buckets(void *ptr, size_t size)
{
    if (g_use_hugepage)
        sim_munmap_huge(ptr, size);
    else
        free(ptr);
}

/*===========================================================================
 * Per-interval statistics
 *===========================================================================*/

struct sim_interval_stats {
    u64 finds;          /* keys processed by add_idx_bulk */
    u64 hits;           /* existing entries (duplicates) */
    u64 adds;           /* newly inserted entries */
    u64 add_skipped;    /* alloc failures */
    u64 maint_expired;
    u64 force_expired;
    u64 add_cycles;
    u64 maint_cycles;
    u64 attack_adds;    /* ddos-burst: attack flow insertions */
};

struct sim_total_stats {
    u64 batches;
    u64 finds;
    u64 hits;
    u64 adds;
    u64 add_skipped;
    u64 maint_expired;
    u64 force_expired;
    u64 add_cycles;
    u64 maint_cycles;
    u64 attack_adds;
};

/*===========================================================================
 * Generator / receiver split
 *===========================================================================*/

struct sim_batch {
    unsigned total_keys;
    unsigned legit_prefix_len;
    unsigned miss_keys;
    int is_burst;
};

struct sim_generator {
    const struct sim_config *cfg;
    struct flow4_key *keys;
    struct flow4_key *legit_keys;
    u32 next_key_id;
    u32 next_attack_key_id;
    unsigned miss_count;
    unsigned hit_count;
};

struct sim_receiver {
    struct ft_table ft;
    struct ft_record_allocator alloc;
    struct sim_record *records;
    struct active_set active;
    void *buckets;
    size_t bucket_size;
    unsigned max_entries;
    enum ft_add_policy add_policy;

    u32 *new_idxv;
    u32 *orig_idxv;
    u32 *unused_idxv;
    u32 *expired_idxv;
    u32 *hit_idxv;
    u32 *work_idxv;
    unsigned nb_work;
};

/*===========================================================================
 * Option parsing
 *===========================================================================*/

static void
print_usage(FILE *out, const char *prog)
{
    fprintf(out,
        "usage: %s [options]\n"
        "\n"
        "Simulate a flow cache using the flowtable library.\n"
        "\n"
        "options:\n"
        "  --pps N             packets per second        [%u]\n"
        "  --batch N           batch size                [%u]\n"
        "  --pool N            pool (max entries)        [%u]\n"
        "  --miss N            miss %% per batch          [%u]\n"
        "  --miss-vary MODE    none|step|sine|burst     [none]\n"
        "  --timeout SEC       flow timeout (seconds)    [%.1f]\n"
        "  --duration SEC      simulation length         [%u]\n"
        "  --maint-max-expired N  maintain max_expired   [%u]\n"
        "  --maint-budget N       adaptive maint budget  [%u]\n"
        "  --fill-target PCT   auto-tune TO for target fill [off]\n"
        "  --inline-maint      combine add+maint timing    [off]\n"
        "  --min-bucket-occupancy N inline-maint bucket gate [10]\n"
        "  --no-force-expire   disable FORCE_EXPIRE       [on]\n"
        "  --arch ARCH         gen|sse|avx2|avx512|auto  [auto]\n"
        "  --pin-core N        pin to CPU core\n"
        "  --no-hugepage       disable hugepages\n"
        "  --help              show this help\n"
        "\n"
        "DDoS burst simulation (--ddos-burst):\n"
        "  --ddos-burst        enable DDoS burst mode       [off]\n"
        "  --legit-pool N      legitimate flow pool size     [%u]\n"
        "  --attack-pct N      attack %% of batch in burst   [%u]\n"
        "  --burst-on N        burst duration (seconds)      [%u]\n"
        "  --burst-off N       gap between bursts (seconds)  [%u]\n"
        "  --dos-threshold N   miss %% to enter DoS mode     [%u]\n"
        "  --normal-threshold N miss %% to exit DoS mode     [%u]\n"
        "  --dos-timeout SEC   timeout in DoS mode           [%.1f]\n",
        prog,
        SIM_DEFAULT_PPS, SIM_DEFAULT_BATCH, SIM_DEFAULT_POOL_SIZE,
        SIM_DEFAULT_MISS_PCT, SIM_DEFAULT_TIMEOUT_SEC,
        SIM_DEFAULT_DURATION, SIM_DEFAULT_MAINT_MAX_EXPIRED,
        SIM_DEFAULT_MAINT_BUDGET,
        SIM_DEFAULT_LEGIT_POOL, SIM_DEFAULT_ATTACK_PCT,
        SIM_DEFAULT_BURST_ON, SIM_DEFAULT_BURST_OFF,
        SIM_DEFAULT_DOS_THRESHOLD, SIM_DEFAULT_NORMAL_THRESHOLD,
        SIM_DEFAULT_DOS_TIMEOUT_SEC);
}

static unsigned
parse_arch_flag(const char *name)
{
    if (strcmp(name, "gen") == 0)    return FT_ARCH_GEN;
    if (strcmp(name, "sse") == 0)    return FT_ARCH_SSE;
    if (strcmp(name, "avx2") == 0)   return FT_ARCH_AVX2;
    if (strcmp(name, "avx512") == 0) return FT_ARCH_AVX512;
    if (strcmp(name, "auto") == 0)   return FT_ARCH_AUTO;
    fprintf(stderr, "unknown arch: %s\n", name);
    exit(1);
}

static enum sim_miss_vary
parse_miss_vary(const char *name)
{
    if (strcmp(name, "none") == 0)  return SIM_MISS_FIXED;
    if (strcmp(name, "step") == 0)  return SIM_MISS_STEP;
    if (strcmp(name, "sine") == 0)  return SIM_MISS_SINE;
    if (strcmp(name, "burst") == 0) return SIM_MISS_BURST;
    fprintf(stderr, "unknown miss-vary mode: %s\n", name);
    exit(1);
}

static const char *
miss_vary_label(enum sim_miss_vary v)
{
    switch (v) {
    case SIM_MISS_FIXED: return "none";
    case SIM_MISS_STEP:  return "step";
    case SIM_MISS_SINE:  return "sine";
    case SIM_MISS_BURST: return "burst";
    }
    return "?";
}

/*
 * Compute the effective miss percentage at a given simulation time.
 *
 *   step  — alternates between base/3 and base×3 every 10 s.
 *   sine  — sine wave, period 20 s, range [base×0.2 .. base×2.0].
 *   burst — baseline rate with 2 s burst at 5×base every 8 s.
 */
static unsigned
sim_current_miss_pct(unsigned base, enum sim_miss_vary vary, double sim_sec)
{
    switch (vary) {
    case SIM_MISS_STEP: {
        int phase = ((int)sim_sec / 10) & 1;
        unsigned lo = base / 3u;
        unsigned hi = base * 5u / 3u;

        if (lo < 1u) lo = 1u;
        return phase ? hi : lo;
    }
    case SIM_MISS_SINE: {
        double mid = (double)base;
        double amp = mid * 0.8;
        double v = mid + amp * sin(2.0 * M_PI * sim_sec / 20.0);

        if (v < 1.0) v = 1.0;
        if (v > 50.0) v = 50.0;
        return (unsigned)(v + 0.5);
    }
    case SIM_MISS_BURST: {
        unsigned sec = (unsigned)sim_sec;

        if ((sec % 8u) >= 6u)  /* 2 s burst every 8 s */
            return base * 5u > 50u ? 50u : base * 5u;
        return base;
    }
    default:
        return base;
    }
}

static int
parse_options(int argc, char **argv, struct sim_config *cfg)
{
    static const struct option long_opts[] = {
        { "pps",             required_argument, NULL, 'P' },
        { "batch",           required_argument, NULL, 'b' },
        { "pool",            required_argument, NULL, 'n' },
        { "miss",            required_argument, NULL, 'm' },
        { "miss-vary",       required_argument, NULL, 'V' },
        { "timeout",         required_argument, NULL, 't' },
        { "duration",        required_argument, NULL, 'd' },
        { "maint-max-expired", required_argument, NULL, 's' },
        { "maint-budget",      required_argument, NULL, 'k' },
        { "fill-target",     required_argument, NULL, 'T' },
        { "inline-maint",    no_argument,       NULL, 'W' },
        { "min-bucket-occupancy", required_argument, NULL, 'B' },
        { "no-force-expire", no_argument,       NULL, 'F' },
        { "arch",            required_argument, NULL, 'a' },
        { "pin-core",        required_argument, NULL, 'p' },
        { "no-hugepage",     no_argument,       NULL, 'H' },
        { "ddos-burst",      no_argument,       NULL, 'D' },
        { "legit-pool",      required_argument, NULL, 'L' },
        { "attack-pct",      required_argument, NULL, 'A' },
        { "burst-on",        required_argument, NULL, 'o' },
        { "burst-off",       required_argument, NULL, 'O' },
        { "dos-threshold",   required_argument, NULL, 'x' },
        { "normal-threshold", required_argument, NULL, 'y' },
        { "dos-timeout",     required_argument, NULL, 'z' },
        { "help",            no_argument,       NULL, 'h' },
        { NULL,              0,                 NULL,  0  }
    };
    int opt;

    cfg->pps             = SIM_DEFAULT_PPS;
    cfg->batch_size      = SIM_DEFAULT_BATCH;
    cfg->pool_size       = SIM_DEFAULT_POOL_SIZE;
    cfg->miss_pct        = SIM_DEFAULT_MISS_PCT;
    cfg->timeout_sec     = SIM_DEFAULT_TIMEOUT_SEC;
    cfg->duration_sec    = SIM_DEFAULT_DURATION;
    cfg->maint_max_expired = SIM_DEFAULT_MAINT_MAX_EXPIRED;
    cfg->maint_budget      = SIM_DEFAULT_MAINT_BUDGET;
    cfg->pin_core        = -1;
    cfg->arch_enable     = FT_ARCH_AUTO;
    cfg->use_hugepage    = 1;
    cfg->use_force_expire = 1;
    cfg->inline_maint         = 0;
    cfg->min_bucket_occupancy = 10u;
    cfg->fill_target_pct  = 0;
    cfg->miss_vary        = SIM_MISS_FIXED;

    cfg->ddos_burst           = 0;
    cfg->legit_pool_size      = SIM_DEFAULT_LEGIT_POOL;
    cfg->attack_pct           = SIM_DEFAULT_ATTACK_PCT;
    cfg->burst_on_sec         = SIM_DEFAULT_BURST_ON;
    cfg->burst_off_sec        = SIM_DEFAULT_BURST_OFF;
    cfg->dos_threshold_pct    = SIM_DEFAULT_DOS_THRESHOLD;
    cfg->normal_threshold_pct = SIM_DEFAULT_NORMAL_THRESHOLD;
    cfg->dos_timeout_sec      = SIM_DEFAULT_DOS_TIMEOUT_SEC;

    while ((opt = getopt_long(argc, argv,
                              "P:b:n:m:V:t:d:s:k:T:WFa:p:HDL:A:o:O:x:y:z:h",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'P': cfg->pps = (unsigned)strtoul(optarg, NULL, 10);      break;
        case 'b':
            cfg->batch_size = (unsigned)strtoul(optarg, NULL, 10);
            if (cfg->batch_size == 0u || cfg->batch_size > SIM_MAX_BATCH) {
                fprintf(stderr, "batch must be 1..%u\n", SIM_MAX_BATCH);
                return -1;
            }
            break;
        case 'n': cfg->pool_size = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'm':
            cfg->miss_pct = (unsigned)strtoul(optarg, NULL, 10);
            if (cfg->miss_pct > 100u) {
                fprintf(stderr, "miss must be 0..100\n");
                return -1;
            }
            break;
        case 'V': cfg->miss_vary = parse_miss_vary(optarg);               break;
        case 't':
            cfg->timeout_sec = strtod(optarg, NULL);
            if (cfg->timeout_sec <= 0.0) {
                fprintf(stderr, "timeout must be positive\n");
                return -1;
            }
            break;
        case 'd': cfg->duration_sec = (unsigned)strtoul(optarg, NULL, 10); break;
        case 's': cfg->maint_max_expired = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'k': cfg->maint_budget = (unsigned)strtoul(optarg, NULL, 10);      break;
        case 'T':
            cfg->fill_target_pct = (unsigned)strtoul(optarg, NULL, 10);
            if (cfg->fill_target_pct == 0u || cfg->fill_target_pct > 100u) {
                fprintf(stderr, "fill-target must be 1..100\n");
                return -1;
            }
            break;
        case 'W': cfg->inline_maint = 1;                                        break;
        case 'B': cfg->min_bucket_occupancy = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'F': cfg->use_force_expire = 0;                              break;
        case 'a': cfg->arch_enable = parse_arch_flag(optarg);              break;
        case 'p': cfg->pin_core = (int)strtol(optarg, NULL, 10);          break;
        case 'H': cfg->use_hugepage = 0;                                  break;
        case 'D': cfg->ddos_burst = 1;                                    break;
        case 'L': cfg->legit_pool_size = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'A':
            cfg->attack_pct = (unsigned)strtoul(optarg, NULL, 10);
            if (cfg->attack_pct > 100u) {
                fprintf(stderr, "attack-pct must be 0..100\n");
                return -1;
            }
            break;
        case 'o': cfg->burst_on_sec  = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'O': cfg->burst_off_sec = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'x': cfg->dos_threshold_pct = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'y': cfg->normal_threshold_pct = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'z':
            cfg->dos_timeout_sec = strtod(optarg, NULL);
            if (cfg->dos_timeout_sec <= 0.0) {
                fprintf(stderr, "dos-timeout must be positive\n");
                return -1;
            }
            break;
        case 'h': print_usage(stdout, argv[0]); exit(0);
        default:  print_usage(stderr, argv[0]); return -1;
        }
    }
    return 0;
}

/*===========================================================================
 * Helpers
 *===========================================================================*/

static unsigned
roundup_pow2(unsigned v)
{
    if (v <= 1u) return 1u;
    v--;
    v |= v >> 1;  v |= v >> 2;
    v |= v >> 4;  v |= v >> 8;
    v |= v >> 16;
    return v + 1u;
}

static const char *
arch_label(unsigned enable)
{
    if (enable == FT_ARCH_AUTO)  return "auto";
    if (enable & FT_ARCH_AVX512) return "avx512";
    if (enable & FT_ARCH_AVX2)   return "avx2";
    if (enable & FT_ARCH_SSE)    return "sse";
    return "gen";
}

/*===========================================================================
 * Dynamic timeout controller
 *
 * Feed-forward:  T_ff = fill_target × N × ln(1 + H/r) / H
 *   where H = measured hit rate, r = measured new-flow rate, N = max_entries.
 * Proportional correction on fill error:
 *   correction = 1 − kp × (fill_actual − fill_target)
 * EMA smoothing:  T = α × T_new + (1−α) × T_prev
 *===========================================================================*/

struct sim_timeout_ctrl {
    double fill_target;   /* e.g. 0.70 */
    double ema_timeout;   /* smoothed timeout in seconds */
    double kp;            /* proportional gain */
    double alpha;         /* EMA coefficient */
    double min_to;        /* minimum timeout (seconds) */
    double max_to;        /* maximum timeout (seconds) */
};

static double
sim_fill_target_ratio(int auto_timeout, const struct sim_timeout_ctrl *tc)
{
    return auto_timeout ? tc->fill_target : 0.7;
}

static double
sim_fill_ratio(unsigned active_count, unsigned max_entries)
{
    if (max_entries == 0u)
        return 0.0;
    return (double)active_count / (double)max_entries;
}

static double
sim_fill_excess_ratio(double fill_ratio, double fill_target)
{
    if (fill_ratio <= fill_target || fill_target >= 1.0)
        return 0.0;
    return (fill_ratio - fill_target) / (1.0 - fill_target);
}

static unsigned
sim_unused_extra_capacity(const struct sim_config *cfg)
{
    return (cfg->maint_max_expired > cfg->maint_budget)
        ? cfg->maint_max_expired : cfg->maint_budget;
}

static unsigned
sim_inline_maint_unused_budget(const struct sim_config *cfg,
                               unsigned active_count,
                               unsigned max_entries,
                               int auto_timeout,
                               const struct sim_timeout_ctrl *tc)
{
    double fill_ratio;
    double fill_target;
    double excess;

    if (cfg->maint_budget == 0u)
        return 0u;
    fill_ratio = sim_fill_ratio(active_count, max_entries);
    fill_target = sim_fill_target_ratio(auto_timeout, tc);
    excess = sim_fill_excess_ratio(fill_ratio, fill_target);
    return (unsigned)((double)cfg->maint_budget * excess);
}

static unsigned
sim_separate_maint_budget(const struct sim_config *cfg,
                          unsigned nb_valid,
                          unsigned active_count,
                          unsigned max_entries,
                          int auto_timeout,
                          const struct sim_timeout_ctrl *tc)
{
    double fill_ratio;
    double fill_target;
    double excess;
    unsigned budget;

    if (cfg->maint_budget == 0u || nb_valid <= cfg->maint_budget)
        return nb_valid;
    if (cfg->maint_budget >= cfg->batch_size)
        return nb_valid;

    fill_ratio = sim_fill_ratio(active_count, max_entries);
    fill_target = sim_fill_target_ratio(auto_timeout, tc);
    excess = sim_fill_excess_ratio(fill_ratio, fill_target);
    budget = cfg->maint_budget;
    budget += (unsigned)((double)(cfg->batch_size - cfg->maint_budget) * excess);
    if (budget > nb_valid)
        budget = nb_valid;
    return budget;
}

static void
timeout_ctrl_init(struct sim_timeout_ctrl *tc, double fill_target,
                  double initial_to)
{
    tc->fill_target = fill_target;
    tc->ema_timeout = initial_to;
    tc->kp          = 2.0;
    tc->alpha       = 0.3;
    tc->min_to      = 0.1;
    tc->max_to      = 60.0;
}

/* Compute initial timeout from configured rates (before any data). */
static double
timeout_ctrl_seed(double fill_target, double hit_rate, double new_rate,
                  unsigned max_entries)
{
    if (hit_rate < 1.0 || new_rate < 1.0)
        return 10.0;
    return fill_target * (double)max_entries
         * log(1.0 + hit_rate / new_rate) / hit_rate;
}

/* Update timeout from measured per-interval rates and current fill. */
static double
timeout_ctrl_update(struct sim_timeout_ctrl *tc, double hit_rate,
                    double new_rate, double fill_ratio,
                    unsigned max_entries)
{
    double t_ff, correction, t_new;

    if (hit_rate < 1.0 || new_rate < 1.0)
        return tc->ema_timeout;

    /* Feed-forward */
    t_ff = tc->fill_target * (double)max_entries
         * log(1.0 + hit_rate / new_rate) / hit_rate;

    /* Proportional correction on fill error */
    correction = 1.0 - tc->kp * (fill_ratio - tc->fill_target);
    if (correction < 0.5)
        correction = 0.5;
    if (correction > 2.0)
        correction = 2.0;

    t_new = t_ff * correction;

    /* EMA smoothing */
    tc->ema_timeout = tc->alpha * t_new
                    + (1.0 - tc->alpha) * tc->ema_timeout;

    /* Clamp */
    if (tc->ema_timeout < tc->min_to)
        tc->ema_timeout = tc->min_to;
    if (tc->ema_timeout > tc->max_to)
        tc->ema_timeout = tc->max_to;

    return tc->ema_timeout;
}

static void
sim_total_accumulate(struct sim_total_stats *total,
                     const struct sim_interval_stats *intv)
{
    total->finds         += intv->finds;
    total->hits          += intv->hits;
    total->adds          += intv->adds;
    total->add_skipped   += intv->add_skipped;
    total->maint_expired += intv->maint_expired;
    total->force_expired += intv->force_expired;
    total->add_cycles    += intv->add_cycles;
    total->maint_cycles  += intv->maint_cycles;
    total->attack_adds   += intv->attack_adds;
}

static int
sim_generator_init(struct sim_generator *gen, const struct sim_config *cfg)
{
    memset(gen, 0, sizeof(*gen));
    gen->cfg = cfg;
    gen->miss_count = cfg->batch_size * cfg->miss_pct / 100u;
    gen->hit_count = cfg->batch_size - gen->miss_count;
    gen->next_key_id = 1u;
    gen->keys = calloc(cfg->batch_size, sizeof(*gen->keys));
    if (gen->keys == NULL)
        return -1;
    if (cfg->ddos_burst) {
        gen->legit_keys = calloc(cfg->legit_pool_size, sizeof(*gen->legit_keys));
        if (gen->legit_keys == NULL)
            return -1;
        for (unsigned i = 0u; i < cfg->legit_pool_size; i++)
            gen->legit_keys[i] = sim_make_key(i + 1u);
        gen->next_attack_key_id = cfg->legit_pool_size + 1u;
    }
    return 0;
}

static void
sim_generator_destroy(struct sim_generator *gen)
{
    free(gen->legit_keys);
    free(gen->keys);
    memset(gen, 0, sizeof(*gen));
}

static void
sim_generator_prepare_batch(struct sim_generator *gen, double sim_sec,
                            const struct sim_receiver *rx,
                            struct sim_batch *batch)
{
    const struct sim_config *cfg = gen->cfg;

    memset(batch, 0, sizeof(*batch));
    if (cfg->ddos_burst) {
        unsigned burst_period = cfg->burst_on_sec + cfg->burst_off_sec;
        unsigned phase = burst_period > 0u
                       ? (unsigned)sim_sec % burst_period : 0u;
        unsigned attack_count;

        batch->is_burst = (phase >= cfg->burst_off_sec);
        attack_count = batch->is_burst
                     ? (cfg->batch_size * cfg->attack_pct / 100u) : 0u;
        batch->legit_prefix_len = cfg->batch_size - attack_count;
        batch->total_keys = cfg->batch_size;

        for (unsigned i = 0u; i < batch->legit_prefix_len; i++) {
            unsigned pidx = (unsigned)(rng_next() % cfg->legit_pool_size);

            gen->keys[i] = gen->legit_keys[pidx];
        }
        for (unsigned i = 0u; i < attack_count; i++)
            gen->keys[batch->legit_prefix_len + i] =
                sim_make_key(gen->next_attack_key_id + i);
        gen->next_attack_key_id += attack_count;
        return;
    }

    if (cfg->miss_vary != SIM_MISS_FIXED) {
        unsigned cur = sim_current_miss_pct(cfg->miss_pct,
                                            cfg->miss_vary, sim_sec);

        gen->miss_count = cfg->batch_size * cur / 100u;
        gen->hit_count  = cfg->batch_size - gen->miss_count;
    }

    batch->legit_prefix_len = 0u;
    batch->miss_keys = (rx->active.count >= gen->hit_count)
                     ? cfg->batch_size - gen->hit_count
                     : cfg->batch_size - rx->active.count;
    batch->total_keys = cfg->batch_size - batch->miss_keys + batch->miss_keys;

    {
        unsigned actual_hits = cfg->batch_size - batch->miss_keys;

        for (unsigned i = 0u; i < actual_hits; i++) {
            unsigned pos = (unsigned)(rng_next() % rx->active.count);
            u32 idx = rx->active.indices[pos];

            gen->keys[i] = rx->records[idx - 1u].entry.key;
        }
        for (unsigned i = 0u; i < batch->miss_keys; i++)
            gen->keys[actual_hits + i] = sim_make_key(gen->next_key_id + i);
        batch->total_keys = actual_hits + batch->miss_keys;
        batch->legit_prefix_len = batch->total_keys;
    }
}

static void
sim_generator_advance(struct sim_generator *gen, const struct sim_batch *batch)
{
    if (!gen->cfg->ddos_burst)
        gen->next_key_id += batch->miss_keys;
}

static int
sim_receiver_init(struct sim_receiver *rx, const struct sim_config *cfg,
                  unsigned max_entries)
{
    struct ft_table_config ft_cfg;

    memset(rx, 0, sizeof(*rx));
    rx->max_entries = max_entries;
    rx->add_policy = cfg->use_force_expire
                   ? FT_ADD_IGNORE_FORCE_EXPIRE : FT_ADD_IGNORE;

    rx->bucket_size = ft_table_bucket_size(max_entries);
    rx->buckets = sim_alloc_buckets(rx->bucket_size);
    if (rx->buckets == NULL)
        return -1;

    rx->records = sim_alloc_zero(max_entries, sizeof(*rx->records));
    if (FT_RECORD_ALLOCATOR_INIT_TYPED(&rx->alloc, rx->records, max_entries,
                                       struct sim_record) != 0)
        return -1;
    for (unsigned i = 0u; i < max_entries; i++)
        rx->records[i].active_pos = UINT32_MAX;

    memset(&ft_cfg, 0, sizeof(ft_cfg));
    if (FT_FLOW4_TABLE_INIT_TYPED(&rx->ft, rx->records, max_entries,
                                  struct sim_record, entry,
                                  rx->buckets, rx->bucket_size, &ft_cfg) != 0)
        return -1;
    if (active_init(&rx->active, max_entries) != 0)
        return -1;

    rx->new_idxv = calloc(cfg->batch_size, sizeof(*rx->new_idxv));
    rx->orig_idxv = calloc(cfg->batch_size, sizeof(*rx->orig_idxv));
    rx->hit_idxv = calloc(cfg->batch_size, sizeof(*rx->hit_idxv));
    rx->unused_idxv = calloc(cfg->batch_size + sim_unused_extra_capacity(cfg),
                             sizeof(*rx->unused_idxv));
    rx->expired_idxv = calloc(cfg->maint_max_expired, sizeof(*rx->expired_idxv));
    rx->work_idxv = calloc(cfg->batch_size, sizeof(*rx->work_idxv));
    if (!rx->new_idxv || !rx->orig_idxv || !rx->hit_idxv
        || !rx->unused_idxv || !rx->expired_idxv || !rx->work_idxv)
        return -1;

    return 0;
}

static void
sim_receiver_destroy(struct sim_receiver *rx)
{
    for (unsigned i = 0u; i < rx->nb_work; i++) {
        (void)FT_RECORD_ALLOCATOR_FREE_IDX_TYPED(
            &rx->alloc, struct sim_record, rx->work_idxv[i]);
    }
    free(rx->work_idxv);
    free(rx->expired_idxv);
    free(rx->unused_idxv);
    free(rx->hit_idxv);
    free(rx->orig_idxv);
    free(rx->new_idxv);
    active_destroy(&rx->active);
    if (rx->ft.buckets != NULL) {
        void *saved_bk = rx->ft.buckets;
        size_t saved_bk_size = (size_t)rx->ft.nb_bk * FT_TABLE_BUCKET_SIZE;

        ft_flow4_table_destroy(&rx->ft);
        sim_free_buckets(saved_bk, saved_bk_size);
    } else if (rx->buckets != NULL) {
        sim_free_buckets(rx->buckets, rx->bucket_size);
    }
    if (rx->records != NULL)
        sim_free_zero(rx->records, rx->max_entries, sizeof(*rx->records));
    ft_record_allocator_destroy(&rx->alloc);
    memset(rx, 0, sizeof(*rx));
}

static void
sim_receiver_refill_work(struct sim_receiver *rx, unsigned batch_size)
{
    while (rx->nb_work < batch_size) {
        u32 eidx = FT_RECORD_ALLOCATOR_ALLOC_IDX_TYPED(
            &rx->alloc, struct sim_record);

        if (!RIX_IDX_IS_VALID(eidx, rx->alloc.capacity))
            break;
        rx->work_idxv[rx->nb_work++] = eidx;
    }
}

static unsigned
sim_receiver_stage_batch(struct sim_receiver *rx, const struct flow4_key *keys,
                         unsigned batch_total)
{
    unsigned staged = 0u;

    for (unsigned i = 0u; i < rx->nb_work && staged < batch_total; i++) {
        struct flow4_entry *entry =
            ft_flow4_table_entry_ptr(&rx->ft, rx->work_idxv[i]);

        if (entry == NULL)
            continue;
        entry->key = keys[staged];
        rx->orig_idxv[staged] = rx->work_idxv[i];
        rx->new_idxv[staged] = rx->work_idxv[i];
        staged++;
    }
    return staged;
}

static void
sim_receiver_process_batch(struct sim_receiver *rx,
                           const struct sim_config *cfg,
                           const struct sim_batch *batch,
                           u64 sim_time_ns,
                           u64 timeout_ns,
                           int auto_timeout,
                           const struct sim_timeout_ctrl *to_ctrl,
                           struct sim_interval_stats *intv,
                           const struct flow4_key *keys)
{
    unsigned to_add = sim_receiver_stage_batch(rx, keys, batch->total_keys);
    unsigned nb_valid = 0u;

    if (to_add < batch->total_keys)
        intv->add_skipped += batch->total_keys - to_add;

    if (to_add > 0u) {
        unsigned n_unused;
        u64 t0, t1;

        t0 = sim_rdtsc();
        if (cfg->inline_maint) {
            unsigned inline_budget;
            unsigned unused_capacity = to_add;

            inline_budget = sim_inline_maint_unused_budget(
                cfg, rx->active.count, rx->max_entries, auto_timeout, to_ctrl);
            unused_capacity += inline_budget;
            n_unused = ft_flow4_table_add_idx_bulk_maint(
                &rx->ft, rx->new_idxv, to_add, rx->add_policy,
                sim_time_ns, timeout_ns,
                rx->unused_idxv, unused_capacity,
                cfg->min_bucket_occupancy);
        } else {
            n_unused = ft_flow4_table_add_idx_bulk(
                &rx->ft, rx->new_idxv, to_add, rx->add_policy,
                sim_time_ns, rx->unused_idxv);
        }
        t1 = sim_rdtsc();
        intv->add_cycles += t1 - t0;
        intv->finds += to_add;

        for (unsigned i = 0u; i < to_add; i++) {
            u32 ridx = rx->new_idxv[i];

            if (ridx == 0u)
                continue;
            rx->hit_idxv[nb_valid++] = ridx;

            if (ridx == rx->orig_idxv[i]) {
                active_add(&rx->active, ridx, rx->records);
                intv->adds++;
                if (cfg->ddos_burst && i >= batch->legit_prefix_len)
                    intv->attack_adds++;
            } else {
                intv->hits++;
            }
        }

        rx->nb_work = 0u;
        for (unsigned i = 0u; i < n_unused; i++) {
            u32 uidx = rx->unused_idxv[i];

            if (uidx == 0u)
                continue;
            if (rx->records[uidx - 1u].active_pos != UINT32_MAX) {
                active_remove(&rx->active, uidx, rx->records);
                if (cfg->inline_maint)
                    intv->maint_expired++;
                else
                    intv->force_expired++;
                (void)FT_RECORD_ALLOCATOR_FREE_IDX_TYPED(
                    &rx->alloc, struct sim_record, uidx);
            } else {
                rx->work_idxv[rx->nb_work++] = uidx;
            }
        }
    } else {
        rx->nb_work = 0u;
    }

    if (!cfg->inline_maint && nb_valid > 0u) {
        unsigned maint_n = sim_separate_maint_budget(
            cfg, nb_valid, rx->active.count, rx->max_entries,
            auto_timeout, to_ctrl);
        unsigned n_expired;
        u64 t0, t1;

        t0 = sim_rdtsc();
        n_expired = ft_flow4_table_maintain_idx_bulk(
            &rx->ft, rx->hit_idxv, maint_n,
            sim_time_ns, timeout_ns,
            rx->expired_idxv, cfg->maint_max_expired,
            8u, 1 /* enable_filter */);
        t1 = sim_rdtsc();
        intv->maint_cycles += t1 - t0;
        intv->maint_expired += n_expired;

        for (unsigned i = 0u; i < n_expired; i++) {
            active_remove(&rx->active, rx->expired_idxv[i], rx->records);
            (void)FT_RECORD_ALLOCATOR_FREE_IDX_TYPED(
                &rx->alloc, struct sim_record, rx->expired_idxv[i]);
        }
    }

    sim_receiver_refill_work(rx, cfg->batch_size);
}

/*===========================================================================
 * main
 *===========================================================================*/

int
main(int argc, char **argv)
{
    struct sim_config cfg;
    struct sim_generator gen;
    struct sim_receiver rx;
    struct sim_batch batch;
    struct sim_total_stats total;
    struct sim_interval_stats intv;
    unsigned max_entries;

    u64 sim_time_ns;
    u64 batch_interval_ns;
    u64 timeout_ns;
    u64 duration_ns;
    double next_report_sec;
    struct sim_timeout_ctrl to_ctrl;
    int auto_timeout;
    u64 dos_timeout_ns = 0u;
    enum sim_dos_state dos_state = SIM_DOS_NORMAL;
    int rc = 1;

    memset(&gen, 0, sizeof(gen));
    memset(&rx, 0, sizeof(rx));

    if (parse_options(argc, argv, &cfg) != 0)
        return 1;

    g_use_hugepage = cfg.use_hugepage;

    /* Pin core */
    if (cfg.pin_core >= 0) {
        cpu_set_t cpuset;

        CPU_ZERO(&cpuset);
        CPU_SET((unsigned)cfg.pin_core, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
            fprintf(stderr, "sched_setaffinity(core=%d): %s\n",
                    cfg.pin_core, strerror(errno));
            return 1;
        }
    }

    ft_arch_init(cfg.arch_enable);

    /* Round pool size up to power-of-2 */
    max_entries = roundup_pow2(cfg.pool_size < 64u ? 64u : cfg.pool_size);

    /* Derived quantities */
    batch_interval_ns = (u64)cfg.batch_size * UINT64_C(1000000000) / cfg.pps;
    duration_ns       = (u64)cfg.duration_sec * UINT64_C(1000000000);
    auto_timeout      = cfg.fill_target_pct > 0u;

    /* Auto-timeout: seed initial TO from formula */
    if (auto_timeout) {
        double ftgt = (double)cfg.fill_target_pct / 100.0;
        double hr = (double)cfg.pps * (100u - cfg.miss_pct) / 100.0;
        double nr = (double)cfg.pps * cfg.miss_pct / 100.0;
        double seed = timeout_ctrl_seed(ftgt, hr, nr, max_entries);

        timeout_ctrl_init(&to_ctrl, ftgt, seed);
        timeout_ns = (u64)(seed * 1e9);
    } else {
        timeout_ns = (u64)(cfg.timeout_sec * 1e9);
        memset(&to_ctrl, 0, sizeof(to_ctrl));
    }

    if (sim_generator_init(&gen, &cfg) != 0) {
        fprintf(stderr, "generator init failed\n");
        goto cleanup;
    }
    if (sim_receiver_init(&rx, &cfg, max_entries) != 0) {
        fprintf(stderr, "receiver init failed\n");
        goto cleanup;
    }
    sim_receiver_refill_work(&rx, cfg.batch_size);

    if (cfg.ddos_burst)
        dos_timeout_ns = (u64)(cfg.dos_timeout_sec * 1e9);

    {
        unsigned batches_per_sec = cfg.pps / cfg.batch_size;
        unsigned new_per_sec = cfg.ddos_burst
                             ? 0u : batches_per_sec * gen.miss_count;
        double effective_to = (double)timeout_ns / 1e9;
        unsigned ss_entries = cfg.ddos_burst
                            ? 0u
                            : (unsigned)((double)new_per_sec * effective_to);

        printf("=== Flow Cache Simulation ===\n");
        printf("  arch:          %s%s\n", arch_label(cfg.arch_enable),
               cfg.use_hugepage ? ", hugepage" : "");
        printf("  pps:           %u\n", cfg.pps);
        if (cfg.ddos_burst)
            printf("  batch:         %u  (ddos-burst)\n", cfg.batch_size);
        else
            printf("  batch:         %u  (hit=%u, miss=%u)\n",
                   cfg.batch_size, gen.hit_count, gen.miss_count);
        if (!cfg.ddos_burst && cfg.miss_vary != SIM_MISS_FIXED)
            printf("  miss vary:     %s (base %u%%)\n",
                   miss_vary_label(cfg.miss_vary), cfg.miss_pct);
        printf("  pool:          %u  (rounded %u)\n", cfg.pool_size, max_entries);
        if (auto_timeout)
            printf("  timeout:       auto (initial %.2f s, target %u%%)\n",
                   effective_to, cfg.fill_target_pct);
        else
            printf("  timeout:       %.1f s\n", effective_to);
        printf("  duration:      %u s\n", cfg.duration_sec);
        printf("  maint max-exp: %u\n", cfg.maint_max_expired);
        printf("  maint budget:  %u%s\n", cfg.maint_budget,
               cfg.inline_maint ? " (unused-cap budget)"
                                : (cfg.maint_budget == 0u ? " (all hits)" : ""));
        printf("  maint mode:    %s\n",
               cfg.inline_maint ? "inline" : "separate");
        if (cfg.inline_maint)
            printf("  min bucket occ:%u/16\n", cfg.min_bucket_occupancy);
        printf("  add policy:    %s\n",
               cfg.use_force_expire ? "FORCE_EXPIRE" : "IGNORE");
        if (cfg.ddos_burst) {
            printf("  ddos-burst:    on\n");
            printf("  legit pool:    %u\n", cfg.legit_pool_size);
            printf("  attack pct:    %u%%\n", cfg.attack_pct);
            printf("  burst cycle:   %u s on / %u s off\n",
                   cfg.burst_on_sec, cfg.burst_off_sec);
            printf("  dos threshold: %u%% → DoS, %u%% → Normal\n",
                   cfg.dos_threshold_pct, cfg.normal_threshold_pct);
            printf("  dos timeout:   %.1f s\n", cfg.dos_timeout_sec);
        }
        printf("  batch interval:%.1f us\n", (double)batch_interval_ns / 1000.0);
        printf("  batches/sec:   %u\n", batches_per_sec);
        if (!cfg.ddos_burst) {
            printf("  new flows/sec: %u\n", new_per_sec);
            printf("  steady-state:  ~%u entries (%.1f%%)\n",
                   ss_entries,
                   100.0 * (double)ss_entries / (double)max_entries);
            if (ss_entries > max_entries)
                printf("  ** WARNING: steady-state exceeds pool!\n");
        }
        printf("\n");
    }
    printf("  nb_bk:         %u\n\n", ft_flow4_table_nb_bk(&rx.ft));

    /* --- header --- */
    if (cfg.ddos_burst) {
        printf("  %5s  %7s %6s %5s %3s"
               "   %8s %8s %7s %7s"
               "  %7s %9s %7s\n",
               "sec", "entries", "fill%", "miss%", "mod",
               "keys/s", "hit/s", "add/s", "exp/s",
               "add_cy", "mnt_cy", "TO(s)");
        printf("  -----  ------- ------ ----- ---"
               "   -------- -------- ------- -------"
               "  ------- --------- -------\n");
    } else {
        printf("  %5s  %7s %6s %5s   %8s %8s %7s %7s"
               "  %7s %9s %7s\n",
               "sec", "entries", "fill%", "miss%",
               "keys/s", "hit/s", "add/s", "exp/s",
               "add_cy", "mnt_cy", "TO(s)");
        printf("  -----  ------- ------ -----"
               "   -------- -------- ------- -------"
               "  ------- --------- -------\n");
    }

    /* --- simulation --- */
    memset(&total, 0, sizeof(total));
    memset(&intv, 0, sizeof(intv));
    sim_time_ns    = UINT64_C(1000000000);   /* avoid timestamp-0 */
    next_report_sec = 1.0;

    while (sim_time_ns < UINT64_C(1000000000) + duration_ns) {
        double sim_sec = (double)(sim_time_ns - UINT64_C(1000000000)) / 1e9;
        sim_generator_prepare_batch(&gen, sim_sec, &rx, &batch);
        sim_receiver_process_batch(&rx, &cfg, &batch, sim_time_ns,
                                   timeout_ns, auto_timeout, &to_ctrl,
                                   &intv, gen.keys);
        sim_generator_advance(&gen, &batch);

        total.batches++;

        /* 6. Periodic report (every 1 simulated second) */
        if (sim_sec >= next_report_sec) {
            double fill = 100.0 * (double)rx.active.count / (double)max_entries;
            double fill_ratio = (double)rx.active.count / (double)max_entries;
            double add_cy = intv.finds > 0u
                          ? (double)intv.add_cycles / (double)intv.finds
                          : 0.0;
            double mnt_cy = intv.finds > 0u
                          ? (double)intv.maint_cycles / (double)intv.finds
                          : 0.0;
            u64 total_exp = intv.maint_expired + intv.force_expired;

            /* Auto-timeout: update controller from measured rates */
            if (auto_timeout) {
                double new_to = timeout_ctrl_update(
                    &to_ctrl,
                    (double)intv.hits,      /* hit rate (per interval = 1s) */
                    (double)intv.adds,      /* new-flow rate (per interval) */
                    fill_ratio, max_entries);
                timeout_ns = (u64)(new_to * 1e9);
            }

            {
                double actual_miss = (intv.adds + intv.hits > 0u)
                    ? 100.0 * (double)intv.adds
                      / (double)(intv.adds + intv.hits)
                    : 0.0;

                /* DoS state machine (--ddos-burst) */
                if (cfg.ddos_burst) {
                    if (dos_state == SIM_DOS_NORMAL) {
                        if (actual_miss > (double)cfg.dos_threshold_pct) {
                            dos_state = SIM_DOS_ACTIVE;
                            timeout_ns = dos_timeout_ns;
                        }
                    } else {
                        if (actual_miss < (double)cfg.normal_threshold_pct) {
                            dos_state = SIM_DOS_NORMAL;
                            if (auto_timeout)
                                timeout_ns = (u64)(to_ctrl.ema_timeout * 1e9);
                            else
                                timeout_ns = (u64)(cfg.timeout_sec * 1e9);
                        }
                    }
                }

                if (cfg.ddos_burst) {
                    const char *mode;

                    if (dos_state == SIM_DOS_ACTIVE)
                        mode = batch.is_burst ? "DB" : " D";
                    else
                        mode = batch.is_burst ? "NB" : " N";

                    printf("  %5.0f  %7u %5.1f%% %4.1f%% %3s"
                           "   %8" PRIu64 " %8" PRIu64
                           " %7" PRIu64 " %7" PRIu64
                           "  %7.1f %9.1f %7.2f\n",
                           sim_sec, rx.active.count, fill, actual_miss,
                           mode,
                           intv.finds, intv.hits, intv.adds, total_exp,
                           add_cy, mnt_cy,
                           (double)timeout_ns / 1e9);
                } else {
                    printf("  %5.0f  %7u %5.1f%% %4.1f%%"
                           "   %8" PRIu64 " %8" PRIu64
                           " %7" PRIu64 " %7" PRIu64
                           "  %7.1f %9.1f %7.2f\n",
                           sim_sec, rx.active.count, fill, actual_miss,
                           intv.finds, intv.hits, intv.adds, total_exp,
                           add_cy, mnt_cy,
                           (double)timeout_ns / 1e9);
                }
            }

            sim_total_accumulate(&total, &intv);
            memset(&intv, 0, sizeof(intv));
            next_report_sec += 1.0;
        }

        sim_time_ns += batch_interval_ns;
    }

    /* flush remaining interval */
    sim_total_accumulate(&total, &intv);

    /* --- summary --- */
    printf("\n=== Summary (%" PRIu64 " batches, %u sec) ===\n",
           total.batches, cfg.duration_sec);
    printf("  keys:            %" PRIu64 "\n", total.finds);
    printf("  hits:            %" PRIu64 " (%.1f%%)\n",
           total.hits,
           total.finds > 0u
           ? 100.0 * (double)total.hits / (double)total.finds : 0.0);
    printf("  adds:            %" PRIu64 "\n", total.adds);
    if (cfg.ddos_burst)
    printf("  attack adds:     %" PRIu64 "\n", total.attack_adds);
    printf("  add skipped:     %" PRIu64 "\n", total.add_skipped);
    printf("  maint expired:   %" PRIu64 "\n", total.maint_expired);
    printf("  force expired:   %" PRIu64 "\n", total.force_expired);
    printf("  final fill:      %u / %u (%.1f%%)\n",
           rx.active.count, max_entries,
           100.0 * (double)rx.active.count / (double)max_entries);
    if (total.finds > 0u) {
        double avg_add = (double)total.add_cycles / (double)total.finds;
        double avg_mnt = (double)total.maint_cycles / (double)total.finds;

        printf("  avg add cy:      %.1f cy/key\n", avg_add);
        printf("  avg maint cy:    %.1f cy/key\n", avg_mnt);
        printf("  total cy/key:    %.1f  (add %.0f%% + maint %.0f%%)\n",
               avg_add + avg_mnt,
               100.0 * avg_add / (avg_add + avg_mnt),
               100.0 * avg_mnt / (avg_add + avg_mnt));
    }

    /* --- table internal stats --- */
    {
        struct ft_table_stats fts;

        ft_flow4_table_stats(&rx.ft, &fts);
        printf("\n  Table stats:\n");
        printf("    lookups:       %" PRIu64 "\n", fts.core.lookups);
        printf("    hits:          %" PRIu64 "\n", fts.core.hits);
        printf("    misses:        %" PRIu64 "\n", fts.core.misses);
        printf("    adds:          %" PRIu64 "\n", fts.core.adds);
        printf("    add_existing:  %" PRIu64 "\n", fts.core.add_existing);
        printf("    add_failed:    %" PRIu64 "\n", fts.core.add_failed);
        printf("    force_expired: %" PRIu64 "\n", fts.core.force_expired);
        printf("    dels:          %" PRIu64 "\n", fts.core.dels);
        printf("    maint_calls:   %" PRIu64 "\n", fts.maint_calls);
        printf("    maint_bk_chk:  %" PRIu64 "\n", fts.maint_bucket_checks);
        printf("    maint_evicts:  %" PRIu64 "\n", fts.maint_evictions);
    }
    rc = 0;

cleanup:
    sim_receiver_destroy(&rx);
    sim_generator_destroy(&gen);
    return rc;
}
