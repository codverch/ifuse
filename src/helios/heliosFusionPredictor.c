/**
 * @file heliosFusionPredictor.c
 *
 * @brief Fusion predictor (paper Sec. IV-A2).
 *
 * @author Ved Lakshminarayanan <vlakshmi@andrew.cmu.edu>
 *
 * The tables are global and only core 0 initializes them, so the counters are
 * charged to core 0.
 *
 * Paper Sec. IV-A2 tournament predictor: a PC-indexed and a PC^history-indexed table,
 * 512 sets x 4 ways each, 8-bit tags, plus a 2048-entry selector. Sizes match. The
 * Murcia reference (TournamentFusionPredictor.h) builds the same structure. Differences:
 *   - Confidence: paper and reference use a 2-bit counter, fuse at 3, reset to 0 on a
 *     misprediction. Here confidence is 16 bits with threshold, increment, and
 *     decrement set by --helios_confidence_*; the paper runs used per-app thresholds.
 *   - Replacement: true LRU counters instead of the paper's pseudo-LRU (MRU) bit.
 *   - Index: PC >> 2 for every table. The reference indexes the local table with
 *     PC[bits-1:0] for CISC; x86 PCs are not 4-byte aligned.
 *   - History: Scarab's branch-predictor global history (pred_global_hist), not a
 *     separate taken/not-taken register updated at Decode.
 */

#include "heliosFusionPredictor.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "globals/global_types.h"

#include "helios.param.h"

#include "statistics.h"

FPEntry localPredictor[HELIOS_NUM_CACHE_SETS][HELIOS_NUM_CACHE_WAYS];
FPEntry globalPredictor[HELIOS_NUM_CACHE_SETS][HELIOS_NUM_CACHE_WAYS];
SelTableEntry selectorTable[HELIOS_SEL_TABLE_SIZE];

/**
 * @brief Updates the LRU count for all entries in the predictor set except the one at the given index.
 *
 * @param set The predictor set to update.
 * @param LRUIndex The index to avoid.
 *
 * @return void
 */
static void updateLRU(FPEntry set[HELIOS_NUM_CACHE_WAYS], int LRUIndex) {
  for (int i = 0; i < HELIOS_NUM_CACHE_WAYS; i++) {
    if (i == LRUIndex) {
      continue;
    } else {
      set[i].lruCount++;
    }
  }

  set[LRUIndex].lruCount = 0;
}

/**
 * @brief Finds a specific entry in a predictor set. Ensures that the entry is valid and
 * that the tag matches.
 *
 * @param set The predictor set to search.
 * @param tag The tag to search for.
 *
 * @return The index of the entry if found, -1 otherwise.
 */
static int findEntry(FPEntry set[HELIOS_NUM_CACHE_WAYS], uint16_t tag) {
  for (int i = 0; i < HELIOS_NUM_CACHE_WAYS; i++) {
    if (set[i].valid && set[i].tag == tag) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Finds the least recently used entry in a predictor set.
 *
 * @param set The predictor set to search.
 *
 * @return The index of the entry to eject, -1 otherwise.
 */
static int findVictimToEject(FPEntry set[HELIOS_NUM_CACHE_WAYS]) {
  int indexToEject = 0;
  uint64_t maxLRUCount = set[0].lruCount;
  for (int i = 1; i < HELIOS_NUM_CACHE_WAYS; i++) {
    if (set[i].lruCount > maxLRUCount) {
      maxLRUCount = set[i].lruCount;
      indexToEject = i;
    }
  }

  return indexToEject;
}

/**
 * @brief Increments an entry's confidence by the desired increment. Confidence
 * level is bounded by the threshold.
 *
 * @param entry The entry to increment.
 *
 * @return void
 */
static void incrementConfidence(FPEntry *entry) {
  uint16_t threshold = (uint16_t)HELIOS_CONFIDENCE_THRESHOLD;
  uint16_t increment = (uint16_t)HELIOS_CONFIDENCE_INCREMENT;

  if ((entry->confidence + increment) <= threshold) {
    entry->confidence += increment;
  } else {
    entry->confidence = threshold;
  }
}

/**
 * @brief Decrements an entry's confidence by the desired decrement. Confidence level
 * is bounded by 0.
 *
 * @param entry The entry to decrement.
 *
 * @return void
 */
static void decrementConfidence(FPEntry *entry) {
  uint16_t decrement = (uint16_t)HELIOS_CONFIDENCE_DECREMENT;

  if (entry->confidence >= decrement) {
    entry->confidence -= decrement;
  } else {
    entry->confidence = 0;
  }
}

/**
 * @brief Return true if a given confidence level is at or above the threshold.
 *
 * @param confidence The confidence level to check.
 *
 * @return Boolean indicating if the entry is at or above the threshold.
 */
static bool isConfident(uint16_t confidence) {
  uint16_t threshold = (uint16_t)HELIOS_CONFIDENCE_THRESHOLD;
  return (confidence >= threshold);
}

/**
 * @brief Search the predictor sets for an entry pertaining to a given op's PC. If the entry is found,
 * return the predicted distance to the head of the fusion pair. The distance returned is dependent
 * on the selector table's determination and the confidence of the predictor set.
 *
 * @param tailPC The PC of a load/store that could be fused.
 * @param globalHistory The branch history up until this point.
 * @param outLocalPredicts  If not NULL, set to true if the local predictor was confident.
 * @param outGlobalPredicts If not NULL, set to true if the global predictor was confident.
 *
 * @return The predicted distance to the head of the fusion pair. 0 if no entry is found, or if
 * if the predictor set is not confident.
 */
uint16_t predictFusion(uint64_t tailPC, uint32_t globalHistory, bool *outLocalPredicts, bool *outGlobalPredicts) {
  uint32_t localIndex = getLocalIndex(tailPC);
  uint32_t globalIndex = getGlobalIndex(tailPC, globalHistory);
  uint32_t selectorIndex = getSelectorIndex(tailPC, globalHistory);
  uint32_t tag = getTag(tailPC);

  bool localPredicts = false;
  bool globalPredicts = false;

  int localEntryIndex = findEntry(localPredictor[localIndex], tag);
  int globalEntryIndex = findEntry(globalPredictor[globalIndex], tag);

  // Neither sub-predictor has seen this PC. Fusion prediction is not possible.
  if (localEntryIndex == -1 && globalEntryIndex == -1) {
    STAT_EVENT(0, HELIOS_FP_LOOKUP_MISS);
    if (outLocalPredicts)
      *outLocalPredicts = false;
    if (outGlobalPredicts)
      *outGlobalPredicts = false;
    return 0;
  }

  uint16_t localDistance = 0;
  uint16_t globalDistance = 0;

  // If the entry exists in the local predictor set, record the local prediction and distance.
  if (localEntryIndex != -1) {
    FPEntry *entry = &localPredictor[localIndex][localEntryIndex];
    localPredicts = isConfident(entry->confidence);
    localDistance = entry->distance;
    updateLRU(localPredictor[localIndex], localEntryIndex);
  }

  // If the entry exists in the global predictor set, record the global prediction and distance.
  if (globalEntryIndex != -1) {
    FPEntry *entry = &globalPredictor[globalIndex][globalEntryIndex];
    globalPredicts = isConfident(entry->confidence);
    globalDistance = entry->distance;
    updateLRU(globalPredictor[globalIndex], globalEntryIndex);
  }

  // Report each sub-predictor's decision to later be tied to the op.
  if (outLocalPredicts)
    *outLocalPredicts = localPredicts;
  if (outGlobalPredicts)
    *outGlobalPredicts = globalPredicts;

  // Determine which predictor set to use based on the selector table.
  bool useGlobal = (selectorTable[selectorIndex].count >= 2) ? true : false;

  uint16_t distanceToUse = 0;

  // Return a distance according to the selector table's determination and
  // whether the predictor set is confident.
  if (useGlobal && globalPredicts) {
    distanceToUse = globalDistance;
    STAT_EVENT(0, HELIOS_FP_LOOKUP_PREDICT);
    return distanceToUse;
  } else if (!useGlobal && localPredicts) {
    distanceToUse = localDistance;
    STAT_EVENT(0, HELIOS_FP_LOOKUP_PREDICT);
    return distanceToUse;
  } else {
    STAT_EVENT(0, HELIOS_FP_LOOKUP_LOW_CONF);
    return 0;
  }
}

/**
 * @brief Given a distance from the UCH, search the UCH for any entry with a distance and tag that match.
 * If only the tag aligns, update the entry's distance and reset the confidence to 1 (increment).
 * If the entry cannot be found, find a victim to eject and add the new entry. Set its confidence to
 * 1 (increment).
 *
 * @param opPC The PC of the operation to add//update.
 * @param globalHistory The branch history up until this point.
 * @param distance The computed distance from the UCH.
 *
 * @return void
 */
void addInstructionToPredictor(uint64_t opPC, uint32_t globalHistory, uint16_t distance) {
  uint32_t localIndex = getLocalIndex(opPC);
  uint32_t globalIndex = getGlobalIndex(opPC, globalHistory);
  uint32_t tag = getTag(opPC);

  int localEntryIndex = findEntry(localPredictor[localIndex], tag);
  int globalEntryIndex = findEntry(globalPredictor[globalIndex], tag);

  // If the entry exists in the local predictor set, update the confidence accordingly.
  if (localEntryIndex != -1) {
    FPEntry *localEntry = &localPredictor[localIndex][localEntryIndex];

    if ((localEntry->valid) && (localEntry->tag == tag) && (localEntry->distance == distance)) {
      STAT_EVENT(0, HELIOS_FP_TRAIN_SAME_DIST);
      incrementConfidence(localEntry);
    } else if ((localEntry->valid) && (localEntry->tag == tag) && (localEntry->distance != distance)) {
      STAT_EVENT(0, HELIOS_FP_TRAIN_NEW_DIST);
      localEntry->distance = distance;
      localEntry->confidence = 0;
      incrementConfidence(localEntry);
    }

    updateLRU(localPredictor[localIndex], localEntryIndex);
  }

  // If the entry cannot be found, find a victim to eject and add the new entry.
  else {
    localEntryIndex = findVictimToEject(localPredictor[localIndex]);

    FPEntry *localEntry = &localPredictor[localIndex][localEntryIndex];
    STAT_EVENT(0, HELIOS_FP_TRAIN_ALLOC);
    if (localEntry->valid)
      STAT_EVENT(0, HELIOS_FP_TRAIN_EVICT_VALID);
    localEntry->tag = tag;
    localEntry->distance = distance;
    localEntry->confidence = 0;
    incrementConfidence(localEntry);
    localEntry->valid = true;

    updateLRU(localPredictor[localIndex], localEntryIndex);
  }

  // If the entry exists in the global predictor set, update the confidence accordingly.
  if (globalEntryIndex != -1) {
    FPEntry *globalEntry = &globalPredictor[globalIndex][globalEntryIndex];

    if ((globalEntry->valid) && (globalEntry->tag == tag) && (globalEntry->distance == distance)) {
      STAT_EVENT(0, HELIOS_FP_TRAIN_SAME_DIST);
      incrementConfidence(globalEntry);
    } else if ((globalEntry->valid) && (globalEntry->tag) == tag && (globalEntry->distance != distance)) {
      STAT_EVENT(0, HELIOS_FP_TRAIN_NEW_DIST);
      globalEntry->distance = distance;
      globalEntry->confidence = 0;
      incrementConfidence(globalEntry);
    }

    updateLRU(globalPredictor[globalIndex], globalEntryIndex);
  }

  // If the entry cannot be found, find a victim to eject and add the new entry.
  else {
    globalEntryIndex = findVictimToEject(globalPredictor[globalIndex]);

    FPEntry *globalEntry = &globalPredictor[globalIndex][globalEntryIndex];
    STAT_EVENT(0, HELIOS_FP_TRAIN_ALLOC);
    if (globalEntry->valid)
      STAT_EVENT(0, HELIOS_FP_TRAIN_EVICT_VALID);
    globalEntry->tag = tag;
    globalEntry->distance = distance;
    globalEntry->confidence = 0;
    incrementConfidence(globalEntry);
    globalEntry->valid = true;

    updateLRU(globalPredictor[globalIndex], globalEntryIndex);
  }
}

/**
 * @brief Penalizes the predictor for a confirmed fusion misprediction that was
 * not handled in place. Rewarding the predictor is handled in addInstructionToPredictor
 * when a distance is computed.
 *
 * Both sub-predictors are decremented on a misprediction.
 *
 * @param opPC The PC of the (tail) operation that mispredicted.
 * @param globalHistory The branch history up until this point.
 *
 * @return void
 */
void updatePredictor(uint64_t opPC, uint32_t globalHistory) {
  STAT_EVENT(0, HELIOS_FP_PENALIZE);
  uint32_t localIndex = getLocalIndex(opPC);
  uint32_t globalIndex = getGlobalIndex(opPC, globalHistory);
  uint32_t tag = getTag(opPC);

  int localEntryIndex = findEntry(localPredictor[localIndex], tag);
  int globalEntryIndex = findEntry(globalPredictor[globalIndex], tag);

  // Decrement the local entry's confidence, if present.
  if (localEntryIndex != -1) {
    FPEntry *entry = &localPredictor[localIndex][localEntryIndex];
    decrementConfidence(entry);
    updateLRU(localPredictor[localIndex], localEntryIndex);
  }

  // Decrement the global entry's confidence, if present.
  if (globalEntryIndex != -1) {
    FPEntry *entry = &globalPredictor[globalIndex][globalEntryIndex];
    decrementConfidence(entry);
    updateLRU(globalPredictor[globalIndex], globalEntryIndex);
  }
}

/**
 * @brief Takes an op's sub-predictions and the actual fusion outcome to update the selector table.
 * The selector is nudged toward whichever sub-predictor agreed with reality. When both agree or disagree,
 * the selector is left unchanged.
 *
 * @param opPC The PC of the (tail) operation.
 * @param globalHistory The branch history up until this point.
 * @param localPredictedFusion  Whether the local sub-predictor predicted fusion for this op.
 * @param globalPredictedFusion Whether the global sub-predictor predicted fusion for this op.
 * @param actuallyFused Whether the predicted fusion was validated as real.
 *
 * @return void
 */
void updateSelector(uint64_t opPC, uint32_t globalHistory, bool localPredictedFusion, bool globalPredictedFusion,
                    bool actuallyFused) {
  uint32_t selectorIndex = getSelectorIndex(opPC, globalHistory);

  bool localRight = (localPredictedFusion == actuallyFused);
  bool globalRight = (globalPredictedFusion == actuallyFused);

  // Decrement the selector count if the local predictor was correct and the global predictor was not.
  if (localRight && !globalRight) {
    if (selectorTable[selectorIndex].count > 0) {
      selectorTable[selectorIndex].count--;
    }
  }

  // Increment the selector count if the global predictor was correct and the local predictor was not.
  else if (globalRight && !localRight) {
    if (selectorTable[selectorIndex].count < 3) {
      selectorTable[selectorIndex].count++;
    }
  }
}

/**
 * @brief Initializes the predictor sets and selector table.
 *
 * @param void
 *
 * @return void
 */
void predictorInit(void) {
  memset(localPredictor, 0, sizeof(localPredictor));
  memset(globalPredictor, 0, sizeof(globalPredictor));
  memset(selectorTable, 0, sizeof(selectorTable));
}