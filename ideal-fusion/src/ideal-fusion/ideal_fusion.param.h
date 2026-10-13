#ifndef __IDEAL_FUSION_PARAM_H__
#define __IDEAL_FUSION_PARAM_H__

#include "globals/global_types.h"

#define DEF_PARAM(name, variable, type, func, def, const) extern const type variable;
#include "ideal-fusion/ideal_fusion.param.def"
#undef DEF_PARAM

#endif /* __IDEAL_FUSION_PARAM_H__ */
