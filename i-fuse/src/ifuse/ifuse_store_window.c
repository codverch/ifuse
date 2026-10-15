#include "ifuse_store_window.h"

#include "globals/global_types.h"

/*
 * Stores fetched within the fusion distance, oldest first, for the RAW
 * check on a fused LD2 (paper Section 4.4). The check uses oracle
 * addresses, like the rest of the frontend classification; the flush it
 * triggers still happens at LD2's execute.
 *
 * The window holds at most one entry per micro-op in the fusion distance,
 * so 1024 entries cover the default distance of 512 with margin. Older
 * stores fall off the ring; they are too old to lie between a live LD1
 * and its LD2.
 */
#define STORE_WINDOW_SIZE 1024U

typedef struct {
  uint64_t op_num;
  uint64_t va;
  unsigned size;
} Window_Store;

static Window_Store window[STORE_WINDOW_SIZE];
static unsigned window_head;   // Next slot to write
static unsigned window_count;  // Valid entries, newest at window_head - 1
static uint64_t last_op_num;

void ifuse_store_window_note_op(const Op* op) {
  // Fetch rewound after a recovery: drop stores at or after this op
  if (op->op_num <= last_op_num) {
    while (window_count) {
      unsigned newest = (window_head + STORE_WINDOW_SIZE - 1U) % STORE_WINDOW_SIZE;
      if (window[newest].op_num < op->op_num)
        break;
      window_head = newest;
      window_count--;
    }
  }
  last_op_num = op->op_num;

  if (op->inst_info->table_info.mem_type != MEM_ST || !op->oracle_info.mem_size)
    return;
  window[window_head] = (Window_Store){op->op_num, op->oracle_info.va, op->oracle_info.mem_size};
  window_head = (window_head + 1U) % STORE_WINDOW_SIZE;
  if (window_count < STORE_WINDOW_SIZE)
    window_count++;
}

bool ifuse_store_window_overlaps(uint64_t ld1_op_num, uint64_t ld2_op_num, uint64_t ld2_va, unsigned ld2_size) {
  for (unsigned i = 1; i <= window_count; i++) {
    const Window_Store* s = &window[(window_head + STORE_WINDOW_SIZE - i) % STORE_WINDOW_SIZE];
    if (s->op_num <= ld1_op_num)
      break;  // Older stores precede LD1
    if (s->op_num < ld2_op_num && s->va < ld2_va + ld2_size && ld2_va < s->va + s->size)
      return true;
  }
  return false;
}
