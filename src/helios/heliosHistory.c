/**
 * @file heliosHistory.c
 *
 * @brief Load Unfused Committed History (paper Sec. IV-A1).
 *
 * @author Ved Lakshminarayanan <vlakshmi@andrew.cmu.edu>
 *
 * Six fully associative entries, invalid-first then oldest replacement. Like
 * the paper's tag match, a pair trains only if both loads touch the same line.
 * Tags are full line addresses and commit numbers are full op numbers, not
 * the paper's 32-bit and 7-bit fields.
 */

#include "heliosHistory.h"

#include <string.h>

#include "globals/global_types.h"

#include "helios.param.h"

static LoadUCHEntry loadUCH[UCH_LOAD_SIZE];

void initLoadUCH(void) {
  memset(loadUCH, 0, sizeof(loadUCH));
}

bool canFuse(uint64_t address1, uint64_t address2, uint8_t size1, uint8_t size2) {
  return isContiguous(address1, address2, size1, size2) || isSameCacheBlock(address1, address2, size1, size2) ||
         isNextBlock(address1, address2, size1, size2);
}

static int findVictim(void) {
  int victim = 0;
  for (int i = 0; i < UCH_LOAD_SIZE; i++) {
    if (!loadUCH[i].valid)
      return i;
    if (loadUCH[i].commitNumber < loadUCH[victim].commitNumber)
      victim = i;
  }
  return victim;
}

uint16_t updateLoadUCH(uint64_t pc, uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber,
                       uint64_t* headPC) {
  uint64_t block = getCacheBlockAddress(virtualAddress);
  LoadUCHEntry* match = NULL;
  for (int i = 0; i < UCH_LOAD_SIZE; i++) {
    LoadUCHEntry* e = &loadUCH[i];
    if (!e->valid || e->accessSize == 0 || e->commitNumber >= commitNumber || e->cacheBlockAddress != block)
      continue;
    if (commitNumber - e->commitNumber > HELIOS_FUSION_WINDOW)
      continue;
    if (!match || e->commitNumber > match->commitNumber)
      match = e;
  }

  if (headPC)
    *headPC = match ? match->pc : 0;
  if (match) {
    // A load fuses with one other load only.
    match->valid = false;
    return (uint16_t)(commitNumber - match->commitNumber);
  }

  LoadUCHEntry* e = &loadUCH[findVictim()];
  e->pc = pc;
  e->cacheBlockAddress = block;
  e->commitNumber = commitNumber;
  e->accessSize = accessSize;
  e->valid = true;
  return 0;
}
