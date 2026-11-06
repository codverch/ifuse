// STD headers
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Custom headers
#include "ifuse_apt.h"
#include "ifuse_aci.h"
#include "ifuse_exec_pair.h"
#include "ifuse_plru.h"
#include "ifuse_rename.h"
#include "../general.param.h"
#include "../statistics.h"
#include "ifuse.param.h"

/**
 * Set-associative Active Pair Table (APT)
 * =======================================
 * LD1 inserts a live prediction keyed by the LD2 PC. LD2 probes the set
 * selected by the low LD2 PC bits and claims one unmatched entry with a
 * matching LD2 PC.
 *
 * At most one dynamic LD1 waits for each LD2 PC, with the invariant
 * that a younger LD1 replaces the older one's entry.
 *
 * On an insert into a full set, tree-PLRU selects the victim way.
 */

static APT_Entry* entries = NULL;
static uint8_t*   plru = NULL;
static unsigned int num_sets = 0;
static unsigned int num_ways = 0;
static bool         apt_initialized = false;
static uint64_t     apt_next_timestamp = 0;
static uint64_t     apt_live_ld2_prediction_count = 0;
static uint64_t     apt_live_ld2_prediction_peak = 0;

static bool apt_power_of_two(unsigned int value) {
    return value && !(value & (value - 1U));
}

static APT_Entry* apt_entry_at(unsigned int set, unsigned int way) {
    return &entries[set * num_ways + way];
}

/* Returns the set for an LD2 PC: its low bits, as in a cache. */
static unsigned int apt_set_index(Addr ld2_pc_addr) {
    return (unsigned int)(ld2_pc_addr & (num_sets - 1U));
}

static void apt_note_prediction_inserted(void) {
    apt_live_ld2_prediction_count++;
    if (apt_live_ld2_prediction_count > apt_live_ld2_prediction_peak) {
        INC_STAT_EVENT(0, APT_LIVE_LD2_PREDICTION_PEAK,
                       apt_live_ld2_prediction_count -
                       apt_live_ld2_prediction_peak);
        apt_live_ld2_prediction_peak = apt_live_ld2_prediction_count;
    }
}

static void apt_note_prediction_removed(void) {
    if (apt_live_ld2_prediction_count > 0) {
        apt_live_ld2_prediction_count--;
    }
}

static void apt_invalidate_pending_aci_prediction(const APT_Entry* entry) {
    if (!entry || !entry->valid || entry->matched) {
        return;
    }

    aci_invalidate_prediction(entry->predicted_ld2_effective_addr,
                              entry->ld1_micro_op_num);
}

static void apt_clear_entry(unsigned int set, unsigned int way,
                            bool preserve_exec_pair) {
    APT_Entry* entry = apt_entry_at(set, way);
    if (!entry->valid) {
        return;
    }

    apt_invalidate_pending_aci_prediction(entry);
    ifuse_free_ld2_physical_reg(entry->ld2_physical_reg_id);
    if (!preserve_exec_pair) {
        ifuse_exec_pair_forget_ld1_prediction(entry->ld1_micro_op_num);
    }

    memset(entry, 0, sizeof(*entry));
    apt_note_prediction_removed();
}

// Returns the live (unmatched) prediction for this LD2 PC. There is
// at most one, since a younger LD1 replaces it in apt_insert_entry().
static APT_Entry* apt_find_matching_entry(unsigned int set, Addr ld2_pc_addr) {
    for (unsigned int way = 0; way < num_ways; ++way) {
        APT_Entry* entry = apt_entry_at(set, way);
        if (entry->valid && !entry->matched && entry->ld2_pc_addr == ld2_pc_addr) {
            return entry;
        }
    }
    return NULL;
}

static APT_Entry* apt_find_entry_by_ld1(unsigned int set, Addr ld2_pc_addr,
                                      unsigned int ld1_micro_op_num) {
    for (unsigned int way = 0; way < num_ways; ++way) {
        APT_Entry* entry = apt_entry_at(set, way);
        if (entry->valid && entry->ld2_pc_addr == ld2_pc_addr &&
            entry->ld1_micro_op_num == ld1_micro_op_num) {
            return entry;
        }
    }
    return NULL;
}

static APT_Entry* apt_find_entry_by_ld1_any_pc(unsigned int ld1_micro_op_num) {
    for (unsigned int set = 0; set < num_sets; ++set) {
        for (unsigned int way = 0; way < num_ways; ++way) {
            APT_Entry* entry = apt_entry_at(set, way);
            if (entry->valid && entry->ld1_micro_op_num == ld1_micro_op_num) {
                return entry;
            }
        }
    }
    return NULL;
}

static APT_Entry* apt_find_matched_entry(unsigned int set, Addr ld2_pc_addr,
                                         unsigned int ld1_micro_op_num) {
    for (unsigned int way = 0; way < num_ways; ++way) {
        APT_Entry* entry = apt_entry_at(set, way);
        if (entry->valid && entry->matched &&
            entry->ld2_pc_addr == ld2_pc_addr &&
            entry->ld1_micro_op_num == ld1_micro_op_num) {
            return entry;
        }
    }
    return NULL;
}

static unsigned int apt_entry_set(const APT_Entry* entry) {
    return (unsigned int)((entry - entries) / num_ways);
}

static unsigned int apt_entry_way(const APT_Entry* entry) {
    return (unsigned int)((entry - entries) % num_ways);
}

static APT_Entry* apt_allocate_entry(unsigned int set) {
    unsigned int invalid_way = num_ways;

    for (unsigned int way = 0; way < num_ways; ++way) {
        if (!apt_entry_at(set, way)->valid) {
            invalid_way = way;
            break;
        }
    }

    unsigned int way = invalid_way;
    if (way == num_ways) {
        way = ifuse_plru_victim(plru, set, num_ways);
        apt_clear_entry(set, way, false);
        STAT_EVENT(0, APT_EVICTIONS);
    }
    // Count the new entry in both cases. An eviction already uncounts
    // the victim in apt_clear_entry(). Counting the insert only when a
    // way was free made the live count drift low after every eviction.
    apt_note_prediction_inserted();

    APT_Entry* entry = apt_entry_at(set, way);
    memset(entry, 0, sizeof(*entry));
    ifuse_plru_touch(plru, set, way, num_ways);
    return entry;
}

void apt_init(void) {
    if (apt_initialized) {
        return;
    }

    num_sets = IFUSE_APT_SETS;
    num_ways = IFUSE_APT_WAYS;
    if (!apt_power_of_two(num_sets) || (num_ways != 4U && num_ways != 8U)) {
        fprintf(stderr,
                "APT: requires power-of-two sets and 4 or 8 ways (got %u x %u)\n",
                num_sets, num_ways);
        exit(1);
    }

    entries = (APT_Entry*)calloc((size_t)num_sets * num_ways, sizeof(*entries));
    plru = (uint8_t*)calloc(num_sets, sizeof(*plru));
    if (!entries || !plru) {
        fprintf(stderr, "APT: allocation failed for %u sets x %u ways\n",
                num_sets, num_ways);
        exit(1);
    }

    apt_next_timestamp = 0;
    apt_live_ld2_prediction_count = 0;
    apt_live_ld2_prediction_peak = 0;
    apt_initialized = true;
    INC_STAT_EVENT(0, APT_CONFIGURED_CAPACITY, num_sets * num_ways);
}

void apt_observe_live_ld2_predictions(uns proc_id) {
    if (!apt_initialized) {
        return;
    }

    STAT_EVENT(proc_id, APT_LIVE_LD2_PREDICTION_OBSERVATIONS);
    INC_STAT_EVENT(proc_id, APT_LIVE_LD2_PREDICTION_TOTAL,
                   apt_live_ld2_prediction_count);
    INC_STAT_EVENT(proc_id, APT_LIVE_LD2_PREDICTION_AVG,
                   apt_live_ld2_prediction_count);
}

/**
 * An in-flight fusion candidate is a valid APT entry: LD1 has been
 * predicted to fuse, and its LD2 has not yet reached rename. The
 * function apt_observe_live_ld2_predictions() samples the same count
 * once per predicted LD1, so busy stretches of code weigh more there.
 * This function samples it once per cycle, which gives the average
 * that a table-sizing argument needs.
 */
void apt_observe_cycle(uns proc_id) {
    if (!apt_initialized) {
        return;
    }

    STAT_EVENT(proc_id, IFUSE_INFLIGHT_CANDIDATES_OBSERVED_CYCLES);
    INC_STAT_EVENT(proc_id, IFUSE_INFLIGHT_CANDIDATES_TOTAL,
                   apt_live_ld2_prediction_count);
    INC_STAT_EVENT(proc_id, IFUSE_INFLIGHT_CANDIDATES_PER_CYCLE,
                   apt_live_ld2_prediction_count);
}

APT_Entry* apt_lookup(Addr ld2_pc_addr) {
    if (!apt_initialized) {
        apt_init();
    }
    if (ld2_pc_addr == 0) {
        return NULL;
    }

    unsigned int set = apt_set_index(ld2_pc_addr);
    APT_Entry* entry = apt_find_matching_entry(set, ld2_pc_addr);
    if (!entry) {
        return NULL;
    }

    entry->matched = true;
    ifuse_plru_touch(plru, set, apt_entry_way(entry), num_ways);
    return entry;
}

APT_Entry* apt_insert_entry(Addr ld1_pc_addr,
                            Addr ld1_effective_addr,
                            unsigned int ld1_micro_op_num,
                            unsigned int predicted_ld2_memory_access_size,
                            Addr ld2_pc_addr,
                            Addr predicted_ld2_effective_addr) {
    if (!apt_initialized) {
        apt_init();
    }
    if (ld2_pc_addr == 0) {
        return NULL;
    }

    unsigned int set = apt_set_index(ld2_pc_addr);

    // When two instances of a pair are in flight, the younger LD1
    // replaces the older one's entry and returns its register to the
    // free list. LD2 thus always maps to the youngest instance (paper
    // Section 4.1).
    APT_Entry* older = apt_find_matching_entry(set, ld2_pc_addr);
    if (older) {
        apt_clear_entry(set, apt_entry_way(older), false);
        STAT_EVENT(0, APT_REPLACED_OLDER_INSTANCE);
    }
    APT_Entry* entry = apt_allocate_entry(set);

    entry->ld2_pc_addr                      = ld2_pc_addr;
    entry->ld1_pc_addr                      = ld1_pc_addr;
    entry->ld1_effective_addr               = ld1_effective_addr;
    entry->ld1_micro_op_num                 = ld1_micro_op_num;
    entry->ld2_physical_reg_id              = 0xFFFF;
    entry->predicted_ld2_effective_addr     = predicted_ld2_effective_addr;
    entry->predicted_ld2_memory_access_size = predicted_ld2_memory_access_size;
    entry->valid                            = true;
    entry->matched                          = false;
    entry->timestamp                        = ++apt_next_timestamp;

    STAT_EVENT(0, APT_INSERTS);
    return entry;
}

void apt_remove_entry_by_ld1_micro_op(Addr ld2_pc_addr,
                                      unsigned int ld1_micro_op_num) {
    if (!apt_initialized || ld2_pc_addr == 0) {
        return;
    }

    unsigned int set = apt_set_index(ld2_pc_addr);
    APT_Entry* entry =
        apt_find_entry_by_ld1(set, ld2_pc_addr, ld1_micro_op_num);
    if (entry) {
        apt_clear_entry(set, apt_entry_way(entry), false);
    }
}

void apt_remove_entry_by_ld1_micro_op_any_pc(unsigned int ld1_micro_op_num) {
    if (!apt_initialized) {
        return;
    }

    APT_Entry* entry = apt_find_entry_by_ld1_any_pc(ld1_micro_op_num);
    if (entry) {
        apt_clear_entry(apt_entry_set(entry), apt_entry_way(entry), false);
    }
}

bool apt_reopen_matched_entry(Addr ld2_pc_addr,
                              unsigned int ld1_micro_op_num) {
    if (!apt_initialized || ld2_pc_addr == 0) {
        return false;
    }

    unsigned int set = apt_set_index(ld2_pc_addr);
    APT_Entry* entry =
        apt_find_matched_entry(set, ld2_pc_addr, ld1_micro_op_num);
    if (!entry) {
        return false;
    }
    // A younger LD1 already waits for this LD2 PC. Keep it, not the
    // reopened older claim, so each LD2 PC has one live prediction.
    if (apt_find_matching_entry(set, ld2_pc_addr)) {
        apt_clear_entry(set, apt_entry_way(entry), false);
        STAT_EVENT(0, APT_REPLACED_OLDER_INSTANCE);
        return false;
    }

    entry->matched = false;
    ifuse_plru_touch(plru, set, apt_entry_way(entry), num_ways);
    aci_insert_prediction(entry->predicted_ld2_effective_addr,
                          entry->ld1_micro_op_num);
    return true;
}

bool apt_set_ld2_physical_reg_id(unsigned int ld1_micro_op_num,
                                 unsigned int ld2_physical_reg_id) {
    if (!apt_initialized || ld2_physical_reg_id == 0xFFFF) {
        return false;
    }

    APT_Entry* entry = apt_find_entry_by_ld1_any_pc(ld1_micro_op_num);
    if (!entry) {
        return false;
    }

    entry->ld2_physical_reg_id = ld2_physical_reg_id;
    return true;
}

bool apt_take_ld2_physical_reg_id(Addr ld2_pc_addr,
                                  unsigned int ld1_micro_op_num,
                                  unsigned int* ld2_physical_reg_id) {
    if (!apt_initialized || ld2_pc_addr == 0 || !ld2_physical_reg_id) {
        return false;
    }

    unsigned int set = apt_set_index(ld2_pc_addr);
    APT_Entry* entry =
        apt_find_entry_by_ld1(set, ld2_pc_addr, ld1_micro_op_num);
    if (!entry || entry->ld2_physical_reg_id == 0xFFFF) {
        return false;
    }

    *ld2_physical_reg_id = entry->ld2_physical_reg_id;
    entry->ld2_physical_reg_id = 0xFFFF;
    apt_clear_entry(set, apt_entry_way(entry), true);
    return true;
}

void apt_cleanup_stale(uint64_t current_op_num) {
    if (!apt_initialized) {
        return;
    }

    for (unsigned int set = 0; set < num_sets; ++set) {
        for (unsigned int way = 0; way < num_ways; ++way) {
            APT_Entry* entry = apt_entry_at(set, way);
            if (entry->valid && !entry->matched &&
                current_op_num > entry->ld1_micro_op_num &&
                current_op_num - entry->ld1_micro_op_num > IFUSE_FUSION_DISTANCE) {
                apt_clear_entry(set, way, false);
            }
        }
    }
}
