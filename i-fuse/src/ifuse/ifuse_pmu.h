#ifndef IFUSE_PMU_H
#define IFUSE_PMU_H

#include "../op.h"

/*
 * PMU counters for what fusion does to each LD2 and to the ops that
 * consume it. These counters only observe. Nothing here changes timing.
 * The IFUSE_PMU_* entries in ifuse.stat.def define each counter.
 */

/**
 * Called from simple_wake() for every source that becomes ready. For
 * each consumer, records which source was a fused LD2 and when the
 * other sources arrived.
 */
void ifuse_pmu_note_wake(const Op* src_op, Op* dep_op);

/** Called once per retired op, in program order. */
void ifuse_pmu_retire(const Op* op);

#endif /* IFUSE_PMU_H */
