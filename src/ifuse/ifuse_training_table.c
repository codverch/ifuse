#include "ifuse_training_table.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../statistics.h"
#include "ifuse_fct.h"
#include "ifuse.param.h"

/**
 * Hardware storage model. The C fields below intentionally use normal
 * host types. Each entry packs the following fields:
 *
 *   - A 32-bit LD1 PC tag (IFUSE_TRAINING_TABLE_PC_TAG_BITS). With at
 *     most a few hundred live entries, the aliasing probability is about
 *     3e-5. A collision only merges the observation counts of two
 *     candidates. It never corrupts a promoted PC. The note on
 *     find_or_allocate() below explains why.
 *   - A full 48-bit LD2 PC, kept untruncated. It doubles as the value
 *     forwarded to FCT_Row.ld2_pc_addr, which must be full precision
 *     (see the comment on pc_tag() below).
 *   - An 11-bit observation counter. It supports insert thresholds up
 *     to 2047 (i.e., 2x headroom over 1000, the highest threshold
 *     actually swept).
 *   - A 6-bit cache-line offset delta.
 *   - A 3-bit log2(LD2 access size).
 *   - A direction bit, a valid bit, and a 2-bit RRPV
 *     replacement-priority field.
 *
 * Each entry totals 104 bits. The default table has 32 sets * 4 ways =
 * 128 entries, so the packed training table costs 128 * 104 = 13,312
 * bits (1.625 KiB).
 *
 * Replacement is RRIP-style (SRRIP). A newly inserted candidate starts
 * at RRPV = IFUSE_TT_RRPV_INSERT (one below the maximum), not at the
 * maximum and not at zero. It must therefore be re-observed to earn
 * protection, instead of being either evicted immediately or fully
 * protected on arrival. Each repeat observation decrements RRPV toward
 * 0 (protected). Eviction always takes the way with the highest RRPV
 * in the set, and ties break to the lowest way index.
 */
#define IFUSE_TRAINING_OBS_COUNTER_MAX 2047U
#define IFUSE_TT_RRPV_BITS 2U
#define IFUSE_TT_RRPV_MAX ((1U << IFUSE_TT_RRPV_BITS) - 1U)
#define IFUSE_TT_RRPV_INSERT (IFUSE_TT_RRPV_MAX - 1U)
typedef struct TrainingEntry {
    uint64_t ld1_tag;
    uint64_t ld2_tag;
    uint16_t observations;
    uint8_t offset_delta;
    uint8_t ld2_mem_size;
    uint8_t rrpv;
    bool direction;
    bool valid;
} TrainingEntry;

static TrainingEntry* entries;
static unsigned int num_sets;
static unsigned int num_ways;
static uint64_t live_entries;
static uint64_t peak_live_entries;
static bool initialized;

static bool power_of_two(unsigned int value) {
    return value && !(value & (value - 1U));
}

/**
 * Truncates ld1_pc to IFUSE_TRAINING_TABLE_PC_TAG_BITS. Truncation
 * applies only to the LD1 tag. In the training table, ld1_pc serves
 * only for lookup and disambiguation and is never read back as a value.
 * In contrast, ld2_pc doubles as the value forwarded through
 * fct_install_runtime_candidate() to FCT_Row.ld2_pc_addr, which must be
 * a full-precision PC (for the same reason given for
 * FCT_Row.ld2_pc_addr in ifuse_fct.h). The ld2_tag field is therefore
 * stored and compared at full width, and this function never touches
 * it.
 */
static uint64_t pc_tag(Addr pc) {
    unsigned int bits = IFUSE_TRAINING_TABLE_PC_TAG_BITS;
    if (bits >= 64U)
        return (uint64_t)pc;
    if (bits == 0U)
        return 0U;
    return (uint64_t)pc & ((1ULL << bits) - 1ULL);
}

/* Returns the set for an LD1 PC: its low bits, as in a cache. The TT is
 * indexed by LD1's PC, as in the paper. */
static unsigned int set_for_ld1_pc(Addr ld1_pc) {
    return (unsigned int)(ld1_pc & (num_sets - 1U));
}

static TrainingEntry* entry_at(unsigned int set, unsigned int way) {
    return &entries[set * num_ways + way];
}

void training_table_init(void) {
    if (initialized)
        return;

    num_sets = IFUSE_TRAINING_TABLE_SETS;
    num_ways = IFUSE_TRAINING_TABLE_WAYS;
    if (!power_of_two(num_sets) || (num_ways != 4U && num_ways != 8U)) {
        fprintf(stderr, "I-Fuse training table requires power-of-two sets "
                        "and 4 or 8 ways (got %u x %u)\n",
                num_sets, num_ways);
        exit(1);
    }
    if (IFUSE_TRAINING_TABLE_PC_TAG_BITS == 0U ||
        IFUSE_TRAINING_TABLE_PC_TAG_BITS > 64U) {
        fprintf(stderr, "I-Fuse training table PC tag bits must be 1-64\n");
        exit(1);
    }
    if (IFUSE_TRAINING_INSERT_THRESHOLD == 0U ||
        IFUSE_TRAINING_INSERT_THRESHOLD > IFUSE_TRAINING_OBS_COUNTER_MAX) {
        fprintf(stderr,
                "I-Fuse training threshold must be 1-%u for the modeled "
                "11-bit counter\n",
                IFUSE_TRAINING_OBS_COUNTER_MAX);
        exit(1);
    }

    entries = (TrainingEntry*)calloc((size_t)num_sets * num_ways,
                                     sizeof(*entries));
    if (!entries) {
        fprintf(stderr, "Could not allocate I-Fuse runtime training table\n");
        exit(1);
    }
    initialized = true;
}

/**
 * Returns the RRIP victim: the way with the highest RRPV (least
 * protected) in the set. Ties break to the lowest way index.
 */
static unsigned int rrip_victim(unsigned int set) {
    unsigned int victim = 0;
    uint8_t victim_rrpv = entry_at(set, 0)->rrpv;
    for (unsigned int way = 1; way < num_ways; ++way) {
        uint8_t rrpv = entry_at(set, way)->rrpv;
        if (rrpv > victim_rrpv) {
            victim = way;
            victim_rrpv = rrpv;
        }
    }
    return victim;
}

static bool keys_match(const TrainingEntry* entry, uint64_t ld1_tag,
                       uint64_t ld2_tag, unsigned int delta, bool direction,
                       unsigned int mem_size) {
    return entry->valid && entry->ld1_tag == ld1_tag &&
           entry->ld2_tag == ld2_tag && entry->offset_delta == delta &&
           entry->direction == direction && entry->ld2_mem_size == mem_size;
}

/**
 * The ld1_tag field (truncated by pc_tag()) exists only to answer "is
 * this the same candidate as an existing entry?" for the hit, miss, and
 * observation-count decisions below. It is never read back to
 * reconstruct a PC. The function training_table_observe() always
 * forwards its own full-precision ld1_pc argument (the caller's real
 * dynamic PC) to fct_install_runtime_candidate(), not the stored
 * ld1_tag. The ld2_tag field stays at full width (see the comment on
 * pc_tag() above), so that the stored and compared value, not only the
 * call-site parameter, carries the exact LD2 PC forward. This is a
 * redundant safeguard alongside the call-site guarantee. It adds
 * essentially no risk, since the 32 bits of ld1_tag alone already make
 * an accidental match astronomically unlikely.
 */
static TrainingEntry* find_or_allocate(Addr ld1_pc, Addr ld2_pc,
                                       unsigned int delta, bool direction,
                                       unsigned int mem_size, uns proc_id) {
    unsigned int set = set_for_ld1_pc(ld1_pc);
    uint64_t ld1_tag = pc_tag(ld1_pc);
    uint64_t ld2_tag = (uint64_t)ld2_pc;
    unsigned int invalid_way = num_ways;

    STAT_EVENT(proc_id, TRAINING_TABLE_LOOKUPS);
    for (unsigned int way = 0; way < num_ways; ++way) {
        TrainingEntry* entry = entry_at(set, way);
        if (keys_match(entry, ld1_tag, ld2_tag, delta, direction, mem_size)) {
            STAT_EVENT(proc_id, TRAINING_TABLE_HITS);
            if (entry->rrpv > 0)
                --entry->rrpv;
            return entry;
        }
        if (!entry->valid && invalid_way == num_ways)
            invalid_way = way;
    }

    STAT_EVENT(proc_id, TRAINING_TABLE_MISSES);
    unsigned int way = invalid_way;
    if (way == num_ways) {
        way = rrip_victim(set);
        STAT_EVENT(proc_id, TRAINING_TABLE_EVICTIONS);
    } else {
        ++live_entries;
        if (live_entries > peak_live_entries) {
            INC_STAT_EVENT(proc_id, TRAINING_TABLE_PEAK_LIVE,
                           live_entries - peak_live_entries);
            peak_live_entries = live_entries;
        }
    }

    TrainingEntry* entry = entry_at(set, way);
    entry->ld1_tag = ld1_tag;
    entry->ld2_tag = ld2_tag;
    entry->offset_delta = (uint8_t)delta;
    entry->ld2_mem_size = (uint8_t)mem_size;
    entry->direction = direction;
    entry->observations = 0;
    entry->rrpv = (uint8_t)IFUSE_TT_RRPV_INSERT;
    entry->valid = true;
    STAT_EVENT(proc_id, TRAINING_TABLE_INSERTS);
    return entry;
}

void training_table_observe(Addr ld1_pc, Addr ld2_pc,
                            Addr ld1_effective_addr,
                            Addr ld2_effective_addr, uns ld2_mem_size,
                            Counter ld1_micro_op_num,
                            Counter ld2_micro_op_num, uns proc_id) {
    if (!IFUSE_RUNTIME_TRAINING_ENABLED)
        return;
    if (!initialized)
        training_table_init();

    unsigned int ld1_offset = (unsigned int)(ld1_effective_addr & 63U);
    unsigned int ld2_offset = (unsigned int)(ld2_effective_addr & 63U);
    bool direction = ld2_offset >= ld1_offset;
    unsigned int delta = direction ? ld2_offset - ld1_offset :
                                     ld1_offset - ld2_offset;

    TrainingEntry* entry = find_or_allocate(ld1_pc, ld2_pc, delta, direction,
                                             ld2_mem_size, proc_id);
    if (entry->observations < IFUSE_TRAINING_INSERT_THRESHOLD)
        ++entry->observations;
    STAT_EVENT(proc_id, TRAINING_TABLE_OBSERVATIONS);

    if (entry->observations < IFUSE_TRAINING_INSERT_THRESHOLD)
        return;

    if (fct_install_runtime_candidate(ld1_pc, ld2_pc, ld1_effective_addr,
                                      ld2_effective_addr, delta, direction,
                                      ld2_mem_size, ld1_micro_op_num,
                                      ld2_micro_op_num, proc_id)) {
        STAT_EVENT(proc_id, TRAINING_TABLE_PROMOTIONS);
        entry->valid = false;
        if (live_entries)
            --live_entries;
    } else {
        STAT_EVENT(proc_id, TRAINING_TABLE_PROMOTION_REJECTS);
    }
}
