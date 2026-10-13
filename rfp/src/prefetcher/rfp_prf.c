/* RFP PRF tracker; see rfp_prf.h
 *
 * Logic is unchanged from src/prefetcher/rfp_prf.c on Scarab branch
 * hpca2027-rfp. */

#include "prefetcher/rfp_prf.h"

#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"

#include "core.param.h"
#include "map_rename.h"   // reg_file_get_reg_type, exported by scarab.patch
#include "memory/memory.param.h"
#include "prefetcher/rfp.param.h"
#include "statistics.h"

/* The tracker spans both physical register files. Their ids overlap, so
   rfp_op_load_prfid() maps vector ids to INT_SIZE + phys. */
#define RFP_PRF_NUM_ENTRIES (REG_TABLE_INTEGER_PHYSICAL_SIZE + REG_TABLE_VECTOR_PHYSICAL_SIZE)

static RFP_Prf_Entry* rfp_prf_table;

static RFP_Prf_Entry* rfp_prf_entry(uns8 proc_id, uns16 prfid) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  if (!rfp_prf_table)
    return NULL;
  if (prfid >= RFP_PRF_NUM_ENTRIES)
    return NULL;
  return &rfp_prf_table[proc_id * RFP_PRF_NUM_ENTRIES + prfid];
}

void rfp_prf_init(void) {
  uns num_entries = NUM_CORES * RFP_PRF_NUM_ENTRIES;
  rfp_prf_table = calloc(num_entries, sizeof(RFP_Prf_Entry));
  ASSERT(0, rfp_prf_table);
}

/* called from dcache_stage.c on a wrong prediction */
void rfp_prf_clear(uns8 proc_id, uns16 prfid) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);
  if (!entry)
    return;

  entry->valid = FALSE;
  entry->prefetched_va = 0;
  entry->ready_cycle = 0;
  entry->owner_op_unique = 0;
  /* Also clear the started/dropped markers. An earlier version had loads wait
     on started-but-not-ready entries, and a stale started bit deadlocked it. */
  entry->started_valid = FALSE;
  entry->started_owner_op_unique = 0;
  entry->dropped_valid = FALSE;
  entry->dropped_owner_op_unique = 0;
}

/* Called from thread.c:recover_seq_op_list for every op a recovery removes,
 * so a recycled prfid cannot inherit a squashed load's prefetch. */
void rfp_prf_clear_op(Op* op) {
  uns16 prfid;

  if (!rfp_op_load_prfid(op, &prfid))
    return;
  rfp_prf_clear(op->proc_id, prfid);
}

/* Called from dcache_stage.c:dcache_process_lsq_rfp_queue on an L1 hit. */
Flag rfp_prf_write(uns8 proc_id, uns16 prfid, Addr va, Counter ready_cycle, Counter owner_op_unique) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);
  if (!entry)
    return FALSE;

  if (entry->dropped_valid && entry->dropped_owner_op_unique == owner_op_unique) {
    /* the load already gave up on this prefetch */
    return FALSE;
  }

  entry->valid = TRUE;
  entry->prefetched_va = va;
  entry->ready_cycle = ready_cycle;
  entry->owner_op_unique = owner_op_unique;
  entry->dropped_valid = FALSE;
  return TRUE;
}

/* Called only from memory.c on the unreachable RFP memory-request path. */
void rfp_prf_set_inflight(uns8 proc_id, uns16 prfid, Counter owner_op_unique) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);
  if (!entry)
    return;

  entry->started_valid = TRUE;
  entry->started_owner_op_unique = owner_op_unique;
}

/* Called from dcache_stage.c when the load dispatches before its prefetch
 * reads the L1, so a later write for that load is ignored. */
void rfp_prf_mark_dropped(uns8 proc_id, uns16 prfid, Counter owner_op_unique) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);
  if (!entry)
    return;

  entry->dropped_valid = TRUE;
  entry->dropped_owner_op_unique = owner_op_unique;
}

/* Checks only the line; dcache_stage.c checks the exact address. The FIFO
 * drain also calls this to discard requests that are already satisfied. */
Flag rfp_prf_ready(uns8 proc_id, uns16 prfid, Addr va, Counter launch_cycle, Counter owner_op_unique,
                   Counter* ready_cycle_out) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);

  if (!entry || !entry->valid)
    return FALSE;
  if (entry->prefetched_va != (va & ~(Addr)(DCACHE_LINE_SIZE - 1)))
    return FALSE;
  if (entry->ready_cycle < launch_cycle)
    return FALSE;
  if (entry->owner_op_unique != owner_op_unique) {
    /* a recycled prfid still holds another load's prefetch */
    STAT_EVENT(proc_id, RFP_PRF_OWNER_MISMATCH);
    entry->valid = FALSE;
    entry->prefetched_va = 0;
    entry->ready_cycle = 0;
    entry->owner_op_unique = 0;
    return FALSE;
  }
  if (ready_cycle_out)
    *ready_cycle_out = entry->ready_cycle;
  return TRUE;
}

/* Called only from memory.c on the unreachable RFP memory-request path. */
Flag rfp_prf_is_inflight(uns8 proc_id, uns16 prfid, Counter owner_op_unique) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);

  if (!entry || !entry->started_valid)
    return FALSE;
  if (entry->started_owner_op_unique != owner_op_unique)
    return FALSE;
  return TRUE;
}

/* Loads with no integer or vector destination are not prefetched. */
Flag rfp_op_load_prfid(const Op* op, uns16* prfid) {
  ASSERT(op->proc_id, op && prfid);

  if (op->inst_info->table_info.num_dest_regs == 0)
    return FALSE;

  uns16 phys = op->dst_reg_id[0][REG_TABLE_TYPE_PHYSICAL];
  if (phys == REG_TABLE_REG_ID_INVALID)
    return FALSE;

  /* Integer and vector physical ids overlap, so map them to disjoint ranges.
     dests[0].id is the flattened logical id reg_file_get_reg_type() expects,
     matching dst_reg_id[0] above. */
  int rtype = reg_file_get_reg_type(op->inst_info->dests[0].id);
  if (rtype == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
    *prfid = phys;
  } else if (rtype == REG_FILE_REG_TYPE_VECTOR) {
    *prfid = (uns16)(REG_TABLE_INTEGER_PHYSICAL_SIZE + phys);
  } else {
    return FALSE;   /* non-int/vector dest (flags/mask): no RFP */
  }

  return TRUE;
}
