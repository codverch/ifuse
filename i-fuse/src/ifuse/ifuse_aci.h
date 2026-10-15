// STD headers
#include <stdint.h>

#ifndef IFUSE_ACI_H
#define IFUSE_ACI_H

/**
 * Set-associative Access Check Index (ACI)
 * ========================================
 * The ACI stores predicted LD2 cache blocks for live LD1 predictions.
 * An arriving LD2 checks the ACI using its actual cache block and the
 * LD1 micro-op number that APT returns. Each set holds up to
 * IFUSE_ACI_WAYS entries with tree-PLRU replacement.
 */

#define ACI_CACHE_LINE_BITS 6

typedef enum {
    ACI_LOOKUP_MATCH = 0,
    ACI_LOOKUP_NO_MATCHING_ENTRY,
    ACI_LOOKUP_WRONG_PREDICTED_BLOCK,
} ACI_Result;

/**
 * Initializes the set-associative Access Check Index.
 */
void aci_init(void);

/**
 * Inserts the cache block predicted for a live LD1.
 *
 * @param predicted_ld2_effective_addr The predicted effective address
 *        of LD2.
 * @param ld1_micro_op_num The dynamic micro-op number of LD1.
 */
void aci_insert_prediction(uint64_t predicted_ld2_effective_addr,
                           uint64_t ld1_micro_op_num);

/**
 * Checks the actual LD2 cache block and consumes the matching LD1 entry.
 *
 * @param actual_ld2_effective_addr The actual effective address that
 *        LD2 accesses.
 * @param predicted_ld2_effective_addr The LD2 effective address that
 *        LD1 predicts.
 * @param ld1_micro_op_num The dynamic micro-op number of the waiting
 *        LD1.
 * @return The ACI lookup result.
 */
ACI_Result aci_check_and_consume_prediction(
    uint64_t actual_ld2_effective_addr,
    uint64_t predicted_ld2_effective_addr,
    uint64_t ld1_micro_op_num);

/**
 * Invalidates the prediction for a predicted LD2 effective address.
 *
 * @param predicted_ld2_effective_addr The predicted effective address
 *        of LD2.
 * @param ld1_micro_op_num The dynamic micro-op number of LD1.
 */
void aci_invalidate_prediction(uint64_t predicted_ld2_effective_addr,
                               uint64_t ld1_micro_op_num);

/**
 * Removes predictions whose LD2 did not arrive within
 * IFUSE_FUSION_DISTANCE.
 *
 * @param current_op_num The current micro-op number.
 */
void aci_cleanup_stale(uint64_t current_op_num);

#endif /* IFUSE_ACI_H */
