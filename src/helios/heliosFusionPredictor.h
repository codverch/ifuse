/**
 * @file heliosFusionPredictor.h
 *
 * @brief Fusion predictor: a tournament of a PC-indexed and a PC xor
 * history-indexed table (paper Sec. IV-A2).
 *
 * @author Ved Lakshminarayanan <vlakshmi@andrew.cmu.edu>
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef HELIOS_PREDICTOR_H
#define HELIOS_PREDICTOR_H

#define HELIOS_NUM_CACHE_SETS 512
#define HELIOS_NUM_CACHE_WAYS 4
#define HELIOS_SEL_TABLE_SIZE 2048
#define HELIOS_PC_BITS 9
#define HELIOS_SEL_BITS 11
#define HELIOS_TAG_BITS 8

typedef struct {
  uint16_t tag;
  uint16_t distance;
  uint16_t confidence;
  uint64_t lruCount;
  bool valid;
} FPEntry;

typedef struct {
  uint16_t count;
} SelTableEntry;

extern FPEntry localPredictor[HELIOS_NUM_CACHE_SETS][HELIOS_NUM_CACHE_WAYS];
extern FPEntry globalPredictor[HELIOS_NUM_CACHE_SETS][HELIOS_NUM_CACHE_WAYS];
extern SelTableEntry selectorTable[HELIOS_SEL_TABLE_SIZE];

uint16_t predictFusion(uint64_t tailPC, uint32_t globalHistory, bool *outLocalPredicts, bool *outGlobalPredicts);
void addInstructionToPredictor(uint64_t opPC, uint32_t globalHistory, uint16_t distance);
void updatePredictor(uint64_t opPC, uint32_t globalHistory);
void updateSelector(uint64_t opPC, uint32_t globalHistory, bool localPredictedFusion, bool globalPredictedFusion,
                    bool actuallyFused);
void predictorInit(void);

static inline uint32_t getLocalIndex(uint64_t PC) {
  return (PC >> 2) & ((1 << HELIOS_PC_BITS) - 1);
}

static inline uint32_t getGlobalIndex(uint64_t PC, uint32_t branchHistory) {
  uint32_t pcBits = (PC >> 2) & ((1 << HELIOS_PC_BITS) - 1);
  uint32_t historyBits = branchHistory & ((1 << HELIOS_PC_BITS) - 1);
  return pcBits ^ historyBits;
}

static inline uint32_t getSelectorIndex(uint64_t PC, uint32_t branchHistory) {
  uint32_t pcBits = (PC >> 2) & ((1 << HELIOS_SEL_BITS) - 1);
  uint32_t historyBits = branchHistory & ((1 << HELIOS_SEL_BITS) - 1);
  return pcBits ^ historyBits;
}

static inline uint32_t getTag(uint64_t PC) {
  return (PC >> (HELIOS_PC_BITS + 2)) & ((1 << HELIOS_TAG_BITS) - 1);
}

#endif