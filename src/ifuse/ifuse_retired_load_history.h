#ifndef IFUSE_RETIRED_LOAD_HISTORY_H
#define IFUSE_RETIRED_LOAD_HISTORY_H

#include <stdbool.h>

#include "globals/global_types.h"

/* Fields a match returns for the older load (LD1) */
typedef struct RetiredLoadHistoryEntry {
    Addr    pc;
    Addr    effective_addr;
    Counter micro_op_num;
} RetiredLoadHistoryEntry;

void retired_load_history_init(void);
void retired_load_history_insert(Addr pc, Addr effective_addr,
                                 Counter micro_op_num);
bool retired_load_history_take_match(Addr effective_addr,
                                     Counter micro_op_num,
                                     RetiredLoadHistoryEntry* match);
void retired_load_history_invalidate_block(Addr effective_addr);
void retired_load_history_invalidate_range(Addr effective_addr, uns mem_size);

#endif
