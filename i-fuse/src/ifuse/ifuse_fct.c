// STD headers
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Custom headers
#include "ifuse_fct.h"
#include "../general.param.h"
#include "../statistics.h"
#include "ifuse_limits.h"
#include "ifuse.param.h"
#include "ifuse_plru.h"

/**
 * Fusion Candidate Table (FCT) runtime policy.
 *
 * The FCT models a single LD2 candidate for each LD1 PC. It receives
 * candidates that the retire-stage training table promotes. Correct
 * predictions reinforce a row, and mispredictions penalize it.
 *
 * The table is organized like a cache, not a CAM. It has
 * 2^IFUSE_FCT_HASH_BITS rows split into IFUSE_FCT_WAYS ways, so 512
 * rows at the default 4 ways form 128 sets of 4. The low LD1 PC bits
 * pick one set, and only that set's tags are compared. When a new LD1
 * needs a row and its set is full, tree-PLRU picks the way to evict.
 * The APT and ACI use the same replacement policy.
 *
 * This design replaced a fully associative table that exited the
 * simulator once all rows were taken. The corebench suite trains more
 * than 512 distinct LD1 PCs, so it could not finish. A real table
 * evicts instead.
 */

static FCT_Row*     fct_rows = NULL;  // The fct_num_sets * fct_num_ways rows, set-major
static uint8_t*     fct_plru = NULL;  // One tree-PLRU state byte per set
static unsigned int fct_num_sets = 0;
static unsigned int fct_num_ways = 0;
static bool         fct_is_initialized = false;

/*
 * Modeled hardware width of confidence_score (see the FCT_Row
 * storage-model comment in ifuse_fct.h). A 10-bit field gives 2x
 * headroom over the current default confidence ceiling of 500
 * (ifuse_fct_runtime_insert_conf).
 */
#define IFUSE_FCT_CONFIDENCE_MAX 1023U

/**
 * Truncates ld1_pc_addr to IFUSE_FCT_PC_TAG_BITS for storage and
 * comparison, the same way pc_tag() in ifuse_training_table.c truncates
 * training-table tags. Only the stored and compared LD1 tag shrinks.
 * The function fct_set_index() below picks the set from the low PC
 * bits, and ld2_pc_addr is never truncated. The FCT_Row
 * storage-model comment in ifuse_fct.h explains why.
 */
static uint64_t fct_pc_tag(Addr pc) {
    unsigned int bits = IFUSE_FCT_PC_TAG_BITS;
    if (bits >= 64U)
        return (uint64_t)pc;
    if (bits == 0U)
        return 0U;
    return (uint64_t)pc & ((1ULL << bits) - 1ULL);
}

void fct_init(void) {
    if (fct_is_initialized) {
        return;
    }

    if (IFUSE_FCT_HASH_BITS == 0U ||
        IFUSE_FCT_HASH_BITS > IFUSE_FCT_MAX_HASH_BITS) {
        fprintf(stderr,
                "FCT: ifuse_fct_hash_bits must be in [1, %u]\n",
                IFUSE_FCT_MAX_HASH_BITS);
        exit(1);
    }
    if (IFUSE_FCT_PC_TAG_BITS == 0U || IFUSE_FCT_PC_TAG_BITS > 64U) {
        fprintf(stderr, "FCT: ifuse_fct_pc_tag_bits must be 1-64\n");
        exit(1);
    }
    if (IFUSE_FCT_RUNTIME_INSERT_CONF > IFUSE_FCT_CONFIDENCE_MAX) {
        fprintf(stderr,
                "FCT: ifuse_fct_runtime_insert_conf "
                "must be <= %u for the modeled 10-bit confidence field\n",
                IFUSE_FCT_CONFIDENCE_MAX);
        exit(1);
    }

    const unsigned int num_rows = 1U << IFUSE_FCT_HASH_BITS;
    if ((IFUSE_FCT_WAYS != 4U && IFUSE_FCT_WAYS != 8U) ||
        num_rows < IFUSE_FCT_WAYS) {
        fprintf(stderr,
                "FCT: ifuse_fct_ways must be 4 or 8 (tree-PLRU) and no larger "
                "than 2^ifuse_fct_hash_bits\n");
        exit(1);
    }
    fct_num_ways = IFUSE_FCT_WAYS;
    fct_num_sets = num_rows / fct_num_ways;
    fct_rows = (FCT_Row*)calloc(num_rows, sizeof(FCT_Row));
    fct_plru = (uint8_t*)calloc(fct_num_sets, sizeof(*fct_plru));
    if (!fct_rows || !fct_plru) {
        fprintf(stderr, "FCT: calloc failed for %u rows\n", num_rows);
        exit(1);
    }
    fct_is_initialized = true;
}

/* Returns the set for an LD1 PC: its low bits, as in a cache. */
static unsigned int fct_set_index(Addr ld1_pc_addr) {
    return (unsigned int)(ld1_pc_addr & (fct_num_sets - 1U));
}

static FCT_Row* fct_row_at(unsigned int set, unsigned int way) {
    return &fct_rows[set * fct_num_ways + way];
}

static unsigned int fct_row_way(const FCT_Row* row) {
    return (unsigned int)(row - fct_rows) % fct_num_ways;
}

/**
 * Returns the configured FCT confidence cap.
 *
 * The runtime policy treats the prediction threshold as the maximum
 * useful confidence. A row inserted at confidence 100 should not grow
 * to 500 and keep predicting through many penalties. Once it reaches
 * the prediction threshold, it is fully trusted until a misprediction
 * lowers it.
 */
static unsigned int fct_max_confidence_score(void) {
    return IFUSE_FCT_RUNTIME_INSERT_CONF;
}

/**
 * Returns the confidence score after applying the configured confidence
 * cap.
 */
static unsigned int fct_saturating_confidence_score(
    unsigned int confidence_score) {
    const unsigned int max_confidence_score = fct_max_confidence_score();
    return (confidence_score > max_confidence_score) ?
        max_confidence_score : confidence_score;
}

/**
 * Reinforces the row without exceeding the configured prediction
 * threshold.
 */
static void fct_increment_confidence_score(FCT_Row* row) {
    if (!row) {
        return;
    }

    row->confidence_score =
        fct_saturating_confidence_score(row->confidence_score + 1U);
}

static void fct_write_row(FCT_Row* row, Addr ld1_pc_addr, Addr ld2_pc_addr,
                          Addr ld1_effective_addr, Addr ld2_effective_addr,
                          unsigned int offset_delta,
                          bool direction, unsigned int ld2_mem_size,
                          Counter ld1_micro_op_num,
                          Counter ld2_micro_op_num,
                          unsigned int confidence_score) {
    row->ld1_pc_addr        = fct_pc_tag(ld1_pc_addr);
    row->ld2_pc_addr        = ld2_pc_addr;
    row->ld1_effective_addr = ld1_effective_addr;
    row->ld2_effective_addr = ld2_effective_addr;
    row->offset_delta       = offset_delta;
    row->direction          = direction;
    row->ld2_mem_size       = ld2_mem_size;
    row->ld1_micro_op_num   = ld1_micro_op_num;
    row->ld2_micro_op_num   = ld2_micro_op_num;
    row->valid              = true;
    row->confidence_score   = fct_saturating_confidence_score(confidence_score);
}

/**
 * Returns the row for ld1_pc_addr, or NULL. Only the set of ld1_pc_addr
 * is searched, the way a cache compares tags within one set.
 */
static FCT_Row* fct_lookup_row(Addr ld1_pc_addr) {
    if (!fct_rows) {
        return NULL;
    }

    uint64_t ld1_tag = fct_pc_tag(ld1_pc_addr);
    unsigned int set = fct_set_index(ld1_pc_addr);
    for (unsigned int way = 0; way < fct_num_ways; way++) {
        FCT_Row* row = fct_row_at(set, way);
        if (row->valid && row->ld1_pc_addr == ld1_tag) {
            return row;
        }
    }
    return NULL;
}

/**
 * Marks the row as the most recently used way in the set of
 * ld1_pc_addr.
 */
static void fct_touch_row(Addr ld1_pc_addr, const FCT_Row* row) {
    ifuse_plru_touch(fct_plru, fct_set_index(ld1_pc_addr), fct_row_way(row),
                     fct_num_ways);
}

/**
 * Returns the FCT row for ld1_pc_addr and allocates one if needed. The
 * allocation uses an invalid way if the set has one. Otherwise, it
 * evicts the tree-PLRU victim.
 */
static FCT_Row* fct_allocate_row_for_load1_pc(Addr ld1_pc_addr) {
    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (row) {
        return row;
    }

    unsigned int set = fct_set_index(ld1_pc_addr);
    for (unsigned int way = 0; way < fct_num_ways; way++) {
        if (!fct_row_at(set, way)->valid) {
            return fct_row_at(set, way);
        }
    }

    STAT_EVENT(0, FCT_EVICTIONS);
    row = fct_row_at(set, ifuse_plru_victim(fct_plru, set, fct_num_ways));
    memset(row, 0, sizeof(*row));
    return row;
}

FCT_Row* fct_lookup(Addr ld1_pc_addr) {
    if (!fct_is_initialized || ld1_pc_addr == 0) {
        return NULL;
    }
    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (row) {
        fct_touch_row(ld1_pc_addr, row);
    }
    return row;
}

void fct_update_confidence(Addr ld1_pc_addr, bool prediction_correct) {
    if (!fct_is_initialized) {
        return;
    }

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        return;
    }

    if (prediction_correct) {
        fct_increment_confidence_score(row);
        return;
    }

    const unsigned int confidence_penalty =
        IFUSE_FCT_MISPRED_CONF_PENALTY;
    row->confidence_score = (row->confidence_score <= confidence_penalty) ?
        0 : row->confidence_score - confidence_penalty;
}

Flag fct_has_load1_pc_entry(Addr ld1_pc_addr) {
    if (!fct_is_initialized) {
        return FALSE;
    }
    return fct_lookup_row(ld1_pc_addr) ? TRUE : FALSE;
}

Flag fct_install_runtime_candidate(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                   Addr ld1_effective_addr,
                                   Addr ld2_effective_addr,
                                   unsigned int offset_delta, bool direction,
                                   unsigned int ld2_mem_size,
                                   Counter ld1_micro_op_num,
                                   Counter ld2_micro_op_num,
                                   unsigned int proc_id) {
    if (!fct_is_initialized)
        fct_init();
    if (!ld1_pc_addr || !ld2_pc_addr || offset_delta > 63U ||
        !ld2_mem_size || ld2_mem_size > 64U)
        return FALSE;

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (row) {
        bool same_candidate = row->ld2_pc_addr == ld2_pc_addr &&
                              row->offset_delta == offset_delta &&
                              row->direction == direction &&
                              row->ld2_mem_size == ld2_mem_size;
        if (same_candidate) {
            if (row->confidence_score < IFUSE_FCT_RUNTIME_INSERT_CONF)
                row->confidence_score = fct_saturating_confidence_score(
                    IFUSE_FCT_RUNTIME_INSERT_CONF);
            STAT_EVENT(proc_id, FCT_RUNTIME_REINFORCEMENTS);
            return TRUE;
        }

        /*
         * A trusted row keeps its LD2. A suppressed row may be
         * relearned.
         */
        if (row->confidence_score > IFUSE_FCT_CONF_THRESHOLD) {
            STAT_EVENT(proc_id, FCT_RUNTIME_CONFLICTS);
            return FALSE;
        }
        STAT_EVENT(proc_id, FCT_RUNTIME_REPLACEMENTS);
    } else {
        row = fct_allocate_row_for_load1_pc(ld1_pc_addr);
        STAT_EVENT(proc_id, FCT_RUNTIME_INSERTS);
    }

    fct_write_row(row, ld1_pc_addr, ld2_pc_addr, ld1_effective_addr,
                  ld2_effective_addr, offset_delta, direction, ld2_mem_size,
                  ld1_micro_op_num, ld2_micro_op_num,
                  IFUSE_FCT_RUNTIME_INSERT_CONF);
    fct_touch_row(ld1_pc_addr, row);
    return TRUE;
}
