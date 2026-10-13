#!/usr/bin/env python3
"""Write a deterministic synthetic op stream for harness.c.

Loads concentrate on a few hot cache blocks so pairs are common, and the
stream mixes in the cases the pass-1 rules care about: stores to hot blocks,
block-crossing loads, loads without a destination, zero addresses, off-path
ops, and latencies long enough that LOAD1 often completes after LOAD2 maps.
"""

import random
import sys


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 50000
    rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
    blocks = [0x10000 + 64 * i for i in range(32)]
    for _ in range(count):
        r = rng.random()
        off = 1 if rng.random() < 0.05 else 0
        pc = 0x400000 + 4 * rng.randrange(256)
        if r < 0.45:
            va = rng.choice(blocks) + rng.randrange(64)
            size = rng.choice([1, 2, 4, 8, 8, 8, 16])
            ndst = 0 if rng.random() < 0.03 else 1
            if rng.random() < 0.01:
                va = 0
            print(f"L {pc:#x} {va:#x} {size} {ndst} {rng.choice([4, 5, 12, 40, 300])} {off}")
        elif r < 0.53:
            va = rng.choice(blocks) + rng.randrange(64)
            print(f"S {pc:#x} {va:#x} 8 0 1 {off}")
        else:
            print(f"O {pc:#x} 0 0 1 1 {off}")


if __name__ == "__main__":
    main()
