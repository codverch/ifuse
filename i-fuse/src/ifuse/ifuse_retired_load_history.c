#include "ifuse_retired_load_history.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../general.param.h"
#include "../statistics.h"
#include "ifuse.param.h"

#define HISTORY_LINE_SIZE   64U
#define HISTORY_OFFSET_BITS 6U  // Log2 of HISTORY_LINE_SIZE

/*
 * Retired Load Buffer (RLB)
 * -------------------------
 *
 * The Retired Load Buffer (RLB) tracks recently retired loads. When a
 * new load retires, the RLB checks whether a load within the previous
 * IFUSE_FUSION_DISTANCE micro-ops accessed the same cache block. If
 * so, the two loads form a fusion candidate, which goes to the
 * training table.
 *
 * Each RLB entry is 93 bits:
 *
 * - 48-bit load PC
 * - 28-bit line tag
 * - 6-bit line offset
 * - 10-bit retirement timestamp
 * - 1-bit valid bit
 *
 * Each field is sized to match its consumer. The PC is kept at full
 * width because the FCT picks its set from the full LD1 PC. A narrower
 * PC would send runtime-trained rows to different sets. For the
 * address, the RLB only needs to identify the cache block and recover
 * LD1's offset. The RLB does not store LD2's access size, because it
 * is available when LD2 retires.
 *
 * The timestamp is a 10-bit retirement counter that increments on
 * every retired micro-op, not just on loads. This matches the
 * definition of fusion distance. In the simulator, op->op_num provides
 * the same quantity: it counts on-path micro-ops and resets after
 * recovery. In hardware, an entry expires when the current counter
 * minus its timestamp, modulo 1024, reaches IFUSE_FUSION_DISTANCE.
 * Note: The simulator keeps the full op_num and compares without the
 * modulo; clock_check() confirms that 10 bits would suffice.
 *
 * Organization
 * ------------
 *
 * The RLB is a small set-associative structure with 128 entries and
 * 8 ways, giving 16 sets:
 *
 * - Index: Low bits of the cache-line address select the set (VA[9:6])
 * - Tag: The next 28 bits identify the line within the set (VA[37:10])
 * - Valid: One bit per entry
 * - Replacement: An invalid way if available, else the oldest entry
 *
 * The index is simple bit selection, as in a conventional L1-D cache.
 * An XOR-based index could reduce conflicts from power-of-two access
 * patterns, but the extra logic is unnecessary unless capacity
 * evictions show significant set imbalance.
 *
 * The tag is deliberately partial. Together, the index and tag
 * identify the low 32 bits of the line address, matching the partial
 * line tag in Helios's commit-time history. Consequently, two lines
 * that differ only above bit 37 can alias. Such an alias can create a
 * spurious fusion candidate, but it does not affect correctness: the
 * candidate must be observed ifuse_training_insert_threshold times
 * before reaching the FCT, and the fused load verifies the predicted
 * address when it executes.
 *
 * The RLB differs from a conventional cache in three ways. First, a
 * hit consumes the entry. Once an older load pairs with a newer load,
 * the RLB clears the entry, because it cannot participate in another
 * fusion. Second, entries expire after IFUSE_FUSION_DISTANCE micro-ops.
 * Third, a store invalidates the entry for the same cache block, since
 * the older load's value may no longer be valid. With partial tags, a
 * store may also invalidate an aliased entry. This can only remove a
 * training opportunity. It cannot cause incorrect execution.
 *
 * Replacement
 * -----------
 *
 * An entry becomes invalid when it has never been filled, has been
 * consumed by a matching load, has been invalidated by a store, or has
 * expired. Inserting a retiring load therefore follows three cases.
 *
 * 1. Existing entry: If the block already has a live entry, overwrite
 *    it. A block never needs two RLB entries, so a lookup matches at
 *    most one way.
 *
 * 2. Invalid way: If the set contains an invalid way, fill it. This is
 *    not a capacity eviction, because no useful entry is displaced.
 *
 * 3. All ways valid: If every way holds a live entry, evict the oldest
 *    one and count an RLB_CAPACITY_EVICTION. This is a genuine capacity
 *    loss: the evicted load could still have formed a fusion pair.
 *    Evicting the oldest entry minimizes that loss, because it has the
 *    least remaining lifetime before expiration.
 */

/*
 * One entry: the hardware fields above, held in wider host types
 * (the tag is masked to 28 bits, and micro_op_num is a full counter;
 * see clock_check()). full_line is simulator-only and counts tag
 * aliases.
 */
typedef struct HistoryRow {
    uint64_t pc;
    uint64_t tag;           // Line-address bits just above the index
    uint8_t  offset;        // Byte offset within the line
    Counter  micro_op_num;
    bool     valid;
    uint64_t full_line;     // Simulator only, detects partial-tag aliasing
} HistoryRow;

static HistoryRow* rows;  // All ways of set 0, then all ways of set 1, etc.
static unsigned int history_num_sets;
static unsigned int history_num_ways;
static unsigned int history_index_bits;  // Log2 of history_num_sets
static uint64_t history_tag_mask;
static Counter last_micro_op_num;  // Newest op count seen, for clock_check()
static bool initialized;

static uint64_t low_bits_mask(unsigned int bits) {
    return bits >= 64U ? ~0ULL : (1ULL << bits) - 1ULL;
}

static uint64_t line_addr(Addr addr) {
    return (uint64_t)addr >> HISTORY_OFFSET_BITS;
}

static unsigned int set_for(Addr addr) {
    return (unsigned int)(line_addr(addr) & (history_num_sets - 1U));
}

static uint64_t tag_for(Addr addr) {
    return (line_addr(addr) >> history_index_bits) & history_tag_mask;
}

static HistoryRow* row_at(unsigned int set, unsigned int way) {
    return &rows[set * history_num_ways + way];
}

void retired_load_history_init(void) {
    if (!IFUSE_FUSION_DISTANCE) {
        fprintf(stderr,
                "Runtime I-Fuse fusion distance must be >= 1 micro-op\n");
        exit(1);
    }
    if (!IFUSE_RLB_CAPACITY || !IFUSE_RLB_WAYS ||
        IFUSE_RLB_CAPACITY % IFUSE_RLB_WAYS) {
        fprintf(stderr,
                "Runtime I-Fuse RLB capacity must be a nonzero multiple of "
                "ifuse_rlb_ways\n");
        exit(1);
    }
    unsigned int sets = IFUSE_RLB_CAPACITY / IFUSE_RLB_WAYS;
    if (sets & (sets - 1U)) {
        fprintf(stderr,
                "Runtime I-Fuse RLB set count (capacity / ways = %u) must be "
                "a power of two\n",
                sets);
        exit(1);
    }

    unsigned int index_bits = 0;
    while ((1U << index_bits) < sets)
        ++index_bits;
    const unsigned int max_line_bits = 64U - HISTORY_OFFSET_BITS;
    if (IFUSE_RLB_LINE_TAG_BITS <= index_bits ||
        IFUSE_RLB_LINE_TAG_BITS > max_line_bits) {
        fprintf(stderr,
                "Runtime I-Fuse RLB line tag bits must be in [%u, %u] "
                "(more than the %u index bits)\n",
                index_bits + 1U, max_line_bits, index_bits);
        exit(1);
    }

    free(rows);
    history_num_sets = sets;
    history_num_ways = IFUSE_RLB_WAYS;
    history_index_bits = index_bits;
    history_tag_mask = low_bits_mask(IFUSE_RLB_LINE_TAG_BITS - index_bits);
    rows = (HistoryRow*)calloc(IFUSE_RLB_CAPACITY, sizeof(*rows));
    if (!rows) {
        fprintf(stderr,
                "Could not allocate I-Fuse retired load history (%u entries)\n",
                IFUSE_RLB_CAPACITY);
        exit(1);
    }
    last_micro_op_num = 0;
    initialized = true;
}

/*
 * Loads retire in program order, so the incoming micro-op count only
 * moves forward. The one exception is a simulator artifact. When fast
 * warmup (--warmup) hands over to the timing model, sim.c resets
 * op_count, and op_num restarts from 1. Every stored entry then
 * carries a timestamp "from the future." Such an entry never looks old
 * enough to expire and never looks older than the current load, so it
 * would sit in its way forever. These entries belong to a different
 * timeline, so clock_check() drops them all. Real hardware has no
 * equivalent.
 */
static void clock_check(Counter current_micro_op_num) {
    if (current_micro_op_num < last_micro_op_num)
        memset(rows, 0, IFUSE_RLB_CAPACITY * sizeof(*rows));
    last_micro_op_num = current_micro_op_num;
}

/*
 * Clears the entries in set that are too old to fuse with any load
 * retiring now. Hardware gets the same effect by checking each way's
 * age on every access. Clearing them here keeps the simulator's valid
 * bits accurate (retired_load_history_insert() relies on them) and
 * counts RLB_AGE_EVICTIONS.
 */
static void expire_set(unsigned int set, Counter current_micro_op_num) {
    for (unsigned int way = 0; way < history_num_ways; ++way) {
        HistoryRow* row = row_at(set, way);
        Counter older = row->micro_op_num;
        if (row->valid && current_micro_op_num > older &&
            current_micro_op_num - older >= IFUSE_FUSION_DISTANCE) {
            STAT_EVENT(0, RLB_AGE_EVICTIONS);
            row->valid = false;
        }
    }
}

/*
 * Records a retired load that did not match anything. See
 * "Replacement" above for how the RLB chooses the way.
 */
void retired_load_history_insert(Addr pc, Addr effective_addr,
                                 Counter micro_op_num) {
    if (!initialized)
        retired_load_history_init();
    clock_check(micro_op_num);

    unsigned int set = set_for(effective_addr);
    uint64_t tag = tag_for(effective_addr);
    expire_set(set, micro_op_num);

    /*
     * Step 1 picks this line's own entry. Step 2 picks an invalid way.
     * Step 3, if neither exists, picks the oldest way.
     */
    HistoryRow* same_line = NULL;
    HistoryRow* invalid = NULL;
    HistoryRow* oldest = NULL;
    for (unsigned int way = 0; way < history_num_ways; ++way) {
        HistoryRow* candidate = row_at(set, way);
        if (!candidate->valid) {
            if (!invalid)
                invalid = candidate;
        } else if (candidate->tag == tag) {
            same_line = candidate;
        } else if (!oldest ||
                   candidate->micro_op_num < oldest->micro_op_num) {
            oldest = candidate;
        }
    }

    HistoryRow* row = same_line ? same_line : invalid;
    if (!row) {
        STAT_EVENT(0, RLB_CAPACITY_EVICTIONS);
        row = oldest;
    }

    row->pc = (uint64_t)pc;
    row->tag = tag;
    row->offset = (uint8_t)(effective_addr & (HISTORY_LINE_SIZE - 1U));
    row->micro_op_num = micro_op_num;
    row->valid = true;
    row->full_line = line_addr(effective_addr);
}

/*
 * Looks for a live entry in the same line as effective_addr. On a hit,
 * fills in *match and clears the entry, because the hit consumes it.
 */
bool retired_load_history_take_match(Addr effective_addr,
                                     Counter micro_op_num,
                                     RetiredLoadHistoryEntry* match) {
    if (!initialized)
        retired_load_history_init();
    clock_check(micro_op_num);

    unsigned int set = set_for(effective_addr);
    uint64_t tag = tag_for(effective_addr);
    expire_set(set, micro_op_num);

    /*
     * This is a plain cache lookup. retired_load_history_insert() keeps
     * one entry per line, so at most one way can hit. The entry must
     * also be strictly older than the load looking it up, so a load
     * never pairs with itself.
     */
    for (unsigned int way = 0; way < history_num_ways; ++way) {
        HistoryRow* row = row_at(set, way);
        if (row->valid && row->tag == tag &&
            row->micro_op_num < micro_op_num) {
            /*
             * The older load's address is this load's line plus the
             * stored offset. Hardware knows exactly this much too.
             */
            match->pc = (Addr)row->pc;
            match->effective_addr =
                (effective_addr & ~(Addr)(HISTORY_LINE_SIZE - 1U)) |
                row->offset;
            match->micro_op_num = row->micro_op_num;
            if (row->full_line != line_addr(effective_addr))
                STAT_EVENT(0, RLB_TAG_ALIAS_MATCHES);
            row->valid = false;
            return true;
        }
    }
    return false;
}

/*
 * A store wrote to this line, so an older load's value may be stale.
 * Clears the line's entry.
 */
void retired_load_history_invalidate_block(Addr effective_addr) {
    if (!initialized)
        retired_load_history_init();

    unsigned int set = set_for(effective_addr);
    uint64_t tag = tag_for(effective_addr);
    for (unsigned int way = 0; way < history_num_ways; ++way) {
        HistoryRow* row = row_at(set, way);
        if (row->valid && row->tag == tag)
            row->valid = false;
    }
}

void retired_load_history_invalidate_range(Addr effective_addr, uns mem_size) {
    if (!mem_size) {
        retired_load_history_invalidate_block(effective_addr);
        return;
    }

    const Addr line_mask = ~(Addr)(HISTORY_LINE_SIZE - 1U);
    Addr first = effective_addr & line_mask;
    Addr last_addr = effective_addr + (Addr)mem_size - 1U;
    if (last_addr < effective_addr)
        last_addr = ~(Addr)0;
    Addr last = last_addr & line_mask;

    for (Addr block = first;; block += HISTORY_LINE_SIZE) {
        retired_load_history_invalidate_block(block);
        if (block == last || block > ~(Addr)0 - HISTORY_LINE_SIZE)
            break;
    }
}
