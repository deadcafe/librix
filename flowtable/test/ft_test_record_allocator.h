#ifndef FT_TEST_RECORD_ALLOCATOR_H
#define FT_TEST_RECORD_ALLOCATOR_H

#include <stdlib.h>

#include <rix/rix_ring.h>

#include "flow_table.h"

struct ft_record_allocator {
    unsigned char   *pool_base;
    size_t           pool_stride;
    struct rix_ring  free_ring;
    u32             *free_idxv;
    u32              free_count;
    u32              capacity;
};

static inline void
ft_record_allocator_sync_free_count(struct ft_record_allocator *alloc)
{
    alloc->free_count = rix_ring_count(&alloc->free_ring);
}

static inline void *
ft_record_allocator_record_ptr(struct ft_record_allocator *alloc, u32 entry_idx)
{
    if (alloc == NULL || alloc->pool_base == NULL
        || !RIX_IDX_IS_VALID(entry_idx, alloc->capacity))
        return NULL;
    return FT_RECORD_PTR(alloc->pool_base, alloc->pool_stride, entry_idx);
}

static inline const void *
ft_record_allocator_record_cptr(const struct ft_record_allocator *alloc,
                                u32 entry_idx)
{
    if (alloc == NULL || alloc->pool_base == NULL
        || !RIX_IDX_IS_VALID(entry_idx, alloc->capacity))
        return NULL;
    return FT_RECORD_CPTR(alloc->pool_base, alloc->pool_stride, entry_idx);
}

static inline int
ft_record_allocator_reset(struct ft_record_allocator *alloc)
{
    if (alloc == NULL)
        return -1;
    if (alloc->capacity == 0u) {
        rix_ring_reset(&alloc->free_ring);
        alloc->free_count = 0u;
        return 0;
    }
    if (alloc->pool_base == NULL || alloc->pool_stride == 0u
        || alloc->free_idxv == NULL)
        return -1;
    rix_ring_reset(&alloc->free_ring);
    rix_ring_push_seq(&alloc->free_ring, 1u, alloc->capacity);
    ft_record_allocator_sync_free_count(alloc);
    return 0;
}

static inline int
ft_record_allocator_init(struct ft_record_allocator *alloc,
                         void *array,
                         unsigned capacity,
                         size_t stride)
{
    if (alloc == NULL)
        return -1;
    alloc->pool_base = (unsigned char *)array;
    alloc->pool_stride = stride;
    alloc->capacity = capacity;
    alloc->free_idxv = NULL;
    alloc->free_count = 0u;
    rix_ring_init(&alloc->free_ring, NULL, 0u);
    if (capacity == 0u)
        return 0;
    if (array == NULL || stride == 0u)
        return -1;
    alloc->free_idxv = malloc((size_t)capacity * sizeof(*alloc->free_idxv));
    if (alloc->free_idxv == NULL)
        return -1;
    rix_ring_init(&alloc->free_ring, alloc->free_idxv, capacity);
    return ft_record_allocator_reset(alloc);
}

static inline void
ft_record_allocator_destroy(struct ft_record_allocator *alloc)
{
    if (alloc == NULL)
        return;
    free(alloc->free_idxv);
    alloc->pool_base = NULL;
    alloc->pool_stride = 0u;
    alloc->capacity = 0u;
    alloc->free_idxv = NULL;
    alloc->free_count = 0u;
    rix_ring_init(&alloc->free_ring, NULL, 0u);
}

#define FT_RECORD_ALLOCATOR_RESET_TYPED(alloc, record_type)                   \
({                                                                            \
    int _ft_alloc_rc = -1;                                                    \
    if ((alloc) != NULL) {                                                    \
        (void)sizeof(record_type);                                            \
        if ((alloc)->capacity == 0u                                           \
            || (alloc)->pool_stride == sizeof(record_type))                   \
            _ft_alloc_rc = ft_record_allocator_reset((alloc));                \
    }                                                                         \
    _ft_alloc_rc;                                                             \
})

#define FT_RECORD_ALLOCATOR_INIT_TYPED(alloc, array, record_capacity,         \
                                       record_type)                           \
({                                                                            \
    int _ft_alloc_rc = ft_record_allocator_init((alloc), (array),             \
                                                (record_capacity),            \
                                                sizeof(record_type));         \
    _ft_alloc_rc;                                                             \
})

#define FT_RECORD_ALLOCATOR_ALLOC_IDX_TYPED(alloc, record_type)               \
({                                                                            \
    u32 _ft_alloc_idx = 0u;                                                   \
    (void)sizeof(record_type);                                                \
    if ((alloc) != NULL && (alloc)->pool_base != NULL) {                      \
        if (rix_ring_pop_burst(&(alloc)->free_ring, &_ft_alloc_idx, 1u) == 1u) \
            ft_record_allocator_sync_free_count((alloc));                     \
        else                                                                  \
            _ft_alloc_idx = 0u;                                               \
    }                                                                         \
    _ft_alloc_idx;                                                            \
})

#define FT_RECORD_ALLOCATOR_FREE_IDX_TYPED(alloc, record_type, entry_idx)     \
({                                                                            \
    int _ft_alloc_rc = -1;                                                    \
    u32 _ft_free_idx = (u32)(entry_idx);                                      \
    (void)sizeof(record_type);                                                \
    if ((alloc) != NULL && (alloc)->pool_base != NULL                         \
        && RIX_IDX_IS_VALID(_ft_free_idx, (alloc)->capacity)) {               \
        if (rix_ring_push_burst(&(alloc)->free_ring, &_ft_free_idx, 1u) == 1u) \
        {                                                                     \
            ft_record_allocator_sync_free_count((alloc));                     \
            _ft_alloc_rc = 0;                                                 \
        }                                                                     \
    }                                                                         \
    _ft_alloc_rc;                                                             \
})

#define FT_RECORD_ALLOCATOR_ALLOC_BULK_TYPED(alloc, record_type, entry_idxv,  \
                                             max_entries)                     \
({                                                                            \
    unsigned _ft_alloc_n = 0u;                                                \
    (void)sizeof(record_type);                                                \
    if ((entry_idxv) != NULL && (alloc) != NULL && (alloc)->pool_base != NULL) { \
        _ft_alloc_n = rix_ring_pop_burst(&(alloc)->free_ring, (entry_idxv),   \
                                         (u32)(max_entries));                 \
        ft_record_allocator_sync_free_count((alloc));                         \
    }                                                                         \
    _ft_alloc_n;                                                              \
})

#define FT_RECORD_ALLOCATOR_FREE_BULK_TYPED(alloc, record_type, entry_idxv,   \
                                            nb_entries)                       \
({                                                                            \
    unsigned _ft_alloc_n = 0u;                                                \
    (void)sizeof(record_type);                                                \
    if ((entry_idxv) != NULL && (alloc) != NULL && (alloc)->pool_base != NULL) { \
        while (_ft_alloc_n < (nb_entries)                                     \
               && RIX_IDX_IS_VALID((u32)(entry_idxv)[_ft_alloc_n],            \
                                   (alloc)->capacity))                        \
            _ft_alloc_n++;                                                    \
        _ft_alloc_n = rix_ring_push_burst(&(alloc)->free_ring, (entry_idxv),  \
                                          (u32)_ft_alloc_n);                  \
        ft_record_allocator_sync_free_count((alloc));                         \
    }                                                                         \
    _ft_alloc_n;                                                              \
})

#define FT_RECORD_ALLOCATOR_RECORD_PTR_AS(alloc, record_type, entry_idx)      \
    ((record_type *)ft_record_allocator_record_ptr((alloc), (entry_idx)))

#endif
/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
