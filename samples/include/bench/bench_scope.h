/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_scope.h - Minimal scoped perf_event_open utility for sample benches.
 *
 * Intended use:
 *   - Measure one hot section at a time
 *   - Run multiple passes with small event sets to avoid multiplexing
 *   - Keep the API small and header-only for sample code
 */

#ifndef _BENCH_SCOPE_H_
#define _BENCH_SCOPE_H_

#include <errno.h>
#include <stdint.h>
#include <string.h>

#ifdef __linux__
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifndef BENCH_SCOPE_MAX_EVENTS
#define BENCH_SCOPE_MAX_EVENTS 8u
#endif

enum bench_scope_event_kind {
    BENCH_SCOPE_EVT_CYCLES = 0,
    BENCH_SCOPE_EVT_INSTRUCTIONS,
    BENCH_SCOPE_EVT_BRANCHES,
    BENCH_SCOPE_EVT_BRANCH_MISSES,
    BENCH_SCOPE_EVT_CACHE_REFERENCES,
    BENCH_SCOPE_EVT_CACHE_MISSES,
    BENCH_SCOPE_EVT_L1D_LOADS,
    BENCH_SCOPE_EVT_L1D_LOAD_MISSES
};

struct bench_scope_value {
    enum bench_scope_event_kind kind;
    uint64_t value;
};

struct bench_scope_sample {
    unsigned count;
    struct bench_scope_value values[BENCH_SCOPE_MAX_EVENTS];
};

struct bench_scope_group {
    unsigned count;
    int leader_fd;
    int fds[BENCH_SCOPE_MAX_EVENTS];
    uint64_t ids[BENCH_SCOPE_MAX_EVENTS];
    enum bench_scope_event_kind kinds[BENCH_SCOPE_MAX_EVENTS];
};

static inline const char *
bench_scope_event_name(enum bench_scope_event_kind kind)
{
    switch (kind) {
    case BENCH_SCOPE_EVT_CYCLES:
        return "cycles";
    case BENCH_SCOPE_EVT_INSTRUCTIONS:
        return "instructions";
    case BENCH_SCOPE_EVT_BRANCHES:
        return "branches";
    case BENCH_SCOPE_EVT_BRANCH_MISSES:
        return "branch-misses";
    case BENCH_SCOPE_EVT_CACHE_REFERENCES:
        return "cache-references";
    case BENCH_SCOPE_EVT_CACHE_MISSES:
        return "cache-misses";
    case BENCH_SCOPE_EVT_L1D_LOADS:
        return "l1d-loads";
    case BENCH_SCOPE_EVT_L1D_LOAD_MISSES:
        return "l1d-load-misses";
    }
    return "unknown";
}

#ifdef __linux__

static inline int
bench_scope_attr_init_(struct perf_event_attr *attr,
                       enum bench_scope_event_kind kind)
{
    memset(attr, 0, sizeof(*attr));
    attr->size = sizeof(*attr);
    attr->disabled = 1u;
    attr->inherit = 0u;
    attr->exclude_hv = 1u;
    attr->exclude_idle = 1u;
    attr->read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;

    switch (kind) {
    case BENCH_SCOPE_EVT_CYCLES:
        attr->type = PERF_TYPE_HARDWARE;
        attr->config = PERF_COUNT_HW_CPU_CYCLES;
        return 0;
    case BENCH_SCOPE_EVT_INSTRUCTIONS:
        attr->type = PERF_TYPE_HARDWARE;
        attr->config = PERF_COUNT_HW_INSTRUCTIONS;
        return 0;
    case BENCH_SCOPE_EVT_BRANCHES:
        attr->type = PERF_TYPE_HARDWARE;
        attr->config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
        return 0;
    case BENCH_SCOPE_EVT_BRANCH_MISSES:
        attr->type = PERF_TYPE_HARDWARE;
        attr->config = PERF_COUNT_HW_BRANCH_MISSES;
        return 0;
    case BENCH_SCOPE_EVT_CACHE_REFERENCES:
        attr->type = PERF_TYPE_HARDWARE;
        attr->config = PERF_COUNT_HW_CACHE_REFERENCES;
        return 0;
    case BENCH_SCOPE_EVT_CACHE_MISSES:
        attr->type = PERF_TYPE_HARDWARE;
        attr->config = PERF_COUNT_HW_CACHE_MISSES;
        return 0;
    case BENCH_SCOPE_EVT_L1D_LOADS:
        attr->type = PERF_TYPE_HW_CACHE;
        attr->config =
            ((uint64_t)PERF_COUNT_HW_CACHE_L1D) |
            ((uint64_t)PERF_COUNT_HW_CACHE_OP_READ << 8) |
            ((uint64_t)PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);
        return 0;
    case BENCH_SCOPE_EVT_L1D_LOAD_MISSES:
        attr->type = PERF_TYPE_HW_CACHE;
        attr->config =
            ((uint64_t)PERF_COUNT_HW_CACHE_L1D) |
            ((uint64_t)PERF_COUNT_HW_CACHE_OP_READ << 8) |
            ((uint64_t)PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static inline int
bench_scope_perf_event_open_(struct perf_event_attr *attr,
                             pid_t pid,
                             int cpu,
                             int group_fd,
                             unsigned long flags)
{
    return (int)syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static inline void
bench_scope_group_init(struct bench_scope_group *group)
{
    unsigned i;

    memset(group, 0, sizeof(*group));
    group->leader_fd = -1;
    for (i = 0; i < BENCH_SCOPE_MAX_EVENTS; i++)
        group->fds[i] = -1;
}

static inline void
bench_scope_close(struct bench_scope_group *group)
{
    unsigned i;

    for (i = 0; i < group->count; i++) {
        if (group->fds[i] >= 0) {
            close(group->fds[i]);
            group->fds[i] = -1;
        }
    }
    group->leader_fd = -1;
    group->count = 0u;
}

static inline int
bench_scope_open(struct bench_scope_group *group,
                 const enum bench_scope_event_kind *kinds,
                 unsigned count)
{
    unsigned i;

    if (count == 0u || count > BENCH_SCOPE_MAX_EVENTS) {
        errno = EINVAL;
        return -1;
    }

    bench_scope_group_init(group);
    group->count = count;

    for (i = 0; i < count; i++) {
        struct perf_event_attr attr;
        int fd;
        int group_fd;

        if (bench_scope_attr_init_(&attr, kinds[i]) != 0)
            goto fail;
        group_fd = (i == 0u) ? -1 : group->leader_fd;
        fd = bench_scope_perf_event_open_(&attr, 0, -1, group_fd, 0ul);
        if (fd < 0)
            goto fail;
        group->fds[i] = fd;
        group->kinds[i] = kinds[i];
        if (i == 0u)
            group->leader_fd = fd;
        if (ioctl(fd, PERF_EVENT_IOC_ID, &group->ids[i]) != 0)
            goto fail;
    }
    return 0;

fail:
    bench_scope_close(group);
    return -1;
}

static inline int
bench_scope_begin(struct bench_scope_group *group)
{
    if (group->leader_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (ioctl(group->leader_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0)
        return -1;
    if (ioctl(group->leader_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0)
        return -1;
    return 0;
}

static inline int
bench_scope_end(struct bench_scope_group *group,
                struct bench_scope_sample *sample)
{
    struct {
        uint64_t nr;
        struct {
            uint64_t value;
            uint64_t id;
        } values[BENCH_SCOPE_MAX_EVENTS];
    } data;
    unsigned i, j;
    ssize_t need;
    ssize_t got;

    if (group->leader_fd < 0 || sample == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ioctl(group->leader_fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) != 0)
        return -1;

    memset(&data, 0, sizeof(data));
    need = (ssize_t)(sizeof(uint64_t) +
        group->count * sizeof(data.values[0]));
    got = read(group->leader_fd, &data, (size_t)need);
    if (got != need) {
        if (got >= 0)
            errno = EIO;
        return -1;
    }

    memset(sample, 0, sizeof(*sample));
    sample->count = group->count;
    for (i = 0; i < group->count; i++)
        sample->values[i].kind = group->kinds[i];

    for (i = 0; i < group->count; i++) {
        for (j = 0; j < data.nr; j++) {
            if (group->ids[i] == data.values[j].id) {
                sample->values[i].value = data.values[j].value;
                break;
            }
        }
    }
    return 0;
}

#else

static inline void
bench_scope_group_init(struct bench_scope_group *group)
{
    memset(group, 0, sizeof(*group));
    group->leader_fd = -1;
}

static inline void
bench_scope_close(struct bench_scope_group *group)
{
    bench_scope_group_init(group);
}

static inline int
bench_scope_open(struct bench_scope_group *group,
                 const enum bench_scope_event_kind *kinds,
                 unsigned count)
{
    (void)group;
    (void)kinds;
    (void)count;
    errno = ENOTSUP;
    return -1;
}

static inline int
bench_scope_begin(struct bench_scope_group *group)
{
    (void)group;
    errno = ENOTSUP;
    return -1;
}

static inline int
bench_scope_end(struct bench_scope_group *group,
                struct bench_scope_sample *sample)
{
    (void)group;
    (void)sample;
    errno = ENOTSUP;
    return -1;
}

#endif

#endif
