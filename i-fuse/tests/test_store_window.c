// Tests for ifuse_store_window.c: the RAW check on fused LD2s.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ifuse/ifuse_store_window.h"

// Fetches one op with the given number, type, address, and size
static void fetch(Counter op_num, Mem_Type type, Addr va, uns size) {
  static Inst_Info info;
  static Op op;
  memset(&info, 0, sizeof(info));
  memset(&op, 0, sizeof(op));
  info.table_info.mem_type = type;
  op.inst_info = &info;
  op.op_num = op_num;
  op.oracle_info.va = va;
  op.oracle_info.mem_size = size;
  ifuse_store_window_note_op(&op);
}

int main(void) {
  // LD1 is op 10, LD2 reads 8 bytes at 0x1010 as op 20
  fetch(10, MEM_LD, 0x1000, 8);
  fetch(12, MEM_ST, 0x2000, 8);  // Another block
  fetch(14, MEM_ST, 0x1018, 8);  // Next to LD2's bytes, no overlap
  assert(!ifuse_store_window_overlaps(10, 20, 0x1010, 8));

  fetch(16, MEM_ST, 0x1014, 4);  // Writes LD2's bytes
  assert(ifuse_store_window_overlaps(10, 20, 0x1010, 8));

  // A store older than LD1 does not count
  assert(!ifuse_store_window_overlaps(16, 20, 0x1010, 8));

  // Fetch rewinds to op 15 after a recovery: store 16 was squashed
  fetch(15, MEM_LD, 0x3000, 8);
  assert(!ifuse_store_window_overlaps(10, 20, 0x1010, 8));

  printf("I-Fuse store window tests passed\n");
  return 0;
}
