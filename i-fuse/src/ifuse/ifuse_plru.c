#include "ifuse_plru.h"

#include <stdbool.h>

/**
 * Each state bit names the subtree to evict next: 0 for left, 1 for
 * right.
 */
void ifuse_plru_touch(uint8_t* state, unsigned int set_idx,
                      unsigned int way, unsigned int num_ways) {
    if (!state || (num_ways != 4U && num_ways != 8U) || way >= num_ways)
        return;

    uint8_t value = state[set_idx];
    unsigned int node = 0;
    unsigned int span = num_ways;
    unsigned int local_way = way;

    while (span > 1U) {
        unsigned int half = span / 2U;
        if (local_way < half) {
            value |= (uint8_t)(1U << node);  /* Touched left, evict right */
            node = node * 2U + 1U;
        } else {
            value &= (uint8_t)~(1U << node); /* Touched right, evict left */
            local_way -= half;
            node = node * 2U + 2U;
        }
        span = half;
    }
    state[set_idx] = value;
}

unsigned int ifuse_plru_victim(const uint8_t* state, unsigned int set_idx,
                               unsigned int num_ways) {
    if (!state || (num_ways != 4U && num_ways != 8U))
        return 0U;

    uint8_t value = state[set_idx];
    unsigned int node = 0;
    unsigned int span = num_ways;
    unsigned int victim = 0;

    while (span > 1U) {
        unsigned int half = span / 2U;
        bool choose_right = (value & (uint8_t)(1U << node)) != 0;
        if (choose_right) {
            victim += half;
            node = node * 2U + 2U;
        } else {
            node = node * 2U + 1U;
        }
        span = half;
    }
    return victim;
}
