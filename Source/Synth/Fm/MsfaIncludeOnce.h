#pragma once

// sin.h, exp2.h, freqlut.h and lfo.h in the vendored msfa core have no
// include guards (they were only ever compiled as standalone .cc files), so
// Fanfare wraps them in its own once-guards for header use.

#ifndef FANFARE_MSFA_SIN_H
#define FANFARE_MSFA_SIN_H
#include "msfa/sin.h"
#endif

#ifndef FANFARE_MSFA_EXP2_H
#define FANFARE_MSFA_EXP2_H
#include "msfa/exp2.h"
#endif

#ifndef FANFARE_MSFA_FREQLUT_H
#define FANFARE_MSFA_FREQLUT_H
#include "msfa/freqlut.h"
#endif

#ifndef FANFARE_MSFA_LFO_H
#define FANFARE_MSFA_LFO_H
#include "msfa/lfo.h"
#endif
