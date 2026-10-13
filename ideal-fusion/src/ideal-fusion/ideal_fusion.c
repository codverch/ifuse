#include "../globals/global_types.h"

#include "ideal_fusion.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../globals/global_vars.h"
#include "../map.h"
#include "../memory/memory.param.h"
#include "../op.h"
#include "../statistics.h"
#include "ideal_fusion.param.h"

/*
 * Pass 1 keeps recently fetched loads grouped by data-cache block. When a
 * later load arrives, the matching step can inspect only earlier loads from
 * the same block instead of scanning the full dynamic instruction history.
 */
#define IDEAL_FUSION_LOAD_CANDIDATE_BUCKETS 4096
#define IDEAL_FUSION_PAIR_INDEX_BUCKETS 1000003
#define IDEAL_FUSION_CSV_FIELDS 11
#define IDEAL_FUSION_CSV_LINE_SIZE 1024
#define IDEAL_FUSION_CSV_HEADER                                                   \
  "load1_pc,load1_data_addr,load1_block_offset,load1_mem_size,"                  \
  "load1_micro_op_num,load2_pc,load2_data_addr,load2_block_offset,"              \
  "load2_mem_size,load2_micro_op_num,micro_op_distance"

typedef struct Ideal_Fusion_Load_Candidate_struct {
  Addr pc;
  Addr virtual_addr;
  Addr cache_block_addr;
  Addr cache_block_offset;
  uns mem_size;
  Counter micro_op_num;
  Flag fused;
  struct Ideal_Fusion_Load_Candidate_struct* next;
} Ideal_Fusion_Load_Candidate;

typedef struct Ideal_Fusion_Pair_struct {
  Counter load1_micro_op_num;
  Counter load2_micro_op_num;
  Addr load1_pc;
  Addr load2_pc;
  Addr load1_data_addr;
  Addr load2_data_addr;
  Addr load1_block_offset;
  Addr load2_block_offset;
  uns load1_mem_size;
  uns load2_mem_size;
  struct Ideal_Fusion_Pair_struct* next;
} Ideal_Fusion_Pair;

Load2BufferNode* load2_buffer_ht[LOAD2_BUFFER_HT_SIZE] = {NULL};

/*
 * Candidate files identify dynamic ops using the order in which on-path ops
 * are fetched. Off-path ops do not advance this counter.
 */
static Counter next_on_path_micro_op_num = 0;

static Ideal_Fusion_Load_Candidate*
  load_candidates[IDEAL_FUSION_LOAD_CANDIDATE_BUCKETS] = {NULL};
static Counter last_load_cleanup_micro_op_num = 0;
static FILE* candidate_log = NULL;
static Counter logged_candidate_pair_count = 0;

/*
 * A fetched op knows only its own sequence number. Store each pair in two
 * indexes so pass 2 can quickly recognize both LOAD1 and LOAD2 without
 * scanning every pair.
 */
static Ideal_Fusion_Pair*
  load1_pair_index[IDEAL_FUSION_PAIR_INDEX_BUCKETS] = {NULL};
static Ideal_Fusion_Pair*
  load2_pair_index[IDEAL_FUSION_PAIR_INDEX_BUCKETS] = {NULL};
static Flag pair_indexes_loaded = FALSE;

static Addr get_cache_block_addr(Addr addr) {
  return (addr / DCACHE_LINE_SIZE) * DCACHE_LINE_SIZE;
}

static uns get_candidate_bucket(Addr cache_block_addr) {
  uns64 key = cache_block_addr;
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdULL;
  key ^= key >> 33;
  return key % IDEAL_FUSION_LOAD_CANDIDATE_BUCKETS;
}

static uns get_pair_index_bucket(Counter micro_op_num) {
  uns64 key = micro_op_num;
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdULL;
  key ^= key >> 33;
  return key % IDEAL_FUSION_PAIR_INDEX_BUCKETS;
}

/*
 * Adds a precomputed LOAD1/LOAD2 relationship to both indexes so either
 * dynamic load can find its partner when it is fetched during pass 2 or 3.
 */
static void index_fusion_pair(const Ideal_Fusion_Pair* pair) {
  Ideal_Fusion_Pair* load1_index_entry;
  Ideal_Fusion_Pair* load2_index_entry;
  uns load1_bucket = get_pair_index_bucket(pair->load1_micro_op_num);
  uns load2_bucket = get_pair_index_bucket(pair->load2_micro_op_num);

  load1_index_entry = (Ideal_Fusion_Pair*)malloc(sizeof(*load1_index_entry));
  load2_index_entry = (Ideal_Fusion_Pair*)malloc(sizeof(*load2_index_entry));
  if (!load1_index_entry || !load2_index_entry) {
    free(load1_index_entry);
    free(load2_index_entry);
    fprintf(stderr, "Ideal fusion: could not allocate pass-2 pair indexes.\n");
    exit(EXIT_FAILURE);
  }

  *load1_index_entry = *pair;
  load1_index_entry->next = load1_pair_index[load1_bucket];
  load1_pair_index[load1_bucket] = load1_index_entry;

  *load2_index_entry = *pair;
  load2_index_entry->next = load2_pair_index[load2_bucket];
  load2_pair_index[load2_bucket] = load2_index_entry;
}

/* Returns the fusion pair when this dynamic op is a precomputed LOAD1. */
static Ideal_Fusion_Pair* lookup_load1_pair(Counter micro_op_num) {
  Ideal_Fusion_Pair* pair;
  uns bucket = get_pair_index_bucket(micro_op_num);

  for (pair = load1_pair_index[bucket]; pair; pair = pair->next) {
    if (pair->load1_micro_op_num == micro_op_num)
      return pair;
  }

  return NULL;
}

/* Returns the fusion pair when this dynamic op is a precomputed LOAD2. */
static Ideal_Fusion_Pair* lookup_load2_pair(Counter micro_op_num) {
  Ideal_Fusion_Pair* pair;
  uns bucket = get_pair_index_bucket(micro_op_num);

  for (pair = load2_pair_index[bucket]; pair; pair = pair->next) {
    if (pair->load2_micro_op_num == micro_op_num)
      return pair;
  }

  return NULL;
}

/*
 * Pass 3 (measure): do not fuse anything. Load the same pair set pass 2 would
 * fuse, let both loads execute normally, and log the real completion
 * (wake/done) cycle of each LOAD1 and LOAD2 as it writes back. Joining these
 * rows by (load1,load2) offline shows whether the older LOAD1 actually
 * finishes before the younger LOAD2, which pass-2 forwarding assumes.
 */
static void load_pair_indexes(void);
static FILE* measure_log = NULL;

static void open_measure_log(void) {
  const char* path;

  if (measure_log)
    return;

  path = IDEAL_FUSION_MEASURE_LOG;
  measure_log = fopen(path, "w");
  if (!measure_log) {
    fprintf(stderr, "Ideal fusion measure: could not open '%s': %s\n", path,
            strerror(errno));
    exit(EXIT_FAILURE);
  }
  fprintf(measure_log,
          "role,this_micro_op_num,load1_micro_op_num,load2_micro_op_num,"
          "done_cycle,wake_cycle,cur_cycle\n");
}

void ideal_fusion_measure_on_wake(Op* op) {
  Ideal_Fusion_Pair* pair;
  const char* role = NULL;

  if (!op || op->off_path || IDEAL_FUSION_PASS != IDEAL_FUSION_PASS_MEASURE)
    return;
  if (op->ideal_fusion_micro_op_num == 0 ||
      op->inst_info->table_info.mem_type != MEM_LD)
    return;

  load_pair_indexes();

  pair = lookup_load1_pair(op->ideal_fusion_micro_op_num);
  if (pair) {
    role = "LOAD1";
  } else {
    pair = lookup_load2_pair(op->ideal_fusion_micro_op_num);
    if (pair)
      role = "LOAD2";
  }
  if (!role)
    return;

  open_measure_log();
  fprintf(measure_log, "%s,%llu,%llu,%llu,%llu,%llu,%llu\n", role,
          op->ideal_fusion_micro_op_num, pair->load1_micro_op_num,
          pair->load2_micro_op_num, op->done_cycle, op->wake_cycle,
          cycle_count);
}

/*
 * Pass 2 tags each fetched load as LOAD1, LOAD2, or neither. The role drives
 * every later pipeline change: ideal_fusion_on_map(), the node_stage and
 * issue_queue bypasses, and ideal_fusion_on_load1_wake().
 */
static void classify_load(Op* op) {
  Ideal_Fusion_Pair* pair;

  if (op->inst_info->table_info.mem_type != MEM_LD ||
      op->oracle_info.va == 0 || op->oracle_info.mem_size == 0)
    return;

  pair = lookup_load1_pair(op->ideal_fusion_micro_op_num);
  if (pair) {
    op->ideal_fusion_load_role = IDEAL_FUSION_LOAD1;
    op->ideal_fusion_partner_micro_op_num = pair->load2_micro_op_num;
    STAT_EVENT(op->proc_id, IDEAL_FUSION_LOAD1_TAGGED);
    return;
  }

  pair = lookup_load2_pair(op->ideal_fusion_micro_op_num);
  if (pair) {
    op->ideal_fusion_load_role = IDEAL_FUSION_LOAD2;
    op->ideal_fusion_partner_micro_op_num = pair->load1_micro_op_num;
    STAT_EVENT(op->proc_id, IDEAL_FUSION_LOAD2_TAGGED);
  }
}

static Flag access_fits_in_cache_block(Addr addr, uns mem_size) {
  Addr cache_block_addr;

  if (mem_size == 0)
    return FALSE;

  cache_block_addr = get_cache_block_addr(addr);
  return addr + mem_size - 1 < cache_block_addr + DCACHE_LINE_SIZE;
}

static void pair_log_error(Counter line_num, const char* message) {
  fprintf(stderr, "Ideal fusion: invalid candidate log '%s' at line %llu: %s\n",
          IDEAL_FUSION_LOG, line_num, message);
  exit(EXIT_FAILURE);
}

static Flag parse_csv_number(const char* text, uns64* value) {
  char* end;

  if (!text || !text[0])
    return FALSE;

  errno = 0;
  *value = strtoull(text, &end, 0);
  return errno == 0 && end != text && *end == '\0';
}

/*
 * Passes 2 and 3 read the complete metadata recorded by pass 1 and check each
 * row for internal consistency, so a malformed log fails loudly. Only the
 * micro-op numbers are used for matching. Nothing checks that they name the
 * same dynamic ops as in pass 1. That holds only with the same trace, ROI,
 * and warmup.
 */
static void load_pair_indexes(void) {
  FILE* pair_log;
  char line[IDEAL_FUSION_CSV_LINE_SIZE];
  Counter line_num = 0;
  Counter loaded_pair_count = 0;
  Counter skipped_distance_count = 0;

  if (pair_indexes_loaded)
    return;

  if (!IDEAL_FUSION_LOG || !IDEAL_FUSION_LOG[0]) {
    fprintf(stderr, "Ideal fusion: candidate log path must not be empty.\n");
    exit(EXIT_FAILURE);
  }

  pair_log = fopen(IDEAL_FUSION_LOG, "r");
  if (!pair_log) {
    fprintf(stderr, "Ideal fusion: could not open candidate log '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (!fgets(line, sizeof(line), pair_log)) {
    fclose(pair_log);
    pair_log_error(1, "missing CSV header");
  }
  line_num++;
  line[strcspn(line, "\r\n")] = '\0';
  if (strcmp(line, IDEAL_FUSION_CSV_HEADER) != 0) {
    fclose(pair_log);
    pair_log_error(line_num, "unexpected CSV header");
  }

  while (fgets(line, sizeof(line), pair_log)) {
    Ideal_Fusion_Pair pair = {0};
    char* fields[IDEAL_FUSION_CSV_FIELDS];
    char* saveptr = NULL;
    char* field;
    uns64 values[IDEAL_FUSION_CSV_FIELDS];
    uns field_count = 0;
    Counter recorded_distance;

    line_num++;
    line[strcspn(line, "\r\n")] = '\0';
    if (!line[0])
      continue;

    for (field = strtok_r(line, ",", &saveptr); field;
         field = strtok_r(NULL, ",", &saveptr)) {
      if (field_count == IDEAL_FUSION_CSV_FIELDS) {
        fclose(pair_log);
        pair_log_error(line_num, "too many CSV fields");
      }
      fields[field_count++] = field;
    }

    if (field_count != IDEAL_FUSION_CSV_FIELDS) {
      fclose(pair_log);
      pair_log_error(line_num, "wrong number of CSV fields");
    }

    for (field_count = 0; field_count < IDEAL_FUSION_CSV_FIELDS;
         field_count++) {
      if (!parse_csv_number(fields[field_count], &values[field_count])) {
        fclose(pair_log);
        pair_log_error(line_num, "invalid numeric field");
      }
    }

    if (values[3] > UINT_MAX || values[8] > UINT_MAX) {
      fclose(pair_log);
      pair_log_error(line_num, "memory size is out of range");
    }

    pair.load1_pc = values[0];
    pair.load1_data_addr = values[1];
    pair.load1_block_offset = values[2];
    pair.load1_mem_size = values[3];
    pair.load1_micro_op_num = values[4];
    pair.load2_pc = values[5];
    pair.load2_data_addr = values[6];
    pair.load2_block_offset = values[7];
    pair.load2_mem_size = values[8];
    pair.load2_micro_op_num = values[9];
    recorded_distance = values[10];

    if (pair.load1_micro_op_num == 0 ||
        pair.load2_micro_op_num <= pair.load1_micro_op_num ||
        pair.load2_micro_op_num - pair.load1_micro_op_num !=
          recorded_distance) {
      fclose(pair_log);
      pair_log_error(line_num, "invalid micro-op sequence numbers");
    }

    if (get_cache_block_addr(pair.load1_data_addr) !=
          get_cache_block_addr(pair.load2_data_addr) ||
        pair.load1_block_offset !=
          pair.load1_data_addr - get_cache_block_addr(pair.load1_data_addr) ||
        pair.load2_block_offset !=
          pair.load2_data_addr - get_cache_block_addr(pair.load2_data_addr) ||
        !access_fits_in_cache_block(pair.load1_data_addr,
                                    pair.load1_mem_size) ||
        !access_fits_in_cache_block(pair.load2_data_addr,
                                    pair.load2_mem_size)) {
      fclose(pair_log);
      pair_log_error(line_num, "inconsistent cache-block metadata");
    }

    /* A smaller distance than pass 1 used keeps a subset of its pairs. */
    if (recorded_distance >= IDEAL_FUSION_DISTANCE) {
      skipped_distance_count++;
      continue;
    }

    index_fusion_pair(&pair);
    loaded_pair_count++;
  }

  if (ferror(pair_log)) {
    fclose(pair_log);
    fprintf(stderr, "Ideal fusion: could not read candidate log '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (fclose(pair_log) != 0) {
    fprintf(stderr, "Ideal fusion: could not close candidate log '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
    exit(EXIT_FAILURE);
  }

  pair_indexes_loaded = TRUE;

  printf("Ideal fusion pass 2: loaded %llu candidate pair(s) from '%s' "
         "(distance < %u, skipped %llu)\n",
         loaded_pair_count, IDEAL_FUSION_LOG, IDEAL_FUSION_DISTANCE,
         skipped_distance_count);
  fflush(stdout);
}

static void candidate_log_error(const char* action) {
  fprintf(stderr, "Ideal fusion: could not %s candidate output file '%s': %s\n",
          action, IDEAL_FUSION_LOG, strerror(errno));
  exit(EXIT_FAILURE);
}

static void close_candidate_log(void) {
  if (!candidate_log)
    return;

  if (fflush(candidate_log) != 0)
    fprintf(stderr, "Ideal fusion: could not flush candidate output file '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
  if (fclose(candidate_log) != 0)
    fprintf(stderr, "Ideal fusion: could not close candidate output file '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
  candidate_log = NULL;
}

static void open_candidate_log(void) {
  if (candidate_log)
    return;

  if (!IDEAL_FUSION_LOG || !IDEAL_FUSION_LOG[0]) {
    fprintf(stderr, "Ideal fusion: candidate output path must not be empty.\n");
    exit(EXIT_FAILURE);
  }

  candidate_log = fopen(IDEAL_FUSION_LOG, "w");
  if (!candidate_log)
    candidate_log_error("open");

  if (fprintf(candidate_log, "%s\n", IDEAL_FUSION_CSV_HEADER) < 0 ||
      fflush(candidate_log) != 0)
    candidate_log_error("initialize");
  atexit(close_candidate_log);
}

static void log_matched_pair(Ideal_Fusion_Load_Candidate* load1, Op* load2) {
  Counter distance = load2->ideal_fusion_micro_op_num - load1->micro_op_num;
  Addr load2_block_addr = get_cache_block_addr(load2->oracle_info.va);

  open_candidate_log();
  if (fprintf(candidate_log,
              "0x%llx,0x%llx,%llu,%u,%llu,0x%llx,0x%llx,%llu,%u,%llu,%llu\n",
              load1->pc, load1->virtual_addr, load1->cache_block_offset,
              load1->mem_size, load1->micro_op_num, load2->inst_info->addr,
              load2->oracle_info.va, load2->oracle_info.va - load2_block_addr,
              load2->oracle_info.mem_size, load2->ideal_fusion_micro_op_num,
              distance) < 0)
    candidate_log_error("write to");

  if (++logged_candidate_pair_count % 1000 == 0 &&
      fflush(candidate_log) != 0)
    candidate_log_error("flush");
}

static Flag load1_matches_load2(const Ideal_Fusion_Load_Candidate* load1,
                                Op* load2, Addr cache_block_addr) {
  return load1->cache_block_addr == cache_block_addr &&
         !load1->fused &&
         access_fits_in_cache_block(load1->virtual_addr, load1->mem_size) &&
         access_fits_in_cache_block(load2->oracle_info.va,
                                    load2->oracle_info.mem_size) &&
         load2->ideal_fusion_micro_op_num - load1->micro_op_num <
           IDEAL_FUSION_DISTANCE;
}

static Ideal_Fusion_Load_Candidate* find_matching_load1(Op* load2) {
  Ideal_Fusion_Load_Candidate* load1;
  Ideal_Fusion_Load_Candidate* best_match = NULL;
  Addr cache_block_addr;
  uns bucket;

  if (load2->inst_info->table_info.mem_type != MEM_LD ||
      load2->inst_info->table_info.num_dest_regs == 0 ||
      load2->oracle_info.va == 0 || load2->oracle_info.mem_size == 0)
    return NULL;

  cache_block_addr = get_cache_block_addr(load2->oracle_info.va);
  bucket = get_candidate_bucket(cache_block_addr);

  for (load1 = load_candidates[bucket]; load1; load1 = load1->next) {
    if (!load1_matches_load2(load1, load2, cache_block_addr))
      continue;

    /* At most one candidate qualifies (README, Limitations). Taking the
     * oldest keeps the choice deterministic regardless. */
    if (!best_match || load1->micro_op_num < best_match->micro_op_num)
      best_match = load1;
  }

  return best_match;
}

static void track_load(Op* op) {
  Ideal_Fusion_Load_Candidate* candidate;
  uns bucket;

  if (op->inst_info->table_info.mem_type != MEM_LD ||
      op->inst_info->table_info.num_dest_regs == 0 ||
      op->oracle_info.va == 0 || op->oracle_info.mem_size == 0)
    return;

  candidate = (Ideal_Fusion_Load_Candidate*)malloc(sizeof(*candidate));
  if (!candidate)
    return;

  candidate->pc = op->inst_info->addr;
  candidate->virtual_addr = op->oracle_info.va;
  candidate->cache_block_addr = get_cache_block_addr(op->oracle_info.va);
  candidate->cache_block_offset =
    op->oracle_info.va - candidate->cache_block_addr;
  candidate->mem_size = op->oracle_info.mem_size;
  candidate->micro_op_num = op->ideal_fusion_micro_op_num;
  candidate->fused = FALSE;

  bucket = get_candidate_bucket(candidate->cache_block_addr);
  candidate->next = load_candidates[bucket];
  load_candidates[bucket] = candidate;
}

/*
 * A store between LOAD1 and LOAD2 to the same cache block may change the data that LOAD2 observes.
 * Conservatively discard every earlier LOAD1 candidate from the store's cache
 * block, matching the original ideal-fusion implementation.
 */
static void invalidate_loads_for_store(Op* store) {
  Ideal_Fusion_Load_Candidate** candidate;
  Addr cache_block_addr;
  uns bucket;

  if (store->inst_info->table_info.mem_type != MEM_ST ||
      store->oracle_info.va == 0)
    return;

  cache_block_addr = get_cache_block_addr(store->oracle_info.va);
  bucket = get_candidate_bucket(cache_block_addr);
  candidate = &load_candidates[bucket];

  while (*candidate) {
    if ((*candidate)->cache_block_addr == cache_block_addr) {
      Ideal_Fusion_Load_Candidate* invalidated_candidate = *candidate;
      *candidate = invalidated_candidate->next;
      free(invalidated_candidate);
    } else {
      candidate = &(*candidate)->next;
    }
  }
}

static void cleanup_stale_loads(Counter current_micro_op_num) {
  uns bucket;

  for (bucket = 0; bucket < IDEAL_FUSION_LOAD_CANDIDATE_BUCKETS; bucket++) {
    Ideal_Fusion_Load_Candidate** candidate = &load_candidates[bucket];

    while (*candidate) {
      if ((*candidate)->fused ||
          current_micro_op_num - (*candidate)->micro_op_num >=
            IDEAL_FUSION_DISTANCE) {
        Ideal_Fusion_Load_Candidate* stale_candidate = *candidate;
        *candidate = stale_candidate->next;
        free(stale_candidate);
      } else {
        candidate = &(*candidate)->next;
      }
    }
  }
}

static void param_error(const char* message) {
  fprintf(stderr, "Ideal fusion: invalid configuration: %s\n", message);
  exit(EXIT_FAILURE);
}

/*
 * Rejects settings the passes would otherwise accept silently: an unknown pass
 * number used to disable fusion, and a zero distance made every pass a no-op.
 */
static void check_params(void) {
  if (IDEAL_FUSION_PASS > IDEAL_FUSION_PASS_MEASURE)
    param_error("--ideal_fusion_pass must be 0, 1, 2, or 3");
  if (IDEAL_FUSION_PASS == IDEAL_FUSION_PASS_OFF)
    return;
  if (IDEAL_FUSION_DISTANCE == 0)
    param_error("--ideal_fusion_distance must be at least 1");
  if (!IDEAL_FUSION_LOG || !IDEAL_FUSION_LOG[0])
    param_error("--ideal_fusion_log must not be empty");
  if (IDEAL_FUSION_PASS == IDEAL_FUSION_PASS_MEASURE &&
      (!IDEAL_FUSION_MEASURE_LOG || !IDEAL_FUSION_MEASURE_LOG[0]))
    param_error("--ideal_fusion_measure_log must not be empty");
}

void ideal_fusion_on_fetch_op(Op* op) {
  static Flag params_checked = FALSE;

  /* The first fetched op precedes every other ideal-fusion hook. */
  if (!params_checked) {
    check_params();
    params_checked = TRUE;
  }

  if (!op || op->off_path)
    return;

  op->ideal_fusion_micro_op_num = ++next_on_path_micro_op_num;

  if (IDEAL_FUSION_PASS == IDEAL_FUSION_PASS_APPLY) {
    load_pair_indexes();
    classify_load(op);
    return;
  }

  if (IDEAL_FUSION_PASS != IDEAL_FUSION_PASS_DISCOVER ||
      IDEAL_FUSION_DISTANCE == 0)
    return;

  if (op->ideal_fusion_micro_op_num - last_load_cleanup_micro_op_num >=
      IDEAL_FUSION_DISTANCE) {
    cleanup_stale_loads(op->ideal_fusion_micro_op_num);
    last_load_cleanup_micro_op_num = op->ideal_fusion_micro_op_num;
  }

  if (op->inst_info->table_info.mem_type == MEM_ST) {
    invalidate_loads_for_store(op);
    return;
  }

  Ideal_Fusion_Load_Candidate* load1 = find_matching_load1(op);

  if (load1) {
    log_matched_pair(load1, op);
    load1->fused = TRUE;
  } else {
    track_load(op);
  }
}

Flag ideal_fusion_load2_is_nop(const Op* op) {
  return op && op->ideal_fusion_load_role == IDEAL_FUSION_LOAD2;
}

Load2BufferNode* ideal_fusion_find_load2_buffer(Counter load1_micro_op_num) {
  uns bucket = get_pair_index_bucket(load1_micro_op_num);
  Load2BufferNode* node;

  for (node = load2_buffer_ht[bucket]; node; node = node->next) {
    if (node->entry.load1_micro_op_num == load1_micro_op_num)
      return node;
  }
  return NULL;
}

Load2BufferNode* ideal_fusion_create_load2_buffer(Counter load1_micro_op_num) {
  Load2BufferNode* node = (Load2BufferNode*)calloc(1, sizeof(*node));
  uns bucket;

  if (!node) {
    fprintf(stderr, "Ideal fusion: could not allocate Load2 buffer entry.\n");
    exit(EXIT_FAILURE);
  }

  node->entry.load1_micro_op_num = load1_micro_op_num;
  bucket = get_pair_index_bucket(load1_micro_op_num);
  node->next = load2_buffer_ht[bucket];
  load2_buffer_ht[bucket] = node;
  return node;
}

void ideal_fusion_remove_load2_buffer(Load2BufferNode* node) {
  Load2BufferNode** slot;
  uns bucket;

  if (!node)
    return;

  bucket = get_pair_index_bucket(node->entry.load1_micro_op_num);
  for (slot = &load2_buffer_ht[bucket]; *slot; slot = &(*slot)->next) {
    if (*slot == node) {
      *slot = node->next;
      free(node);
      return;
    }
  }
}

static void ideal_fusion_complete_load2(Op* load2, Counter load1_wake_cycle,
                                        Counter load1_done_cycle,
                                        void (*wake_action)(Op*, Op*, uns)) {
  load2->wake_cycle = load1_wake_cycle;
  load2->done_cycle = load1_done_cycle;
  wake_up_ops(load2, REG_DATA_DEP, wake_action);
}

void ideal_fusion_on_map(Op* op, void (*wake_action)(Op*, Op*, uns)) {
  Load2BufferNode* node;

  if (!op || op->off_path || IDEAL_FUSION_PASS != IDEAL_FUSION_PASS_APPLY)
    return;

  if (op->ideal_fusion_load_role == IDEAL_FUSION_LOAD1) {
    node = ideal_fusion_find_load2_buffer(op->ideal_fusion_micro_op_num);
    if (!node)
      node = ideal_fusion_create_load2_buffer(op->ideal_fusion_micro_op_num);
    return;
  }

  if (!ideal_fusion_load2_is_nop(op))
    return;

  node = ideal_fusion_find_load2_buffer(op->ideal_fusion_partner_micro_op_num);
  if (!node)
    node = ideal_fusion_create_load2_buffer(op->ideal_fusion_partner_micro_op_num);

  node->entry.load2 = op;
  node->entry.load2_unique_num = op->unique_num;
  node->entry.load2_micro_op_num = op->ideal_fusion_micro_op_num;

  if (node->entry.load1_completed && !node->entry.pair_completed) {
    ideal_fusion_complete_load2(op, node->entry.load1_wake_cycle,
                                node->entry.load1_done_cycle, wake_action);
    node->entry.pair_completed = TRUE;
    ideal_fusion_remove_load2_buffer(node);
  } else {
    node->entry.load2_waiting = TRUE;
    node->entry.pair_completed = FALSE;
  }
}

void ideal_fusion_on_load1_wake(Op* load1, void (*wake_action)(Op*, Op*, uns)) {
  Load2BufferNode* node;
  Op* load2;

  if (!load1 || load1->off_path ||
      IDEAL_FUSION_PASS != IDEAL_FUSION_PASS_APPLY ||
      load1->ideal_fusion_load_role != IDEAL_FUSION_LOAD1)
    return;

  node = ideal_fusion_find_load2_buffer(load1->ideal_fusion_micro_op_num);
  if (!node)
    node = ideal_fusion_create_load2_buffer(load1->ideal_fusion_micro_op_num);

  node->entry.load1_completed = TRUE;
  node->entry.load1_wake_cycle = load1->wake_cycle;
  node->entry.load1_done_cycle = load1->done_cycle;

  if (!node->entry.load2 || !node->entry.load2_waiting ||
      node->entry.pair_completed)
    return;

  load2 = node->entry.load2;
  if (!load2->op_pool_valid ||
      load2->unique_num != node->entry.load2_unique_num ||
      !ideal_fusion_load2_is_nop(load2)) {
    ideal_fusion_remove_load2_buffer(node);
    return;
  }

  ideal_fusion_complete_load2(load2, node->entry.load1_wake_cycle,
                              node->entry.load1_done_cycle, wake_action);
  node->entry.pair_completed = TRUE;
  ideal_fusion_remove_load2_buffer(node);
}
