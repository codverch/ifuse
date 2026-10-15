#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ifuse/ifuse_plru.h"
#include "ifuse/ifuse_retired_load_history.h"

const uns IFUSE_FUSION_DISTANCE = 512;
const uns IFUSE_RLB_CAPACITY = 128;
const uns IFUSE_RLB_WAYS = 8;
const uns IFUSE_RLB_LINE_TAG_BITS = 32;
const uns IFUSE_TRAINING_TABLE_PC_TAG_BITS = 32;
const uns IFUSE_FCT_PC_TAG_BITS = 32;

static void test_micro_op_distance(void) {
    RetiredLoadHistoryEntry match;

    retired_load_history_init();
    retired_load_history_insert(0x1000, 0x2040, 10);
    assert(retired_load_history_take_match(0x2048, 521, &match));
    assert(match.pc == 0x1000);

    retired_load_history_init();
    retired_load_history_insert(0x1000, 0x2040, 10);
    assert(!retired_load_history_take_match(0x2048, 522, &match));
}

static void test_most_recent_and_store_invalidation(void) {
    RetiredLoadHistoryEntry match;

    /*
     * A second load to the same block overwrites the first, so the
     * block has one entry: the newer load.
     */
    retired_load_history_init();
    retired_load_history_insert(0x1000, 0x3000, 10);
    retired_load_history_insert(0x2000, 0x3008, 20);
    assert(retired_load_history_take_match(0x3010, 30, &match));
    assert(match.pc == 0x2000);
    assert(!retired_load_history_take_match(0x3018, 31, &match));

    retired_load_history_init();
    retired_load_history_insert(0x1000, 0x4000, 10);
    retired_load_history_invalidate_block(0x4018);
    assert(!retired_load_history_take_match(0x4020, 20, &match));

    retired_load_history_init();
    retired_load_history_insert(0x1000, 0x4038, 10);
    retired_load_history_insert(0x2000, 0x4040, 11);
    retired_load_history_invalidate_range(0x403c, 8);
    assert(!retired_load_history_take_match(0x4038, 20, &match));
    assert(!retired_load_history_take_match(0x4040, 20, &match));
}

/*
 * Must match set_for() in ifuse_retired_load_history.c, which selects
 * the set from the low line-address bits.
 */
static unsigned int test_set_for(Addr block) {
    return (unsigned int)((block >> 6) & (IFUSE_RLB_CAPACITY / IFUSE_RLB_WAYS - 1U));
}

static void test_set_conflict_eviction(void) {
    RetiredLoadHistoryEntry match;
    Addr same_set[IFUSE_RLB_WAYS + 1];
    Addr other_set = 0;
    unsigned int found = 0;

    /*
     * Find one more block than a set can hold, all in the same set, plus
     * one block in a different set.
     */
    unsigned int target = test_set_for(0x100000);
    for (Addr block = 0x100000; found < IFUSE_RLB_WAYS + 1 || !other_set;
         block += 0x40) {
        if (test_set_for(block) == target) {
            if (found < IFUSE_RLB_WAYS + 1)
                same_set[found++] = block;
        } else if (!other_set)
            other_set = block;
    }

    retired_load_history_init();
    retired_load_history_insert(0x8000, other_set, 1);
    for (unsigned int i = 0; i < IFUSE_RLB_WAYS; ++i)
        retired_load_history_insert(0x5000 + i, same_set[i], i + 2);

    /*
     * The set is now full. Inserting one more block evicts the set's
     * oldest entry, even though that entry is nowhere near
     * IFUSE_FUSION_DISTANCE old and the rest of the RLB is almost empty.
     */
    retired_load_history_insert(0x9000, same_set[IFUSE_RLB_WAYS],
                                IFUSE_RLB_WAYS + 2);
    assert(!retired_load_history_take_match(same_set[0] + 8, 100, &match));

    /* Nothing else evicted, in this set or any other */
    for (unsigned int i = 1; i < IFUSE_RLB_WAYS; ++i) {
        assert(retired_load_history_take_match(same_set[i] + 8, 100, &match));
        assert(match.pc == 0x5000 + i);
    }
    assert(retired_load_history_take_match(same_set[IFUSE_RLB_WAYS] + 8, 100,
                                           &match));
    assert(match.pc == 0x9000);
    assert(retired_load_history_take_match(other_set + 8, 100, &match));
    assert(match.pc == 0x8000);
}

static void test_clock_going_backwards_clears(void) {
    RetiredLoadHistoryEntry match;

    /*
     * Fast warmup ends, and op_num restarts from 1. The warmup entry's
     * timestamp is now in the future. It must not linger or match.
     */
    retired_load_history_init();
    retired_load_history_insert(0x1000, 0x6000, 5000);
    assert(!retired_load_history_take_match(0x6008, 3, &match));
    retired_load_history_insert(0x2000, 0x6000, 4);
    assert(retired_load_history_take_match(0x6010, 5, &match));
    assert(match.pc == 0x2000);
}

static void test_narrow_fields(void) {
    RetiredLoadHistoryEntry match;

    /*
     * The RLB keeps the full PC. The older load's address comes back as
     * the matching load's line plus the stored offset.
     */
    retired_load_history_init();
    retired_load_history_insert(0x7f12345678ABULL, 0x7000 + 0x18, 10);
    assert(retired_load_history_take_match(0x7000 + 0x30, 20, &match));
    assert(match.pc == 0x7f12345678ABULL);
    assert(match.effective_addr == 0x7018);
    assert(match.micro_op_num == 10);
}

static void test_partial_tag_aliasing(void) {
    RetiredLoadHistoryEntry match;

    /*
     * Index plus tag cover line-address bits [31:0] (i.e., VA[37:6]).
     * By design, lines that differ only above VA bit 37 look identical.
     */
    Addr line = 0x8000;
    Addr alias = line + (1ULL << 38);
    retired_load_history_init();
    retired_load_history_insert(0x1000, line + 8, 10);
    assert(retired_load_history_take_match(alias, 20, &match));
    assert(match.effective_addr == alias + 8);

    /* VA bit 37 (one bit lower) is still inside the tag, so no match */
    retired_load_history_init();
    retired_load_history_insert(0x1000, line + 8, 10);
    assert(!retired_load_history_take_match(line + (1ULL << 37), 20, &match));
}

static void test_plru(void) {
    uint8_t state[1] = {0};
    ifuse_plru_touch(state, 0, 0, 4);
    assert(ifuse_plru_victim(state, 0, 4) != 0);
    ifuse_plru_touch(state, 0, 7, 8);
    assert(ifuse_plru_victim(state, 0, 8) != 7);
}

int main(void) {
    test_micro_op_distance();
    test_most_recent_and_store_invalidation();
    test_set_conflict_eviction();
    test_clock_going_backwards_clears();
    test_narrow_fields();
    test_partial_tag_aliasing();
    test_plru();
    puts("runtime I-Fuse history tests passed");
    return 0;
}
