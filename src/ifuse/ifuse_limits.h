#ifndef IFUSE_LIMITS_H
#define IFUSE_LIMITS_H

/**
 * Compile-time upper bounds for tunable I-Fuse table parameters. The
 * runtime sizes come from the IFUSE_* knobs in ifuse.param.def.
 */

#define IFUSE_FCT_MAX_HASH_BITS     24U
#define IFUSE_MAX_BUCKETS           65536U
#define IFUSE_MAX_NODES             (1U << 24)

#endif /* IFUSE_LIMITS_H */
