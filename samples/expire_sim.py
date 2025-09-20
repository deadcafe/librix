#!/usr/bin/env python3
"""
Simulate miss-rate-driven adaptive timeout + fill-rate-driven scan.
Matches C implementation: _cache_adjust_timeout() + _cache_expire_scan().
"""

import math

# --- Parameters ---
POOL_SIZE      = 1_000_000
BASE_TIMEOUT_S = 5.0
BATCH_SIZE     = 256
PPS            = 2_000_000
SCAN_MIN       = 64
SCAN_MAX       = 1024
SIM_DURATION_S = 60

# Timeout adjustment parameters (match C defines)
DECAY_SHIFT   = 12   # log2(256) + 4
RECOVER_SHIFT = 8    # 1/256 per batch
MIN_TIMEOUT_S = 1.0  # minimum timeout floor (seconds)

# Age tracking: sub-second buckets
AGE_BUCKET_SEC = 0.1
AGE_BUCKETS    = int(BASE_TIMEOUT_S * 2 / AGE_BUCKET_SEC) + 2


def get_scan_level(nb, max_ent):
    half = max_ent >> 1
    if nb <= half:
        return 0
    excess = nb - half
    shift = int(math.log2(max_ent)) - 4
    level = excess >> shift
    return min(level, 15)


def get_scan(level):
    if level >= 4:
        return SCAN_MAX
    return SCAN_MIN << level


def simulate(miss_pct, pool_size=POOL_SIZE, base_timeout=BASE_TIMEOUT_S,
             pps=PPS, min_timeout=MIN_TIMEOUT_S):
    """
    Simulate per-bucket-interval (0.1s).
    miss_pct: fraction of batch that are misses (0.0-1.0)
    """
    batches_per_sec = pps / BATCH_SIZE
    batches_per_bucket = int(batches_per_sec * AGE_BUCKET_SEC)

    age_hist = [0.0] * AGE_BUCKETS
    nb_entries = 0
    eff_timeout = base_timeout

    results = []
    misses_per_batch = int(BATCH_SIZE * miss_pct)

    for step in range(int(SIM_DURATION_S / AGE_BUCKET_SEC)):
        sec = step * AGE_BUCKET_SEC

        # --- Adjust timeout (per batch, aggregated over bucket interval) ---
        for _ in range(batches_per_bucket):
            # decay
            if misses_per_batch > 0:
                decay = eff_timeout * misses_per_batch / (1 << DECAY_SHIFT)
                if decay < 1e-15:
                    decay = 1e-15
                if eff_timeout > min_timeout + decay:
                    eff_timeout -= decay
                else:
                    eff_timeout = min_timeout
            # recovery
            eff_timeout += (base_timeout - eff_timeout) / (1 << RECOVER_SHIFT)

        # --- Scan level from fill rate ---
        level = get_scan_level(nb_entries, pool_size)
        scan_per_batch = get_scan(level)

        # --- Expire ---
        eff_to_bucket = int(eff_timeout / AGE_BUCKET_SEC)
        total_scan = scan_per_batch * batches_per_sec * AGE_BUCKET_SEC
        if nb_entries > 0:
            scan_fraction = min(total_scan / pool_size, 1.0)
        else:
            scan_fraction = 1.0

        evicted = 0
        for i in range(eff_to_bucket + 1, AGE_BUCKETS):
            can_evict = age_hist[i] * scan_fraction
            evicted += can_evict
            age_hist[i] -= can_evict

        nb_entries = int(nb_entries - evicted)
        if nb_entries < 0:
            nb_entries = 0

        # --- Insert ---
        new_this_step = misses_per_batch * batches_per_sec * AGE_BUCKET_SEC
        actually_inserted = min(new_this_step, pool_size - nb_entries)
        if actually_inserted < 0:
            actually_inserted = 0

        # --- Age histogram shift ---
        for i in range(AGE_BUCKETS - 1, 0, -1):
            age_hist[i] = age_hist[i - 1]
        age_hist[0] = actually_inserted

        nb_entries = int(nb_entries + actually_inserted)
        if nb_entries > pool_size:
            nb_entries = pool_size

        # Record at ~integer seconds
        if abs(sec - round(sec)) < AGE_BUCKET_SEC / 2 and \
           abs(sec - int(sec)) < 0.01:
            fill_pct = 100.0 * nb_entries / pool_size
            results.append((sec, fill_pct, level, eff_timeout,
                            scan_per_batch,
                            int(evicted / AGE_BUCKET_SEC),
                            int(actually_inserted / AGE_BUCKET_SEC)))

    return results


def simulate_pps(pps, pool_size=POOL_SIZE, base_timeout=BASE_TIMEOUT_S,
                 min_timeout=MIN_TIMEOUT_S):
    """Run simulation for a given pps, return per-miss-rate steady state."""
    batches_per_sec = pps / BATCH_SIZE
    batches_per_bucket = int(batches_per_sec * AGE_BUCKET_SEC)

    miss_rates = [0.05, 0.10, 0.20, 0.30, 0.50]
    steady = []

    for mp in miss_rates:
        misses_per_batch = int(BATCH_SIZE * mp)
        age_hist = [0.0] * AGE_BUCKETS
        nb_entries = 0
        eff_timeout = base_timeout

        for step in range(int(SIM_DURATION_S / AGE_BUCKET_SEC)):
            for _ in range(batches_per_bucket):
                if misses_per_batch > 0:
                    decay = eff_timeout * misses_per_batch / (1 << DECAY_SHIFT)
                    if decay < 1e-15:
                        decay = 1e-15
                    if eff_timeout > min_timeout + decay:
                        eff_timeout -= decay
                    else:
                        eff_timeout = min_timeout
                eff_timeout += (base_timeout - eff_timeout) / (1 << RECOVER_SHIFT)

            level = get_scan_level(nb_entries, pool_size)
            scan_per_batch = get_scan(level)

            eff_to_bucket = int(eff_timeout / AGE_BUCKET_SEC)
            total_scan = scan_per_batch * batches_per_sec * AGE_BUCKET_SEC
            if nb_entries > 0:
                scan_fraction = min(total_scan / pool_size, 1.0)
            else:
                scan_fraction = 1.0

            evicted = 0
            for i in range(eff_to_bucket + 1, AGE_BUCKETS):
                can_evict = age_hist[i] * scan_fraction
                evicted += can_evict
                age_hist[i] -= can_evict

            nb_entries = int(nb_entries - evicted)
            if nb_entries < 0:
                nb_entries = 0

            new_this_step = misses_per_batch * batches_per_sec * AGE_BUCKET_SEC
            actually_inserted = min(new_this_step, pool_size - nb_entries)
            if actually_inserted < 0:
                actually_inserted = 0

            for i in range(AGE_BUCKETS - 1, 0, -1):
                age_hist[i] = age_hist[i - 1]
            age_hist[0] = actually_inserted

            nb_entries = int(nb_entries + actually_inserted)
            if nb_entries > pool_size:
                nb_entries = pool_size

        fill_pct = 100.0 * nb_entries / pool_size
        new_per_sec = misses_per_batch * batches_per_sec
        steady.append((mp, fill_pct, level, eff_timeout, scan_per_batch,
                       new_per_sec))

    return steady


def main():
    miss_rates = [0.05, 0.10, 0.20, 0.30, 0.50]
    pool_sizes = [100_000, 1_000_000, 4_000_000, 16_000_000]
    pps = PPS  # 2Mpps

    print(f"=== Pool Size Scaling (min_TO={MIN_TIMEOUT_S}s, {pps/1e6:.0f}Mpps) ===")
    print(f"  Base timeout: {BASE_TIMEOUT_S}s, min: {MIN_TIMEOUT_S}s")
    print(f"  Decay: 1/{1<<DECAY_SHIFT}, Recovery: 1/{1<<RECOVER_SHIFT}")
    print(f"  Rate: {pps/1e6:.0f}Mpps, batch={BATCH_SIZE}")
    print()

    key_times = set([0,1,2,3,5,10,20,30,59])

    for ps in pool_sizes:
        batches_per_sec = pps / BATCH_SIZE
        print(f"{'='*70}")
        print(f"  Pool: {ps:>12,} entries  ({ps*128/1e6:.0f} MB)")
        print(f"{'='*70}")

        for mp in miss_rates:
            misses = int(BATCH_SIZE * mp)
            new_per_sec = misses * batches_per_sec
            print(f"--- {mp*100:.0f}% miss ({new_per_sec:,.0f} new/sec) ---")
            print(f"  {'sec':>5s}  {'fill%':>7s}  {'lvl':>3s}  "
                  f"{'eff_TO':>7s}  {'scan':>5s}  {'evict/s':>9s}  "
                  f"{'insert/s':>9s}")

            results = simulate(mp, pool_size=ps, pps=pps)

            for (sec, fill, lvl, eff_to, scan, evict, inserted) in results:
                if int(sec) in key_times:
                    print(f"  {sec:5.0f}  {fill:6.1f}%  {lvl:>3d}  "
                          f"{eff_to:6.2f}s  {scan:>5d}  {evict:>9,d}  "
                          f"{inserted:>9,d}")

            sec, fill, lvl, eff_to, scan, evict, inserted = results[-1]
            print(f"  *** steady: fill={fill:.1f}%, eff_TO={eff_to:.2f}s, "
                  f"scan={scan} ***")
            print()

    # --- summary table ---
    print(f"\n{'='*70}")
    print(f"  SUMMARY: steady-state @ {pps/1e6:.0f}Mpps "
          f"(base_TO={BASE_TIMEOUT_S}s, min_TO={MIN_TIMEOUT_S}s)")
    print(f"{'='*70}")
    print(f"  {'miss%':>5s}  {'new/s':>9s}  ", end="")
    for ps in pool_sizes:
        label = f"{ps/1e6:.1f}M" if ps >= 1_000_000 else f"{ps//1000}K"
        print(f"{'fill%':>7s} {'TO':>5s} {'scan':>5s}  ", end="")
    print()
    print(f"  {'':>5s}  {'':>9s}  ", end="")
    for ps in pool_sizes:
        label = f"{ps/1e6:.1f}M" if ps >= 1_000_000 else f"{ps//1000}K"
        print(f"  -- {label:>5s} --   ", end="")
    print()

    for mp in miss_rates:
        misses = int(BATCH_SIZE * mp)
        batches_per_sec = pps / BATCH_SIZE
        new_per_sec = misses * batches_per_sec
        print(f"  {mp*100:4.0f}%  {new_per_sec:>9,.0f}  ", end="")
        for ps in pool_sizes:
            steady = simulate_pps(pps, pool_size=ps)
            idx = miss_rates.index(mp)
            _, fill, _, eff_to, scan, _ = steady[idx]
            print(f"{fill:6.1f}% {eff_to:4.1f}s {scan:>5d}  ", end="")
        print()


if __name__ == "__main__":
    main()
