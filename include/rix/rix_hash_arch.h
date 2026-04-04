/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * rix_hash_arch.h - arch-dependent infrastructure shared by all hash variants
 *
 * Provides runtime dispatch (Generic / SSE / AVX2 / AVX-512) for:
 *   - Fingerprint/key search:  find_u32x16, find_u64x16
 *   - Hash computation:        hash_bytes, hash_u32, hash_u64
 *
 * Used by rix_hash.h, rix_hash32.h, rix_hash64.h, and rix_hash_key.h.
 *
 * Included via the _RIX_HASH_COMMON_ guard so that including multiple hash
 * variant headers in one source file compiles this block only once.
 *
 * Each source file starts in the Generic dispatch mode by default.
 * Call rix_hash_arch_init() in a source file that uses hash tables to
 * enable the best SIMD level allowed by `enable`.
 *
 * Dispatch matrix (find / hash):
 *
 *   compiled with   runtime enable        find          hash
 *   -------------   --------------------  ------------  --------
 *   (any)           0 (force GEN)         GEN scalar    GEN / CRC32C *
 *   -msse4.2        RIX_HASH_ARCH_SSE     SSE XMM       CRC32C
 *   -mavx2          RIX_HASH_ARCH_AVX2    AVX2 YMM      CRC32C
 *   -mavx512f       RIX_HASH_ARCH_AVX512  AVX-512 ZMM   CRC32C
 *                   RIX_HASH_ARCH_AUTO    best above    best above
 *
 *   *GEN hash: Knuth/mix/FNV-1a (non-x86_64 or build without SSE4.2)
 *    CRC32C hash: when __SSE4_2__ is defined (always on x86_64 builds)
 *
 *   SSE find uses XMM 128-bit registers (cmpeq_epi32/SSE, cmpeq_epi64/SSE4.1,
 *   CRC32C/SSE4.2); all covered by -msse4.2 which is the x86_64 minimum here.
 *
 * Priority: AVX-512 > AVX2 > SSE > GEN.
 * SSE is unconditional on x86_64; AVX2 / AVX-512 verified via CPUID.
 * Note: pure AVX (without AVX2) has no 256-bit integer SIMD - not supported.
 */

#ifndef _RIX_HASH_ARCH_H_
#  define _RIX_HASH_ARCH_H_

#  include "rix_defs_private.h"

#  include <stdint.h>
#  include <string.h>

#  if defined(__x86_64__)
#    include <immintrin.h>
#  endif

#  ifndef _RIX_HASH_COMMON_
#    define _RIX_HASH_COMMON_

/* Silence unused-variable warnings for static symbols */
#    ifndef RIX_UNUSED
#      define RIX_UNUSED __attribute__((unused))
#    endif

/*===========================================================================
 * Constants shared across all hash variants
 *===========================================================================*/
#    define RIX_HASH_BUCKET_ENTRY_SZ 16
#    define RIX_HASH_FOLLOW_DEPTH 4

/*===========================================================================
 * rix_hash_arch_init() enable-mask bits
 *
 *   0                    - force Generic (scalar); useful for benchmarking
 *   RIX_HASH_ARCH_SSE   - allow SSE/SSE4.1 XMM 128-bit find (x86_64 only)
 *   RIX_HASH_ARCH_AVX2   - allow AVX2 YMM 256-bit find
 *   RIX_HASH_ARCH_AVX512 - allow AVX-512F ZMM 512-bit find
 *   RIX_HASH_ARCH_AUTO   - use best available (recommended default)
 *
 * Priority: AVX-512 > AVX2 > SSE > GEN.
 * Higher-level flags imply lower ones as fallback
 *   (AVX-512 > AVX2 > SSE > GEN).
 * AVX2 / AVX-512 / SSE are verified with compiler CPU builtins when available.
 * Each source file defaults to Generic until rix_hash_arch_init() is
 * called there.
 *===========================================================================*/
#    define RIX_HASH_ARCH_AVX2   (1u << 0)
#    define RIX_HASH_ARCH_AVX512 (1u << 1)
#    define RIX_HASH_ARCH_SSE   (1u << 2)
#    define RIX_HASH_ARCH_AUTO   \
    (RIX_HASH_ARCH_AVX2 | RIX_HASH_ARCH_AVX512 | RIX_HASH_ARCH_SSE)

/*===========================================================================
 * Hash union
 *===========================================================================*/
union rix_hash_hash_u {
    u64 val64;
    u32 val32[2];
};

/*===========================================================================
 * Arch handler - runtime dispatch
 *
 * find_u32x16 / find_u64x16:
 *   Search 16-element arrays; return 16-bit bitmask of matching positions.
 *   Dispatched to GEN scalar, SSE, AVX2, or AVX-512 depending on CPU.
 *
 * hash_bytes / hash_u32 / hash_u64:
 *   Compute two independent 32-bit hashes such that
 *   (val32[0] & mask) != (val32[1] & mask) is always guaranteed.
 *   Dispatched to CRC32C (x86_64 + SSE4.2) or GEN multiplicative fallback.
 *===========================================================================*/
struct rix_hash_arch_s {
    /* find val in u32[16], returns 16-bit bitmask of hit positions */
    u32 (*find_u32x16)(const u32 *arr, u32 val);
    /*
     * find two vals in u32[16], returns two 16-bit hit bitmasks.
     * Used by the fingerprint hash insert fast path to find:
     *   - fp hits (duplicate candidates)
     *   - zero slots (empty buckets)
     * in a single bucket scan.
     */
    void (*find_u32x16_2)(const u32 *arr, u32 val0, u32 val1,
                          u32 *mask0, u32 *mask1);
    /* find val in u64[16], returns 16-bit bitmask of hit positions */
    u32 (*find_u64x16)(const u64 *arr, u64  val);
    /* hash arbitrary-length byte key */
    union rix_hash_hash_u (*hash_bytes)(const void *key, size_t key_bytes,
                                        u32 mask);
    /* hash u32 key */
    union rix_hash_hash_u (*hash_u32)(u32 key, u32 mask);
    /* hash u64 key */
    union rix_hash_hash_u (*hash_u64)(u64 key, u32 mask);
};

/* Per-TU dispatch handle.  Defaults to Generic until init enables SIMD. */
static RIX_UNUSED const struct rix_hash_arch_s rix_hash_arch_GEN;
static RIX_UNUSED const struct rix_hash_arch_s *rix_hash_arch =
    &rix_hash_arch_GEN;

/*===========================================================================
 * Generic (scalar) find implementations
 *===========================================================================*/
static RIX_FORCE_INLINE u32
rix_hash_find_u32x16_GEN(const u32 *arr,
                         u32 val)
{
    u32 mask = 0u;
    for (unsigned i = 0u; i < 16u; i++)
        if (arr[i] == val)
            mask |= (1u << i);
    return mask;
}

static RIX_FORCE_INLINE void
rix_hash_find_u32x16_2_GEN(const u32 *arr,
                           u32 val0,
                           u32 val1,
                           u32 *mask0,
                           u32 *mask1)
{
    u32 m0 = 0u;
    u32 m1 = 0u;
    for (unsigned i = 0u; i < 16u; i++) {
        u32 v = arr[i];
        if (v == val0)
            m0 |= (1u << i);
        if (v == val1)
            m1 |= (1u << i);
    }
    *mask0 = m0;
    *mask1 = m1;
}

static RIX_FORCE_INLINE u32
rix_hash_find_u64x16_GEN(const u64 *arr,
                         u64 val)
{
    u32 mask = 0u;
    for (unsigned i = 0u; i < 16u; i++)
        if (arr[i] == val)
            mask |= (1u << i);
    return mask;
}

/*===========================================================================
 * Generic (scalar) hash implementations
 *
 * hash_u32: Knuth multiplicative hashing
 * hash_u64: 64-bit mix into two independent 32-bit hashes
 * hash_bytes: FNV-1a over byte buffer
 *
 * All variants guarantee (val32[0] & mask) != (val32[1] & mask).
 *===========================================================================*/
static RIX_FORCE_INLINE union rix_hash_hash_u
rix_hash_u32_GEN(u32 key,
                 u32 mask)
{
    union rix_hash_hash_u r;
    u32 h0  = key * 2654435761u;
    u32 bk0 = h0 & mask;
    u32 h1  = (key ^ h0) * 2246822519u;
    u32 inc = 1u;
    while ((h1 & mask) == bk0) {
        h1  = (h1 ^ inc) * 2246822519u;
        inc++;
    }
    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
}

static RIX_FORCE_INLINE union rix_hash_hash_u
rix_hash_u64_GEN(u64 key,
                 u32 mask)
{
    union rix_hash_hash_u r;
    u32 lo  = (u32)key;
    u32 hi  = (u32)(key >> 32);
    u32 h0  = (lo ^ (hi * 2654435761u)) * 2246822519u;
    u32 bk0 = h0 & mask;
    u32 h1  = (hi ^ (lo * 2246822519u)) * 2654435761u ^ h0;
    u32 inc = 1u;
    while ((h1 & mask) == bk0) {
        h1  = (h1 ^ inc) * 2246822519u;
        inc++;
    }
    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
}

static RIX_FORCE_INLINE union rix_hash_hash_u
rix_hash_bytes_GEN(const void *key,
                   size_t key_bytes,
                   u32 mask)
{
    union rix_hash_hash_u r;
    const u8 *p = (const u8 *)key;
    /* FNV-1a */
    u32 h0 = 2166136261u;
    for (size_t i = 0u; i < key_bytes; i++) {
        h0 ^= p[i];
        h0 *= 16777619u;
    }
    h0 |= !h0;
    u32 bk0 = h0 & mask;
    u32 h1  = (h0 ^ 0x5bd1e995u) * 2246822519u;
    u32 inc = 1u;
    h1 |= !h1;
    while ((h1 & mask) == bk0) {
        h1  = (h1 ^ inc) * 2246822519u;
        h1 |= !h1;
        inc++;
    }
    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
}

/*===========================================================================
 * CRC32C (SSE4.2) hash implementations
 *===========================================================================*/
#    if defined(__x86_64__) && defined(__SSE4_2__)

/*
 * rix_crc32_bytes - CRC32C over an arbitrary-length byte buffer.
 *
 * Processes the buffer in 8->4->2->1-byte chunks using the CRC32C hardware
 * instruction (SSE4.2, -msse4.2).  key_bytes may be any value including
 * non-multiples of 8; memcpy is used for safe unaligned partial reads.
 */
static RIX_FORCE_INLINE u32
rix_crc32_bytes(u32 crc,
                const void *key,
                size_t key_bytes)
{
    const u8 *p = (const u8 *)key;
    size_t rem = key_bytes;

    while (rem >= 8u) {
        u64 w;
        memcpy(&w, p, 8u);
        crc = (u32)__builtin_ia32_crc32di((u64)crc, w);
        p += 8u;
        rem -= 8u;
    }
    if (rem >= 4u) {
        u32 w;
        memcpy(&w, p, 4u);
        crc = (u32)__builtin_ia32_crc32si(crc, w);
        p += 4u;
        rem -= 4u;
    }
    if (rem >= 2u) {
        u16 w;
        memcpy(&w, p, 2u);
        crc = (u32)__builtin_ia32_crc32hi(crc, (unsigned short)w);
        p += 2u;
        rem -= 2u;
    }
    if (rem >= 1u) {
        crc = (u32)__builtin_ia32_crc32qi(crc, (unsigned char)*p);
    }
    return crc;
}

static RIX_FORCE_INLINE union rix_hash_hash_u
rix_hash_bytes_CRC32(const void *key,
                     size_t key_bytes,
                     u32 mask)
{
    union rix_hash_hash_u r;
    u32 h0   = rix_crc32_bytes(0u, key, key_bytes);
    h0       |= !h0;
    u32 bk0  = h0 & mask;
    u32 seed = ~h0;
    u32 h1;
    do {
        h1   = rix_crc32_bytes(seed, key, key_bytes);
        h1 |= !h1;
        seed = (u32)__builtin_ia32_crc32di((u64)seed, (u64)h0);
    } while ((h1 & mask) == bk0);
    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
}

static RIX_FORCE_INLINE union rix_hash_hash_u
rix_hash_u32_CRC32(u32 key,
                   u32 mask)
{
    union rix_hash_hash_u r;
    u32 h0   = (u32)__builtin_ia32_crc32si(0u, key);
    h0      |= !h0;
    u32 bk0  = h0 & mask;
    u32 seed = ~h0;
    u32 h1;
    do {
        seed = (u32)__builtin_ia32_crc32si(seed, ~key);
        h1   = (u32)__builtin_ia32_crc32si(seed, key);
        h1  |= !h1;
    } while ((h1 & mask) == bk0);
    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
}

static RIX_FORCE_INLINE union rix_hash_hash_u
rix_hash_u64_CRC32(u64 key,
                   u32 mask)
{
    union rix_hash_hash_u r;
    u32 h0   = (u32)__builtin_ia32_crc32di(0ULL, key);
    h0      |= !h0;
    u32 bk0  = h0 & mask;
    u32 seed = ~h0;
    u32 h1;
    do {
        seed = (u32)__builtin_ia32_crc32di((u64)seed, ~key);
        h1   = (u32)__builtin_ia32_crc32di((u64)seed, key);
        h1  |= !h1;
    } while ((h1 & mask) == bk0);
    r.val32[0] = h0;
    r.val32[1] = h1;
    return r;
}

/* GEN find + CRC32 hash: for SIMD=gen (scalar scan, hardware hash) */
static RIX_UNUSED const struct rix_hash_arch_s rix_hash_arch_GEN = {
    rix_hash_find_u32x16_GEN,
    rix_hash_find_u32x16_2_GEN,
    rix_hash_find_u64x16_GEN,
    rix_hash_bytes_CRC32,
    rix_hash_u32_CRC32,
    rix_hash_u64_CRC32,
};

#    else /* !(__x86_64__ && __SSE4_2__) */

/* GEN find + GEN hash: pure portable fallback */
static RIX_UNUSED const struct rix_hash_arch_s rix_hash_arch_GEN = {
    rix_hash_find_u32x16_GEN,
    rix_hash_find_u32x16_2_GEN,
    rix_hash_find_u64x16_GEN,
    rix_hash_bytes_GEN,
    rix_hash_u32_GEN,
    rix_hash_u64_GEN,
};

#    endif /* __x86_64__ && __SSE4_2__ */

/*===========================================================================
 * SSE / SSE4.1 XMM (128-bit) find implementations
 *
 * find_u32x16: _mm_cmpeq_epi32 (SSE) - 4 elements/reg, 4 loads
 * find_u64x16: _mm_cmpeq_epi64 (SSE4.1) - 2 elements/reg, 8 loads
 *
 * Compiled when __SSE4_2__ is defined (SSE4.2 > SSE4.1 > SSE).
 * SSE is mandated by the x86_64 ABI; no CPUID check is needed at runtime.
 *===========================================================================*/
#    if defined(__x86_64__) && defined(__SSE4_2__)

static RIX_FORCE_INLINE u32
rix_hash_find_u32x16_SSE(const u32 *arr,
                         u32 val)
{
    __m128i vval = _mm_set1_epi32((int)val);
    __m128i eq0  = _mm_cmpeq_epi32(
        _mm_loadu_si128((const __m128i *)(const void *)(arr +  0)), vval);
    __m128i eq1  = _mm_cmpeq_epi32(
        _mm_loadu_si128((const __m128i *)(const void *)(arr +  4)), vval);
    __m128i eq2  = _mm_cmpeq_epi32(
        _mm_loadu_si128((const __m128i *)(const void *)(arr +  8)), vval);
    __m128i eq3  = _mm_cmpeq_epi32(
        _mm_loadu_si128((const __m128i *)(const void *)(arr + 12)), vval);
    u32 m0  = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq0));
    u32 m1  = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq1));
    u32 m2  = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq2));
    u32 m3  = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq3));
    return m0 | (m1 << 4) | (m2 << 8) | (m3 << 12);
}

static RIX_FORCE_INLINE void
rix_hash_find_u32x16_2_SSE(const u32 *arr,
                           u32 val0,
                           u32 val1,
                           u32 *mask0,
                           u32 *mask1)
{
    __m128i v0 = _mm_loadu_si128((const __m128i *)(const void *)(arr +  0));
    __m128i v1 = _mm_loadu_si128((const __m128i *)(const void *)(arr +  4));
    __m128i v2 = _mm_loadu_si128((const __m128i *)(const void *)(arr +  8));
    __m128i v3 = _mm_loadu_si128((const __m128i *)(const void *)(arr + 12));
    __m128i vv0 = _mm_set1_epi32((int)val0);
    __m128i vv1 = _mm_set1_epi32((int)val1);
    __m128i eq00 = _mm_cmpeq_epi32(v0, vv0);
    __m128i eq01 = _mm_cmpeq_epi32(v1, vv0);
    __m128i eq02 = _mm_cmpeq_epi32(v2, vv0);
    __m128i eq03 = _mm_cmpeq_epi32(v3, vv0);
    __m128i eq10 = _mm_cmpeq_epi32(v0, vv1);
    __m128i eq11 = _mm_cmpeq_epi32(v1, vv1);
    __m128i eq12 = _mm_cmpeq_epi32(v2, vv1);
    __m128i eq13 = _mm_cmpeq_epi32(v3, vv1);
    u32 m00 = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq00));
    u32 m01 = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq01));
    u32 m02 = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq02));
    u32 m03 = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq03));
    u32 m10 = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq10));
    u32 m11 = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq11));
    u32 m12 = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq12));
    u32 m13 = (u32)_mm_movemask_ps(_mm_castsi128_ps(eq13));
    *mask0 = m00 | (m01 << 4) | (m02 << 8) | (m03 << 12);
    *mask1 = m10 | (m11 << 4) | (m12 << 8) | (m13 << 12);
}

static RIX_FORCE_INLINE u32
rix_hash_find_u64x16_SSE(const u64 *arr,
                         u64 val)
{
    __m128i vval = _mm_set1_epi64x((long long)val);
    __m128i eq0  = _mm_cmpeq_epi64(
        _mm_loadu_si128((const __m128i *)(const void *)(arr +  0)), vval);
    __m128i eq1  = _mm_cmpeq_epi64(
        _mm_loadu_si128((const __m128i *)(const void *)(arr +  2)), vval);
    __m128i eq2  = _mm_cmpeq_epi64(
        _mm_loadu_si128((const __m128i *)(const void *)(arr +  4)), vval);
    __m128i eq3  = _mm_cmpeq_epi64(
        _mm_loadu_si128((const __m128i *)(const void *)(arr +  6)), vval);
    __m128i eq4  = _mm_cmpeq_epi64(
        _mm_loadu_si128((const __m128i *)(const void *)(arr +  8)), vval);
    __m128i eq5  = _mm_cmpeq_epi64(
        _mm_loadu_si128((const __m128i *)(const void *)(arr + 10)), vval);
    __m128i eq6  = _mm_cmpeq_epi64(
        _mm_loadu_si128((const __m128i *)(const void *)(arr + 12)), vval);
    __m128i eq7  = _mm_cmpeq_epi64(
        _mm_loadu_si128((const __m128i *)(const void *)(arr + 14)), vval);
    u32 m0  = (u32)_mm_movemask_pd(_mm_castsi128_pd(eq0));
    u32 m1  = (u32)_mm_movemask_pd(_mm_castsi128_pd(eq1));
    u32 m2  = (u32)_mm_movemask_pd(_mm_castsi128_pd(eq2));
    u32 m3  = (u32)_mm_movemask_pd(_mm_castsi128_pd(eq3));
    u32 m4  = (u32)_mm_movemask_pd(_mm_castsi128_pd(eq4));
    u32 m5  = (u32)_mm_movemask_pd(_mm_castsi128_pd(eq5));
    u32 m6  = (u32)_mm_movemask_pd(_mm_castsi128_pd(eq6));
    u32 m7  = (u32)_mm_movemask_pd(_mm_castsi128_pd(eq7));
    return  m0        | (m1 <<  2) | (m2 <<  4) | (m3 <<  6) |
           (m4 <<  8) | (m5 << 10) | (m6 << 12) | (m7 << 14);
}

/* SSE find + CRC32 hash (SSE4.2 is our baseline, so CRC32 always available) */
static RIX_UNUSED const struct rix_hash_arch_s rix_hash_arch_SSE = {
    rix_hash_find_u32x16_SSE,
    rix_hash_find_u32x16_2_SSE,
    rix_hash_find_u64x16_SSE,
    rix_hash_bytes_CRC32,
    rix_hash_u32_CRC32,
    rix_hash_u64_CRC32,
};

#    endif /* __x86_64__ && __SSE4_2__ */

/*===========================================================================
 * AVX2 find implementations
 *===========================================================================*/
#    if defined(__x86_64__) && defined(__AVX2__)

static RIX_FORCE_INLINE u32
rix_hash_find_u32x16_AVX2(const u32 *arr,
                          u32 val)
{
    __m256i vval = _mm256_set1_epi32((int)val);
    __m256i a0   = _mm256_loadu_si256((const __m256i *)(const void *)arr);
    __m256i a1   = _mm256_loadu_si256((const __m256i *)(const void *)(arr + 8));
    __m256i eq0  = _mm256_cmpeq_epi32(a0, vval);
    __m256i eq1  = _mm256_cmpeq_epi32(a1, vval);
    u32 lo  = (u32)(unsigned)
        _mm256_movemask_ps(_mm256_castsi256_ps(eq0));
    u32 hi  = (u32)(unsigned)
        _mm256_movemask_ps(_mm256_castsi256_ps(eq1));
    return lo | (hi << 8);
}

static RIX_FORCE_INLINE void
rix_hash_find_u32x16_2_AVX2(const u32 *arr,
                            u32 val0,
                            u32 val1,
                            u32 *mask0,
                            u32 *mask1)
{
    __m256i a0  = _mm256_loadu_si256((const __m256i *)(const void *)arr);
    __m256i a1  = _mm256_loadu_si256((const __m256i *)(const void *)(arr + 8));
    __m256i vv0 = _mm256_set1_epi32((int)val0);
    __m256i vv1 = _mm256_set1_epi32((int)val1);
    __m256i eq00 = _mm256_cmpeq_epi32(a0, vv0);
    __m256i eq01 = _mm256_cmpeq_epi32(a1, vv0);
    __m256i eq10 = _mm256_cmpeq_epi32(a0, vv1);
    __m256i eq11 = _mm256_cmpeq_epi32(a1, vv1);
    u32 lo0 = (u32)(unsigned)
        _mm256_movemask_ps(_mm256_castsi256_ps(eq00));
    u32 hi0 = (u32)(unsigned)
        _mm256_movemask_ps(_mm256_castsi256_ps(eq01));
    u32 lo1 = (u32)(unsigned)
        _mm256_movemask_ps(_mm256_castsi256_ps(eq10));
    u32 hi1 = (u32)(unsigned)
        _mm256_movemask_ps(_mm256_castsi256_ps(eq11));
    *mask0 = lo0 | (hi0 << 8);
    *mask1 = lo1 | (hi1 << 8);
}

static RIX_FORCE_INLINE u32
rix_hash_find_u64x16_AVX2(const u64 *arr,
                          u64 val)
{
    __m256i vval = _mm256_set1_epi64x((long long)val);
    __m256i v0   =
        _mm256_loadu_si256((const __m256i *)(const void *)(arr +  0));
    __m256i v1   =
        _mm256_loadu_si256((const __m256i *)(const void *)(arr +  4));
    __m256i v2   =
        _mm256_loadu_si256((const __m256i *)(const void *)(arr +  8));
    __m256i v3   =
        _mm256_loadu_si256((const __m256i *)(const void *)(arr + 12));
    u32 m0  = (u32)
        _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v0, vval)));
    u32 m1  = (u32)
        _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v1, vval)));
    u32 m2  = (u32)
        _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v2, vval)));
    u32 m3  = (u32)
        _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v3, vval)));
    return m0 | (m1 << 4) | (m2 << 8) | (m3 << 12);
}

/* AVX2 find + CRC32 hash (AVX2 always implies SSE4.2) */
static RIX_UNUSED const struct rix_hash_arch_s rix_hash_arch_AVX2 = {
    rix_hash_find_u32x16_AVX2,
    rix_hash_find_u32x16_2_AVX2,
    rix_hash_find_u64x16_AVX2,
    rix_hash_bytes_CRC32,
    rix_hash_u32_CRC32,
    rix_hash_u64_CRC32,
};

#    endif /* __x86_64__ && __AVX2__ */

/*===========================================================================
 * AVX-512 find implementations
 *===========================================================================*/
#    if defined(__x86_64__) && defined(__AVX512F__)

static RIX_FORCE_INLINE u32
rix_hash_find_u32x16_AVX512(const u32 *arr,
                            u32 val)
{
    __m512i vval = _mm512_set1_epi32((int)val);
    __m512i va   = _mm512_loadu_si512((const void *)arr);
    __mmask16 m  = _mm512_cmpeq_epi32_mask(va, vval);
    return (u32)m;
}

static RIX_FORCE_INLINE void
rix_hash_find_u32x16_2_AVX512(const u32 *arr,
                              u32 val0,
                              u32 val1,
                              u32 *mask0,
                              u32 *mask1)
{
    __m512i va  = _mm512_loadu_si512((const void *)arr);
    __m512i vv0 = _mm512_set1_epi32((int)val0);
    __m512i vv1 = _mm512_set1_epi32((int)val1);
    *mask0 = (u32)_mm512_cmpeq_epi32_mask(va, vv0);
    *mask1 = (u32)_mm512_cmpeq_epi32_mask(va, vv1);
}

static RIX_FORCE_INLINE u32
rix_hash_find_u64x16_AVX512(const u64 *arr,
                            u64 val)
{
    __m512i vval = _mm512_set1_epi64((long long)val);
    __m512i va   = _mm512_loadu_si512((const void *)(arr + 0));
    __m512i vb   = _mm512_loadu_si512((const void *)(arr + 8));
    __mmask8 m0  = _mm512_cmpeq_epi64_mask(va, vval);
    __mmask8 m1  = _mm512_cmpeq_epi64_mask(vb, vval);
    return (u32)m0 | ((u32)m1 << 8);
}

/* AVX-512 find + CRC32 hash (AVX-512 always implies SSE4.2) */
static RIX_UNUSED const struct rix_hash_arch_s rix_hash_arch_AVX512 = {
    rix_hash_find_u32x16_AVX512,
    rix_hash_find_u32x16_2_AVX512,
    rix_hash_find_u64x16_AVX512,
    rix_hash_bytes_CRC32,
    rix_hash_u32_CRC32,
    rix_hash_u64_CRC32,
};

#    endif /* __x86_64__ && __AVX512F__ */

static RIX_FORCE_INLINE union rix_hash_hash_u
rix_hash_bytes_fast(const void *key,
                    size_t key_bytes,
                    u32 mask)
{
    return rix_hash_arch->hash_bytes(key, key_bytes, mask);
}

/*---------------------------------------------------------------------------
 * rix_hash_arch_init - enable the best dispatch level for this source file.
 *
 * enable: bitmask of RIX_HASH_ARCH_* flags that are allowed.
 *   0                    -> GEN scalar - useful for benchmarking
 *   RIX_HASH_ARCH_SSE    -> SSE XMM 128-bit find
 *   RIX_HASH_ARCH_AVX2   -> AVX2 YMM 256-bit find
 *   RIX_HASH_ARCH_AVX512 -> AVX-512F ZMM 512-bit find
 *   RIX_HASH_ARCH_AUTO   -> best available (recommended default)
 *
 * Implication chain (higher > lower): AVX-512 > AVX2 > SSE > GEN.
 * Enabling a higher level automatically falls back to lower levels if the
 * CPU or compile-time flags do not support it.
 * Safe default: if this function is never called, the source file remains on
 * GEN.
 * For SIMD acceleration, call this in each source file that uses hash
 * table operations.
 *---------------------------------------------------------------------------*/
static RIX_FORCE_INLINE void
rix_hash_arch_init(u32 enable)
{
#    if defined(__x86_64__)
    rix_hash_arch = &rix_hash_arch_GEN; /* per-TU fallback */

    if (!enable)
        return;

#      if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
#        if defined(__AVX512F__)
    if ((enable & RIX_HASH_ARCH_AVX512) &&
        __builtin_cpu_supports("avx512f")) {
        rix_hash_arch = &rix_hash_arch_AVX512;
        return;
    }
#        endif
#        if defined(__AVX2__)
    if ((enable & (RIX_HASH_ARCH_AVX2 | RIX_HASH_ARCH_AVX512)) &&
        __builtin_cpu_supports("avx2")) {
        rix_hash_arch = &rix_hash_arch_AVX2;
        return;
    }
#        endif
#        if defined(__SSE4_2__)
    if ((enable & (RIX_HASH_ARCH_SSE | RIX_HASH_ARCH_AVX2 |
                   RIX_HASH_ARCH_AVX512)) &&
        __builtin_cpu_supports("sse4.2")) {
        rix_hash_arch = &rix_hash_arch_SSE;
        return;
    }
#        endif
#      elif defined(__SSE4_2__)
    if (enable & (RIX_HASH_ARCH_SSE | RIX_HASH_ARCH_AVX2 |
                  RIX_HASH_ARCH_AVX512))
        rix_hash_arch = &rix_hash_arch_SSE;
#      endif

#    else /* !__x86_64__ */
    (void)enable;
    rix_hash_arch = &rix_hash_arch_GEN;
#    endif
}

#  endif /* _RIX_HASH_COMMON_ */

#endif /* _RIX_HASH_ARCH_H_ */
