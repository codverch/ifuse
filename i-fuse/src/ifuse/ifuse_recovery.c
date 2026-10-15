#include "ifuse_recovery.h"

#include "../globals/assert.h"
#include "ifuse_apt.h"

static Flag    ifuse_recovery_flushing = FALSE;
static Counter ifuse_recovery_op_num = 0;
static Flag    ifuse_frontend_redirect_pending = FALSE;
static Counter ifuse_frontend_redirect_op_num = 0;

void ifuse_recovery_begin_flush(Counter recovery_op_num) {
    ifuse_recovery_flushing = TRUE;
    ifuse_recovery_op_num = recovery_op_num;
}

void ifuse_recovery_end_flush(void) {
    ifuse_recovery_flushing = FALSE;
    ifuse_recovery_op_num = 0;
}

Flag ifuse_recovery_is_flushing(void) {
    return ifuse_recovery_flushing;
}

void ifuse_recovery_note_frontend_redirect(Counter flush_op_num) {
    ifuse_frontend_redirect_pending = TRUE;
    ifuse_frontend_redirect_op_num = flush_op_num;
}

Flag ifuse_recovery_frontend_rebuild_needed(Counter recovery_op_num) {
    (void)recovery_op_num;
    /*
     * Any recovery supersedes a fetch-time I-Fuse redirect. The winner
     * may be an older branch, LD2 itself, or a younger branch found on
     * the temporary redirected stream. In every case, the frontend must
     * seek to the winner's recovery PC and rebuild its saved FT.
     */
    return ifuse_frontend_redirect_pending;
}

void ifuse_recovery_clear_frontend_redirect(void) {
    ifuse_frontend_redirect_pending = FALSE;
    ifuse_frontend_redirect_op_num = 0;
}

void ifuse_recover_flushed_op(const Op* op) {
    if (!ifuse_recovery_flushing || !op ||
        op->op_num <= ifuse_recovery_op_num) {
        return;
    }

    if (op->ifuse_load_role == LOAD1) {
        /*
         * LD1 is leaving the machine, so its prediction can no longer be
         * fulfilled. APT removal also invalidates the ACI entry and frees
         * any extra physical register that LD1 allocated before it was
         * squashed.
         */
        apt_remove_entry_by_ld1_micro_op_any_pc((unsigned int)op->op_num);
        return;
    }

    if (op->ifuse_load_role == LOAD2 ||
        op->ifuse_load_role == PREDICTED_NOT_FUSED) {
        /*
         * If LD2 had not reached rename, APT still owns a matched entry.
         * Reopen that claim for the surviving LD1 so that the re-fetched
         * LD2 can match it. If rename already removed the entry, this is
         * a no-op.
         */
        apt_reopen_matched_entry(op->inst_info->addr,
                                 (unsigned int)op->ifuse_partner_op_num);
    }
}
