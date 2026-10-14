/* Scarab parameters read by heliosHistory.c and heliosFusionPredictor.c.
 * helios.param.h declares them const, so they are defined here, in a file that
 * does not include it, and harness.c sets them per test. Defaults match
 * src/helios/helios.param.def. */

#include "globals/global_types.h"

uns HELIOS_FUSION_WINDOW = 64;
uns HELIOS_CONFIDENCE_THRESHOLD = 3;
uns HELIOS_CONFIDENCE_INCREMENT = 1;
uns HELIOS_CONFIDENCE_DECREMENT = 3;
