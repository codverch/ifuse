#ifndef __IFUSE_PARAM_H__
#define __IFUSE_PARAM_H__

#include "globals/global_types.h"

#define DEF_PARAM(name, variable, type, func, def, const) extern const type variable;
#include "ifuse.param.def"
#undef DEF_PARAM

#endif