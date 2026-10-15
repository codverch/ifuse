#include "ifuse_train_retire.h"

#include <stdbool.h>

#include "../memory/memory.param.h"
#include "../statistics.h"
#include "ifuse_retired_load_history.h"
#include "ifuse_training_table.h"
#include "ifuse.param.h"

static bool access_fits_in_line(Addr addr, uns size) {
    return size && (addr % DCACHE_LINE_SIZE) + size <= DCACHE_LINE_SIZE;
}

void ifuse_train_retired_op(Op* op) {
    if (!IFUSE_RUNTIME_TRAINING_ENABLED || !op || op->off_path ||
        !op->inst_info)
        return;

    Mem_Type mem_type = op->inst_info->table_info.mem_type;
    if (mem_type == MEM_ST && op->oracle_info.va) {
        retired_load_history_invalidate_range(op->oracle_info.va,
                                              op->oracle_info.mem_size);
        STAT_EVENT(op->proc_id, IFUSE_TRAINING_STORE_INVALIDATIONS);
        return;
    }

    if (mem_type != MEM_LD || !op->oracle_info.va ||
        !op->oracle_info.mem_size ||
        !op->inst_info->table_info.num_dest_regs)
        return;

    if (!access_fits_in_line(op->oracle_info.va, op->oracle_info.mem_size)) {
        STAT_EVENT(op->proc_id, IFUSE_TRAINING_CROSS_LINE_LOADS);
        return;
    }

    /* The frontend confidence path validates existing predictions */
    if (op->ifuse_load_role != NOT_FUSION_CANDIDATE)
        return;

    RetiredLoadHistoryEntry ld1;
    if (retired_load_history_take_match(op->oracle_info.va, op->op_num,
                                        &ld1)) {
        training_table_observe(ld1.pc, op->inst_info->addr,
                               ld1.effective_addr, op->oracle_info.va,
                               op->oracle_info.mem_size,
                               ld1.micro_op_num, op->op_num, op->proc_id);
        STAT_EVENT(op->proc_id, IFUSE_TRAINING_PAIRS_DISCOVERED);
        return;
    }

    retired_load_history_insert(op->inst_info->addr, op->oracle_info.va,
                                op->op_num);
}
