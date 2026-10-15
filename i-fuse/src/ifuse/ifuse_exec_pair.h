#ifndef IFUSE_EXEC_PAIR_H
#define IFUSE_EXEC_PAIR_H

#include "../op.h"

/**
 * Tracks a mapped LD1 or LD2 in the execution-side I-Fuse pair buffer.
 *
 * APT handles register ownership during rename. This separate buffer
 * exists only to coordinate the modeled early availability of LD2's
 * result.
 */
void ifuse_exec_pair_track_mapped_load(Op* op,
                                       void (*wake_action)(Op*, Op*, uns));

/**
 * Handles the execution-side I-Fuse effect of a producer wake-up.
 *
 * When LD1 completes, its validated LD2 may wake dependents
 * immediately.
 */
void ifuse_exec_pair_handle_producer_wakeup(
    Op* op, Dep_Type type, void (*wake_action)(Op*, Op*, uns));

/**
 * Returns TRUE when LD2 already emitted its fused early wake-up signal.
 */
Flag ifuse_exec_pair_skip_duplicate_wakeup(const Op* op, Dep_Type type);

/**
 * Returns TRUE when a validated LD2 skips its L1-D access.
 *
 * A validated LD2 still takes a ROB entry, an LQ entry, and an RS
 * entry, and it issues to a load port to generate its address. Its
 * value comes from LD1's fused access, so it makes no L1-D access and
 * sends no memory request.
 */
Flag ifuse_exec_pair_bypass_ld2_memory_pipeline(const Op* op);

/**
 * Completes a validated LD2's address generation.
 *
 * Called from the d-cache stage when LD2 arrives after its address
 * generation. LD2 is done once both this and its value from LD1
 * (ifuse_exec_pair_signal_ld2()) have happened, in either order.
 */
void ifuse_exec_pair_complete_ld2_agu(Op* op);

/**
 * Keeps a fused LD2's earlier wake cycle if a later completion would
 * overwrite it. Dependents that map afterward thus still see the
 * fused data's time.
 */
void ifuse_exec_pair_restore_early_wake(Op* op);

/**
 * Removes the execution-side record that one LD1 prediction owns.
 *
 * APT calls this when a prediction expires or is discarded before a
 * successful LD2 handoff.
 */
void ifuse_exec_pair_forget_ld1_prediction(Counter ld1_op_num);

/**
 * Removes any execution-side pair-buffer entry that refers to op.
 *
 * Scarab calls this before it returns an Op to the reusable op pool.
 */
void ifuse_exec_pair_forget_op(const Op* op);

void ifuse_exec_pair_observe_live_pairs(uns proc_id);

#endif /* IFUSE_EXEC_PAIR_H */
