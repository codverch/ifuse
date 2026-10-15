#ifndef IFUSE_RECOVERY_H
#define IFUSE_RECOVERY_H

#include "../op.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Marks the interval in which Scarab is freeing squashed operations.
 */
void ifuse_recovery_begin_flush(Counter recovery_op_num);
void ifuse_recovery_end_flush(void);
Flag ifuse_recovery_is_flushing(void);
void ifuse_recovery_note_frontend_redirect(Counter flush_op_num);
Flag ifuse_recovery_frontend_rebuild_needed(Counter recovery_op_num);
void ifuse_recovery_clear_frontend_redirect(void);

#ifdef __cplusplus
}
#endif

/**
 * Repairs fetch-time I-Fuse table state for one squashed operation.
 */
void ifuse_recover_flushed_op(const Op* op);

#endif /* IFUSE_RECOVERY_H */
