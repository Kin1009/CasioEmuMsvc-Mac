#pragma once
// ---------------------------------------------------------------------------
// sh4_abi.h - canonical ABI include for this plugin's build + harness.
//
// Re-exports the vendored, byte-exact host headers kept by the design agent in
// abi/ (PluginApi.h + IArchitectureProvider.h). Those are copies of:
//   /Volumes/Code/_imports/Desktop/CasioEmuMsvc-Mac/CasioEmuMsvc/src/Plugin/
// and define the real vtable order. Do NOT redeclare those classes here -
// doing so once caused a vtable mismatch when this shim had an extra virtual
// destructor. Including abi/ guarantees harness and plugin agree.
// ---------------------------------------------------------------------------
#include "abi/PluginApi.h"

// HW_CG100 = 10 in the original Windows fork; 14 in the authoritative Mac
// host tree used here (EPS ids 10-13 were shifted). Validation accepts both.
#define SH4_HW_CG100     10
#define SH4_HW_CG100_ALT 14

#if defined(_WIN32)
#  define SH4_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define SH4_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
