/* Register File Prefetch (RFP): load-address prediction table and pipeline hooks.
 *
 * RFP predicts a load's address at rename and reads the L1 into the load's
 * physical destination register before the load dispatches. A correct, timely
 * prefetch lets the load skip the L1 access. A wrong or late one costs nothing
 * but L1 bandwidth. Design and paper correspondence: rfp/README.md.
 *
 * The hooks below run at four pipeline points in Scarab:
 *   fetch   icache_stage.c:icache_process_ops  -> rfp_predict_at_fetch
 *   rename  map_stage.c:stage_process_op       -> rfp_prefetch_launch
 *   retire  node_stage.c:node_retire           -> rfp_train_retire
 *   squash  thread.c:recover_seq_op_list       -> rfp_track_squash
 * The prefetch itself is serviced and consumed in dcache_stage.c. */

#ifndef __RFP_H__
#define __RFP_H__

#include "globals/global_types.h"
#include "op.h"
#include "prefetcher/rfp.param.h"

/* always the PRF model; the old memory-request model is gone */
static inline Flag rfp_use_prf_model(void) {
  return RFP_ON;
}

/* Static array bounds. The live sizes come from --rfp_pt_* and --rfp_pat_*
 * and must fit inside these (checked in rfp_init). */
#define RFP_PT_MAX_SETS 512
#define RFP_PT_MAX_WAYS 8
#define RFP_PAT_MAX_SETS 64
#define RFP_PAT_MAX_WAYS 8

#define RFP_TAG_BITS 16
#define RFP_TAG_MASK ((1u << RFP_TAG_BITS) - 1)

/* low PC bits skipped before the set index */
#define RFP_PC_SET_SHIFT 2

/* default threshold, and the largest one the field allows */
#define RFP_CONF_MAX_DEFAULT 1
#define RFP_CONF_COUNTER_MAX 7

/* default and maximum --rfp_stride_bits */
#define RFP_STRIDE_BITS_DEFAULT 5
#define RFP_STRIDE_BITS_MAX 16

/* A PT entry stores only the 12-bit page offset of the last address. The page
 * frame lives in the shared PAT, and the entry points to it with pat_ptr. */
#define RFP_PAGE_OFFSET_BITS 12
#define RFP_PAGE_OFFSET_MASK ((1u << RFP_PAGE_OFFSET_BITS) - 1)
#define RFP_PFN_BITS 44
#define RFP_PFN_MASK (((Addr)1 << RFP_PFN_BITS) - 1)

typedef struct RFP_Pat_Entry_struct {
  Flag valid;
  Addr pfn; /* 44-bit page frame number, 4 KiB pages */
} RFP_Pat_Entry;

typedef struct RFP_PT_Entry_struct {
  uns16 tag;
  uns8 pat_ptr;      /* flat index into rfp_pat[][] */
  uns16 page_offset; /* 12 bits */
  /* bytes; with 16 unsigned bits, strides above 32767 wrap and never confirm */
  int16 stride;
  uns8 confidence;
  uns8 utility;      /* 2-bit replacement counter */
  uns8 inflight;     /* 7 bits; renamed but not retired instances of this PC */
  Flag valid;
} RFP_PT_Entry;

void rfp_init(void);
Flag rfp_predict(uns8 proc_id, Addr pc, Addr* predicted_addr);
void rfp_predict_at_fetch(Op* op);
void rfp_prefetch_launch(Op* op);
void rfp_train_retire(Op* op);
void rfp_track_squash(Op* op);
void rfp_reset_confidence_on_mispredict(uns8 proc_id, Addr pc);
Flag rfp_blocked_by_older_stores(uns8 proc_id, Counter load_op_num, Addr pred_addr, uns load_size);

#endif
