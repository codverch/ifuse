#ifndef IFUSE_STORE_WINDOW_H
#define IFUSE_STORE_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#include "op.h"

/**
 * Records a fetched on-path op. Stores enter the window; an op number at or
 * below the last one seen means fetch rewound after a recovery, so younger
 * stores leave the window.
 *
 * @param op The fetched on-path op.
 */
void ifuse_store_window_note_op(const Op* op);

/**
 * Returns whether a store fetched between LD1 and LD2 writes any byte LD2
 * reads (a RAW hazard on the fused value).
 *
 * @param ld1_op_num LD1's micro-op number.
 * @param ld2_op_num LD2's micro-op number.
 * @param ld2_va LD2's effective address.
 * @param ld2_size LD2's access size in bytes.
 * @return TRUE if such a store exists.
 */
bool ifuse_store_window_overlaps(uint64_t ld1_op_num, uint64_t ld2_op_num, uint64_t ld2_va, unsigned ld2_size);

#endif /* IFUSE_STORE_WINDOW_H */
