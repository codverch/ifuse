/*
 * Unit tests for the Helios address checks, the load UCH, and the fusion
 * predictor (heliosHistory.c, heliosFusionPredictor.c), without the rest of
 * Scarab. heliosFusion.c needs live pipeline ops, so simulation checks it
 * instead (see ../README.md, Validation).
 *
 * Each check prints "ok" or "FAIL". The exit status is the number of failures.
 */

#include <stdio.h>
#include <stdlib.h>

#include "helios/heliosFusionPredictor.h"
#include "helios/heliosHistory.h"
#include "statistics.h"

extern uns HELIOS_FUSION_WINDOW;
extern uns HELIOS_CONFIDENCE_THRESHOLD;
extern uns HELIOS_CONFIDENCE_INCREMENT;
extern uns HELIOS_CONFIDENCE_DECREMENT;

Stat** global_stat_array;

static int fails;

static void check(const char* name, long long got, long long want) {
  if (got == want) {
    printf("ok   %s\n", name);
  } else {
    printf("FAIL %s: got %lld, want %lld\n", name, got, want);
    fails++;
  }
}

static Counter stat(Stat_Enum s) {
  return global_stat_array[0][s].count;
}

static void test_regions(void) {
  check("isContiguous: back to back", isContiguous(0x1008, 0x1000, 8, 8), 1);
  check("isContiguous: either order", isContiguous(0x1000, 0x1008, 8, 8), 1);
  check("isContiguous: gap", isContiguous(0x1010, 0x1000, 8, 8), 0);
  check("isSameCacheBlock: both inside one line", isSameCacheBlock(0x1000, 0x1038, 8, 8), 1);
  check("isSameCacheBlock: one access crosses the line", isSameCacheBlock(0x1000, 0x103c, 8, 8), 0);
}

static void test_can_fuse(void) {
  check("canFuse: contiguous bytes", canFuse(0x1008, 0x1000, 8, 8), 1);
  check("canFuse: same line, gap between", canFuse(0x1030, 0x1000, 8, 8), 1);
  check("canFuse: overlapping, different sizes", canFuse(0x1004, 0x1000, 4, 8), 1);
  check("canFuse: 64 B span across two lines", canFuse(0x1040, 0x1008, 8, 8), 1);
  check("canFuse: 65 B span across two lines", canFuse(0x1041, 0x1008, 8, 8), 0);
  check("canFuse: two lines apart", canFuse(0x1080, 0x1000, 8, 8), 0);
}

static void test_load_uch(void) {
  uint64_t pc = 0;
  initLoadUCH();
  check("load UCH: first access inserts", updateLoadUCH(0x10, 0x1000, 8, 10, &pc), 0);
  check("load UCH: same line pairs, distance = CN difference", updateLoadUCH(0x14, 0x1010, 8, 15, &pc), 5);
  check("load UCH: matched head is invalidated", updateLoadUCH(0x18, 0x1020, 8, 20, &pc), 0);
  check("load UCH: unmatched op becomes a head", updateLoadUCH(0x1c, 0x1028, 8, 30, &pc), 10);

  initLoadUCH();
  updateLoadUCH(0x10, 0x2000, 8, 100, &pc);
  check("load UCH: distance 65 exceeds the 64 window", updateLoadUCH(0x14, 0x2008, 8, 165, &pc), 0);
  initLoadUCH();
  updateLoadUCH(0x10, 0x2000, 8, 100, &pc);
  check("load UCH: distance 64 trains", updateLoadUCH(0x14, 0x2008, 8, 164, &pc), 64);

  // The paper's UCH matches line tags, so only same-line pairs train.
  initLoadUCH();
  updateLoadUCH(0x10, 0x303c, 4, 1, &pc);
  check("load UCH: contiguous across a line boundary does not train", updateLoadUCH(0x14, 0x3040, 4, 2, &pc), 0);

  initLoadUCH();
  updateLoadUCH(0x10, 0x5000, 0, 1, &pc);
  check("load UCH: a zero-size load never heads a pair", updateLoadUCH(0x14, 0x5008, 8, 2, &pc), 0);

  initLoadUCH();
  for (int i = 0; i < UCH_LOAD_SIZE + 1; i++)
    updateLoadUCH(0x10, 0x4000 + 64 * i, 8, 1 + i, &pc);
  check("load UCH: 7th line evicts the oldest of 6 entries", updateLoadUCH(0x14, 0x4000, 8, 20, &pc), 0);
  check("load UCH: newest entry still present", updateLoadUCH(0x14, 0x4000 + 64 * UCH_LOAD_SIZE, 8, 21, &pc),
        21 - (1 + UCH_LOAD_SIZE));
}

// Paper Sec. IV-A2: 2-bit counter, fuse at 3, reset to 0 on a misprediction.
static void paper_counter(void) {
  HELIOS_CONFIDENCE_THRESHOLD = 3;
  HELIOS_CONFIDENCE_INCREMENT = 1;
  HELIOS_CONFIDENCE_DECREMENT = 3;
}

static uint16_t predict(uint64_t pc, uint32_t hist) {
  bool l, g;
  return predictFusion(pc, hist, &l, &g);
}

static void test_predictor(void) {
  const uint64_t pc = 0x401234;
  const uint32_t hist = 0x5a;

  paper_counter();
  predictorInit();
  Counter miss = stat(HELIOS_FP_LOOKUP_MISS);
  check("FP: untrained PC predicts nothing", predict(pc, hist), 0);
  check("FP: untrained lookup counts a miss", stat(HELIOS_FP_LOOKUP_MISS) - miss, 1);

  addInstructionToPredictor(pc, hist, 5);
  check("FP: allocation sets confidence to the increment (1)", localPredictor[getLocalIndex(pc)][0].confidence, 1);
  check("FP: confidence 1 does not predict", predict(pc, hist), 0);
  addInstructionToPredictor(pc, hist, 5);
  addInstructionToPredictor(pc, hist, 5);
  check("FP: confidence 3 predicts the trained distance", predict(pc, hist), 5);

  addInstructionToPredictor(pc, hist, 7);
  check("FP: a new distance restarts confidence at 1, as in the paper", predict(pc, hist), 0);
  addInstructionToPredictor(pc, hist, 7);
  addInstructionToPredictor(pc, hist, 7);
  check("FP: the new distance predicts after retraining", predict(pc, hist), 7);

  updatePredictor(pc, hist);
  check("FP: a misprediction drops confidence below threshold", predict(pc, hist), 0);

  // Selector starts at 0 (local). Training under a second history leaves the
  // local entry confident while the global entry for this history is new.
  predictorInit();
  for (int i = 0; i < 3; i++)
    addInstructionToPredictor(pc, hist, 9);
  check("FP: a different history still predicts through the local table", predict(pc, hist ^ 0x1), 9);

  // Tuned knobs (threshold 100, increment 1): 100 trainings before the first
  // prediction, against 3 for the paper's 2-bit counter.
  HELIOS_CONFIDENCE_THRESHOLD = 100;
  HELIOS_CONFIDENCE_INCREMENT = 1;
  HELIOS_CONFIDENCE_DECREMENT = 10;
  predictorInit();
  for (int i = 0; i < 99; i++)
    addInstructionToPredictor(pc, hist, 4);
  check("FP (tuned knobs): 99 trainings do not predict", predict(pc, hist), 0);
  addInstructionToPredictor(pc, hist, 4);
  check("FP (tuned knobs): the 100th training predicts", predict(pc, hist), 4);
}

int main(void) {
  global_stat_array = malloc(sizeof(Stat*));
  global_stat_array[0] = calloc(NUM_GLOBAL_STATS, sizeof(Stat));

  test_regions();
  test_can_fuse();
  test_load_uch();
  test_predictor();

  printf("%d failure(s)\n", fails);
  return fails;
}
