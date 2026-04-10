#ifndef FT_TEST_RECORD_ALLOCATOR_H
#define FT_TEST_RECORD_ALLOCATOR_H

#include "flow_table.h"

RIX_SLIST_HEAD(ft_record_free_head, ft_record_free_stub);

struct ft_record_allocator {
    unsigned char             *pool_base;
    size_t                     pool_stride;
    struct ft_record_free_head free_head;
    u32                        free_count;
    u32                        capacity;
};

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
    RIX_SLIST_INIT(&alloc->free_head);
    alloc->free_count = 0u;
    if (capacity == 0u)
        return 0;
    if (array == NULL || stride == 0u)
        return -1;
    return 0;
}

#define FT_RECORD_ALLOCATOR_RESET_TYPED(alloc, record_type, link_member)       \
({                                                                             \
    int _ft_alloc_rc = 0;                                                      \
                                                                               \
    if ((alloc) == NULL) {                                                     \
        _ft_alloc_rc = -1;                                                     \
    } else if ((alloc)->capacity == 0u) {                                      \
        RIX_SLIST_INIT(&(alloc)->free_head);                                   \
        (alloc)->free_count = 0u;                                              \
    } else {                                                                   \
        record_type *_ft_alloc_base =                                          \
            (record_type *)(void *)(alloc)->pool_base;                         \
                                                                               \
        if (_ft_alloc_base == NULL                                             \
            || (alloc)->pool_stride != sizeof(record_type)) {                  \
            _ft_alloc_rc = -1;                                                 \
        } else {                                                               \
            unsigned _ft_alloc_i;                                              \
                                                                               \
            RIX_SLIST_INIT(&(alloc)->free_head);                               \
            for (_ft_alloc_i = (alloc)->capacity; _ft_alloc_i != 0u;           \
                 _ft_alloc_i--) {                                              \
                record_type *_ft_alloc_rec =                                   \
                    &_ft_alloc_base[_ft_alloc_i - 1u];                         \
                                                                               \
                _ft_alloc_rec->link_member.rsle_next = RIX_NIL;                \
                RIX_SLIST_INSERT_HEAD(&(alloc)->free_head, _ft_alloc_base,     \
                                      _ft_alloc_rec, link_member);             \
            }                                                                  \
            (alloc)->free_count = (alloc)->capacity;                           \
        }                                                                      \
    }                                                                          \
    _ft_alloc_rc;                                                              \
})

#define FT_RECORD_ALLOCATOR_INIT_TYPED(alloc, array, record_capacity,          \
                                       record_type, link_member)               \
({                                                                             \
    int _ft_alloc_rc = ft_record_allocator_init((alloc), (array),              \
                                                (record_capacity),             \
                                                sizeof(record_type));          \
    if (_ft_alloc_rc == 0) {                                                   \
        if ((alloc)->capacity == 0u) {                                         \
            RIX_SLIST_INIT(&(alloc)->free_head);                               \
            (alloc)->free_count = 0u;                                          \
        } else {                                                               \
            record_type *_ft_init_base =                                       \
                (record_type *)(void *)(alloc)->pool_base;                     \
                                                                               \
            if (_ft_init_base == NULL                                          \
                || (alloc)->pool_stride != sizeof(record_type)) {              \
                _ft_alloc_rc = -1;                                             \
            } else {                                                           \
                unsigned _ft_init_i;                                           \
                                                                               \
                RIX_SLIST_INIT(&(alloc)->free_head);                           \
                for (_ft_init_i = (alloc)->capacity; _ft_init_i != 0u;         \
                     _ft_init_i--) {                                           \
                    record_type *_ft_init_rec =                                \
                        &_ft_init_base[_ft_init_i - 1u];                       \
                                                                               \
                    _ft_init_rec->link_member.rsle_next = RIX_NIL;             \
                    RIX_SLIST_INSERT_HEAD(&(alloc)->free_head, _ft_init_base,  \
                                          _ft_init_rec, link_member);          \
                }                                                              \
                (alloc)->free_count = (alloc)->capacity;                       \
            }                                                                  \
        }                                                                      \
    }                                                                          \
    _ft_alloc_rc;                                                              \
})

#define FT_RECORD_ALLOCATOR_ALLOC_IDX_TYPED(alloc, record_type, link_member)   \
({                                                                             \
    u32 _ft_alloc_idx = 0u;                                                    \
                                                                               \
    if ((alloc) != NULL && (alloc)->pool_base != NULL) {                       \
        record_type *_ft_alloc_base =                                          \
            (record_type *)(void *)(alloc)->pool_base;                         \
        record_type *_ft_alloc_rec =                                           \
            RIX_SLIST_FIRST(&(alloc)->free_head, _ft_alloc_base);              \
                                                                               \
        if (_ft_alloc_rec != NULL) {                                           \
            _ft_alloc_idx = RIX_IDX_FROM_PTR(_ft_alloc_base, _ft_alloc_rec);   \
            RIX_SLIST_REMOVE_HEAD(&(alloc)->free_head, _ft_alloc_base,         \
                                  link_member);                                \
            _ft_alloc_rec->link_member.rsle_next = _ft_alloc_idx;              \
            (alloc)->free_count--;                                             \
        }                                                                      \
    }                                                                          \
    _ft_alloc_idx;                                                             \
})

#define FT_RECORD_ALLOCATOR_FREE_IDX_TYPED(alloc, record_type, link_member,    \
                                           entry_idx)                          \
({                                                                             \
    int _ft_alloc_rc = -1;                                                     \
    u32 _ft_free_idx = (u32)(entry_idx);                                       \
                                                                               \
    if ((alloc) != NULL && (alloc)->pool_base != NULL                          \
        && (alloc)->free_count < (alloc)->capacity                             \
        && _ft_free_idx != RIX_NIL                                             \
        && _ft_free_idx <= (alloc)->capacity) {                                \
        record_type *_ft_alloc_base =                                          \
            (record_type *)(void *)(alloc)->pool_base;                         \
                                                                               \
        {                                                                      \
            record_type *_ft_alloc_rec = &_ft_alloc_base[_ft_free_idx - 1u];   \
                                                                               \
            if (_ft_alloc_rec->link_member.rsle_next == _ft_free_idx) {        \
                RIX_SLIST_INSERT_HEAD(&(alloc)->free_head, _ft_alloc_base,     \
                                      _ft_alloc_rec, link_member);             \
                (alloc)->free_count++;                                         \
                _ft_alloc_rc = 0;                                              \
            }                                                                  \
        }                                                                      \
    }                                                                          \
    _ft_alloc_rc;                                                              \
})

#define FT_RECORD_ALLOCATOR_ALLOC_BULK_TYPED(alloc, record_type, link_member,  \
                                             entry_idxv, max_entries)          \
({                                                                             \
    unsigned _ft_alloc_n;                                                      \
                                                                               \
    if ((entry_idxv) == NULL) {                                                \
        _ft_alloc_n = 0u;                                                      \
    } else {                                                                   \
        for (_ft_alloc_n = 0u; _ft_alloc_n < (max_entries);                    \
             _ft_alloc_n++) {                                                  \
            u32 _ft_bulk_idx = 0u;                                             \
                                                                               \
            if ((alloc) != NULL && (alloc)->pool_base != NULL) {               \
                record_type *_ft_bulk_base =                                   \
                    (record_type *)(void *)(alloc)->pool_base;                 \
                record_type *_ft_bulk_rec =                                    \
                    RIX_SLIST_FIRST(&(alloc)->free_head, _ft_bulk_base);       \
                                                                               \
                if (_ft_bulk_rec != NULL) {                                    \
                    _ft_bulk_idx =                                             \
                        RIX_IDX_FROM_PTR(_ft_bulk_base, _ft_bulk_rec);         \
                    RIX_SLIST_REMOVE_HEAD(&(alloc)->free_head, _ft_bulk_base,  \
                                          link_member);                        \
                    _ft_bulk_rec->link_member.rsle_next = _ft_bulk_idx;        \
                    (alloc)->free_count--;                                     \
                }                                                              \
            }                                                                  \
            if (_ft_bulk_idx == 0u)                                            \
                break;                                                         \
            (entry_idxv)[_ft_alloc_n] = _ft_bulk_idx;                          \
        }                                                                      \
    }                                                                          \
    _ft_alloc_n;                                                               \
})

#define FT_RECORD_ALLOCATOR_FREE_BULK_TYPED(alloc, record_type, link_member,   \
                                            entry_idxv, nb_entries)            \
({                                                                             \
    unsigned _ft_alloc_n;                                                      \
                                                                               \
    if ((entry_idxv) == NULL) {                                                \
        _ft_alloc_n = 0u;                                                      \
    } else {                                                                   \
        for (_ft_alloc_n = 0u; _ft_alloc_n < (nb_entries);                     \
             _ft_alloc_n++) {                                                  \
            u32 _ft_free_idx = (u32)(entry_idxv)[_ft_alloc_n];                 \
                                                                               \
            if ((alloc) == NULL || (alloc)->pool_base == NULL                  \
                || (alloc)->free_count >= (alloc)->capacity                    \
                || _ft_free_idx == RIX_NIL                                     \
                || _ft_free_idx > (alloc)->capacity)                           \
                break;                                                         \
            else {                                                             \
                record_type *_ft_free_base =                                   \
                    (record_type *)(void *)(alloc)->pool_base;                 \
                record_type *_ft_free_rec =                                    \
                    &_ft_free_base[_ft_free_idx - 1u];                         \
                                                                               \
                if (_ft_free_rec->link_member.rsle_next != _ft_free_idx)       \
                    break;                                                     \
                RIX_SLIST_INSERT_HEAD(&(alloc)->free_head, _ft_free_base,      \
                                      _ft_free_rec, link_member);              \
                (alloc)->free_count++;                                         \
            }                                                                  \
        }                                                                      \
    }                                                                          \
    _ft_alloc_n;                                                               \
})

#define FT_RECORD_ALLOCATOR_RECORD_PTR_AS(alloc, record_type, entry_idx)       \
    ((record_type *)ft_record_allocator_record_ptr((alloc), (entry_idx)))

#endif
