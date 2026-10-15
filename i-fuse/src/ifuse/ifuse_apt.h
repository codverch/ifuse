// STD headers
#include <stdbool.h>
#include <stdint.h>

// Custom headers
#include "globals/global_types.h"

#ifndef IFUSE_APT_H
#define IFUSE_APT_H

/**
 * Stores one live LD1 prediction waiting for its predicted LD2.
 */
typedef struct APT_Entry {
    // Predicted LD2 identity
    Addr         ld2_pc_addr;

    // Dynamic LD1 context
    Addr         ld1_pc_addr;
    Addr         ld1_effective_addr;
    unsigned int ld1_micro_op_num;
    unsigned int ld2_physical_reg_id;

    // Predicted LD2 memory access
    Addr         predicted_ld2_effective_addr;
    unsigned int predicted_ld2_memory_access_size;

    // Bookkeeping
    bool         valid;
    bool         matched;
    uint64_t     timestamp;
} APT_Entry;

/**
 * Initializes the set-associative Active Pair Table.
 */
void apt_init(void);

/**
 * Returns and claims the unmatched entry waiting for ld2_pc_addr.
 *
 * There is at most one, since a younger LD1 replaces an older one's
 * entry.
 *
 * @param ld2_pc_addr The PC address of the candidate LD2.
 * @return The unmatched APT entry for this LD2 PC, or NULL.
 */
APT_Entry* apt_lookup(Addr ld2_pc_addr);

/**
 * Inserts a live LD1 prediction for a future LD2.
 *
 * A younger LD1 replaces any unmatched entry that an older LD1 holds
 * for the same LD2 PC.
 *
 * @param ld1_pc_addr The PC address of the predicting LD1.
 * @param ld1_effective_addr The effective memory address that LD1
 *        accesses.
 * @param ld1_micro_op_num The dynamic micro-op number of LD1.
 * @param predicted_ld2_memory_access_size The predicted LD2 access size.
 * @param ld2_pc_addr The predicted LD2 PC address.
 * @param predicted_ld2_effective_addr The predicted LD2 effective address.
 * @return The inserted APT entry, or NULL if insertion fails.
 */
APT_Entry* apt_insert_entry(Addr ld1_pc_addr,
                            Addr ld1_effective_addr,
                            unsigned int ld1_micro_op_num,
                            unsigned int predicted_ld2_memory_access_size,
                            Addr ld2_pc_addr,
                            Addr predicted_ld2_effective_addr);

/**
 * Removes the entry for a dynamic LD1 after rename-stage bookkeeping
 * completes.
 *
 * @param ld2_pc_addr The predicted LD2 PC address used to find the APT
 *        set.
 * @param ld1_micro_op_num The dynamic micro-op number of LD1.
 */
void apt_remove_entry_by_ld1_micro_op(Addr ld2_pc_addr,
                                      unsigned int ld1_micro_op_num);

/**
 * Removes the live prediction that a dynamic LD1 owns, regardless of
 * the LD2 PC.
 *
 * Recovery calls this when it flushes LD1 before its pending APT
 * prediction expires.
 */
void apt_remove_entry_by_ld1_micro_op_any_pc(unsigned int ld1_micro_op_num);

/**
 * Reopens an APT claim made by an LD2 that was flushed before rename.
 *
 * The matching LD1 survives recovery, so its APT and ACI prediction can
 * be reused when LD2 is fetched again.
 *
 * @return TRUE if a matched APT entry was reopened.
 */
bool apt_reopen_matched_entry(Addr ld2_pc_addr,
                              unsigned int ld1_micro_op_num);

/**
 * Records the speculative physical register that LD1 allocates for
 * LD2's result.
 *
 * @return TRUE if the matching live LD1 entry was found.
 */
bool apt_set_ld2_physical_reg_id(unsigned int ld1_micro_op_num,
                                 unsigned int ld2_physical_reg_id);

/**
 * Transfers LD1's speculative physical register from APT to LD2.
 *
 * The APT entry is removed after ownership transfers to LD2.
 *
 * @return TRUE if the matching entry owned a speculative register.
 */
bool apt_take_ld2_physical_reg_id(Addr ld2_pc_addr,
                                  unsigned int ld1_micro_op_num,
                                  unsigned int* ld2_physical_reg_id);

/**
 * Observes the current number of live APT LD2 predictions.
 *
 * The average number of live APT LD2 predictions is reported as
 * APT_LIVE_LD2_PREDICTION_AVG (TOTAL / OBSERVATIONS). Peak occupancy
 * is APT_LIVE_LD2_PREDICTION_PEAK. Use the peak to size a finite APT.
 */
void apt_observe_live_ld2_predictions(uns proc_id);
void apt_observe_cycle(uns proc_id);

/**
 * Removes entries whose LD2 did not arrive within
 * IFUSE_FUSION_DISTANCE.
 *
 * @param current_op_num The current micro-op number.
 */
void apt_cleanup_stale(uint64_t current_op_num);

#endif /* IFUSE_APT_H */
