/* cg100core.h -- compatibility shim for the CG100 SH4 plugin.
 *
 * Function names are defined by CasioEmuMsvc (the host) in
 * src/Plugin/ArchCoreABI.h; the architecture plugin IMPLEMENTS them.
 * This file is intentionally a THIN SHIM: the host header is the single
 * source of truth for every fPlugin*, casioemu_arch_core_*, cg100_* and
 * cg100_usb_* declaration.  Signatures are unchanged, so the plugin still
 * compiles and exports the identical symbols / binary ABI.
 *
 * ARCH ABI v1 (2026-11-21).  The existing build cache resolves the host
 * include dir to a different checkout, so this shim includes the local,
 * byte-identical mirror src/ArchCoreABI.h (cmp against the host original).
 * The original, fully-commented declaration body is preserved at
 * /Volumes/Code/casio-tools/temp/ARCH_ABI_backup/cg100core.h.orig.
 */
#ifndef CG100CORE_H
#define CG100CORE_H

#include "ArchCoreABI.h"

#endif /* CG100CORE_H */
