/**
 * @file heliosFusion.h
 *
 * @brief Helios load-pair fusion: the pipeline hooks. See helios/README.md.
 *
 * Model of Singh et al., "Exploring Instruction Fusion Opportunities in General
 * Purpose Processors", MICRO 2022, restricted to load pairs.
 *
 * @author Ved Lakshminarayanan <vlakshmi@andrew.cmu.edu>
 */

#ifndef HELIOS_FUSION_H
#define HELIOS_FUSION_H

#include <stdbool.h>
#include <stdint.h>

#include "helios.param.h"

#include "heliosFusionPredictor.h"
#include "heliosHistory.h"
#include "op.h"

/* Register ids recorded per op for the deadlock and serializing checks. */
#define MAX_FUSION_REGS 4

/* Unfused loads that can still become a head. The residency check at the
 * allocation queue (heliosAllocQueueHook) decides whether a head is still in
 * the queue; this table only maps an op number to its op. Paper: 140-entry
 * allocation queue. */
#define LOAD_HEAD_TABLE_SIZE 140

/* Fetch-order register history for the deadlock and serializing checks. Must
 * cover the fusion window. */
#define REG_TRACK_TABLE_SIZE 1024

/* Paper Sec. IV-B2: at most two overlapping non-consecutive fusions. */
#define MAX_NCSF_NEST 2

/* In-flight pairs, for the nesting cap and recovery. Must exceed the pairs in
 * flight between fetch and retire. */
#define FUSION_RING_SIZE 2048

/* Tail source operands a fused head may wait on (paper: three sources per
 * fused micro-op, so a tail adds at most a few). */
#define HELIOS_MAX_TAIL_SRCS HELIOS_OP_MAX_TAIL_SRCS

/* Liveness guard: cycles a fused head may block retire while waiting on its
 * tail's sources before the wait is dropped (heliosUnblockRobHead). */
#define HELIOS_STUCK_HEAD_CYCLES 4096

typedef struct {
  Counter op_num;
  Addr va;
  uns8 size;
  bool valid;
  bool used;  // already the head of a pair
  Op* op;
} LoadHeadTableEntry;

typedef struct {
  Counter op_num;
  uint8_t numDestRegs;
  uint8_t numSrcRegs;
  uint16_t destRegs[MAX_FUSION_REGS];
  uint16_t srcRegs[MAX_FUSION_REGS];
  Addr va;  // memory ops: oracle address and size
  uns8 size;
  bool isLoad;
  bool isStore;
  bool valid;
  bool isSerializing;
} RegTrackEntry;

typedef struct {
  Counter head_op_num;
  Counter tail_op_num;
  Op* head;  // may outlive the head table entry
  Counter head_unique_num;
  bool csf;
  bool valid;
} FusionIntervalEntry;

#ifdef __cplusplus
extern "C" {
#endif

void heliosFusionInit(void);

/* Fetch (ft.cc): predict, validate, and fuse. Runs once per on-path op. */
void heliosFetchHook(Op* op);
/* Allocation queue entry (idq_stage.cc): cancel a fusion whose head has
 * already left the queue. */
void heliosAllocQueueHook(Op* op);
/* Rename (map_stage.c): the tail renames, releases its held head, and gets
 * its data from the head. */
void heliosRenameHook(Op* op);
/* Dispatch (node_stage.c): dispatch slots this op uses, 1 or 2. */
uns heliosDispatchSlots(const Op* op);
/* D-cache (dcache_stage.c): a head load resolved; complete its tail. */
void heliosCompleteFusedTail(Op* head);
/* Retire (node_stage.c). */
Flag heliosExtCommitGroupBlocks(Op* head);
void heliosUnblockRobHead(Op* op);
void heliosCommit(Op* op);
/* Recovery (cmp_model.c): forget state younger than the recovery point. */
void flushTables(Counter op_num);

/* Exposed for tests. */
bool checkDeadlock(const Op* tail, Counter head_op_num);

/* A fused tail takes no ROB, RS, or LQ entry and never issues. */
static inline Flag heliosTailFreesEntries(const Op* op) {
  return HELIOS_DO_FUSION && op->helios_fused;
}

#ifdef __cplusplus
}
#endif

#endif
