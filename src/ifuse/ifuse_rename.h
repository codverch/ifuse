#ifndef IFUSE_RENAME_H
#define IFUSE_RENAME_H

#include "../op.h"

/**
 * Applies I-Fuse-specific register renaming after normal renaming.
 *
 * For a fused LD2, this function rebinds the architectural destination
 * to the physical register that LD1 reserved. It frees the destination
 * that ordinary rename just allocated, so producers and consumers share
 * one physical register.
 */
void ifuse_rename_op(Op* op);

/**
 * Releases an unbound I-Fuse-reserved physical register when LD2
 * retires. It is a no-op after a successful fused handoff into
 * dst_reg_id.
 */
void ifuse_retire_op(Op* op);

/**
 * Releases an unbound I-Fuse-reserved physical register when LD2 is
 * flushed. It is a no-op after a successful fused handoff into
 * dst_reg_id.
 */
void ifuse_recover_op(Op* op);

/**
 * Releases an I-Fuse-reserved physical register when its APT prediction
 * expires or the handoff cannot install it as LD2's destination.
 */
void ifuse_free_ld2_physical_reg(unsigned int ld2_physical_reg_id);

/**
 * Returns the register LD1 reserved for this LD2's destination, or
 * REG_TABLE_REG_ID_INVALID, so that LD2 takes no fresh register.
 */
int ifuse_take_ld2_dest_reg(Op* op, int arch_reg_id);

#endif /* IFUSE_RENAME_H */
