#pragma once

// Stub that replaces Dexed's debugger/Logger.h for the vendored msfa core
// (Source/Synth/Fm/msfa/*). The msfa sources only include this header for
// the TRACE macro; tracing is compiled out in Fanfare.
#ifndef TRACE
#define TRACE(...) ((void)0)
#endif
