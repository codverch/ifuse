#ifndef IFUSE_TRAINING_TABLE_H
#define IFUSE_TRAINING_TABLE_H

#include "globals/global_types.h"

void training_table_init(void);
void training_table_observe(Addr ld1_pc, Addr ld2_pc,
                            Addr ld1_effective_addr,
                            Addr ld2_effective_addr, uns ld2_mem_size,
                            Counter ld1_micro_op_num,
                            Counter ld2_micro_op_num, uns proc_id);

#endif
