#ifndef __HELIOS_PARAM_H__
#define __HELIOS_PARAM_H__

#include "globals/global_types.h"

#define DEF_PARAM(name, variable, type, func, def, const) extern const type variable;
#include "helios/helios.param.def"
#undef DEF_PARAM

#endif /* __HELIOS_PARAM_H__ */
