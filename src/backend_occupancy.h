#ifndef __BACKEND_OCCUPANCY_H__
#define __BACKEND_OCCUPANCY_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set by the RS dispatcher when the oldest undispatched op found no RS
 * entry */
extern Flag rs_dispatch_blocked;

void backend_occupancy_observe_cycle(uns proc_id);

#ifdef __cplusplus
}
#endif

#endif /* __BACKEND_OCCUPANCY_H__ */
