/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2025 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 */
#ifndef _RIX_TREE_H_
#  define _RIX_TREE_H_

#  include "rix_defs_private.h"

/*
 * RB Tree
 */
#  define RIX_RB_HEAD(name)                                                   \
    struct name {                                                             \
        unsigned rrh_root;                                                    \
    }

#  define RIX_RB_HEAD_INITIALIZER(var)                                        \
    { RIX_NIL }

#  define RIX_RB_INIT(head)                                                   \
    do {                                                                      \
        (head)->rrh_root = RIX_NIL;                                           \
    } while (0)

#  define RIX_RB_ENTRY(type)                                                  \
    struct {                                                                  \
        unsigned rbe_parent;                                                  \
        unsigned rbe_left;                                                    \
        unsigned rbe_right;                                                   \
        unsigned rbe_color;                                                   \
    }

#  define RIX_RB_RED ((unsigned)0)
#  define RIX_RB_BLACK ((unsigned)1)

#  define RIX_RB_ROOT(head, base) (RIX_PTR_FROM_IDX((base), (head)->rrh_root))
#  define RIX_RB_EMPTY(head) ((head)->rrh_root == RIX_NIL)
#  define RIX_RB_PARENT(elm, base, field)                                     \
    (RIX_PTR_FROM_IDX((base), (elm)->field.rbe_parent))
#  define RIX_RB_LEFT(elm, base, field)                                       \
    (RIX_PTR_FROM_IDX((base), (elm)->field.rbe_left))
#  define RIX_RB_RIGHT(elm, base, field)                                      \
    (RIX_PTR_FROM_IDX((base), (elm)->field.rbe_right))
#  define RIX_RB_COLOR(elm, field) ((elm)->field.rbe_color)

#  define RIX_RB_PROTOTYPE_INTERNAL(name, type, field, cmp, attr)             \
    attr type *name##_RIX_RB_INSERT(struct name *head, type *base, type *elm); \
    attr type *name##_RIX_RB_REMOVE(struct name *head, type *base, type *elm); \
    attr type *name##_RIX_RB_FIND(struct name *head, type *base,              \
                                   const type *key);                          \
    attr type *name##_RIX_RB_NFIND(struct name *head, type *base,             \
                                    const type *key);                         \
    attr type *name##_RIX_RB_MINMAX(struct name *head, type *base, int dir);  \
    attr type *name##_RIX_RB_NEXT(type *base, type *elm);                     \
    attr type *name##_RIX_RB_PREV(type *base, type *elm);

#  define RIX_RB_PROTOTYPE(name, type, field, cmp)                            \
    RIX_RB_PROTOTYPE_INTERNAL(name, type, field, cmp, )

#  define RIX_RB_PROTOTYPE_STATIC(name, type, field, cmp)                     \
    RIX_RB_PROTOTYPE_INTERNAL(name, type, field, cmp, RIX_UNUSED static)

#  define RIX_RB_INSERT(name, head, base, elm)                                \
    name##_RIX_RB_INSERT((head), (base), (elm))
#  define RIX_RB_REMOVE(name, head, base, elm)                                \
    name##_RIX_RB_REMOVE((head), (base), (elm))
#  define RIX_RB_FIND(name, head, base, key)                                  \
    name##_RIX_RB_FIND((head), (base), (key))
#  define RIX_RB_NFIND(name, head, base, key)                                 \
    name##_RIX_RB_NFIND((head), (base), (key))
#  define RIX_RB_MIN(name, head, base) name##_RIX_RB_MINMAX((head), (base), -1)
#  define RIX_RB_MAX(name, head, base) name##_RIX_RB_MINMAX((head), (base), +1)
#  define RIX_RB_NEXT(name, base, elm) name##_RIX_RB_NEXT((base), (elm))
#  define RIX_RB_PREV(name, base, elm) name##_RIX_RB_PREV((base), (elm))

#  define RIX_RB_FOREACH(x, name, head, base)                                 \
    for ((x) = RIX_RB_MIN(name, (head), (base)); (x) != NULL;                 \
         (x) = name##_RIX_RB_NEXT((base), (x)))

#  define RIX_RB_FOREACH_REVERSE(x, name, head, base)                         \
    for ((x) = RIX_RB_MAX(name, (head), (base)); (x) != NULL;                 \
         (x) = name##_RIX_RB_PREV((base), (x)))

#  define RIX_RB_GENERATE(name, type, field, cmp)                             \
    RIX_RB_GENERATE_INTERNAL(name, type, field, cmp, )

#  define RIX_RB_GENERATE_STATIC(name, type, field, cmp)                      \
    RIX_RB_GENERATE_INTERNAL(name, type, field, cmp, RIX_UNUSED static)

#  define RIX_RB_GENERATE_INTERNAL(name, type, field, cmp, attr)              \
    static RIX_FORCE_INLINE unsigned name##_idx(type *base, const type *p) {  \
        return RIX_IDX_FROM_PTR(base, (type *)p);                             \
    }                                                                         \
    static RIX_FORCE_INLINE type *name##_ptr(type *base, unsigned idx) {      \
        return RIX_PTR_FROM_IDX(base, idx);                                   \
    }                                                                         \
    static RIX_FORCE_INLINE unsigned name##_color_idx(type *base,             \
                                                      unsigned idx) {         \
        return (idx == RIX_NIL) ? RIX_RB_BLACK                                \
                                : name##_ptr(base, idx)->field.rbe_color;     \
    }                                                                         \
    static RIX_FORCE_INLINE void name##_set_color_idx(type *base, unsigned idx, \
                                                      unsigned char c) {      \
        if (idx != RIX_NIL)                                                   \
            name##_ptr(base, idx)->field.rbe_color = c;                       \
    }                                                                         \
    static RIX_FORCE_INLINE unsigned name##_left_idx(type *base, unsigned idx) { \
        return (idx == RIX_NIL) ? RIX_NIL : name##_ptr(base, idx)->field.rbe_left; \
    }                                                                         \
    static RIX_FORCE_INLINE unsigned name##_right_idx(type *base,             \
                                                      unsigned idx) {         \
        return (idx == RIX_NIL) ? RIX_NIL                                     \
                                : name##_ptr(base, idx)->field.rbe_right;     \
    }                                                                         \
    static RIX_FORCE_INLINE unsigned name##_parent_idx(type *base,            \
                                                       unsigned idx) {        \
        return (idx == RIX_NIL) ? RIX_NIL                                     \
                                : name##_ptr(base, idx)->field.rbe_parent;    \
    }                                                                         \
    static RIX_FORCE_INLINE void name##_set_left(type *base, unsigned p,      \
                                                 unsigned c) {                \
        if (p != RIX_NIL)                                                     \
            name##_ptr(base, p)->field.rbe_left = c;                          \
        if (c != RIX_NIL)                                                     \
            name##_ptr(base, c)->field.rbe_parent = p;                        \
    }                                                                         \
    static RIX_FORCE_INLINE void name##_set_right(type *base, unsigned p,     \
                                                  unsigned c) {               \
        if (p != RIX_NIL)                                                     \
            name##_ptr(base, p)->field.rbe_right = c;                         \
        if (c != RIX_NIL)                                                     \
            name##_ptr(base, c)->field.rbe_parent = p;                        \
    }                                                                         \
    static RIX_FORCE_INLINE void name##_transplant(                           \
        struct name *head, type *base, unsigned u, unsigned v) {              \
        unsigned up = name##_parent_idx(base, u);                             \
        if (up == RIX_NIL) {                                                  \
            (head)->rrh_root = v;                                             \
            if (v != RIX_NIL)                                                 \
                name##_ptr(base, v)->field.rbe_parent = RIX_NIL;              \
        } else if (u == name##_left_idx(base, up)) {                          \
            name##_set_left(base, up, v);                                     \
        } else {                                                              \
            name##_set_right(base, up, v);                                    \
        }                                                                     \
    }                                                                         \
    static RIX_FORCE_INLINE void name##_rotate_left(struct name *head,        \
                                                    type *base, unsigned x) { \
        unsigned y = name##_right_idx(base, x);                               \
        unsigned yL = name##_left_idx(base, y);                               \
        name##_set_right(base, x, yL);                                        \
        unsigned xp = name##_parent_idx(base, x);                             \
        if (xp == RIX_NIL) {                                                  \
            (head)->rrh_root = y;                                             \
            if (y != RIX_NIL)                                                 \
                name##_ptr(base, y)->field.rbe_parent = RIX_NIL;              \
        } else if (x == name##_left_idx(base, xp)) {                          \
            name##_set_left(base, xp, y);                                     \
        } else {                                                              \
            name##_set_right(base, xp, y);                                    \
        }                                                                     \
        name##_set_left(base, y, x);                                          \
    }                                                                         \
    static RIX_FORCE_INLINE void name##_rotate_right(struct name *head,       \
                                                     type *base, unsigned x) { \
        unsigned y = name##_left_idx(base, x);                                \
        unsigned yR = name##_right_idx(base, y);                              \
        name##_set_left(base, x, yR);                                         \
        unsigned xp = name##_parent_idx(base, x);                             \
        if (xp == RIX_NIL) {                                                  \
            (head)->rrh_root = y;                                             \
            if (y != RIX_NIL)                                                 \
                name##_ptr(base, y)->field.rbe_parent = RIX_NIL;              \
        } else if (x == name##_left_idx(base, xp)) {                          \
            name##_set_left(base, xp, y);                                     \
        } else {                                                              \
            name##_set_right(base, xp, y);                                    \
        }                                                                     \
        name##_set_right(base, y, x);                                         \
    }                                                                         \
    attr type *name##_RIX_RB_INSERT(struct name *head, type *base, type *elm) { \
        unsigned z = name##_idx(base, elm);                                   \
        /* find place */                                                      \
        unsigned y = RIX_NIL;                                                 \
        unsigned x = (head)->rrh_root;                                        \
        while (x != RIX_NIL) {                                                \
            y = x;                                                            \
            int c = (cmp)(elm, name##_ptr(base, x));                          \
            if (c < 0)                                                        \
                x = name##_left_idx(base, x);                                 \
            else if (c > 0)                                                   \
                x = name##_right_idx(base, x);                                \
            else                                                              \
                return name##_ptr(base, x);                                   \
        }                                                                     \
        /* link z under y */                                                  \
        elm->field.rbe_parent = y;                                            \
        elm->field.rbe_left = RIX_NIL;                                        \
        elm->field.rbe_right = RIX_NIL;                                       \
        elm->field.rbe_color = RIX_RB_RED;                                    \
        if (y == RIX_NIL)                                                     \
            (head)->rrh_root = z;                                             \
        else if ((cmp)(elm, name##_ptr(base, y)) < 0)                         \
            name##_set_left(base, y, z);                                      \
        else                                                                  \
            name##_set_right(base, y, z);                                     \
        /* fix-up */                                                          \
        while ((z != (head)->rrh_root) &&                                     \
               name##_color_idx(base, name##_parent_idx(base, z)) == RIX_RB_RED) { \
            unsigned p = name##_parent_idx(base, z);                          \
            unsigned g = name##_parent_idx(base, p);                          \
            if (p == name##_left_idx(base, g)) {                              \
                unsigned u = name##_right_idx(base, g); /* uncle */           \
                if (name##_color_idx(base, u) == RIX_RB_RED) {                \
                    name##_set_color_idx(base, p, RIX_RB_BLACK);              \
                    name##_set_color_idx(base, u, RIX_RB_BLACK);              \
                    name##_set_color_idx(base, g, RIX_RB_RED);                \
                    z = g;                                                    \
                } else {                                                      \
                    if (z == name##_right_idx(base, p)) {                     \
                        z = p;                                                \
                        name##_rotate_left(head, base, z);                    \
                        p = name##_parent_idx(base, z);                       \
                        g = name##_parent_idx(base, p);                       \
                    }                                                         \
                    name##_set_color_idx(base, p, RIX_RB_BLACK);              \
                    name##_set_color_idx(base, g, RIX_RB_RED);                \
                    name##_rotate_right(head, base, g);                       \
                }                                                             \
            } else { /* mirror */                                             \
                unsigned u = name##_left_idx(base, g);                        \
                if (name##_color_idx(base, u) == RIX_RB_RED) {                \
                    name##_set_color_idx(base, p, RIX_RB_BLACK);              \
                    name##_set_color_idx(base, u, RIX_RB_BLACK);              \
                    name##_set_color_idx(base, g, RIX_RB_RED);                \
                    z = g;                                                    \
                } else {                                                      \
                    if (z == name##_left_idx(base, p)) {                      \
                        z = p;                                                \
                        name##_rotate_right(head, base, z);                   \
                        p = name##_parent_idx(base, z);                       \
                        g = name##_parent_idx(base, p);                       \
                    }                                                         \
                    name##_set_color_idx(base, p, RIX_RB_BLACK);              \
                    name##_set_color_idx(base, g, RIX_RB_RED);                \
                    name##_rotate_left(head, base, g);                        \
                }                                                             \
            }                                                                 \
        }                                                                     \
        name##_set_color_idx(base, (head)->rrh_root, RIX_RB_BLACK);           \
        if ((head)->rrh_root != RIX_NIL)                                      \
            name##_ptr(base, (head)->rrh_root)->field.rbe_parent = RIX_NIL;   \
        return NULL;                                                          \
    }                                                                         \
    attr type *name##_RIX_RB_MINMAX(struct name *head, type *base, int dir) { \
        unsigned x = (head)->rrh_root;                                        \
        if (x == RIX_NIL)                                                     \
            return NULL;                                                      \
        if (dir < 0) { /* MIN */                                              \
            while (name##_left_idx(base, x) != RIX_NIL)                       \
                x = name##_left_idx(base, x);                                 \
        } else {                                                              \
            while (name##_right_idx(base, x) != RIX_NIL)                      \
                x = name##_right_idx(base, x);                                \
        }                                                                     \
        return name##_ptr(base, x);                                           \
    }                                                                         \
    /* ---- successor / predecessor ---- */                                   \
    attr type *name##_RIX_RB_NEXT(type *base, type *elm) {                    \
        unsigned x = RIX_IDX_FROM_PTR(base, elm);                             \
        if (x == RIX_NIL)                                                     \
            return NULL;                                                      \
        unsigned r = name##_right_idx(base, x);                               \
        if (r != RIX_NIL) {                                                   \
            x = r;                                                            \
            while (name##_left_idx(base, x) != RIX_NIL)                       \
                x = name##_left_idx(base, x);                                 \
            return name##_ptr(base, x);                                       \
        } else {                                                              \
            unsigned p = name##_parent_idx(base, x);                          \
            while (p != RIX_NIL && x == name##_right_idx(base, p)) {          \
                x = p;                                                        \
                p = name##_parent_idx(base, x);                               \
            }                                                                 \
            return name##_ptr(base, p);                                       \
        }                                                                     \
    }                                                                         \
    attr type *name##_RIX_RB_PREV(type *base, type *elm) {                    \
        unsigned x = RIX_IDX_FROM_PTR(base, elm);                             \
        if (x == RIX_NIL)                                                     \
            return NULL;                                                      \
        unsigned l = name##_left_idx(base, x);                                \
        if (l != RIX_NIL) {                                                   \
            x = l;                                                            \
            while (name##_right_idx(base, x) != RIX_NIL)                      \
                x = name##_right_idx(base, x);                                \
            return name##_ptr(base, x);                                       \
        } else {                                                              \
            unsigned p = name##_parent_idx(base, x);                          \
            while (p != RIX_NIL && x == name##_left_idx(base, p)) {           \
                x = p;                                                        \
                p = name##_parent_idx(base, x);                               \
            }                                                                 \
            return name##_ptr(base, p);                                       \
        }                                                                     \
    }                                                                         \
    attr type *name##_RIX_RB_FIND(struct name *head, type *base,              \
                                   const type *key) {                         \
        unsigned x = (head)->rrh_root;                                        \
        while (x != RIX_NIL) {                                                \
            int c = (cmp)(key, name##_ptr(base, x));                          \
            if (c < 0)                                                        \
                x = name##_left_idx(base, x);                                 \
            else if (c > 0)                                                   \
                x = name##_right_idx(base, x);                                \
            else                                                              \
                return name##_ptr(base, x);                                   \
        }                                                                     \
        return NULL;                                                          \
    }                                                                         \
    attr type *name##_RIX_RB_NFIND(struct name *head, type *base,             \
                                    const type *key) {                        \
        unsigned x = (head)->rrh_root, res = RIX_NIL;                         \
        while (x != RIX_NIL) {                                                \
            int c = (cmp)(key, name##_ptr(base, x));                          \
            if (c <= 0) {                                                     \
                res = x;                                                      \
                x = name##_left_idx(base, x);                                 \
            } else                                                            \
                x = name##_right_idx(base, x);                                \
        }                                                                     \
        return name##_ptr(base, res);                                         \
    }                                                                         \
    attr type *name##_RIX_RB_REMOVE(struct name *head, type *base, type *elm) { \
        unsigned z = RIX_IDX_FROM_PTR(base, elm);                             \
        unsigned y = z;                                                       \
        unsigned y_color = name##_color_idx(base, y);                         \
        unsigned x = RIX_NIL;                                                 \
        unsigned x_parent = RIX_NIL;                                          \
        if (name##_left_idx(base, z) == RIX_NIL) {                            \
            x = name##_right_idx(base, z);                                    \
            x_parent = name##_parent_idx(base, z);                            \
            name##_transplant(head, base, z, x);                              \
        } else if (name##_right_idx(base, z) == RIX_NIL) {                    \
            x = name##_left_idx(base, z);                                     \
            x_parent = name##_parent_idx(base, z);                            \
            name##_transplant(head, base, z, x);                              \
        } else {                                                              \
            /* successor */                                                   \
            y = name##_right_idx(base, z);                                    \
            while (name##_left_idx(base, y) != RIX_NIL)                       \
                y = name##_left_idx(base, y);                                 \
            y_color = name##_color_idx(base, y);                              \
            x = name##_right_idx(base, y);                                    \
            if (name##_parent_idx(base, y) == z) {                            \
                x_parent = y;                                                 \
                if (x != RIX_NIL)                                             \
                    name##_ptr(base, x)->field.rbe_parent = y;                \
            } else {                                                          \
                x_parent = name##_parent_idx(base, y);                        \
                name##_transplant(head, base, y, x);                          \
                name##_set_right(base, y, name##_right_idx(base, z));         \
            }                                                                 \
            name##_transplant(head, base, z, y);                              \
            name##_set_left(base, y, name##_left_idx(base, z));               \
            name##_set_color_idx(base, y, name##_color_idx(base, z));         \
        }                                                                     \
        /* fix-up */                                                          \
        if (y_color == RIX_RB_BLACK) {                                        \
            unsigned xi = x;                                                  \
            unsigned xpi = x_parent;                                          \
            while ((xi != (head)->rrh_root) &&                                \
                   (name##_color_idx(base, xi) == RIX_RB_BLACK)) {            \
                if (xi == name##_left_idx(base, xpi)) {                       \
                    unsigned w = name##_right_idx(base, xpi);                 \
                    if (name##_color_idx(base, w) == RIX_RB_RED) {            \
                        name##_set_color_idx(base, w, RIX_RB_BLACK);          \
                        name##_set_color_idx(base, xpi, RIX_RB_RED);          \
                        name##_rotate_left(head, base, xpi);                  \
                        w = name##_right_idx(base, xpi);                      \
                    }                                                         \
                    if (name##_color_idx(base, name##_left_idx(base, w)) ==   \
                            RIX_RB_BLACK &&                                   \
                        name##_color_idx(base, name##_right_idx(base, w)) ==  \
                            RIX_RB_BLACK) {                                   \
                        name##_set_color_idx(base, w, RIX_RB_RED);            \
                        xi = xpi;                                             \
                        xpi = name##_parent_idx(base, xi);                    \
                    } else {                                                  \
                        if (name##_color_idx(base, name##_right_idx(base, w)) == \
                            RIX_RB_BLACK) {                                   \
                            name##_set_color_idx(base, name##_left_idx(base, w), \
                                                 RIX_RB_BLACK);               \
                            name##_set_color_idx(base, w, RIX_RB_RED);        \
                            name##_rotate_right(head, base, w);               \
                            w = name##_right_idx(base, xpi);                  \
                        }                                                     \
                        name##_set_color_idx(base, w, name##_color_idx(base, xpi)); \
                        name##_set_color_idx(base, xpi, RIX_RB_BLACK);        \
                        name##_set_color_idx(base, name##_right_idx(base, w), \
                                             RIX_RB_BLACK);                   \
                        name##_rotate_left(head, base, xpi);                  \
                        xi = (head)->rrh_root;                                \
                        xpi = RIX_NIL;                                        \
                    }                                                         \
                } else { /* mirror */                                         \
                    unsigned w = name##_left_idx(base, xpi);                  \
                    if (name##_color_idx(base, w) == RIX_RB_RED) {            \
                        name##_set_color_idx(base, w, RIX_RB_BLACK);          \
                        name##_set_color_idx(base, xpi, RIX_RB_RED);          \
                        name##_rotate_right(head, base, xpi);                 \
                        w = name##_left_idx(base, xpi);                       \
                    }                                                         \
                    if (name##_color_idx(base, name##_right_idx(base, w)) ==  \
                            RIX_RB_BLACK &&                                   \
                        name##_color_idx(base, name##_left_idx(base, w)) ==   \
                            RIX_RB_BLACK) {                                   \
                        name##_set_color_idx(base, w, RIX_RB_RED);            \
                        xi = xpi;                                             \
                        xpi = name##_parent_idx(base, xi);                    \
                    } else {                                                  \
                        if (name##_color_idx(base, name##_left_idx(base, w)) == \
                            RIX_RB_BLACK) {                                   \
                            name##_set_color_idx(base, name##_right_idx(base, w), \
                                                 RIX_RB_BLACK);               \
                            name##_set_color_idx(base, w, RIX_RB_RED);        \
                            name##_rotate_left(head, base, w);                \
                            w = name##_left_idx(base, xpi);                   \
                        }                                                     \
                        name##_set_color_idx(base, w, name##_color_idx(base, xpi)); \
                        name##_set_color_idx(base, xpi, RIX_RB_BLACK);        \
                        name##_set_color_idx(base, name##_left_idx(base, w),  \
                                             RIX_RB_BLACK);                   \
                        name##_rotate_right(head, base, xpi);                 \
                        xi = (head)->rrh_root;                                \
                        xpi = RIX_NIL;                                        \
                    }                                                         \
                }                                                             \
            }                                                                 \
            name##_set_color_idx(base, xi, RIX_RB_BLACK);                     \
        }                                                                     \
        if ((head)->rrh_root != RIX_NIL)                                      \
            name##_ptr(base, (head)->rrh_root)->field.rbe_parent = RIX_NIL;   \
        elm->field.rbe_parent = elm->field.rbe_left = elm->field.rbe_right =  \
            RIX_NIL;                                                          \
        elm->field.rbe_color = RIX_RB_RED;                                    \
        return elm;                                                           \
    }

#endif /* _RIX_TREE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
