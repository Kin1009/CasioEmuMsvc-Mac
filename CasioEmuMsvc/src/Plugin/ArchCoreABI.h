/* ArchCoreABI.h -- CasioEmuMsvc architecture C ABI (host-owned names).
 *
 * Function names are defined by CasioEmuMsvc (host); the architecture plugin
 * implements them.  The plugin's cg100core.h is a compatibility shim that
 * includes/contracts to this header.  This applies to the EXISTING CG100 SH4
 * plugin (CG100SH4.plugin): nothing here invents a new architecture.
 *
 * Version: ARCH ABI v1 -- 2026-11-21.  Additive changes only; never change an
 * existing signature or ordinal.  New functions require a new version comment
 * and a matching update to the plugin shim / contract document.
 *
 * Source of truth: THIS file in the host tree.  The existing plugin build
 * cache resolves ${PLUGIN_API_DIR} to a different host checkout, so the
 * plugin's cg100core.h includes a BYTE-IDENTICAL mirror copy installed at
 * cg100-plugin-mac/src/ArchCoreABI.h.  Keep the two files in sync (cmp/shasum).
 */
#ifndef CASIOEMU_ARCHCORE_ABI_H
#define CASIOEMU_ARCHCORE_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
class PluginApi;                 /* declared fully in host PluginApi.h */
#else
typedef struct PluginApi PluginApi;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- *
 * Handle / geometry
 * ------------------------------------------------------------------------- */
typedef void* CG100Core;                 /* opaque Go core handle */

#define CG100_FB_W 396
#define CG100_FB_H 224

/* ------------------------------------------------------------------------- *
 * Plugin handshake (host calls these on dlopen)
 * ------------------------------------------------------------------------- */
void          fPluginLoad(PluginApi* api);
void          fPluginLoadArch(PluginApi* api);
int           fPluginApiVersion(void);
unsigned long fPluginAbiSignature(void);

/* ------------------------------------------------------------------------- *
 * Generic architecture-core bridge -- implemented by CG100SH4.plugin
 * (the IEmulatorCore* is passed as void*; see dllmain.cpp for details).
 * ------------------------------------------------------------------------- */
int    casioemu_arch_core_framebuffer(void* core, unsigned char* out, int outLen);
void*  casioemu_arch_core_handle(void* core);
void   casioemu_arch_core_key_down(void* core, int row, int col);
void   casioemu_arch_core_key_up(void* core, int row, int col);
void   casioemu_arch_core_key_matrix(void* core, int row, int col);
void   casioemu_arch_core_power_on(void* core);
void   casioemu_arch_core_release_all(void* core);
void   casioemu_arch_core_reboot(void* core);
void   casioemu_arch_core_enter_diag(void* core, int mode);

/* ------------------------------------------------------------------------- *
 * CG100 SH4 core C ABI -- implemented by libcig100core.dylib (Go c-shared),
 * consumed and re-exported through the plugin.  Signatures are byte-identical
 * to the historical cg100core.h so the shipped plugin keeps its ABI.
 * ------------------------------------------------------------------------- */
CG100Core   cg100_create(const uint8_t* flash32mb, uint32_t flashSize);
void        cg100_destroy(CG100Core core);
void        cg100_reset(CG100Core core);
void        cg100_reset_no_boot(CG100Core core);
uint64_t    cg100_run(CG100Core core, int steps);
int         cg100_run_to_idle(CG100Core core, int maxSteps);
void        cg100_key(CG100Core core, uint32_t row, uint32_t col);
void        cg100_key_down(CG100Core core, uint32_t row, uint32_t col);
void        cg100_key_up(CG100Core core, uint32_t row, uint32_t col);
void        cg100_power_on(CG100Core core);
void        cg100_power_on_down(CG100Core core);   /* alias: hold ON line */
void        cg100_power_on_up(CG100Core core);     /* alias: release ON line */
void        cg100_release_all(CG100Core core);
void        cg100_framebuffer(CG100Core core, uint16_t* out, uint32_t outLen);
uint64_t    cg100_cycles(CG100Core core);
const char* cg100_fault(CG100Core core);           /* NULL when no fault */
uint64_t    cg100_pushes(CG100Core core);          /* LCD present count */
void        cg100_set_ips(CG100Core core, uint64_t ips);
int         cg100_goto_menu(CG100Core core);
uint32_t    cg100_state_len(CG100Core core);
uint32_t    cg100_save_state(CG100Core core, uint8_t* out, uint32_t cap);
int         cg100_load_state(CG100Core core, const uint8_t* blob, uint32_t len);

/* RESERVED NAME SLOTS -- host reserves these names for future save-state
 * convenience helpers.  They are NOT exported by the current Go core or the
 * shipped CG100SH4.plugin; do not call them without implementing them first.
 * Signatures below are placeholders matching cg100_save_state and must be
 * confirmed before use. */
uint32_t    cg100_plugin_state(CG100Core core, uint8_t* out, uint32_t cap);
uint32_t    cg100_home_state(CG100Core core, uint8_t* out, uint32_t cap);

/* ------------------------------------------------------------------------- *
 * CG100 USB Drive ABI (16 functions, additive)
 * ------------------------------------------------------------------------- */
#define CG100_USB_DISCONNECTED 0
#define CG100_USB_CONNECTED    1
#define CG100_USB_CONFIGURED   2
#define CG100_USB_MOUNTED      3
#define CG100_USB_EJECTED      4

#define CG100_CONN_NONE                  0
#define CG100_CONN_USB_FLASH             1
#define CG100_CONN_OS_UPDATE             2
#define CG100_CONN_USB_FLASH_UNLOCK_EXAM 3

int         cg100_usb_plug(CG100Core core);
int         cg100_usb_eject(CG100Core core);
int         cg100_usb_status(CG100Core core);
const char* cg100_usb_status_text(CG100Core core);
int         cg100_usb_mode(CG100Core core);
int         cg100_usb_set_mode(CG100Core core, int mode);
int         cg100_usb_list(CG100Core core, char* out, int cap);
int         cg100_usb_list_count(CG100Core core);
int         cg100_usb_put(CG100Core core, const char* name, const uint8_t* data, int len);
int         cg100_usb_get(CG100Core core, const char* name, uint8_t* out, int cap);
int         cg100_usb_get_size(CG100Core core, const char* name);
int         cg100_usb_rm(CG100Core core, const char* name);
int         cg100_usb_read_raw(CG100Core core, uint32_t addr, uint8_t* out, int n);
int         cg100_usb_write_raw(CG100Core core, uint32_t addr, const uint8_t* data, int n);
int         cg100_usb_exam_off(CG100Core core);
int         cg100_usb_test_echo(int v);

#ifdef __cplusplus
}
#endif

#endif /* CASIOEMU_ARCHCORE_ABI_H */
