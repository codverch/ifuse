/***************************************************************************************
 * File         : backend_occupancy.c
 *    Per-cycle backend structure conditions, each sampled independently.
 *
 *    The top-down breakdown charges each stalled slot to exactly one cause, so
 *    a cycle in which the PRF is full and the ROB head waits on a D-cache miss
 *    counts only once. These counters instead ask, for every structure, "was
 *    this condition true this cycle?" Conditions overlap, so they do not sum
 *    to 100%.
 *
 *    Event-style conditions (port contention, bank conflicts, MSHR full,
 *    busy FUs) are derived from the change in their existing event counters
 *    since the previous sample, which runs once per cycle after every stage.
 ***************************************************************************************/

#include "backend_occupancy.h"

#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"

#include "core.param.h"

#include "ifuse/ifuse_stats.h"

#include "lsq.h"
#include "map.h"
#include "map_rename.h"
#include "node_stage.h"
#include "op.h"
#include "statistics.h"

Flag rs_dispatch_blocked = FALSE;

/* Event counters whose per-cycle change marks an event-style condition */
typedef struct {
  Stat_Enum stats[2];
  uns num_stats;
  Stat_Enum cycles_stat;
} Event_Condition;

static const Event_Condition event_conditions[] = {
    {{ISSUE_QUEUE_OP_READY_UNISSUED_TOTAL}, 1, BE_PORT_CONTENTION_CYCLES},
    {{FUS_BUSY_ON_PATH, FUS_BUSY_OFF_PATH}, 2, BE_FU_HELD_CYCLES},
    {{DCACHE_READ_PORT_UNAVAILABLE_ONPATH, DCACHE_READ_PORT_UNAVAILABLE_OFFPATH}, 2, BE_DCACHE_BANK_CONFLICT_CYCLES},
    {{DCACHE_FILL_PORT_UNAVAILABLE_ONPATH, DCACHE_FILL_PORT_UNAVAILABLE_OFFPATH}, 2, BE_DCACHE_FILL_PORT_CONFLICT_CYCLES},
    {{DCACHE_MISS_WAITMEM}, 1, BE_MSHR_FULL_CYCLES},
};
#define NUM_EVENT_CONDITIONS (sizeof(event_conditions) / sizeof(event_conditions[0]))

static Counter last_event_total[MAX_NUM_PROCS][NUM_EVENT_CONDITIONS];

/* Same test as rename's reg_file_check_reg_num(), for one register type */
static Flag reg_type_full(uns reg_type) {
  struct reg_free_list* free_list = map_data->reg_file[reg_type]->reg_table[REG_TABLE_TYPE_PHYSICAL]->free_list;
  return free_list->reg_free_num < REG_FILE_MAX_DESTS[reg_type] * RENAME_WIDTH;
}

void backend_occupancy_observe_cycle(uns proc_id) {
  STAT_EVENT(proc_id, BE_OBSERVED_CYCLES);

  // Register-cycles: per-cycle sums of allocated and I-Fuse-reserved GP
  // registers. Same work in both configs, so the sums compare directly.
  struct reg_free_list* gp_free =
      map_data->reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]->reg_table[REG_TABLE_TYPE_PHYSICAL]->free_list;
  INC_STAT_EVENT(proc_id, BE_GP_REGS_IN_USE_SUM, REG_TABLE_INTEGER_PHYSICAL_SIZE - gp_free->reg_free_num);
  INC_STAT_EVENT(proc_id, BE_GP_REGS_RESERVED_SUM, ifuse_extra_reg_in_use_now());

  Flag gp_full = reg_type_full(REG_FILE_REG_TYPE_GENERAL_PURPOSE);
  Flag vec_full = reg_type_full(REG_FILE_REG_TYPE_VECTOR);
  if (gp_full)
    STAT_EVENT(proc_id, BE_PRF_GP_FULL_CYCLES);
  if (vec_full)
    STAT_EVENT(proc_id, BE_PRF_VEC_FULL_CYCLES);
  if (node->node_count == NODE_TABLE_SIZE)
    STAT_EVENT(proc_id, BE_ROB_FULL_CYCLES);
  if (!lsq_available(MEM_LD))
    STAT_EVENT(proc_id, BE_LQ_FULL_CYCLES);
  if (!lsq_available(MEM_ST))
    STAT_EVENT(proc_id, BE_SQ_FULL_CYCLES);
  if (rs_dispatch_blocked)
    STAT_EVENT(proc_id, BE_RS_FULL_CYCLES);

  Op* head = node->node_head;
  if (head && head->state != OS_DONE && !OP_DONE(head)) {
    if (head->engine_info.dcmiss)
      STAT_EVENT(proc_id, BE_HEAD_DCACHE_MISS_CYCLES);
    if (head->engine_info.l1_miss)
      STAT_EVENT(proc_id, BE_HEAD_LLC_MISS_CYCLES);
  }

  for (uns ii = 0; ii < NUM_EVENT_CONDITIONS; ii++) {
    const Event_Condition* cond = &event_conditions[ii];
    Counter total = 0;
    for (uns jj = 0; jj < cond->num_stats; jj++)
      total += GET_STAT_EVENT(proc_id, cond->stats[jj]);
    // Use != rather than >: the warmup reset can lower the running total.
    if (total != last_event_total[proc_id][ii])
      STAT_EVENT(proc_id, cond->cycles_stat);
    last_event_total[proc_id][ii] = total;
  }
}
