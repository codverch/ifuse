#include "ifuse_pmu.h"

#include "../globals/global_defs.h"
#include "../globals/global_types.h"
#include "../globals/utils.h"
#include "../memory/memory.param.h"
#include "../op_info.h"
#include "../statistics.h"

/*
 * A consumer retires after its LD2, and by then the LD2 Op may have
 * been recycled. Each retired fused LD2 therefore leaves its two times
 * here, keyed by unique_num. Its consumers look them up when they
 * retire. The 64K entries cover any consumer within 64K ops of its LD2,
 * far beyond a 512-entry ROB.
 */
#define IFUSE_PMU_LD2_LOG_BITS 16
#define IFUSE_PMU_LD2_LOG_SIZE (1U << IFUSE_PMU_LD2_LOG_BITS)

typedef struct {
  Counter unique_num;  // Zero if empty
  Counter fused_cycle;  // LD2's value visible, with fusion
  Counter nofuse_cycle;  // LD2's value visible without fusion (estimate)
} IFuse_PMU_LD2;

static IFuse_PMU_LD2 ld2_log[IFUSE_PMU_LD2_LOG_SIZE];

typedef enum {
  CLASS_LOAD,
  CLASS_STORE,
  CLASS_BRANCH,
  CLASS_INT_ALU,
  CLASS_INT_MULDIV,
  CLASS_FP_VECTOR,
  CLASS_OTHER,
} Consumer_Class;

/**
 * A fused LD2's wake-up comes from LD1's access
 * (ifuse_exec_pair_wake_ld2()), which sets ifuse_ld2_early_wake_cycle
 * before it wakes any consumer.
 */
static Flag is_fused_ld2(const Op* op) {
  return op->ifuse_load_role == LOAD2 && op->ifuse_ld2_early_wake_cycle != MAX_CTR &&
         op->wake_cycle == op->ifuse_ld2_early_wake_cycle;
}

void ifuse_pmu_note_wake(const Op* src_op, Op* dep_op) {
  if (is_fused_ld2(src_op))
    dep_op->ifuse_pmu_ld2_unique_num = src_op->unique_num;
  else
    dep_op->ifuse_pmu_other_rdy_cycle = MAX2(dep_op->ifuse_pmu_other_rdy_cycle, src_op->wake_cycle);
}

/**
 * Returns the earliest cycle an op could issue: the cycle after it
 * entered the scheduler, or the cycle its last source became ready,
 * whichever is later.
 */
static Counter earliest_issue(const Op* op, Counter sources_ready) {
  if (op->issue_cycle == MAX_CTR)
    return sources_ready;
  return MAX2(sources_ready, op->issue_cycle + 1);
}

/**
 * Returns the cycle when LD2's value would be visible without fusion.
 * LD2 issues as soon as its address sources are ready and generates its
 * address in the next cycle. If that happens before LD1 reaches the
 * L1-D, LD2 would be first to the line and would see LD1's latency.
 * Otherwise, the line is present or arriving, so LD2 sees the hit
 * latency, but no earlier than LD1's data.
 *
 * For example, LD1 accesses the cache at cycle 100 and misses, and its
 * data arrives at cycle 300. LD2's address is ready at cycle 150, so LD2
 * reaches the cache at cycle 151, merges with the miss, and gets data
 * at cycle 300. With fusion, LD2 gets data at cycle 301 (1-cycle wake
 * delay), so it is 1 cycle late. If LD2's address were ready at cycle
 * 80, LD2 would reach the cache at cycle 81 and get data at cycle 281.
 */
static Counter nofuse_cycle(const Op* ld2) {
  Counter dcache_cycle = earliest_issue(ld2, ld2->rdy_cycle) + 1;
  Counter ld1_dcache_cycle = ld2->ifuse_ld1_dcache_cycle;
  Counter ld1_data_cycle = ld2->ifuse_ld2_data_cycle;

  if (ld1_dcache_cycle != MAX_CTR && dcache_cycle < ld1_dcache_cycle)
    return dcache_cycle + (ld1_data_cycle - ld1_dcache_cycle);
  return MAX2(dcache_cycle + DCACHE_CYCLES + ld2->inst_info->extra_ld_latency, ld1_data_cycle);
}

static Consumer_Class consumer_class(const Op* op) {
  const Table_Info* t = &op->inst_info->table_info;

  if (t->mem_type == MEM_LD)
    return CLASS_LOAD;
  if (t->mem_type == MEM_ST)
    return CLASS_STORE;
  if (t->cf_type != NOT_CF)
    return CLASS_BRANCH;
  switch (t->op_type) {
    case OP_MOV:
    case OP_CMOV:
    case OP_LDA:
    case OP_IADD:
    case OP_ICMP:
    case OP_LOGIC:
    case OP_SHIFT:
      return CLASS_INT_ALU;
    case OP_IMUL:
    case OP_IDIV:
      return CLASS_INT_MULDIV;
    case OP_NOP:
    case OP_INV:
    case OP_CF:
      return CLASS_OTHER;
    default:
      return CLASS_FP_VECTOR;
  }
}

static void retire_ld2(const Op* op) {
  STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_RETIRED);
  if (!op->ifuse_ld2_early_wake_signaled) {
    STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_ISSUED);
    return;
  }
  STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_FUSED);

  Counter fused = op->ifuse_ld2_early_wake_cycle;
  Counter nofuse = nofuse_cycle(op);
  if (nofuse > fused) {
    STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_EARLY);
    INC_STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_EARLY_CYCLES, nofuse - fused);
    INC_STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_AVG_EARLY_CYCLES, nofuse - fused);
    INC_STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_AVG_SAVED_CYCLES, nofuse - fused);
  } else {
    STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_NOT_EARLY);
    INC_STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_LATE_CYCLES, fused - nofuse);
    INC_STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_AVG_LOST_CYCLES, fused - nofuse);
  }

  IFuse_PMU_LD2* e = &ld2_log[op->unique_num & (IFUSE_PMU_LD2_LOG_SIZE - 1)];
  e->unique_num = op->unique_num;
  e->fused_cycle = fused;
  e->nofuse_cycle = nofuse;
}

static void retire_consumer(const Op* op) {
  const IFuse_PMU_LD2* e = &ld2_log[op->ifuse_pmu_ld2_unique_num & (IFUSE_PMU_LD2_LOG_SIZE - 1)];

  STAT_EVENT(op->proc_id, IFUSE_PMU_CONSUMERS);
  if (e->unique_num != op->ifuse_pmu_ld2_unique_num) {
    STAT_EVENT(op->proc_id, IFUSE_PMU_CONSUMERS_UNKNOWN);
    return;
  }

  Counter other = op->ifuse_pmu_other_rdy_cycle;
  Counter with = earliest_issue(op, op->rdy_cycle);
  Counter without = earliest_issue(op, MAX2(other, e->nofuse_cycle));

  if (without > with) {
    Counter gain = without - with;
    Consumer_Class c = consumer_class(op);

    STAT_EVENT(op->proc_id, IFUSE_PMU_CONSUMERS_EARLY);
    INC_STAT_EVENT(op->proc_id, IFUSE_PMU_CONSUMERS_EARLY_CYCLES, gain);
    INC_STAT_EVENT(op->proc_id, IFUSE_PMU_CONSUMERS_AVG_EARLY_CYCLES, gain);
    STAT_EVENT(op->proc_id, IFUSE_PMU_CONSUMERS_EARLY_LOAD + c);
    INC_STAT_EVENT(op->proc_id, IFUSE_PMU_CONSUMERS_EARLY_CYCLES_LOAD + c, gain);
  } else if (e->nofuse_cycle <= e->fused_cycle) {
    STAT_EVENT(op->proc_id, IFUSE_PMU_CONSUMERS_LD2_NOT_EARLY);
  } else if (other >= e->nofuse_cycle) {
    STAT_EVENT(op->proc_id, IFUSE_PMU_CONSUMERS_OTHER_SOURCE);
  } else {
    STAT_EVENT(op->proc_id, IFUSE_PMU_CONSUMERS_BOUND_BY_ISSUE);
  }
}

void ifuse_pmu_retire(const Op* op) {
  if (op->off_path)
    return;
  /* An LD2 retires before its consumers, so its record is in place. */
  if (op->ifuse_load_role == LOAD2)
    retire_ld2(op);
  else if (op->ifuse_load_role == PREDICTED_NOT_FUSED && op->ifuse_ld2_prediction_failed) {
    STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_RETIRED);
    STAT_EVENT(op->proc_id, IFUSE_PMU_LD2_MISPREDICTED);
  }
  if (op->ifuse_pmu_ld2_unique_num)
    retire_consumer(op);
}
