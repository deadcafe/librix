/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _SAMPLES_FLOW_KEY_H_
#define _SAMPLES_FLOW_KEY_H_

#include <stdint.h>

struct flow_hashtbl_elm {
    uint32_t cur_hash;
    uint16_t slot;
    uint16_t reserved0;
};

struct flow4_key {
    uint8_t  family;
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;
    uint32_t vrfid;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t zero;
};
struct flow4_entry_hdr {
    struct flow4_key key;
    struct flow_hashtbl_elm htbl_elm;
};

struct flow6_key {
    uint8_t  family;
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;
    uint32_t vrfid;
    uint8_t src_ip[16];
    uint8_t dst_ip[16];
} __attribute__((packed));
struct flow6_entry_hdr {
    struct flow6_key key;
    struct flow_hashtbl_elm htbl_elm;
};

struct flowu_key {
    uint8_t  family;
    uint8_t  proto;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t pad;
    uint32_t vrfid;
    union {
        struct {
            uint32_t src;
            uint32_t dst;
            uint8_t  _pad[24];
        } v4;
        struct {
            uint8_t src[16];
            uint8_t dst[16];
        } v6;
    } addr;
} __attribute__((packed));
struct flowu_entry_hdr {
    struct flowu_key key;
    struct flow_hashtbl_elm htbl_elm;
};

#endif
