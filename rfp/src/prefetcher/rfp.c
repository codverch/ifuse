/* Register File Prefetch (RFP): prediction table (PT) and page address table (PAT)
 *
 * The PT is a PC-indexed stride predictor trained at retire. To keep entries
 * small, an entry stores only the page offset of the last address and a
 * pointer into the PAT, which holds page frames shared by many entries.
 *
 * Logic is unchanged from src/prefetcher/rfp.c on Scarab branch hpca2027-rfp.
 * Callers are listed in rfp.h. */

#include "prefetcher/rfp.h"
#include "prefetcher/rfp_prf.h"

#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"
#include "globals/global_vars.h"
#include "statistics.h"

#include "cmp_model.h"
#include "core.param.h"
#include "dcache_stage.h"
#include "freq.h"
#include "lsq.h"
#include "memory/memory.h"
#include "memory/memory.param.h"
#include "model.h"

static RFP_PT_Entry rfp_pt[RFP_PT_MAX_SETS][RFP_PT_MAX_WAYS];
static RFP_Pat_Entry rfp_pat[RFP_PAT_MAX_SETS][RFP_PAT_MAX_WAYS];
static uns rfp_set_index_mask;
static uns rfp_pc_tag_shift;

static inline void rfp_power_pt_read(uns8 proc_id) {
  if (RFP_ON)
    STAT_EVENT(proc_id, POWER_RFP_PT_READ);
}

static inline void rfp_power_pt_write(uns8 proc_id) {
  if (RFP_ON)
    STAT_EVENT(proc_id, POWER_RFP_PT_WRITE);
}

static inline void rfp_power_pat_read(uns8 proc_id) {
  if (RFP_ON)
    STAT_EVENT(proc_id, POWER_RFP_PAT_READ);
}

static inline void rfp_power_pat_write(uns8 proc_id) {
  if (RFP_ON)
    STAT_EVENT(proc_id, POWER_RFP_PAT_WRITE);
}
#define RFP_INFLIGHT_MAX 127

static inline uns rfp_log2_u(uns v) {
  uns shift = 0;
  while (v >>= 1)
    shift++;
  return shift;
}

static inline uns rfp_pat_num_entries(void) {
  return RFP_PAT_NUM_SETS * RFP_PAT_NUM_WAYS;
}

static inline int64 rfp_stride_min(void) {
  if (RFP_STRIDE_SIGNED)
    return -(1LL << (RFP_STRIDE_BITS - 1));
  return 0;
}

static inline int64 rfp_stride_max(void) {
  if (RFP_STRIDE_SIGNED)
    return (1LL << (RFP_STRIDE_BITS - 1)) - 1;
  return (1LL << RFP_STRIDE_BITS) - 1;
}

static inline Flag rfp_stride_in_range(int64 stride) {
  return stride >= rfp_stride_min() && stride <= rfp_stride_max();
}

static inline Addr rfp_va_pfn(Addr va) {
  return (va >> RFP_PAGE_OFFSET_BITS) & RFP_PFN_MASK;
}

static inline uns16 rfp_va_page_offset(Addr va) {
  return (uns16)(va & RFP_PAGE_OFFSET_MASK);
}

static inline uns rfp_pat_set(Addr pfn) {
  return (uns)(pfn & (RFP_PAT_NUM_SETS - 1));
}

static inline uns8 rfp_pat_ptr_encode(uns set, uns way) {
  ASSERT(0, set < RFP_PAT_NUM_SETS && way < RFP_PAT_NUM_WAYS);
  return (uns8)(set * RFP_PAT_NUM_WAYS + way);
}

static inline void rfp_pat_ptr_decode(uns8 pat_ptr, uns* set, uns* way) {
  ASSERT(0, pat_ptr < rfp_pat_num_entries());
  *set = pat_ptr / RFP_PAT_NUM_WAYS;
  *way = pat_ptr % RFP_PAT_NUM_WAYS;
}

static inline Addr rfp_entry_va(uns8 proc_id, const RFP_PT_Entry* entry) {
  uns set;
  uns way;

  rfp_pat_ptr_decode(entry->pat_ptr, &set, &way);
  rfp_power_pat_read(proc_id);
  ASSERT(0, rfp_pat[set][way].valid);
  return (rfp_pat[set][way].pfn << RFP_PAGE_OFFSET_BITS) | entry->page_offset;
}

static int rfp_pat_lookup_ptr(uns8 proc_id, Addr pfn) {
  uns set = rfp_pat_set(pfn);

  rfp_power_pat_read(proc_id);
  for (uns way = 0; way < RFP_PAT_NUM_WAYS; way++) {
    if (rfp_pat[set][way].valid && rfp_pat[set][way].pfn == pfn)
      return (int)rfp_pat_ptr_encode(set, way);
  }
  return -1;
}

/* Eviction does not touch PT entries that point to the victim, so they
 * rebuild their address with the new frame. The address check in
 * dcache_stage.c catches the wrong predictions this causes. */
static int rfp_pat_alloc_ptr(uns8 proc_id, Addr pfn) {
  int ptr = rfp_pat_lookup_ptr(proc_id, pfn);
  if (ptr >= 0)
    return ptr;

  uns set = rfp_pat_set(pfn);

  for (uns way = 0; way < RFP_PAT_NUM_WAYS; way++) {
    if (!rfp_pat[set][way].valid) {
      rfp_pat[set][way].valid = TRUE;
      rfp_pat[set][way].pfn = pfn;
      rfp_power_pat_write(proc_id);
      STAT_EVENT(proc_id, RFP_PAT_ALLOC);
      return (int)rfp_pat_ptr_encode(set, way);
    }
  }

  /* full set: evict way 0, a simplification of a real replacement policy */
  STAT_EVENT(proc_id, RFP_PAT_EVICT);
  rfp_pat[set][0].valid = TRUE;
  rfp_pat[set][0].pfn = pfn;
  rfp_power_pat_write(proc_id);
  return (int)rfp_pat_ptr_encode(set, 0);
}

static void rfp_entry_set_va(uns8 proc_id, RFP_PT_Entry* entry, Addr va) {
  int pat_ptr = rfp_pat_alloc_ptr(proc_id, rfp_va_pfn(va));

  ASSERT(proc_id, pat_ptr >= 0 && pat_ptr < (int)rfp_pat_num_entries());
  entry->pat_ptr = (uns8)pat_ptr;
  entry->page_offset = rfp_va_page_offset(va);
}

/* Called from memory.c:init_memory. pat_ptr is 8 bits, so the PAT holds at
 * most 256 entries. */
void rfp_init(void) {
  if (!RFP_ON)
    return;

  ASSERT(0, RFP_PT_NUM_SETS > 0 && RFP_PT_NUM_WAYS > 0);
  ASSERT(0, RFP_PAT_NUM_SETS > 0 && RFP_PAT_NUM_WAYS > 0);
  ASSERT(0, RFP_PT_NUM_SETS <= RFP_PT_MAX_SETS && RFP_PT_NUM_WAYS <= RFP_PT_MAX_WAYS);
  ASSERT(0, RFP_PAT_NUM_SETS <= RFP_PAT_MAX_SETS && RFP_PAT_NUM_WAYS <= RFP_PAT_MAX_WAYS);
  ASSERT(0, (RFP_PT_NUM_SETS & (RFP_PT_NUM_SETS - 1)) == 0);
  ASSERT(0, (RFP_PAT_NUM_SETS & (RFP_PAT_NUM_SETS - 1)) == 0);
  ASSERT(0, rfp_pat_num_entries() <= 256);
  ASSERT(0, RFP_CONF_MAX > 0 && RFP_CONF_MAX <= RFP_CONF_COUNTER_MAX);
  ASSERT(0, RFP_STRIDE_BITS > 0 && RFP_STRIDE_BITS <= RFP_STRIDE_BITS_MAX);

  rfp_set_index_mask = RFP_PT_NUM_SETS - 1;
  rfp_pc_tag_shift = RFP_PC_SET_SHIFT + rfp_log2_u(RFP_PT_NUM_SETS);

  if (rfp_use_prf_model())
    rfp_prf_init();

  memset(rfp_pt, 0, sizeof(rfp_pt));
  memset(rfp_pat, 0, sizeof(rfp_pat));
}

static inline uns rfp_set(Addr pc) {
  Addr index = pc >> RFP_PC_SET_SHIFT;
  return (uns)(index & rfp_set_index_mask);
}

static inline uns16 rfp_tag(Addr pc) {
  Addr tag = pc >> rfp_pc_tag_shift;
  return (uns16)(tag & RFP_TAG_MASK);
}

static int rfp_lookup(uns8 proc_id, uns set, uns16 tag) {
  rfp_power_pt_read(proc_id);
  for (int way = 0; way < (int)RFP_PT_NUM_WAYS; way++)
    if (rfp_pt[set][way].valid && rfp_pt[set][way].tag == tag)
      return way;

  return -1;
}

static inline RFP_PT_Entry* rfp_get_entry(uns8 proc_id, Addr pc) {
  uns set = rfp_set(pc);
  uns16 tag = rfp_tag(pc);
  int way = rfp_lookup(proc_id, set, tag);
  if (way < 0)
    return NULL;
  return &rfp_pt[set][way];
}

/* called at rename for every on-path load, predicted or not */
static inline void rfp_inflight_inc(uns8 proc_id, Addr pc) {
  RFP_PT_Entry* entry = rfp_get_entry(proc_id, pc);
  if (!entry)
    return;
  if (entry->inflight < RFP_INFLIGHT_MAX) {
    entry->inflight++;
    rfp_power_pt_write(proc_id);
  }
}

/* called at retire and on recovery */
static inline void rfp_inflight_dec(uns8 proc_id, Addr pc) {
  RFP_PT_Entry* entry = rfp_get_entry(proc_id, pc);
  if (!entry)
    return;
  if (entry->inflight > 0) {
    entry->inflight--;
    rfp_power_pt_write(proc_id);
  }
}

/* an invalid way, else the lowest utility */
static int rfp_victim(uns set) {
  for (int way = 0; way < (int)RFP_PT_NUM_WAYS; way++)
    if (!rfp_pt[set][way].valid)
      return way;

  int victim = 0;
  for (int way = 1; way < (int)RFP_PT_NUM_WAYS; way++)
    if (rfp_pt[set][way].utility < rfp_pt[set][victim].utility)
      victim = way;
  return victim;
}

/* A confirmed stride raises confidence only with probability 1/2^RFP_PROB_SHIFT,
 * so a 1-bit counter still needs a stable stride many times before it predicts.
 * The draw uses libc rand(), which other Scarab code also calls (e.g.
 * libs/cache_lib.c), so results are repeatable but shift if those callers
 * change. A stride too wide for the field resets the entry. */
static void rfp_train_entry(uns8 proc_id, Addr pc, Addr va) {
  uns set = rfp_set(pc);
  uns16 tag = rfp_tag(pc);
  int way = rfp_lookup(proc_id, set, tag);

  if (way < 0) {
    way = rfp_victim(set);
    rfp_pt[set][way].tag = tag;
    rfp_entry_set_va(proc_id, &rfp_pt[set][way], va);
    rfp_pt[set][way].stride = 0;
    rfp_pt[set][way].confidence = 0;
    rfp_pt[set][way].utility = 0;
    rfp_pt[set][way].inflight = 0;
    rfp_pt[set][way].valid = TRUE;
    rfp_power_pt_write(proc_id);
    STAT_EVENT(proc_id, RFP_PT_ALLOC);
    return;
  }

  STAT_EVENT(proc_id, RFP_PT_HIT);

  RFP_PT_Entry *entry = &rfp_pt[set][way];
  int64 observed_stride = (int64)((int64)va - (int64)rfp_entry_va(proc_id, entry));

  if (!rfp_stride_in_range(observed_stride)) {
    STAT_EVENT(proc_id, RFP_STRIDE_OUT_OF_RANGE);
    entry->confidence = 0;
    entry->utility = 0;
    rfp_entry_set_va(proc_id, entry, va);
    rfp_power_pt_write(proc_id);
    return;
  }

  if (observed_stride == entry->stride) {
    STAT_EVENT(proc_id, RFP_STRIDE_CONFIRM);
    if (entry->confidence < RFP_CONF_MAX) {
      if (RFP_PROB_SHIFT == 0 || (rand() & ((1u << RFP_PROB_SHIFT) - 1)) == 0) {
        entry->confidence++;
        STAT_EVENT(proc_id, RFP_CONF_INC);
      }
    }
    if (entry->utility < 3)
      entry->utility++;
  } else {
    STAT_EVENT(proc_id, RFP_STRIDE_CHANGE);
    entry->stride = (int16)observed_stride;
    entry->confidence = 0;
    entry->utility = 0;
  }

  rfp_entry_set_va(proc_id, entry, va);
  rfp_power_pt_write(proc_id);
}

/* Older instances of this PC that are renamed but not retired have not
 * trained the PT yet, so skip ahead by inflight + 1 strides. */
Flag rfp_predict(uns8 proc_id, Addr pc, Addr *predicted_addr) {
  uns set = rfp_set(pc);
  uns16 tag = rfp_tag(pc);
  int way = rfp_lookup(proc_id, set, tag);

  if (way < 0 || rfp_pt[set][way].confidence < RFP_CONF_MAX)
    return FALSE;

  RFP_PT_Entry *entry = &rfp_pt[set][way];
  int64 lookahead = (int64)entry->inflight + 1;
  *predicted_addr =
      (Addr)((int64)rfp_entry_va(proc_id, entry) + (int64)entry->stride * lookahead);
  return TRUE;
}

/* called from dcache_stage.c:update_dcache_stage on an address mismatch */
void rfp_reset_confidence_on_mispredict(uns8 proc_id, Addr pc) {
  RFP_PT_Entry* entry;

  if (!RFP_ON)
    return;

  entry = rfp_get_entry(proc_id, pc);
  if (!entry || entry->confidence == 0)
    return;

  entry->confidence = 0;
  rfp_power_pt_write(proc_id);
  STAT_EVENT(proc_id, RFP_CONF_RESET_ON_MISPRED);
}

/* An older store may write the predicted bytes, so RFP waits rather than read
 * stale data. Checks the store queue (lsq.cc:LSQ_Unit::older_store_overlaps,
 * which also waits on stores with unknown addresses) and the post-retire store
 * buffer (memory.c:scan_stores). Called from
 * dcache_stage.c:dcache_process_lsq_rfp_queue. */
Flag rfp_blocked_by_older_stores(uns8 proc_id, Counter load_op_num, Addr pred_addr, uns load_size) {
  if (!RFP_ON || !pred_addr || !load_size)
    return FALSE;

  if (lsq_older_store_overlaps(proc_id, load_op_num, pred_addr, load_size))
    return TRUE;
  if (scan_stores(pred_addr, load_size))
    return TRUE;
  return FALSE;
}

/* Called from icache_stage.c:icache_process_ops. Scarab recycles Op structs,
 * so the RFP fields must be reset. Despite the name, prediction happens at
 * rename (rfp_prefetch_launch), where the lookahead can see the same inflight
 * count that rename increments. */
void rfp_predict_at_fetch(Op *op) {
  ASSERT(op->proc_id, op);

  if (!RFP_ON)
    return;

  op->rfp_predicted = FALSE;
  op->rfp_predicted_addr = 0;
  op->rfp_dropped_load_first = FALSE;
  op->rfp_served = FALSE;
  op->rfp_partial_mitigated = FALSE;
  op->rfp_pred_resolved = FALSE;
}

/* The request carries the load's prfid so the L1 read is credited to that
 * register. A full FIFO (lsq.cc:LSQ::allocate_rfp) drops the prefetch. */
static void rfp_prefetch_launch_prf(Op* op, Addr line_addr) {
  uns16 prfid;

  if (!rfp_op_load_prfid(op, &prfid)) {
    STAT_EVENT(op->proc_id, RFP_PRF_LAUNCH_NO_DEST);
    STAT_EVENT(op->proc_id, RFP_ELIGIBLE_NOT_ISSUED);
    return;
  }

  op->rfp_prfid = prfid;
  STAT_EVENT(op->proc_id, RFP_PRF_LAUNCH);

  if (lsq_rfp_enqueue(op->proc_id, op, line_addr, prfid, op->rfp_launch_cycle)) {
    STAT_EVENT(op->proc_id, RFP_PREFETCH_INJECTED);
    STAT_EVENT(op->proc_id, RFP_ISSUED);
  } else {
    STAT_EVENT(op->proc_id, RFP_ELIGIBLE_NOT_ISSUED);
  }
}

/* Called from map_stage.c:stage_process_op right after reg_file_rename, so
 * the destination prfid is known. */
void rfp_prefetch_launch(Op* op) {
  Addr line_addr;
  Counter launch_cycle;

  ASSERT(op->proc_id, op);

  if (!RFP_ON)
    return;

  if (op->off_path)
    return;
  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;

  STAT_EVENT(op->proc_id, RFP_ALL_LOADS);

  /* Look up before rfp_inflight_inc so the lookahead counts only older
     instances. Predicting at fetch missed instances still between fetch and
     rename, which shortened the lookahead in tight loops. */
  Addr predicted_addr = 0;
  if (rfp_predict(op->proc_id, op->inst_info->addr, &predicted_addr)) {
    op->rfp_predicted = TRUE;
    op->rfp_predicted_addr = predicted_addr;
    STAT_EVENT(op->proc_id, RFP_PREDICTION_MADE);
    STAT_EVENT(op->proc_id, RFP_ELIGIBLE);
  }

  rfp_inflight_inc(op->proc_id, op->inst_info->addr);

  if (!op->rfp_predicted)
    return;
  if (model->mem != MODEL_MEM)
    return;

  STAT_EVENT(op->proc_id, RFP_PREDICTED_AT_RENAME);

  line_addr = op->rfp_predicted_addr & ~(Addr)(DCACHE_LINE_SIZE - 1);
  launch_cycle = freq_cycle_count(FREQ_DOMAIN_CORES[op->proc_id]);
  op->rfp_launch_cycle = launch_cycle;
  rfp_prefetch_launch_prf(op, line_addr);
}

/* Called from node_stage.c:node_retire. Training at retire keeps wrong-path
 * addresses out of the PT. */
void rfp_train_retire(Op *op) {
  ASSERT(op->proc_id, op);

  if (!RFP_ON)
    return;

  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;
  rfp_inflight_dec(op->proc_id, op->inst_info->addr);
  if (!op->oracle_info.va)
    return;

  STAT_EVENT(op->proc_id, RFP_RETIRE_LOAD);
  rfp_train_entry(op->proc_id, op->inst_info->addr, op->oracle_info.va);

  /* coverage over committed loads only */
  if (op->rfp_predicted) {
    STAT_EVENT(op->proc_id, RFP_RETIRE_PRED);
    if (op->rfp_served) {
      STAT_EVENT(op->proc_id, RFP_RETIRE_COVERED);
      STAT_EVENT(op->proc_id, RFP_RETIRE_PRED_CORRECT);
    } else if (op->rfp_mispred_accounted) {
      STAT_EVENT(op->proc_id, RFP_RETIRE_PRED_WRONG);
    } else if (op->rfp_dropped_load_first) {
      /* right address, prefetch was late */
      STAT_EVENT(op->proc_id, RFP_RETIRE_PRED_CORRECT);
    }
  }
}

/* Called from thread.c:recover_seq_op_list, the only rollback path that
 * removes renamed on-path loads. */
void rfp_track_squash(Op* op) {
  ASSERT(op->proc_id, op);
  if (!RFP_ON)
    return;
  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;
  /* rfp_prefetch_launch increments only for on-path loads, so skip off-path
     ones here to keep the increments and decrements paired. */
  if (op->off_path)
    return;
  rfp_inflight_dec(op->proc_id, op->inst_info->addr);
}
