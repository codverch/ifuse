#ifndef IFUSE_PLRU_H
#define IFUSE_PLRU_H

#include <stdint.h>

/**
 * Tree-PLRU replacement for 4-way or 8-way set-associative I-Fuse
 * tables.
 */
void ifuse_plru_touch(uint8_t* state, unsigned int set_idx,
                      unsigned int way, unsigned int num_ways);
unsigned int ifuse_plru_victim(const uint8_t* state, unsigned int set_idx,
                               unsigned int num_ways);

#endif
