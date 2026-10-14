/**
 * @file heliosHistory.h
 *
 * @brief Load Unfused Committed History (UCH) and the address checks for a
 * fusible pair.
 *
 * @author Ved Lakshminarayanan <vlakshmi@andrew.cmu.edu>
 */

#ifndef HELIOS_HISTORY_H
#define HELIOS_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

/* Paper Sec. IV-A1: 6-entry fully associative load UCH. */
#define UCH_LOAD_SIZE 6

#define CACHEBLOCK_SIZE 64
#define CACHEBLOCK_BITS 6

typedef struct {
  uint64_t pc;
  uint64_t cacheBlockAddress;
  uint64_t commitNumber;
  uint8_t accessSize;  // 0 never matches
  bool valid;
} LoadUCHEntry;

#ifdef __cplusplus
extern "C" {
#endif

void initLoadUCH(void);

/* A committing unfused load searches the UCH for the youngest older load to the
 * same line within the fusion window. On a match it returns the distance and
 * invalidates the match; otherwise it inserts the load and returns 0. */
uint16_t updateLoadUCH(uint64_t pc, uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber,
                       uint64_t* headPC);

/* Can one access serve both loads? Contiguous, same line, or a span under one
 * line across two lines. */
bool canFuse(uint64_t address1, uint64_t address2, uint8_t size1, uint8_t size2);

#ifdef __cplusplus
}
#endif

static inline uint64_t getCacheBlockAddress(uint64_t virtualAddress) {
  return virtualAddress & ~((1ULL << CACHEBLOCK_BITS) - 1);
}

/* One access ends where (or after) the other starts. */
static inline bool isContiguous(uint64_t address1, uint64_t address2, uint8_t size1, uint8_t size2) {
  if (address1 <= address2)
    return address2 - address1 <= size1;
  return address1 - address2 <= size2;
}

/* Both accesses lie entirely in one line. */
static inline bool isSameCacheBlock(uint64_t address1, uint64_t address2, uint8_t size1, uint8_t size2) {
  uint64_t block = getCacheBlockAddress(address1);
  return block == getCacheBlockAddress(address2) && block == getCacheBlockAddress(address1 + size1 - 1) &&
         block == getCacheBlockAddress(address2 + size2 - 1);
}

/* The two accesses together span less than one line. */
static inline bool isNextBlock(uint64_t address1, uint64_t address2, uint8_t size1, uint8_t size2) {
  uint64_t end1 = address1 + size1 - 1;
  uint64_t end2 = address2 + size2 - 1;
  return (address1 <= address2 && end2 - address1 < CACHEBLOCK_SIZE) ||
         (address2 <= address1 && end1 - address2 < CACHEBLOCK_SIZE);
}

#endif
