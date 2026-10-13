/* RFP physical register file (PRF) tracker.
 *
 * Scarab does not model register values, so RFP cannot literally write data
 * into a register. Instead this table records, per physical register, which
 * dynamic load a completed L1 prefetch belongs to, which line it read, and the
 * cycle the value would be usable. dcache_stage.c consults it when the load
 * dispatches. */

#ifndef __RFP_PRF_H__
#define __RFP_PRF_H__

#include "globals/global_types.h"
#include "map_rename.h"
#include "op.h"


/* indexed by prfid */
typedef struct RFP_Prf_Entry_struct {
  /* Ready: completed prefetch sitting in the PRF. */
  Flag    valid;
  Addr    prefetched_va;     /* line-aligned VA prefetched */
  Counter ready_cycle;      /* cycle the value arrived and is usable */
  Counter owner_op_unique;

  /* Started: prefetch began (won an L1 port) but may not be ready yet. Only
     memory.c sets this, on the RFP memory-request path, which nothing
     reaches now that L1 misses are dropped. */
  Flag    started_valid;
  Counter started_owner_op_unique;

  /* Dropped: load beat its prefetch; suppress the late writeback. */
  Flag    dropped_valid;
  Counter dropped_owner_op_unique;
} RFP_Prf_Entry;

void rfp_prf_init(void);
void rfp_prf_clear(uns8 proc_id, uns16 prfid);
void rfp_prf_clear_op(Op* op);
Flag rfp_prf_write(uns8 proc_id, uns16 prfid, Addr va, Counter ready_cycle, Counter owner_op_unique);
void rfp_prf_set_inflight(uns8 proc_id, uns16 prfid, Counter owner_op_unique);
void rfp_prf_mark_dropped(uns8 proc_id, uns16 prfid, Counter owner_op_unique);
/* TRUE if this load's prefetch has read the L1. *ready_cycle_out may lie in
 * the future, which is what gives partial latency savings. */
Flag rfp_prf_ready(uns8 proc_id, uns16 prfid, Addr va, Counter launch_cycle, Counter owner_op_unique,
                   Counter* ready_cycle_out);
Flag rfp_prf_is_inflight(uns8 proc_id, uns16 prfid, Counter owner_op_unique);
Flag rfp_op_load_prfid(const Op* op, uns16* prfid);

#endif 
