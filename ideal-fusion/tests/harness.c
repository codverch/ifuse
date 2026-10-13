/*
 * Drives ideal_fusion.c without the rest of Scarab, so its pass logic can be
 * checked without traces and compared against the original implementation.
 *
 * Usage: harness <pass> < stream
 *
 * Each stream line is one fetched op:
 *   <kind> <pc> <va> <mem_size> <num_dest_regs> <latency> <off_path>
 * kind is L (load), S (store), or O (other). Numbers accept 0x prefixes.
 *
 * Every op is fetched, then mapped one cycle per op in fetch order. A load
 * completes <latency> cycles after it is mapped. This is enough to exercise
 * both orders in which the Load2 buffer can see LOAD1 and LOAD2.
 *
 * Pass 2 prints one line per tagged load (LOAD2 lines add when its
 * dependents woke) and the tag counts. Passes 1 and 3 write their CSV files,
 * which run.sh reads after the harness exits.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "globals/global_types.h"
#include "ideal-fusion/ideal_fusion.h"
#include "map.h"
#include "op.h"
#include "statistics.h"

/* Scarab globals ideal_fusion.c reads. Defined here instead of linking all of
 * Scarab; the parameter headers declare them const, so this file must not
 * include them. Names and defaults match the param.def files. */
uns IDEAL_FUSION_PASS = 1;
uns IDEAL_FUSION_DISTANCE = 512;
char* IDEAL_FUSION_LOG = "ideal_fusion_candidates.csv";
int IDEAL_FUSION_TYPE = 0;
char* IDEAL_FUSION_MEASURE_LOG = "ideal_fusion_measure.csv";
uns DCACHE_LINE_SIZE = 64;
Counter cycle_count = 0;
Stat** global_stat_array;

typedef struct {
  Op op;
  Inst_Info info;
  Counter map_cycle;
  Counter woke_at;  // cycle wake_up_ops() ran for this op; 0 if never
} Sim_Op;

void wake_up_ops(Op* op, Dep_Type type, void (*wake_action)(Op*, Op*, uns)) {
  (void)type;
  (void)wake_action;
  ((Sim_Op*)op)->woke_at = cycle_count;  // op is the first member of Sim_Op
}

static void dummy_wake(Op* src, Op* dep, uns idx) {
  (void)src;
  (void)dep;
  (void)idx;
}

static void env_override(void) {
  const char* v;

  if ((v = getenv("IF_DISTANCE")))
    IDEAL_FUSION_DISTANCE = strtoul(v, NULL, 0);
  if ((v = getenv("IF_TYPE")))
    IDEAL_FUSION_TYPE = strtol(v, NULL, 0);
  if ((v = getenv("IF_LOG")))
    IDEAL_FUSION_LOG = (char*)v;
  if ((v = getenv("IF_MEASURE_LOG")))
    IDEAL_FUSION_MEASURE_LOG = (char*)v;
}

static Sim_Op* read_stream(size_t* count) {
  size_t cap = 1024, n = 0;
  Sim_Op* ops = malloc(cap * sizeof(*ops));
  char kind;
  unsigned long long pc, va, size, ndst, lat, off;

  while (scanf(" %c %lli %lli %lli %lli %lli %lli", &kind, &pc, &va, &size,
               &ndst, &lat, &off) == 7) {
    Sim_Op* s;

    if (n == cap)
      ops = realloc(ops, (cap *= 2) * sizeof(*ops));
    s = &ops[n++];
    memset(s, 0, sizeof(*s));
    s->info.addr = pc;
    s->info.table_info.mem_type =
      kind == 'L' ? MEM_LD : kind == 'S' ? MEM_ST : NOT_MEM;
    s->info.table_info.num_dest_regs = ndst;
    s->op.oracle_info.va = va;
    s->op.oracle_info.mem_size = size;
    s->op.off_path = off;
    s->op.unique_num = n;
    s->op.op_pool_valid = TRUE;
    s->op.wake_cycle = lat;  // latency until the op is mapped; then a cycle
  }
  for (size_t i = 0; i < n; i++)  // after realloc has stopped moving ops
    ops[i].op.inst_info = &ops[i].info;
  *count = n;
  return ops;
}

static int by_done_cycle(const void* a, const void* b) {
  const Sim_Op* x = *(Sim_Op* const*)a;
  const Sim_Op* y = *(Sim_Op* const*)b;

  if (x->op.done_cycle != y->op.done_cycle)
    return x->op.done_cycle < y->op.done_cycle ? -1 : 1;
  return x < y ? -1 : x > y;
}

int main(int argc, char** argv) {
  size_t n, i;
  Sim_Op* ops;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <pass> < stream\n", argv[0]);
    return 2;
  }
  IDEAL_FUSION_PASS = strtoul(argv[1], NULL, 0);
  env_override();

  global_stat_array = malloc(sizeof(Stat*));
  global_stat_array[0] = calloc(NUM_GLOBAL_STATS, sizeof(Stat));

  ops = read_stream(&n);

  /* Fetch: every op, in order, as ft.cc does. */
  for (i = 0; i < n; i++) {
    Counter latency = ops[i].op.wake_cycle;

    ops[i].op.wake_cycle = 0;
    ops[i].op.ideal_fusion_micro_op_num = 0;
    ops[i].op.ideal_fusion_load_role = IDEAL_FUSION_NOT_CANDIDATE;
    ops[i].op.ideal_fusion_partner_micro_op_num = 0;
    ideal_fusion_on_fetch_op(&ops[i].op);
    ops[i].map_cycle = i + 1;
    ops[i].op.done_cycle = ops[i].map_cycle + latency;  // completion cycle
  }

  /* Map and complete, cycle by cycle. A cycle maps its op before any
   * completions, matching map_stage running before wake-up of older ops. */
  {
    Sim_Op** done = malloc(n * sizeof(*done));
    size_t num_done = 0, next = 0;

    /* A fused LOAD2 never executes, so it never completes on its own. */
    for (i = 0; i < n; i++)
      if (ops[i].info.table_info.mem_type == MEM_LD &&
          !ideal_fusion_load2_is_nop(&ops[i].op))
        done[num_done++] = &ops[i];
    qsort(done, num_done, sizeof(*done), by_done_cycle);

    for (cycle_count = 1; cycle_count <= n || next < num_done; cycle_count++) {
      if (cycle_count <= n)
        ideal_fusion_on_map(&ops[cycle_count - 1].op, dummy_wake);
      for (; next < num_done && done[next]->op.done_cycle == cycle_count;
           next++) {
        Op* op = &done[next]->op;

        op->wake_cycle = cycle_count;
        ideal_fusion_on_load1_wake(op, dummy_wake);
        ideal_fusion_measure_on_wake(op);
      }
    }
    free(done);
  }

  if (IDEAL_FUSION_PASS == 2) {
    for (i = 0; i < n; i++) {
      Op* op = &ops[i].op;

      if (op->ideal_fusion_load_role == IDEAL_FUSION_NOT_CANDIDATE)
        continue;
      printf("%llu role=%d partner=%llu", op->ideal_fusion_micro_op_num,
             op->ideal_fusion_load_role, op->ideal_fusion_partner_micro_op_num);
      if (op->ideal_fusion_load_role == IDEAL_FUSION_LOAD2)
        printf(" woke_at=%llu wake=%llu done=%llu", ops[i].woke_at,
               op->wake_cycle, op->done_cycle);
      printf("\n");
    }
    printf("LOAD1_TAGGED %llu\nLOAD2_TAGGED %llu\n",
           global_stat_array[0][IDEAL_FUSION_LOAD1_TAGGED].count,
           global_stat_array[0][IDEAL_FUSION_LOAD2_TAGGED].count);
  }

  /* ideal_fusion.c closes the pass-1 log at exit; flush it first. */
  fflush(NULL);
  exit(0);
}
