#include "ifuse_load_tracker.h"

static uint64_t ifuse_load_num = 0;

uint64_t ifuse_next_load_num(void) {
    return ++ifuse_load_num;
}
