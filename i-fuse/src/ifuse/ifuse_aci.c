// STD headers
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Custom headers
#include "ifuse_aci.h"
#include "ifuse_plru.h"
#include "../general.param.h"
#include "../statistics.h"
#include "ifuse.param.h"

/**
 * Set-associative Access Check Index (ACI)
 * ========================================
 * LD1 inserts the predicted LD2 cache block. LD2 validates against the
 * set selected by the low block-number bits and consumes the matching entry
 * that LD1 owns.
 *
 * On an insert into a full set, tree-PLRU selects the victim way.
 */

typedef struct ACI_Entry {
    uint64_t predicted_ld2_effective_addr;
    uint64_t timestamp;
    uint64_t ld1_micro_op_num;
    bool     valid;
} ACI_Entry;

static ACI_Entry* entries = NULL;
static uint8_t*   plru = NULL;
static unsigned int num_sets = 0;
static unsigned int num_ways = 0;
static bool         aci_initialized = false;
static uint64_t     aci_next_timestamp = 0;

static bool aci_power_of_two(unsigned int value) {
    return value && !(value & (value - 1U));
}

static ACI_Entry* aci_entry_at(unsigned int set, unsigned int way) {
    return &entries[set * num_ways + way];
}

static uint64_t aci_get_cacheblock_addr(uint64_t effective_addr) {
    return effective_addr >> ACI_CACHE_LINE_BITS;
}

/* Returns the set for a block number: its low bits, as in a cache. */
static unsigned int aci_set_index(uint64_t cacheblock_addr) {
    return (unsigned int)(cacheblock_addr & (num_sets - 1U));
}

static ACI_Entry* aci_find_prediction(unsigned int set,
                                      uint64_t cacheblock_addr,
                                      uint64_t ld1_micro_op_num) {
    for (unsigned int way = 0; way < num_ways; ++way) {
        ACI_Entry* entry = aci_entry_at(set, way);
        if (entry->valid &&
            aci_get_cacheblock_addr(entry->predicted_ld2_effective_addr) ==
                cacheblock_addr &&
            entry->ld1_micro_op_num == ld1_micro_op_num) {
            return entry;
        }
    }
    return NULL;
}

static ACI_Entry* aci_find_oldest_same_block(unsigned int set,
                                             uint64_t cacheblock_addr) {
    ACI_Entry* oldest = NULL;

    for (unsigned int way = 0; way < num_ways; ++way) {
        ACI_Entry* entry = aci_entry_at(set, way);
        if (!entry->valid ||
            aci_get_cacheblock_addr(entry->predicted_ld2_effective_addr) !=
                cacheblock_addr) {
            continue;
        }
        if (!oldest || entry->timestamp < oldest->timestamp) {
            oldest = entry;
        }
    }
    return oldest;
}

static void aci_clear_entry(unsigned int set, unsigned int way) {
    ACI_Entry* entry = aci_entry_at(set, way);
    if (!entry->valid) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
}

static ACI_Entry* aci_allocate_entry(unsigned int set) {
    unsigned int invalid_way = num_ways;

    for (unsigned int way = 0; way < num_ways; ++way) {
        if (!aci_entry_at(set, way)->valid) {
            invalid_way = way;
            break;
        }
    }

    unsigned int way = invalid_way;
    if (way == num_ways) {
        way = ifuse_plru_victim(plru, set, num_ways);
        aci_clear_entry(set, way);
        STAT_EVENT(0, ACI_EVICTIONS);
    }

    ACI_Entry* entry = aci_entry_at(set, way);
    memset(entry, 0, sizeof(*entry));
    entry->valid = true;
    ifuse_plru_touch(plru, set, way, num_ways);
    return entry;
}

void aci_init(void) {
    if (aci_initialized) {
        return;
    }

    num_sets = IFUSE_ACI_SETS;
    num_ways = IFUSE_ACI_WAYS;
    if (!aci_power_of_two(num_sets) || (num_ways != 4U && num_ways != 8U)) {
        fprintf(stderr,
                "ACI: requires power-of-two sets and 4 or 8 ways (got %u x %u)\n",
                num_sets, num_ways);
        exit(1);
    }

    entries = (ACI_Entry*)calloc((size_t)num_sets * num_ways, sizeof(*entries));
    plru = (uint8_t*)calloc(num_sets, sizeof(*plru));
    if (!entries || !plru) {
        fprintf(stderr, "ACI: allocation failed for %u sets x %u ways\n",
                num_sets, num_ways);
        exit(1);
    }

    aci_next_timestamp = 0;
    aci_initialized = true;
}

void aci_insert_prediction(uint64_t predicted_ld2_effective_addr,
                           uint64_t ld1_micro_op_num) {
    if (!aci_initialized) {
        aci_init();
    }

    uint64_t predicted_ld2_cacheblock_addr =
        aci_get_cacheblock_addr(predicted_ld2_effective_addr);
    unsigned int set = aci_set_index(predicted_ld2_cacheblock_addr);
    ACI_Entry* replayed =
        aci_find_prediction(set, predicted_ld2_cacheblock_addr,
                            ld1_micro_op_num);
    ACI_Entry* oldest_same_block =
        aci_find_oldest_same_block(set, predicted_ld2_cacheblock_addr);

    if (replayed) {
        replayed->timestamp = ++aci_next_timestamp;
        ifuse_plru_touch(plru, set,
                         (unsigned int)(replayed - entries) % num_ways,
                         num_ways);
        STAT_EVENT(0, ACI_REPLAYED_INSERTS);
        return;
    }

    ACI_Entry* entry = aci_allocate_entry(set);
    entry->predicted_ld2_effective_addr = predicted_ld2_effective_addr;
    entry->ld1_micro_op_num             = ld1_micro_op_num;
    entry->timestamp                    = ++aci_next_timestamp;

    STAT_EVENT(0, ACI_PREDICTION_INSERTS);
    if (oldest_same_block) {
        STAT_EVENT(0, ACI_SAME_CACHEBLOCK_INSERTS);
    }
}

ACI_Result aci_check_and_consume_prediction(
    uint64_t actual_ld2_effective_addr,
    uint64_t predicted_ld2_effective_addr,
    uint64_t ld1_micro_op_num) {
    if (!aci_initialized) {
        aci_init();
    }

    uint64_t actual_ld2_cacheblock_addr =
        aci_get_cacheblock_addr(actual_ld2_effective_addr);
    unsigned int set = aci_set_index(actual_ld2_cacheblock_addr);
    ACI_Entry* entry =
        aci_find_prediction(set, actual_ld2_cacheblock_addr, ld1_micro_op_num);
    if (!entry) {
        STAT_EVENT(0, ACI_LOOKUP_MISSES);
        if (predicted_ld2_effective_addr != 0 &&
            actual_ld2_cacheblock_addr !=
            aci_get_cacheblock_addr(predicted_ld2_effective_addr)) {
            return ACI_LOOKUP_WRONG_PREDICTED_BLOCK;
        }
        return ACI_LOOKUP_NO_MATCHING_ENTRY;
    }

    unsigned int way = (unsigned int)(entry - entries) % num_ways;
    aci_clear_entry(set, way);
    STAT_EVENT(0, ACI_LOOKUP_HITS);
    return ACI_LOOKUP_MATCH;
}

static void aci_invalidate_cacheblock_prediction(uint64_t cacheblock_addr,
                                                 uint64_t ld1_micro_op_num) {
    if (!aci_initialized) {
        return;
    }

    unsigned int set = aci_set_index(cacheblock_addr);
    for (unsigned int way = 0; way < num_ways; ++way) {
        ACI_Entry* entry = aci_entry_at(set, way);
        if (!entry->valid) {
            continue;
        }

        bool same_cacheblock =
            aci_get_cacheblock_addr(entry->predicted_ld2_effective_addr) ==
            cacheblock_addr;
        bool same_load1 =
            ld1_micro_op_num == 0 ||
            entry->ld1_micro_op_num == ld1_micro_op_num;

        if (same_cacheblock && same_load1) {
            aci_clear_entry(set, way);
            STAT_EVENT(0, ACI_PREDICTION_INVALIDATIONS);
            if (ld1_micro_op_num != 0) {
                return;
            }
        }
    }
}

void aci_invalidate_prediction(uint64_t predicted_ld2_effective_addr,
                               uint64_t ld1_micro_op_num) {
    aci_invalidate_cacheblock_prediction(
        aci_get_cacheblock_addr(predicted_ld2_effective_addr),
        ld1_micro_op_num);
}

void aci_cleanup_stale(uint64_t current_op_num) {
    if (!aci_initialized) {
        return;
    }

    for (unsigned int set = 0; set < num_sets; ++set) {
        for (unsigned int way = 0; way < num_ways; ++way) {
            ACI_Entry* entry = aci_entry_at(set, way);
            if (entry->valid && current_op_num > entry->ld1_micro_op_num &&
                current_op_num - entry->ld1_micro_op_num > IFUSE_FUSION_DISTANCE) {
                aci_clear_entry(set, way);
                STAT_EVENT(0, ACI_STALE_PREDICTION_REMOVALS);
            }
        }
    }
}
