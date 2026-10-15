#ifndef IFUSE_LOAD_TRACKER_H
#define IFUSE_LOAD_TRACKER_H

#include <stdint.h>

/**
 * Returns the next dynamic on-path load number.
 *
 * Note: The fusion distance is measured in micro-ops. Load numbers only
 * pace the periodic cleanup of stale APT and ACI predictions.
 */
uint64_t ifuse_next_load_num(void);

#endif /* IFUSE_LOAD_TRACKER_H */
