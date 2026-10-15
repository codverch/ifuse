#ifndef IFUSE_STATS_H
#define IFUSE_STATS_H

#include "../globals/global_types.h"

/**
 * Samples GP physical-register occupancy and utilization for I-Fuse
 * studies.
 *
 * Average occupied registers:
 *   IFUSE_GP_REG_OCCUPIED_TOTAL / IFUSE_GP_REG_OCCUPIED_OBSERVATIONS.
 * Average utilization (%):
 *   IFUSE_GP_REG_UTIL_PCT_TOTAL / IFUSE_GP_REG_OCCUPIED_OBSERVATIONS.
 */
void ifuse_observe_gp_reg_pressure(uns proc_id);

/**
 * Samples the number of extra physical registers held for predicted or
 * fused LD2s.
 *
 * Average:
 *   IFUSE_EXTRA_REG_IN_USE_TOTAL / IFUSE_EXTRA_REG_IN_USE_OBSERVATIONS.
 */
void ifuse_observe_extra_reg_pressure(uns proc_id);

void ifuse_extra_reg_note_alloc(void);
void ifuse_extra_reg_note_free(void);

/**
 * Returns the number of physical registers now reserved for LD2s that
 * have not renamed.
 */
uns64 ifuse_extra_reg_in_use_now(void);

#endif /* IFUSE_STATS_H */
