#include "ifuse_stats.h"

#include "../core.param.h"
#include "../map.h"
#include "../map_rename.h"
#include "../statistics.h"

static uint64_t ifuse_extra_reg_in_use = 0;
static uint64_t ifuse_extra_reg_in_use_peak = 0;
static uint64_t ifuse_gp_reg_occupied_peak = 0;

void ifuse_extra_reg_note_alloc(void) {
    ifuse_extra_reg_in_use++;
    if (ifuse_extra_reg_in_use > ifuse_extra_reg_in_use_peak) {
        INC_STAT_EVENT(0, IFUSE_EXTRA_REG_IN_USE_PEAK,
                       ifuse_extra_reg_in_use - ifuse_extra_reg_in_use_peak);
        ifuse_extra_reg_in_use_peak = ifuse_extra_reg_in_use;
    }
}

void ifuse_extra_reg_note_free(void) {
    if (ifuse_extra_reg_in_use > 0) {
        ifuse_extra_reg_in_use--;
    }
}

void ifuse_observe_extra_reg_pressure(uns proc_id) {
    STAT_EVENT(proc_id, IFUSE_EXTRA_REG_IN_USE_OBSERVATIONS);
    INC_STAT_EVENT(proc_id, IFUSE_EXTRA_REG_IN_USE_TOTAL, ifuse_extra_reg_in_use);
}

void ifuse_observe_gp_reg_pressure(uns proc_id) {
    struct reg_table* physical_reg_table =
        map_data->reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]
            ->reg_table[REG_TABLE_TYPE_PHYSICAL];
    uns total_regs = physical_reg_table->size;
    uns free_regs = physical_reg_table->free_list->reg_free_num;
    uns occupied_regs = total_regs - free_regs;
    uns util_pct = total_regs ? (occupied_regs * 100U) / total_regs : 0U;

    STAT_EVENT(proc_id, IFUSE_GP_REG_OCCUPIED_OBSERVATIONS);
    INC_STAT_EVENT(proc_id, IFUSE_GP_REG_OCCUPIED_TOTAL, occupied_regs);
    INC_STAT_EVENT(proc_id, IFUSE_GP_REG_UTIL_PCT_TOTAL, util_pct);

    if (occupied_regs > ifuse_gp_reg_occupied_peak) {
        INC_STAT_EVENT(proc_id, IFUSE_GP_REG_OCCUPIED_PEAK,
                       occupied_regs - ifuse_gp_reg_occupied_peak);
        ifuse_gp_reg_occupied_peak = occupied_regs;
    }
}

uns64 ifuse_extra_reg_in_use_now(void) {
    return ifuse_extra_reg_in_use;
}
