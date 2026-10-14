/**
 * @file heliosFusion.c
 *
 * @brief Helios load-pair fusion: predict and validate at fetch, confirm in the
 * allocation queue, and model the fused pair through rename, issue, the
 * D-cache, and retire. See helios/README.md for the paper correspondence.
 *
 * @author Ved Lakshminarayanan <vlakshmi@andrew.cmu.edu>
 *
 * Paper: Singh et al., "Exploring Instruction Fusion Opportunities in General
 * Purpose Processors", MICRO 2022, Sec. IV. Only load pairs are modeled.
 *
 * Life of a pair:
 *   1. Fetch (heliosFetchHook). A load that directly follows an unfused load
 *      with the same address registers and a contiguous access fuses
 *      statically (consecutive fusion, CSF). Otherwise the fusion predictor
 *      gives a distance to a head. The pair is checked with oracle addresses:
 *      a pair that does not fit one region flushes at the tail's execute; a
 *      deadlock or serializing catalyst unfuses at the tail's rename.
 *   2. Allocation queue (heliosAllocQueueHook). The paper fuses only while
 *      the head is still in the queue between decode and rename (Scarab's
 *      IDQ). If the head has already been renamed, the fusion is cancelled.
 *   3. Rename (heliosRenameHook). The head is held in the RS until its tail
 *      renames (NCS_Ready). Then the head also waits for the tail's source
 *      operands, including ones produced inside the catalyst.
 *   4. The tail takes no ROB, RS, or LQ entry and never issues. Its data
 *      arrives with the head's single access (heliosCompleteFusedTail), one
 *      cycle later if the pair spans two lines.
 *   5. Retire. The head waits for its whole group (heliosExtCommitGroupBlocks).
 *      Unfused loads train the UCH and the predictor (heliosCommit).
 */

#include "heliosFusion.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "globals/global_vars.h"

#include "bp/bp.param.h"

#include "bp/bp.h"
#include "bp/cbp_to_scarab.h"

#include "issue_queue.h"
#include "map.h"
#include "map_rename.h"
#include "model.h"
#include "statistics.h"

static LoadHeadTableEntry loadHeadTable[LOAD_HEAD_TABLE_SIZE];
static RegTrackEntry regTrackTable[REG_TRACK_TABLE_SIZE];
static int regTrackTableHead;
static FusionIntervalEntry activeFusions[FUSION_RING_SIZE];
static int activeFusionsHead;

/* op_num of the last micro-op of the macro being fetched. A flush keeps the
 * flushing op's whole macro, so every micro-op carries this bound. */
static Counter macroLastOpNum[MAX_NUM_PROCS];

void heliosFusionInit(void) {
  predictorInit();
  initLoadUCH();
  memset(loadHeadTable, 0, sizeof(loadHeadTable));
  memset(regTrackTable, 0, sizeof(regTrackTable));
  memset(activeFusions, 0, sizeof(activeFusions));
  regTrackTableHead = 0;
  activeFusionsHead = 0;
}

/**************************************************************************************/
/* Small helpers */

static inline Flag isLoad(const Op* op) {
  return op->uop->mem_type == MEM_LD;
}

/* Is `op` still the in-flight op numbered op_num? Ops are recycled. */
static inline Flag opLive(const Op* op, Counter op_num) {
  return op && op->op_pool_valid && op->op_num == op_num;
}

/* Histogram bucket for a distance: 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65+. */
static inline uns distBucket(Counter distance) {
  uns b = 0;
  for (Counter limit = 1; b < 7 && distance > limit; limit <<= 1)
    b++;
  return b;
}

static Flag sameSrcRegs(const Op* a, const Op* b) {
  if (a->uop->num_src_regs != b->uop->num_src_regs || a->uop->num_src_regs == 0)
    return FALSE;
  for (uns i = 0; i < a->uop->num_src_regs && i < MAX_SRCS; i++)
    if (a->uop->srcs[i].id != b->uop->srcs[i].id)
      return FALSE;
  return TRUE;
}

/* Does `tail` read a register that `head` writes? */
static Flag readsDestOf(const Op* tail, const Op* head) {
  for (uns s = 0; s < tail->uop->num_src_regs && s < MAX_SRCS; s++)
    for (uns d = 0; d < head->uop->num_dest_regs && d < MAX_DESTS; d++)
      if (tail->uop->srcs[s].id == head->uop->dests[d].id)
        return TRUE;
  return FALSE;
}

/**************************************************************************************/
/* Head table */

static LoadHeadTableEntry* findHead(Counter op_num) {
  for (int i = 0; i < LOAD_HEAD_TABLE_SIZE; i++)
    if (loadHeadTable[i].valid && loadHeadTable[i].op_num == op_num)
      return &loadHeadTable[i];
  return NULL;
}

/* Make an unfused load available as a head. Replaces an invalid entry, else
 * the oldest. */
static void addLoadHead(Op* op) {
  if (findHead(op->op_num))
    return;
  int slot = 0;
  for (int i = 0; i < LOAD_HEAD_TABLE_SIZE; i++) {
    if (!loadHeadTable[i].valid) {
      slot = i;
      break;
    }
    if (loadHeadTable[i].op_num < loadHeadTable[slot].op_num)
      slot = i;
  }
  LoadHeadTableEntry* e = &loadHeadTable[slot];
  e->op_num = op->op_num;
  e->va = op->oracle_info.va;
  e->size = op->oracle_info.mem_size;
  e->valid = TRUE;
  e->used = FALSE;
  e->op = op;
}

/**************************************************************************************/
/* Deadlock and serializing checks (paper Sec. IV-B2). Rename-time checks in the
 * paper; here they replay the fetch-order register history. */

static void trackRegWrites(const Op* op) {
  Flag serializing = op->uop->bar_type != NOT_BAR;
  Mem_Type mem = op->uop->mem_type;
  if (op->uop->num_dest_regs == 0 && !serializing && mem == NOT_MEM)
    return;
  RegTrackEntry* e = &regTrackTable[regTrackTableHead];
  e->op_num = op->op_num;
  e->isLoad = mem == MEM_LD;
  e->isStore = mem == MEM_ST;
  e->va = op->oracle_info.va;
  e->size = op->oracle_info.mem_size;
  e->numDestRegs = MIN2(op->uop->num_dest_regs, MAX_FUSION_REGS);
  e->numSrcRegs = MIN2(op->uop->num_src_regs, MAX_FUSION_REGS);
  e->isSerializing = serializing;
  for (int i = 0; i < e->numDestRegs; i++)
    e->destRegs[i] = op->uop->dests[i].id;
  for (int i = 0; i < e->numSrcRegs; i++)
    e->srcRegs[i] = op->uop->srcs[i].id;
  e->valid = TRUE;
  regTrackTableHead = (regTrackTableHead + 1) % REG_TRACK_TABLE_SIZE;
}

/* The paper's Deadlock_Tag: a deadlock exists if the tail reads, directly or
 * through the catalyst, a register the head writes. The tag starts on the
 * head's destinations, follows each catalyst op from sources to destinations,
 * and is cleared by an untainted write, as a RAT write would.
 *
 * Scarab also makes a load wait for an older store to the same bytes (oracle
 * store dependences), so the tag also follows memory: a tainted catalyst store
 * taints its bytes, and a catalyst load that reads them is tainted. Without
 * this, head -> store -> load -> tail source is a deadlock the paper's
 * register-only tag misses. */
#define MAX_TAINTED_STORES 64
bool checkDeadlock(const Op* tail, Counter head_op_num) {
  bool taint[NUM_REG_IDS];
  int catalyst[REG_TRACK_TABLE_SIZE];
  int numCatalyst = 0;
  Addr storeVa[MAX_TAINTED_STORES];
  uns8 storeSize[MAX_TAINTED_STORES];
  int numStores = 0;
  memset(taint, 0, sizeof(taint));

  for (int i = 0; i < REG_TRACK_TABLE_SIZE; i++) {
    RegTrackEntry* e = &regTrackTable[i];
    if (!e->valid)
      continue;
    if (e->op_num == head_op_num) {
      for (int d = 0; d < e->numDestRegs; d++)
        if (e->destRegs[d] < NUM_REG_IDS)
          taint[e->destRegs[d]] = true;
    } else if (e->op_num > head_op_num && e->op_num < tail->op_num) {
      catalyst[numCatalyst++] = i;
    }
  }

  // Replay the catalyst in program order.
  for (int a = 1; a < numCatalyst; a++) {
    int key = catalyst[a];
    int b = a - 1;
    while (b >= 0 && regTrackTable[catalyst[b]].op_num > regTrackTable[key].op_num) {
      catalyst[b + 1] = catalyst[b];
      b--;
    }
    catalyst[b + 1] = key;
  }
  for (int c = 0; c < numCatalyst; c++) {
    RegTrackEntry* e = &regTrackTable[catalyst[c]];
    bool tainted = false;
    for (int s = 0; s < e->numSrcRegs && !tainted; s++)
      tainted = e->srcRegs[s] < NUM_REG_IDS && taint[e->srcRegs[s]];
    for (int t = 0; t < numStores && e->isLoad && !tainted; t++)
      tainted = e->va < storeVa[t] + storeSize[t] && storeVa[t] < e->va + e->size;
    if (e->isStore && tainted) {
      if (numStores == MAX_TAINTED_STORES)
        return true;  // too many to track: assume the worst
      storeVa[numStores] = e->va;
      storeSize[numStores++] = e->size;
    }
    for (int d = 0; d < e->numDestRegs; d++)
      if (e->destRegs[d] < NUM_REG_IDS)
        taint[e->destRegs[d]] = tainted;
  }

  for (uns s = 0; s < tail->uop->num_src_regs && s < MAX_SRCS; s++) {
    uint16_t r = tail->uop->srcs[s].id;
    if (r < NUM_REG_IDS && taint[r])
      return true;
  }
  return false;
}

static bool serializingInCatalyst(Counter head_op_num, Counter tail_op_num) {
  for (int i = 0; i < REG_TRACK_TABLE_SIZE; i++) {
    RegTrackEntry* e = &regTrackTable[i];
    if (e->valid && e->isSerializing && e->op_num > head_op_num && e->op_num < tail_op_num)
      return true;
  }
  return false;
}

/**************************************************************************************/
/* Fusion intervals: the nesting cap (paper Sec. IV-B2) and recovery. Every pair
 * that holds its head is recorded, so a recovery that squashes the tail can
 * free the surviving head. Consecutive pairs do not count toward the cap: their
 * tail leaves the pipeline before rename. */

static int countOverlappingFusions(Counter head_op_num, Counter tail_op_num) {
  int count = 0;
  for (int i = 0; i < FUSION_RING_SIZE; i++) {
    FusionIntervalEntry* e = &activeFusions[i];
    if (e->valid && !e->csf && e->head_op_num < tail_op_num && head_op_num < e->tail_op_num)
      count++;
  }
  return count;
}

static void recordFusion(Op* head, Counter tail_op_num, Flag csf) {
  FusionIntervalEntry* e = &activeFusions[activeFusionsHead];
  ASSERTM(0, !e->valid, "FUSION_RING_SIZE too small\n");
  e->head_op_num = head->op_num;
  e->tail_op_num = tail_op_num;
  e->head = head;
  e->head_unique_num = head->unique_num;
  e->csf = csf;
  e->valid = TRUE;
  activeFusionsHead = (activeFusionsHead + 1) % FUSION_RING_SIZE;
}

static void forgetFusion(Counter head_op_num, Counter tail_op_num) {
  for (int i = 0; i < FUSION_RING_SIZE; i++) {
    FusionIntervalEntry* e = &activeFusions[i];
    if (e->valid && e->head_op_num == head_op_num && e->tail_op_num == tail_op_num)
      e->valid = FALSE;
  }
}

/**************************************************************************************/
/* The head's wait for its tail (paper NCS_Ready and RaW handling) */

/* In the RS and not yet selected. A held head can reach OS_READY without
 * joining the ready list (simple_wake sets the state; cmp_wake skips the list). */
static inline Flag waitingInRS(const Op* op) {
  return op->state >= OS_IN_RS && op->state <= OS_READY && !op->in_rdy_list;
}

/* Push a released head onto the ready list if nothing else will. */
static void wakeIfReady(Op* head) {
  if (head->helios_head_pending_tail || !waitingInRS(head) || !op_sources_not_rdy_is_clear(head) ||
      cycle_count < op_get_dispatch_cycle(head))
    return;
  head->state = cycle_count + 1 >= op_get_rdy_cycle(head) ? OS_READY : OS_WAIT_FWD;
  issue_queue_wakeup(head);
}

static void releaseHeldHead(Op* head) {
  if (!head->helios_head_pending_tail)
    return;
  head->helios_head_pending_tail = FALSE;
  wakeIfReady(head);
}

/* Stop the head waiting on its tail's sources. Used when the pair dissolves
 * and by the liveness guard. */
static void dropTailSrcs(Op* head) {
  for (uns i = 0; i < head->helios_num_tail_src_bits; i++) {
    uns bit = head->helios_tail_src_bits[i];
    if (op_sources_test_not_rdy(head, bit))
      op_sources_clear_not_rdy(head, bit);
  }
  head->helios_num_tail_src_bits = 0;
  wakeIfReady(head);
}

/* The tail has renamed: the head now also waits for the tail's address
 * registers, then may issue. */
static void waitForTailSrcs(Op* tail, Op* head) {
  if (!head->helios_head_pending_tail)
    return;
  head->helios_head_pending_tail = FALSE;
  ASSERT(head->proc_id, !head->in_rdy_list && head->state <= OS_READY);  // the hold kept it in the RS
  for (uns i = 0; i < tail->num_srcs; i++) {
    if (tail->src_info[i].type != REG_DATA_DEP)
      continue;
    Op* producer = tail->src_info[i].op;
    if (!producer || !producer->op_pool_valid || producer->unique_num != tail->src_info[i].unique_num)
      continue;
    ASSERT(head->proc_id, producer != head);  // checkDeadlock rules this out
    uns bit = helios_wire_fused_reg_src(head, producer);
    if (bit == MAX_UNS)
      continue;
    ASSERT(head->proc_id, head->helios_num_tail_src_bits < HELIOS_MAX_TAIL_SRCS);
    head->helios_tail_src_bits[head->helios_num_tail_src_bits++] = bit;
    STAT_EVENT(head->proc_id, HELIOS_TAIL_SRCS_WAITED);
    if (producer->op_num > head->op_num)
      STAT_EVENT(head->proc_id, HELIOS_TAIL_SRCS_FROM_CATALYST);
  }
  STAT_EVENT(head->proc_id, HELIOS_HEAD_HELD_FOR_TAIL);
  // New sources pending: cmp_wake lists the head when they arrive, but only from OS_IN_RS.
  if (waitingInRS(head) && !op_sources_not_rdy_is_clear(head))
    head->state = OS_IN_RS;
  wakeIfReady(head);
}

/* Called each cycle the oldest op cannot retire. A fused head's wait for its
 * tail can stall forever in Scarab: the hold, if the tail cannot rename, and
 * the tail-source wait, if a dependence cycle the deadlock check misses (for
 * example across two nested pairs) or a full RS starves a catalyst producer.
 * Release the hold at once. Drop the tail-source wait after
 * HELIOS_STUCK_HEAD_CYCLES, longer than any memory latency. Both are counted. */
void heliosUnblockRobHead(Op* op) {
  if (op->helios_head_pending_tail) {
    STAT_EVENT(op->proc_id, HELIOS_FUSED_HEAD_FORCE_RELEASED);
    releaseHeldHead(op);
    return;
  }
  Flag waiting = FALSE;
  for (uns i = 0; i < op->helios_num_tail_src_bits && !waiting; i++)
    waiting = op_sources_test_not_rdy(op, op->helios_tail_src_bits[i]);
  if (!waiting) {
    op->helios_stuck_since = 0;
    return;
  }
  if (!op->helios_stuck_since)
    op->helios_stuck_since = cycle_count;
  else if (cycle_count - op->helios_stuck_since > HELIOS_STUCK_HEAD_CYCLES) {
    STAT_EVENT(op->proc_id, HELIOS_FUSED_HEAD_FORCE_RELEASED);
    dropTailSrcs(op);
  }
}

/**************************************************************************************/
/* Forming and cancelling pairs */

/* Pair `tail` with the head in `entry`. A fused pair and a pair to be unfused
 * at rename both hold the head until the tail renames. */
static void pairWithHead(Op* tail, LoadHeadTableEntry* entry, Flag fused, Flag csf) {
  Op* head = entry->op;
  tail->helios_head = head;
  tail->helios_partner_op_num = entry->op_num;
  tail->helios_fused = fused;
  tail->helios_unfuse_at_rename = !fused;
  tail->helios_csf = csf;
  tail->helios_two_lines = !isSameCacheBlock(tail->oracle_info.va, entry->va, tail->oracle_info.mem_size, entry->size);
  entry->used = TRUE;
  head->helios_head_used = TRUE;
  head->helios_head_pending_tail = TRUE;
  if (fused)
    head->helios_group_last_op_num = tail->op_num;
  recordFusion(head, tail->op_num, csf);
}

/* Undo pairWithHead. The tail becomes an ordinary load. */
static void unpair(Op* tail) {
  Op* head = tail->helios_head;
  LoadHeadTableEntry* entry = findHead(tail->helios_partner_op_num);
  if (entry)
    entry->used = FALSE;
  if (opLive(head, tail->helios_partner_op_num)) {
    head->helios_head_used = FALSE;
    head->helios_group_last_op_num = 0;
    releaseHeldHead(head);
  }
  forgetFusion(tail->helios_partner_op_num, tail->op_num);
  tail->helios_fused = FALSE;
  tail->helios_unfuse_at_rename = FALSE;
  tail->helios_csf = FALSE;
  tail->helios_two_lines = FALSE;
  tail->helios_head = NULL;
}

/* Consecutive fusion (paper Sec. II and V-A, CSF-SBR): a load directly after
 * an unfused load with the same address registers and a contiguous access. In
 * the paper this is decoded statically; it needs no prediction. */
static Flag tryConsecutiveFusion(Op* tail) {
  if (tail->op_num == 0)
    return FALSE;
  LoadHeadTableEntry* entry = findHead(tail->op_num - 1);
  if (!entry || entry->used || !opLive(entry->op, entry->op_num))
    return FALSE;
  Op* head = entry->op;
  if (!sameSrcRegs(tail, head) || readsDestOf(tail, head))
    return FALSE;
  if (!isContiguous(tail->oracle_info.va, entry->va, tail->oracle_info.mem_size, entry->size))
    return FALSE;
  pairWithHead(tail, entry, TRUE, TRUE);
  return TRUE;
}

/* Paper Sec. IV-A2 fusion conditions and Sec. IV-C repair cases. */
static void validatePrediction(Op* tail) {
  Counter distance = tail->helios_pred_distance;
  LoadHeadTableEntry* entry = distance <= tail->op_num ? findHead(tail->op_num - distance) : NULL;
  if (!entry || !opLive(entry->op, entry->op_num)) {
    // Not an unfused in-flight load: retired, already a tail, or not a load.
    STAT_EVENT(tail->proc_id, HELIOS_REJECT_HEAD_NOT_FOUND);
    return;
  }
  if (entry->used) {
    STAT_EVENT(tail->proc_id, HELIOS_REJECT_HEAD_USED);
    return;
  }
  if (countOverlappingFusions(entry->op_num, tail->op_num) >= MAX_NCSF_NEST) {
    // Paper: the head enters rename with Max Active NCS saturated and stays unfused.
    STAT_EVENT(tail->proc_id, HELIOS_REJECT_NEST_LIMIT);
    return;
  }
  // Repair cases 2 and 4: found when the tail renames; the head waits until then.
  if (checkDeadlock(tail, entry->op_num)) {
    STAT_EVENT(tail->proc_id, HELIOS_UNFUSE_DEADLOCK);
    pairWithHead(tail, entry, FALSE, FALSE);
    return;
  }
  if (serializingInCatalyst(entry->op_num, tail->op_num)) {
    STAT_EVENT(tail->proc_id, HELIOS_UNFUSE_SERIALIZING);
    pairWithHead(tail, entry, FALSE, FALSE);
    return;
  }
  // Repair case 5: the pair does not fit one region. Flush at the tail's execute.
  if (!canFuse(tail->oracle_info.va, entry->va, tail->oracle_info.mem_size, entry->size)) {
    STAT_EVENT(tail->proc_id, HELIOS_REJECT_ADDR_MISMATCH);
    tail->helios_addr_mispredict = TRUE;
    tail->helios_flush_pending = TRUE;
    tail->helios_head = entry->op;
    tail->helios_partner_op_num = entry->op_num;
    tail->bp_pred_info->recovery_point = RECOVER_AT_EXEC;
    return;
  }
  pairWithHead(tail, entry, TRUE, FALSE);
}

static void predictHead(Op* op) {
  bool localPredicts = false;
  bool globalPredicts = false;
  uint16_t distance =
      predictFusion(op->inst->addr, op->bp_pred_info->pred_global_hist, &localPredicts, &globalPredicts);
  op->helios_local_pred = localPredicts;
  op->helios_global_pred = globalPredicts;
  if (localPredicts && !globalPredicts)
    STAT_EVENT(op->proc_id, HELIOS_SELECTOR_LOCAL_WINS);
  else if (globalPredicts && !localPredicts)
    STAT_EVENT(op->proc_id, HELIOS_SELECTOR_GLOBAL_WINS);
  if (distance > HELIOS_FUSION_WINDOW)
    STAT_EVENT(op->proc_id, HELIOS_FP_PREDICT_OUT_OF_WINDOW);
  if (distance > 0 && distance <= HELIOS_FUSION_WINDOW) {
    op->helios_candidate = TRUE;
    op->helios_pred_distance = distance;
  }
}

static void resetFields(Op* op) {
  op->helios_candidate = FALSE;
  op->helios_local_pred = FALSE;
  op->helios_global_pred = FALSE;
  op->helios_pred_distance = 0;
  op->helios_fused = FALSE;
  op->helios_csf = FALSE;
  op->helios_two_lines = FALSE;
  op->helios_unfuse_at_rename = FALSE;
  op->helios_addr_mispredict = FALSE;
  op->helios_flush_pending = FALSE;
  op->helios_partner_op_num = 0;
  op->helios_head = NULL;
  op->helios_head_used = FALSE;
  op->helios_head_pending_tail = FALSE;
  op->helios_group_last_op_num = 0;
  op->helios_tail = NULL;
  op->helios_tail_unique_num = 0;
  op->helios_tail_done = FALSE;
  op->helios_num_tail_src_bits = 0;
  op->helios_stuck_since = 0;
  op->helios_macro_next_fetch_addr = 0;  // set when ft.cc takes a flush snapshot
}

void heliosFetchHook(Op* op) {
  if (op->bom)
    macroLastOpNum[op->proc_id] = op->op_num + op->inst->num_uop - 1;
  op->helios_macro_last_op_num = macroLastOpNum[op->proc_id];
  resetFields(op);

  if (isLoad(op)) {
    STAT_EVENT(op->proc_id, HELIOS_ONPATH_LOADS);
    if (!tryConsecutiveFusion(op)) {
      predictHead(op);
      if (op->helios_candidate) {
        STAT_EVENT(op->proc_id, HELIOS_FUSION_CANDIDATES);
        validatePrediction(op);
      }
    }
    // Any unfused load may head a later pair, including a rejected candidate.
    if (!op->helios_fused && !op->helios_unfuse_at_rename && !op->helios_flush_pending)
      addLoadHead(op);
  }
  trackRegWrites(op);
}

/**************************************************************************************/
/* Allocation queue */

static void countFusion(const Op* tail) {
  uns8 proc = tail->proc_id;
  const Op* head = tail->helios_head;
  Counter distance = tail->op_num - tail->helios_partner_op_num;
  STAT_EVENT(proc, HELIOS_FUSIONS);
  STAT_EVENT(proc, tail->helios_csf ? HELIOS_FUSED_CSF : HELIOS_FUSED_NCSF);
  INC_STAT_EVENT(proc, HELIOS_FUSED_DIST_SUM, distance);
  if (isContiguous(tail->oracle_info.va, head->oracle_info.va, tail->oracle_info.mem_size, head->oracle_info.mem_size))
    STAT_EVENT(proc, HELIOS_FUSED_CONTIGUOUS);
  else if (!tail->helios_two_lines)
    STAT_EVENT(proc, HELIOS_FUSED_SAME_LINE);
  if (tail->helios_two_lines)
    STAT_EVENT(proc, HELIOS_FUSED_TWO_LINES);
  STAT_EVENT(proc, sameSrcRegs(tail, head) ? HELIOS_FUSED_SAME_SRC_REGS : HELIOS_FUSED_DIFF_SRC_REGS);
}

/* The tail enters the allocation queue (IDQ). Paper Sec. IV-A2 condition 3:
 * fusion succeeds only if the head is still in the queue or in the tail's
 * decode group, that is, not yet renamed. */
void heliosAllocQueueHook(Op* op) {
  if (op->off_path)
    return;
  Op* head = op->helios_head;
  Flag paired = op->helios_fused || op->helios_unfuse_at_rename;
  if (!paired && !op->helios_flush_pending)
    return;
  Flag headInQueue = opLive(head, op->helios_partner_op_num) && op_get_map_cycle(head) == MAX_CTR;
  if (headInQueue) {
    if (op->helios_fused)
      countFusion(op);
    return;
  }

  STAT_EVENT(op->proc_id, HELIOS_REJECT_HEAD_LEFT_QUEUE);
  addLoadHead(op);
  if (paired) {
    unpair(op);
  } else {
    // A pair that never fused cannot be found wrong: no flush.
    op->helios_flush_pending = FALSE;
    op->helios_addr_mispredict = FALSE;
    op->helios_head = NULL;
    op->bp_pred_info->recovery_point = RECOVER_AT_NONE;
    if (BP_MECH == TAGE64K_BP && op->helios_macro_next_fetch_addr)
      bp_helios_drop_checkpoint_TAGE64K(op->proc_id, 0, op);  // main predictor
  }
}

/**************************************************************************************/
/* Rename and data delivery */

/* Deliver the tail's register. `ready` is when the head's access returns. */
static void completeTail(Op* tail, Counter ready) {
  if (tail->helios_two_lines)
    ready++;  // second of two serialized accesses (paper Sec. II-B, cacheline crossers)
  op_set_dcache_cycle(tail, cycle_count);
  op_set_done_cycle(tail, ready);
  op_set_wake_cycle(tail, ready);
  // The tail never executes; stamp what retire expects and balance the
  // source-register accounting it took at rename.
  op_set_sched_cycle(tail, cycle_count);
  op_set_exec_cycle(tail, cycle_count);
  tail->state = OS_SCHEDULED;
  tail->helios_tail_done = TRUE;
  reg_file_consume(tail);
  wake_up_ops(tail, REG_DATA_DEP, model->wake_hook);
}

void heliosRenameHook(Op* tail) {
  if (!tail->helios_fused && !tail->helios_unfuse_at_rename)
    return;
  Op* head = tail->helios_head;
  Flag headLive = opLive(head, tail->helios_partner_op_num) && head->proc_id == tail->proc_id;

  if (tail->helios_unfuse_at_rename) {
    // Repair cases 2 and 4: the head was held for nothing and issues unfused.
    if (headLive)
      releaseHeldHead(head);
    return;
  }

  if (headLive)
    waitForTailSrcs(tail, head);

  if (headLive && !head->wake_up_signaled[REG_DATA_DEP]) {
    // The head completes this tail when its access returns.
    head->helios_tail = tail;
    head->helios_tail_unique_num = tail->unique_num;
    return;
  }
  // The head already produced, or retired: its data is in the cache now.
  STAT_EVENT(tail->proc_id, HELIOS_TAIL_IMMEDIATE_COMPLETE);
  completeTail(tail, headLive ? MAX2(cycle_count, op_get_done_cycle(head)) : cycle_count);
}

uns heliosDispatchSlots(const Op* op) {
  // Repair cases 2-4: the tail writes the head's IQ entry and takes its own.
  return HELIOS_DO_FUSION && op->helios_unfuse_at_rename ? 2 : 1;
}

void heliosCompleteFusedTail(Op* head) {
  Op* tail = head->helios_tail;
  if (!tail)
    return;
  head->helios_tail = NULL;
  if (!tail->op_pool_valid || tail->unique_num != head->helios_tail_unique_num || tail->helios_tail_done)
    return;
  STAT_EVENT(head->proc_id, HELIOS_HEAD_DRIVEN_TAIL_COMPLETIONS);
  completeTail(tail, op_get_done_cycle(head));
}

/**************************************************************************************/
/* Retire */

/* Paper Sec. IV-B3: a fused head retires only when its catalyst and tail are
 * all ready, so a fault or mispredict in the catalyst can still unfuse it. */
Flag heliosExtCommitGroupBlocks(Op* head) {
  if (!head->helios_head_used || head->helios_group_last_op_num <= head->op_num)
    return FALSE;
  Flag blocked = FALSE;
  for (Op* o = head->next_node; o; o = o->next_node) {
    // A nested head still held cannot become ready while this group blocks retire.
    if (o->helios_head_pending_tail)
      heliosUnblockRobHead(o);
    if (!(o->state == OS_DONE || OP_DONE(o)) || o->recovery_scheduled || o->redirect_scheduled)
      blocked = TRUE;
    if (o->op_num >= head->helios_group_last_op_num)
      return blocked;
  }
  return TRUE;  // the tail is not in the window yet
}

/* Paper Sec. IV-A1: unfused loads search the UCH at commit, and a same-line
 * match trains the predictor for this PC. */
void heliosCommit(Op* op) {
  if (!isLoad(op))
    return;
  if (op->helios_candidate && op->helios_addr_mispredict) {
    STAT_EVENT(op->proc_id, HELIOS_FUSION_MISPREDICT);
    updatePredictor(op->inst->addr, op->bp_pred_info->pred_global_hist);
  }
  Flag predictedFused = op->helios_fused && !op->helios_csf;
  if (op->helios_candidate && (predictedFused || op->helios_addr_mispredict))
    updateSelector(op->inst->addr, op->bp_pred_info->pred_global_hist, op->helios_local_pred, op->helios_global_pred,
                   predictedFused);

  if (op->helios_fused || op->helios_unfuse_at_rename)
    forgetFusion(op->helios_partner_op_num, op->op_num);
  if (op->helios_fused) {
    Counter distance = op->op_num - op->helios_partner_op_num;
    STAT_EVENT(op->proc_id, HELIOS_FUSIONS_COMMITTED);
    STAT_EVENT(op->proc_id, HELIOS_COMMIT_DIST_1 + distBucket(distance));
    INC_STAT_EVENT(op->proc_id, HELIOS_COMMIT_DIST_SUM, distance);
    return;
  }
  if (op->helios_head_used)
    return;  // the UCH holds only unfused loads

  uint64_t headPC = 0;
  uint16_t distance =
      updateLoadUCH(op->inst->addr, op->oracle_info.va, (uint8_t)op->oracle_info.mem_size, op->op_num, &headPC);
  if (distance == 0)
    return;
  addInstructionToPredictor(op->inst->addr, op->bp_pred_info->pred_global_hist, distance);
  // A pair Helios missed.
  STAT_EVENT(op->proc_id, HELIOS_TRAIN_DIST_1 + distBucket(distance));
  INC_STAT_EVENT(op->proc_id, HELIOS_TRAIN_DIST_SUM, distance);
  STAT_EVENT(op->proc_id, op->helios_candidate ? HELIOS_MISSED_PAIR_REJECTED : HELIOS_MISSED_PAIR_NO_PREDICTION);
}

/**************************************************************************************/
/* Recovery */

/* The tail of a pair was squashed but its head survives: the pair dissolves
 * and the head may pair again. */
static void reopenSurvivingHead(const FusionIntervalEntry* e) {
  LoadHeadTableEntry* entry = findHead(e->head_op_num);
  if (entry)
    entry->used = FALSE;
  Op* head = e->head;
  if (!opLive(head, e->head_op_num) || head->unique_num != e->head_unique_num)
    return;
  head->helios_head_used = FALSE;
  head->helios_group_last_op_num = 0;
  head->helios_tail = NULL;
  releaseHeldHead(head);
  dropTailSrcs(head);
}

void flushTables(Counter op_num) {
  for (int i = 0; i < LOAD_HEAD_TABLE_SIZE; i++)
    if (loadHeadTable[i].valid && loadHeadTable[i].op_num > op_num)
      loadHeadTable[i].valid = FALSE;
  for (int i = 0; i < REG_TRACK_TABLE_SIZE; i++)
    if (regTrackTable[i].valid && regTrackTable[i].op_num > op_num)
      regTrackTable[i].valid = FALSE;
  for (int i = 0; i < FUSION_RING_SIZE; i++) {
    FusionIntervalEntry* e = &activeFusions[i];
    if (e->valid && e->tail_op_num > op_num) {
      if (e->head_op_num <= op_num)
        reopenSurvivingHead(e);
      e->valid = FALSE;
    }
  }
}
