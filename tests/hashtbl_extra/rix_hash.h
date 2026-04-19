/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * Compatibility shim for tests/hashtbl_extra. Pulls in the slot_extra
 * variant in addition to the umbrella. The umbrella does NOT include
 * rix_hash_slot_extra.h, so tests must include it explicitly.
 */
#include "../../include/rix/rix_hash.h"
#include "../../include/rix/rix_hash_slot_extra.h"
