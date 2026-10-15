/*
 * Checks the PMU counters in ifuse_pmu.c on hand-built ops. The tests
 * cover the no-fusion estimate for LD2's value and the classification
 * of each consumer.
 *
 * Cycles below: LD1 reaches the L1-D at 100. A hit returns data at
 * 105, and a miss returns data at 300. With the 1-cycle wake delay,
 * fused LD2 data is visible 1 cycle after LD1's.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "globals/global_types.h"
#include "ifuse/ifuse_pmu.h"
#include "op.h"
#include "statistics.h"

/*
 * Scarab globals that ifuse_pmu.c reads. The test defines them here
 * instead of linking Scarab.
 */
uns DCACHE_CYCLES = 5;
Stat** global_stat_array;

static Inst_Info load_info, alu_info, branch_info;
static Counter next_unique = 1;

static Counter count(Stat_Enum s) { return global_stat_array[0][s].count; }

static void clear_stats(void) { memset(global_stat_array[0], 0, NUM_GLOBAL_STATS * sizeof(Stat)); }

static Op make_op(Inst_Info* info) {
  Op op;

  memset(&op, 0, sizeof(op));
  op.inst_info = info;
  op.unique_num = next_unique++;
  op.rdy_cycle = 1;
  op.issue_cycle = 10;
  op.ifuse_load_role = NOT_FUSION_CANDIDATE;
  op.ifuse_ld2_early_wake_cycle = MAX_CTR;
  op.ifuse_ld1_dcache_cycle = MAX_CTR;
  return op;
}

/* Fused LD2 whose address sources were ready at addr_ready */
static Op make_fused_ld2(Counter addr_ready, Counter ld1_data) {
  Op ld2 = make_op(&load_info);

  ld2.ifuse_load_role = LOAD2;
  ld2.rdy_cycle = addr_ready;
  ld2.ifuse_ld1_dcache_cycle = 100;
  ld2.ifuse_ld2_data_cycle = ld1_data;
  ld2.ifuse_ld2_early_wake_cycle = ld1_data + 1;
  ld2.wake_cycle = ld1_data + 1;
  ld2.ifuse_ld2_early_wake_signaled = TRUE;
  return ld2;
}

/* Consumer of ld2, plus another source that woke at other (0 for none) */
static Op make_consumer(Inst_Info* info, const Op* ld2, Counter other, Counter issue) {
  Op c = make_op(info);
  Op src = make_op(&alu_info);

  c.issue_cycle = issue;
  c.rdy_cycle = ld2->wake_cycle;
  ifuse_pmu_note_wake(ld2, &c);
  if (other) {
    src.wake_cycle = other;
    c.rdy_cycle = c.rdy_cycle > other ? c.rdy_cycle : other;
    ifuse_pmu_note_wake(&src, &c);
  }
  return c;
}

static void test_ld2_estimate(void) {
  Op ld2;

  /*
   * Hit, with LD2's address ready late at 400. LD2 would reach the
   * cache at 401 and get data at 406. Fused data at 106 is 300 cycles
   * earlier.
   */
  clear_stats();
  ld2 = make_fused_ld2(400, 105);
  ifuse_pmu_retire(&ld2);
  assert(count(IFUSE_PMU_LD2_FUSED) == 1 && count(IFUSE_PMU_LD2_EARLY) == 1);
  assert(count(IFUSE_PMU_LD2_EARLY_CYCLES) == 300);

  /*
   * Miss, with LD2's address ready at 150. LD2 reaches the cache at
   * 151, merges with LD1's miss, and gets data at 300. Fused data at
   * 301 is 1 cycle late.
   */
  clear_stats();
  ld2 = make_fused_ld2(150, 300);
  ifuse_pmu_retire(&ld2);
  assert(count(IFUSE_PMU_LD2_NOT_EARLY) == 1 && count(IFUSE_PMU_LD2_LATE_CYCLES) == 1);

  /*
   * Miss, with LD2's address ready at 80. LD2 reaches the cache first,
   * at 81, and sees LD1's 200-cycle latency, so it gets data at 281.
   * Fused data is 20 cycles late.
   */
  clear_stats();
  ld2 = make_fused_ld2(80, 300);
  ifuse_pmu_retire(&ld2);
  assert(count(IFUSE_PMU_LD2_NOT_EARLY) == 1 && count(IFUSE_PMU_LD2_LATE_CYCLES) == 20);

  /* LD2 that lost its pair and issued on its own */
  clear_stats();
  ld2 = make_op(&load_info);
  ld2.ifuse_load_role = LOAD2;
  ifuse_pmu_retire(&ld2);
  assert(count(IFUSE_PMU_LD2_ISSUED) == 1 && count(IFUSE_PMU_LD2_FUSED) == 0);
}

static void test_consumers(void) {
  Op early = make_fused_ld2(400, 105);  // Fused 106, no fusion 406
  Op late = make_fused_ld2(150, 300);  // Fused 301, no fusion 300
  Op c;

  clear_stats();
  ifuse_pmu_retire(&early);
  ifuse_pmu_retire(&late);

  /* Only source is LD2, so ready at 106 instead of 406 */
  c = make_consumer(&branch_info, &early, 0, 50);
  ifuse_pmu_retire(&c);
  assert(count(IFUSE_PMU_CONSUMERS_EARLY) == 1 && count(IFUSE_PMU_CONSUMERS_EARLY_CYCLES) == 300);
  assert(count(IFUSE_PMU_CONSUMERS_EARLY_BRANCH) == 1);
  assert(count(IFUSE_PMU_CONSUMERS_EARLY_CYCLES_BRANCH) == 300);

  /* Second source at 200, so ready at 200 instead of 406 (206 earlier) */
  c = make_consumer(&load_info, &early, 200, 50);
  ifuse_pmu_retire(&c);
  assert(count(IFUSE_PMU_CONSUMERS_EARLY_LOAD) == 1);
  assert(count(IFUSE_PMU_CONSUMERS_EARLY_CYCLES_LOAD) == 206);

  /* Second source at 500, after LD2's no-fusion time, so no gain */
  c = make_consumer(&alu_info, &early, 500, 50);
  ifuse_pmu_retire(&c);
  assert(count(IFUSE_PMU_CONSUMERS_OTHER_SOURCE) == 1);

  /* Entered the scheduler at 450, after both times, so no gain */
  c = make_consumer(&alu_info, &early, 0, 450);
  ifuse_pmu_retire(&c);
  assert(count(IFUSE_PMU_CONSUMERS_BOUND_BY_ISSUE) == 1);

  /* LD2's value not earlier */
  c = make_consumer(&alu_info, &late, 0, 50);
  ifuse_pmu_retire(&c);
  assert(count(IFUSE_PMU_CONSUMERS_LD2_NOT_EARLY) == 1);

  /* LD2's record overwritten */
  c = make_consumer(&alu_info, &early, 0, 50);
  c.ifuse_pmu_ld2_unique_num += 1 << 16;
  ifuse_pmu_retire(&c);
  assert(count(IFUSE_PMU_CONSUMERS_UNKNOWN) == 1);

  assert(count(IFUSE_PMU_CONSUMERS) == 6);
  assert(count(IFUSE_PMU_CONSUMERS_EARLY) == 2);
}

int main(void) {
  global_stat_array = malloc(sizeof(Stat*));
  global_stat_array[0] = calloc(NUM_GLOBAL_STATS, sizeof(Stat));

  load_info.table_info.mem_type = MEM_LD;
  load_info.table_info.cf_type = NOT_CF;
  load_info.table_info.op_type = OP_ILD;
  alu_info.table_info.mem_type = NOT_MEM;
  alu_info.table_info.cf_type = NOT_CF;
  alu_info.table_info.op_type = OP_IADD;
  branch_info.table_info.mem_type = NOT_MEM;
  branch_info.table_info.cf_type = CF_CBR;
  branch_info.table_info.op_type = OP_CF;

  test_ld2_estimate();
  test_consumers();
  puts("I-Fuse PMU tests passed");
  return 0;
}
