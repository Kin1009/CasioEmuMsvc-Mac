# Building `.plugin` packages for CasioEmuMsvc

This guide explains how to build one `.plugin` file that works on **macOS,
Linux, Windows and Android**, what goes in the manifest, how the host picks the
right native binary, and what ABI the native code must implement.

Tool: `tools/build_plugin.py` (Python 3, standard library only).
Worked example: `docs/examples/build_hello.sh` + `docs/examples/dummy/`.

---

## 1. What a `.plugin` file is

A `.plugin` is an ordinary **ZIP archive** (the host checks the `PK\x03\x04`
magic in `src/Plugin/PluginPackage.cpp`). Minimum layout:

```
my-plugin.plugin                 (the ZIP file itself)
├── plugin.json                  manifest, schema 1  (required)
├── assets/info.json             declarative info document (entry_point)
├── my-plugin-macos-x64.dylib    tagged native binaries (0..N)
├── my-plugin-macos-arm64.dylib
├── my-plugin-linux-x64.so
├── my-plugin-windows-x64.dll
├── my-plugin-android-arm64-android.so
└── lib/                         optional embedded dependency dylibs
    └── libcig100core.dylib
```

Rules enforced by the host when installing:

* every entry path must be a **safe relative path** (no leading `/`, no `\`, no
  `.` / `..` components);
* no duplicate **or case-insensitively colliding** entry names (macOS volumes
  are usually case-insensitive);
* no symlinks, hard links, device/FIFO/socket entries, or encrypted entries;
* entry-count / size / compression-ratio limits apply (zip-bomb protection);
* native binaries must be **regular files with executable bits (0755)** in the
  ZIP so the OS can `dlopen` them after extraction. JSON entries are 0644.
  The builder sets the Unix `create_system` flag so the modes survive.

See `src/Plugin/PluginPackage.cpp` (`ExtractPackage`) for the exact checks.

---

## 2. Quick start — one package, four platforms

```bash
python3 tools/build_plugin.py \
  --name hello --id com.example.hello --version 1.0.0 \
  --author "Example Author" --description "Tiny cross-platform example plugin." \
  --type tool --permissions ui.shell \
  --os-tags macos,linux,windows,android \
  --arch-tags x64,arm64,arm64-android \
  --binary-os macos   --binary-arch x64           --binary core.dylib \
  --binary-os macos   --binary-arch arm64         --binary core-arm64.dylib \
  --binary-os linux   --binary-arch x64           --binary core.so \
  --binary-os windows --binary-arch x64           --binary core.dll \
  --binary-os android --binary-arch arm64-android --binary core-android.so \
  --out /tmp/hello.plugin
```

Run `bash docs/examples/build_hello.sh /tmp/hello.plugin` for a self-contained
demo using the dummy files in `docs/examples/dummy/` (placeholder text, **not**
real native libraries).

The tool prints the output SHA-256 and a full entry listing, then re-opens the
finished ZIP and verifies: ZIP magic, exact entry list, every entry's bytes,
every entry's Unix mode, and that both JSON documents parse with `schema == 1`.
Invalid input or a failed verification exits non-zero (nothing is installed).

---

## 3. CLI reference

| Flag | Meaning |
|---|---|
| `--name NAME` | Display name, 1–80 characters. Also used as the binary base name. |
| `--id ID` | Reverse-DNS id (3–128 chars; lower-case letters, digits, `.`, `_`, `-`; at least one dot; no leading/trailing/double dot). Example: `com.example.hello`. |
| `--version SEMVER` | Plugin version, `MAJOR.MINOR.PATCH` (optional `-pre` / `+build`). Default `1.0.0`. |
| `--author TEXT`, `--description TEXT` | Optional metadata (description ≤ 500 chars). Copied into `plugin.json` and `assets/info.json`. |
| `--min-host SEMVER` | Minimum host version. Default `2.1.0`; current host is `2.2.0` (`kHostVersion` in `src/Plugin/PluginManifest.h`). |
| `--os-tags a,b,...` | OS tags declared in `platforms`. See §5. |
| `--arch-tags a,b,...` | Architecture tags, written as advisory `architectures` metadata. See §5. |
| `--permissions a,b,...` | Permission list. See §8. Comma-separated or repeated. |
| `--type TYPE` | `theme`, `tab`, `inject`, `ui_revamp`, `tool` (default `tool`). |
| `--binary PATH` | Native binary to embed (repeatable). Archive name is auto-derived: `<name>-<os>-<arch>.<ext>`. |
| `--binary-os OS`, `--binary-arch ARCH` | OS/arch for subsequent `--binary` flags. **Sticky** until changed. If omitted, OS is derived from the extension (`.dylib`→macos, `.dll`→windows, `.so`→linux, `.plugin`→macos) and arch defaults to `any` (or the sole declared `--arch-tags` value). |
| `--asset SRC[=DEST]` | Extra data asset (repeatable). `DEST` defaults to the source basename; must be a safe relative path. |
| `--lib PATH` | Dependency dylib (repeatable); embedded as `lib/<basename>` with mode 0755. |
| `--core PATH` | Used only by `--preset cg100-sh4`. |
| `--preset cg100-sh4` | Reproduce the bundled CG100 SH4 package exactly (see §10). |
| `--out PATH` | Output `.plugin` ZIP (required). Existing file is replaced atomically. |

Validation mirrors the host: unknown permission/tag/type, bad id, bad semver,
unsafe paths, case-colliding entries, wrong extension, or a `theme`/`inject`
plugin with native code are all rejected before any ZIP is written.

### Per-platform binary commands

```bash
# macOS (Intel) — .dylib implies macos
--binary-os macos   --binary-arch x64           --binary build/macos-x64/core.dylib
# macOS (Apple Silicon)
--binary-os macos   --binary-arch arm64         --binary build/macos-arm64/core.dylib
# Linux x86-64 — .so implies linux
--binary-os linux   --binary-arch x64           --binary build/linux-x64/core.so
# Windows x86-64 — .dll implies windows
--binary-os windows --binary-arch x64           --binary build/win-x64/core.dll
# Android 64-bit ARM — note the hyphenated arch tag
--binary-os android --binary-arch arm64-android --binary build/android-arm64/core.so
# Universal fallback (matches any host)
--binary-os any     --binary-arch any           --binary build/portable/core.so
```

Native extensions recognised by the host are `.dylib`, `.so`, `.dll`, and
`.plugin` (`IsNativeLibraryExtension` in `src/Plugin/PlatformTags.h`).

---

## 4. `plugin.json` fields (schema 1)

Source of truth: `ParseManifest()` in `src/Plugin/PluginManifest.cpp`.

| Field | Required | Type | Notes |
|---|---|---|---|
| `schema` | yes | int | Must be `1`. |
| `id` | yes | string | Reverse-DNS-ish; validated as described above. |
| `name` | yes | string | 1–80 chars. |
| `version` | yes | string | semver. |
| `author`, `description`, `homepage` | no | string | description ≤ 500 chars. |
| `min_host` | no | string | semver; host rejects a value newer than its own `2.2.0`. |
| `platforms` | no | array of OS tags | Defaults to `["macos"]` when absent; the updated host accepts all tags in §5. Builder writes it when `--os-tags` is given. |
| `architectures` | no | array of arch tags | Builder-generated advisory metadata for tooling. **The host selects binaries by filename tags, not by this field.** |
| `type` | yes | string | One of the five types below. |
| `permissions` | no | array of strings | No duplicates; all names must be known (§8). |
| `entry_point` | yes | string | Safe relative path; use `assets/info.json`. |
| `native_entry` | conditional | string | **Required** for `tab` and `tool`. **Forbidden** for `theme` and `inject`. Only used as a legacy fallback when the package contains no tagged binaries. |
| `assets` | no | array of strings | Safe relative paths; normally just `["assets/info.json"]`. |
| `api_version` | no | int | Host accepts `1..3` (`kPluginHostApiMinVersion..kPluginHostApiVersion` in `src/Plugin/PluginVersion.h`); builder relies on the default. |

Plugin types: `theme` (data and appearance only, no native code), `tab` (adds a native tab),
`inject` (payloads, no native code), `ui_revamp` (theme + layout), `tool`
(native tool / architecture core). `native_entry` points at the first embedded
binary when the builder makes a `tab` or `tool`; it is ignored whenever tagged
binaries are present.

Generated example (from the demo, trimmed for readability):

```json
{
  "schema": 1,
  "id": "com.example.hello",
  "name": "hello",
  "version": "1.0.0",
  "type": "tool",
  "permissions": ["ui.shell"],
  "entry_point": "assets/info.json",
  "platforms": ["macos", "linux", "windows", "android"],
  "architectures": ["x64", "arm64", "arm64-android"],
  "native_entry": "hello-macos-x64.dylib"
}
```

`assets/info.json` is a small object with `name` and `description`.

---

## 5. OS and architecture tags

Authoritative source: `src/Plugin/PlatformTags.h`.

### OS tags (`kKnownOsTags`)

| Tag | Meaning |
|---|---|
| `macos` | Apple macOS (desktop / laptop) |
| `linux` | Linux |
| `windows` | Microsoft Windows |
| `android` | Android |
| `any` | Any OS — wildcard |

### Architecture tags (`kKnownArchTags`)

| Tag | Meaning |
|---|---|
| `x64` | 64-bit x86 (AMD64 / Intel64) |
| `x32` | 32-bit x86 (IA-32) — note: the old draft name `x6432` is **not** valid |
| `arm64` | 64-bit ARM on laptops/desktops (Apple Silicon, Windows on ARM, Linux aarch64) |
| `arm64-android` | 64-bit ARM on Android phones / tablets |
| `any` | Any CPU architecture — wildcard |

`any` is a wildcard **on either side**: a binary tagged `any-any` matches every
build, and a build whose own tag is `any` accepts any binary.

---

## 6. Native binary naming convention

Pattern: `<name>-<os>-<arch>.<ext>`. Examples:

```
my-plugin1-macos-x64.dylib
my-plugin2-macos-arm64.dylib
my-plugin1-windows-x64.dll
my-plugin1-linux-arm64.so
my-plugin1-android-arm64-android.so
my-plugin-any-any.so
```

Notes:

* The `-<os>-<arch>` suffix must be at the **end of the filename stem**; the
  base name before it must be non-empty. This is what
  `ParseTaggedNativeFileName()` in `PlatformTags.h` checks.
* `arm64-android` contains a hyphen; the parser matches known suffixes rather
  than splitting on `-`, so do not "fix" that name.
* **Several binaries per platform are allowed** — use distinct base names
  (`my-plugin1`, `my-plugin2`, …). Every matching binary is loaded.
* A binary with **no** tag suffix (e.g. the legacy `CG100SH4.plugin`) is *not*
  a tagged binary; it is only considered through the manifest's `native_entry`.

The builder derives these names automatically from `--name`, the per-binary
OS/arch tags and the source extension, and refuses to emit a name the host
cannot parse.

---

## 7. How the host chooses binaries

`SelectPlatformBinariesForRoot()` in `src/Plugin/PlatformTags.h` scans the
**entire extracted package recursively**:

1. For each regular file, parse `-<os>-<arch>` from the filename (and confirm a
   native extension).
2. Record its `<os>-<arch>` key in the "available" list.
3. If OS and arch match the running build (`any` wildcards included), add the
   file to "matched" (all matches load, sorted by path).
4. Result:
   * **`NoTaggedBinaries`** — no tagged files at all → fall back to
     `native_entry` (legacy single-dylib packages).
   * **`Matched`** — one or more files match → load **all** of them.
   * **`NoMatch`** — tagged files exist but none match → reject the plugin with
     `NoCompatibleBinaryError()`, e.g. **`No native binary for macos-x64`**.

The current build's OS/arch come from `CASIOEMU_PLATFORM_OS` /
`CASIOEMU_PLATFORM_ARCH`. If the build system does not override them, the header
derives them from compiler predefines (`__APPLE__` → `macos`, `_WIN32` →
`windows`, `__ANDROID__` → `android`, `__linux__` → `linux`; `__aarch64__` →
`arm64`, `__x86_64__` → `x64`, `__i386__` → `x32`, Android ARM64 →
`arm64-android`).

---

## 8. Permissions

Manifest names and descriptions: `kPermissionNames` / `kPermissionDescriptions`
in `src/Plugin/PluginManifest.cpp`; runtime grants: `kGrants` in
`src/Plugin/PluginPermissions.cpp`; capability enum:
`src/Plugin/PluginPermissions.h`.

| Permission | Grants (capabilities) | What it lets the plugin do |
|---|---|---|
| `emulator.read` | `EmulatorRead` | Observe the model, ROM, memory, registers and debugger state. |
| `emulator.write` | `EmulatorWrite` | Write RAM, registers and memory monitors. |
| `emulator.code` | `EmulatorCode`, `EmulatorWrite` | Patch ROM/code bytes and hot-reload images. |
| `emulator.inject` | `Inject` | Register and run injection payloads. |
| `filesystem.read` | `FilesystemRead` | Read files outside the plugin package. |
| `filesystem.write` | `FilesystemWrite` | Write files outside the plugin package. |
| `network` | `Network` | Make outbound network requests. |
| `process` | `Process`, `Native` | Spawn processes, including terminal shells. |
| `ui.shell` | `Shell`, `Window`, `Theme`, `Native` | Add/modify tabs, tools, layout, themes or the shell. |
| `clipboard` | `Clipboard` | Read or write the clipboard. |
| `arch` | `Arch`, `Native` | Register an architecture provider and define custom hardware IDs / model classes. |

`arch` is **additive**: it grants only `Arch + Native` and does not absorb or
narrow `ui.shell`, `emulator.*` or any other permission. An architecture core
that also supplies its own UI buttons/tabs keeps `ui.shell` (the bundled CG100
package declares `["ui.shell", "arch"]`). Capability checks happen in the
permission facade (`src/Plugin/PluginApi_Impl.cpp`); a denial is recorded with
`plugin.permission` and shown as "limited" in the plugin manager.

Declare the **narrowest** set that works — permissions are shown to the user at
install time.

---

## 9. Native ABI contract (very important)

**The host owns the ABI function names; the plugin implements them.**

* Host declarations (source of truth): `src/Plugin/ArchCoreABI.h`
  ("ARCH ABI v1", 2026-11-21). The plugin's `cg100core.h` / `ArchCoreABI.h` must
  be a byte-identical mirror or a thin shim that includes the host header.
* The plugin implements each declared symbol with `extern "C"` and an
  identical signature; the host calls it after `dlopen`.
* Handshake symbols: `fPluginLoad`, `fPluginLoadArch`, `fPluginApiVersion`,
  `fPluginAbiSignature`. The current CG100 plugin exports the first two and
  takes the documented legacy/unverified path for the version/signature pair.
* Generic bridge: `casioemu_arch_core_framebuffer`, `_handle`, `_key_down`,
  `_key_up`, `_key_matrix`, `_power_on`, `_release_all`, `_reboot`,
  `_enter_diag` — implemented by `CG100SH4.plugin`.
* CG100 core: `cg100_create`, `cg100_run`, `cg100_framebuffer`, `cg100_*` USB
  helpers, etc. — implemented by `libcig100core.dylib` and resolved as imports
  of the plugin.
* **Additive changes only.** Never change an existing signature, parameter
  width, return type or symbol spelling. Adding a symbol requires a new version
  comment in `ArchCoreABI.h`, an updated plugin mirror, and a re-check with
  `nm -gU` (defined exports) and `nm -gm` (imports) before repackaging.
* Host API version is `PLUGIN_API_VERSION` (`src/Plugin/PluginVersion.h`,
  currently 3); the UI-window revision is
  `kPluginUiWindowAbiRevision` / `HostUiWindowAbiSignature()`.

### macOS dynamic-library details

* Native `.dylib` files need executable bits (the builder sets 0755).
* The host adds `@loader_path` and `@loader_path/lib` to the dylib search
  rpath, so **embedded dependencies belong in `lib/` inside the ZIP** — e.g.
  `lib/libcig100core.dylib`. Use `--lib /path/to/libcig100core.dylib`.
* Keep the dylib's install name relative (`@rpath/...` or `@loader_path/...`);
  an absolute path would make the host look outside the extracted package.

---

## 10. Preset: reproduce the bundled CG100 SH4 package

```bash
python3 tools/build_plugin.py --preset cg100-sh4 \
  --core /path/to/CG100SH4.plugin \
  --lib  /path/to/libcig100core.dylib \
  --out  /tmp/CG100SH4.plugin
```

The preset fixes the manifest to the known-good values (id
`com.casioemu.arch.cg100sh4`, permissions `["ui.shell", "arch"]`,
`native_entry: "CG100SH4.plugin"`, `platforms: ["macos"]`), writes the same
`assets/info.json`, and embeds the dependency as `lib/libcig100core.dylib`.
`--core` / `--lib` default to the app bundle's plugins directory relative to
this checkout. Verified: rebuilding from the known-good embedded binaries
reproduces the bundled ZIP **byte-for-byte** (SHA-256
`7479bf1841f3622d46e591e15a453cdb20944c72dbeda9670cf4d38ca90c9356`).

The original CG100-specific helper, `tools/make_bundled_plugin.py`, is left
untouched; the new tool is a superset.

---

## 11. Install and enable

1. In the emulator, open **Settings → Plugins** (also reachable via
   *File → Plugins* / the command palette; `src/Gui/Theme.cpp`,
   `RenderPluginList`).
2. Click **Install .plugin...** and pick the `.plugin` ZIP.
   (`src/Gui/PluginLogWindow.cpp` shows the same button in the legacy log
   window.)
3. The host extracts and validates it into its plugin cache, shows the
   requested permissions, and lists the entry.
4. Enable the plugin (and relaunch if asked). Optional model packages can
   declare a dependency with `extra.requires_plugins` in `config.json`
   (e.g. `"requires_plugins": "com.casioemu.arch.cg100sh4"`).

---

## 12. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `not a readable zip archive` / bad magic | The file is a raw dylib, not a ZIP. Use this builder; native binaries go **inside** the ZIP. |
| `No native binary for macos-x64` | No tagged binary matches this build. Ship a matching `<name>-<os>-<arch>.<ext>` (or an `any-any` fallback), or fix a typo such as `x6432` → `x32`. |
| `only the 'macos' platform is supported` | The running host predates multi-platform tags. Update the host (≥ 2.2.0 with `PlatformTags.h`). |
| Plugin loads but `dlopen` fails for a dependency | Dependency dylib is not in `lib/` or uses an absolute install name. Put it in `lib/` with `--lib` and use `@rpath`/`@loader_path`. |
| `permission denied: ... requires the '<capability>' capability` | Add the permission that grants it (§8), e.g. `arch` for architecture registration, `ui.shell` for tabs/UI. |
| `unsafe zip entry` / `case-colliding zip entry` | Rename the entry: no absolute paths, `..`, backslashes, duplicates or case-only differences. |
| `type 'tool' requires native_entry` / `theme plugins cannot include native code` | Match the type to the payload: native code needs `tab`/`tool`; `theme`/`inject` must have none. |
| `unsupported schema version` | Manifest must have `"schema": 1`. |
| `plugin requires host X or newer` | Lower `min_host` or update the host. |
| Dylib fails to load after install | Check ZIP mode bits are 0755 (`unzip -l` / `python3 -m zipfile -l`); the builder adds them automatically. |

### Inspect a built package

```bash
python3 -m zipfile -l /tmp/hello.plugin
python3 - <<'PY'
import zipfile, json
with zipfile.ZipFile('/tmp/hello.plugin') as z:
    m = json.loads(z.read('plugin.json'))
    print(m['id'], m['type'], m['permissions'])
    for i in z.infolist():
        print(i.filename, oct((i.external_attr >> 16) & 0o777))
PY
```
