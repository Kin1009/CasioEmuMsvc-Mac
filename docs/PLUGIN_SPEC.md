# `.plugin` specification v1

Status: frozen
Host: CasioEmuMsvc preview
Platform: macOS only (v1)
Package: zip archive named `<name>.plugin`

This document is the contract between the app and plugin authors. The manifest
schema is frozen at version 1. New capabilities are added as new optional
manifest fields or new `PluginApi` interfaces; existing fields and v1 vtables do
not change.

---

## 1. Package layout

A `.plugin` file is a zip archive. `plugin.json` is required at the archive
root. The rest of the tree is free-form; the host reserves these names:

```
example.plugin
  plugin.json          required, UTF-8, no BOM
  assets/              optional, referenced by theme/layout/inject
  lib/                 optional, native dylibs
  docs/                optional, shown by the plugin manager
  src/                 optional, source for reference
```

Rules:

- Paths are case-sensitive on the host even if the zip is not.
- Absolute paths, `..` components, symlinks, and hard links are rejected.
- The host extracts to a cache directory keyed by the package SHA-256. It never
  writes outside that directory.
- A package may contain any additional folders and files. The loader ignores
  files it does not need.
- The archive may be stored or deflated. Encrypted zips are rejected.

Create and package from a folder:

```console
$ mkdir ExampleTheme
$ cd ExampleTheme
$ $EDITOR plugin.json
$ mkdir assets && $EDITOR assets/theme.json
$ zip -r ../ExampleTheme.plugin .
```

The zip must contain `plugin.json` at the root, not inside an extra folder.

---

## 2. Manifest

`plugin.json`:

```json
{
  "schema": 1,
  "id": "com.example.theme",
  "name": "Example Theme",
  "version": "1.0.0",
  "author": "Author",
  "description": "A short description shown in the plugin manager.",
  "homepage": "https://example.com/plugin",
  "min_host": "2.1.0",
  "platforms": ["macos"],
  "type": "theme",
  "permissions": ["ui.shell"],
  "entry_point": "assets/theme.json",
  "assets": ["assets/theme.json"],
  "native_entry": null
}
```

### 2.1 Required fields

| Field | Type | Rules |
| --- | --- | --- |
| `schema` | integer | Must be `1` for this spec. |
| `id` | string | Reverse-DNS, `[a-z0-9._-]`, 3-128 chars, globally unique. |
| `name` | string | Display name, 1-80 chars. |
| `version` | string | Semver `MAJOR.MINOR.PATCH`, optional prerelease/build. |
| `type` | string | One of `theme`, `tab`, `inject`, `ui_revamp`, `tool`. |
| `entry_point` | string | Relative path inside the package. Meaning depends on `type`. |

### 2.2 Optional fields

| Field | Type | Rules |
| --- | --- | --- |
| `author` | string | Display name. |
| `description` | string | 1-500 chars. |
| `homepage` | string | HTTPS URL. |
| `min_host` | string | Semver; the host refuses to load below this version. |
| `platforms` | string[] | v1 accepts only `["macos"]`; absent means `["macos"]`. |
| `permissions` | string[] | See section 3. Duplicates are rejected. |
| `assets` | string[] | Relative paths the host may expose to the plugin. |
| `native_entry` | string | Relative path to a `.dylib`; required for native `tab`/`tool`, optional for `ui_revamp`. |
| `api_version` | integer | Native API major version; defaults to `1`. |

`entry_point` by type:

| Type | `entry_point` points to | Native code |
| --- | --- | --- |
| `theme` | Theme token JSON | Not allowed |
| `inject` | Injection payload JSON | Not allowed |
| `ui_revamp` | Layout/shell JSON | Optional via `native_entry` |
| `tab` | Documentation or icon JSON | Required native dylib |
| `tool` | Documentation or icon JSON | Required native dylib |

A native `ui_revamp` plugin may provide both a declarative shell fallback and a
native shell. The declarative fallback is used if the native shell fails to
load or register.

---

## 3. Permissions

Permissions are declarations. The host enforces them for declarative plugins
and displays them before loading any plugin. Native code runs in-process and
cannot be sandboxed by the manifest; the trust prompt is the security boundary.

| Permission | Grants |
| --- | --- |
| `emulator.read` | Observe model, ROM, memory, registers, and debugger state. |
| `emulator.write` | Write RAM, registers, and memory monitors. |
| `emulator.code` | Patch ROM/code bytes and hot-reload images. |
| `emulator.inject` | Register and run injection payloads. |
| `filesystem.read` | Read files outside the plugin package. |
| `filesystem.write` | Write files outside the plugin package. |
| `network` | Make outbound network requests. |
| `process` | Spawn processes, including the terminal shell. |
| `ui.shell` | Add or modify tabs, tools, layout, themes, or the shell. |
| `clipboard` | Read or write the clipboard. |

Rules:

- `emulator.write` does not imply `emulator.code` or `emulator.inject`.
- `filesystem.write` does not imply `filesystem.read`; declare both if needed.
- `process` is required for terminal integrations and external tools.
- A plugin with no permissions may load without a permission warning, but the
  manager still shows its identity and the first-install confirmation.
- Unknown permissions are a validation error. The host never silently ignores
  them.
- The trust modal lists every permission with a one-line explanation, plus a
  plain statement when `native_entry` is present: "This plugin includes native
  code that runs with the same access as the app."

---

## 4. Trust, enablement, and reload

The host stores trust records outside the package:

```
~/Library/Application Support/CasioEmuMsvc/plugin-trust.json
```

A record contains `id`, `version`, `sha256`, `permissions`, `enabled`, `blocked`,
and `trusted_at`.

Load policy:

1. Scan the plugin folders and read every manifest.
2. Reject invalid packages and record the reason in the load log.
3. Compare the package SHA-256 and permissions with the trust record.
4. If the plugin is new, or a native package changed version/hash, show the
   in-app trust modal.
5. If permissions changed, always show the modal again, even for declarative
   updates.
6. If the user blocks, record `blocked` and never load that hash automatically.
7. If the user allows, record the hash and load.

Hot reload:

- Declarative plugins (`theme`, `inject`, declarative `ui_revamp`) reload in
  place after the file changes.
- Native plugins are never `dlclose`d while the app is running. Disable a
  native plugin and restart, or use the explicit Unload action, which marks the
  library as unloaded and refuses further calls.
- Removing or disabling a plugin removes its UI and restores the previous
  theme/layout.

---

## 5. Declarative plugin formats

### 5.1 `theme`

`entry_point` is a JSON document with a schema and token groups:

```json
{
  "schema": 1,
  "name": "Example Dark",
  "variant": "dark",
  "tokens": {
    "colors": {
      "background": "#1f1f1f",
      "surface": "#181818",
      "surfaceAlt": "#202020",
      "foreground": "#cccccc",
      "muted": "#9d9d9d",
      "border": "#2b2b2b",
      "accent": "#0078d4",
      "accentHover": "#026ec1",
      "selection": "#264f78",
      "hover": "#2a2d2e",
      "error": "#f14c4c",
      "warning": "#cca700",
      "success": "#89d185"
    },
    "metrics": {
      "windowRounding": 6,
      "frameRounding": 4,
      "borderSize": 1,
      "spacing": 8,
      "padding": 8,
      "scrollbarSize": 10
    },
    "typography": {
      "uiScale": 1.0,
      "monoFamily": "SF Mono",
      "uiFamily": "SF Pro Text"
    }
  }
}
```

Rules:

- Colors are `#RRGGBB` or `#RRGGBBAA`.
- Unknown token groups are ignored with a warning; unknown color keys are an
  error in strict mode.
- `variant` is `dark` or `light` and selects the base style before tokens apply.
- Fonts must already be installed on the system. A theme cannot ship binary
  fonts in v1.
- A theme applies immediately and is remembered as the active plugin theme.
  Removing it restores the built-in preset.

### 5.2 `inject`

`entry_point` is a JSON document listing named payloads:

```json
{
  "schema": 1,
  "payloads": [
    {
      "id": "example.hello",
      "name": "Hello payload",
      "description": "Writes a short banner into RAM.",
      "target": "fx-580VN X",
      "address": "0xE9E0",
      "bytes": "3C 3C 66 66 7E 7E 66 66",
      "safe": true
    }
  ]
}
```

Rules:

- `address` is a hex string or integer; it must fit the active model's address
  space.
- `bytes` is whitespace-separated hex; an odd number of digits is an error.
- `target` is a model name or `"any"`.
- `safe` is informational. The host still requires `emulator.inject` and still
  shows the address and length before the first run.
- Injections are listed in the Injection view and can be edited before running.
  The plugin file itself is never modified.

### 5.3 `ui_revamp` layout tree

`entry_point` is a declarative layout document:

```json
{
  "schema": 1,
  "name": "Compact Layout",
  "shell": {
    "type": "default",
    "sidebar": { "visible": true, "width": 240 },
    "bottom": { "visible": true, "height": 160, "active": "output" },
    "tools": {
      "visible": true,
      "width": 360,
      "tabs": ["Variables", "Hardware", "Labels", "Watch", "Funcs", "Code"],
      "hidden": ["Rop Compiler", "Bitmap"]
    },
    "sessions": { "tabs": "top", "showClose": true }
  }
}
```

Supported actions:

- reorder tool tabs;
- hide tools;
- show/hide and size the sidebar and bottom panel;
- choose the default bottom tab;
- set the session tab position (top only in v1);
- set the default calculator zoom.

A declarative `ui_revamp` cannot draw arbitrary widgets or replace the shell
tree. For that, use a native shell plugin (section 6.3).

---

## 6. Native plugin API

### 6.1 Entry point and lifecycle

A native plugin exports:

```cpp
extern "C" void fPluginLoad(casioemu::PluginApi* api);
```

The host calls it once after the package is trusted and the ImGui context
exists. `api->RegisterPlugin(...)` must be called before `api->AddWindow(...)`.
The plugin keeps the `PluginApi*` for its lifetime; the host keeps the library
loaded until shutdown.

A plugin may export optional hooks:

```cpp
extern "C" void fPluginUnload();
extern "C" const char* fPluginBuildId();
```

`fPluginBuildId` should return a compiler/SDK identifier for diagnostics. It
does not replace the package hash.

### 6.2 Stable versioned C++ ABI

The v1 interfaces in `PluginApi.h` are frozen:

- Existing virtual methods are never removed, reordered, or changed.
- New capabilities are new interfaces obtained through
  `PluginApi::QueryInterface<T>()`.
- A plugin built against v1 continues to load in later hosts unless its
  `min_host`/`api_version` says otherwise.
- Host API major version bumps require a rebuild. The plugin manager reports
  the host API version and the plugin's required version.
- The macOS SDK targets the system libc++ and the architectures in the app
  bundle (`arm64` + `x86_64`). Plugins must be built with the same C++ standard
  library and a compatible deployment target.

This is a stable ABI in the practical sense: v1 binaries keep working across
additive host releases, but a major-version change is explicit and gated.

### 6.3 Full shell takeover

A native `ui_revamp` plugin can obtain the shell interface:

```cpp
auto* shell = api->QueryInterface<IPluginShell>();
```

`IPluginShell` (added in API v2; declared in the SDK, not in the frozen v1
header):

```cpp
class IPluginShell {
public:
    // Return false to decline and keep the default shell.
    virtual bool Install() = 0;
    virtual void Uninstall() = 0;
    // Called once per frame from the host, inside the ImGui frame.
    virtual void Render() = 0;
    // Optional: called when the active session changes.
    virtual void OnActiveSessionChanged(SessionHandle session) = 0;
};
```

Rules:

- Only one shell plugin may be active. Installing a second one fails with a
  readable error.
- The host validates the plugin, records trust, and calls `Install()` before
  the first shell frame.
- If `Install()` returns false or throws, the host logs the reason and keeps
  the default shell.
- A shell plugin must render into the host viewport and must not create a second
  SDL window.
- The default shell remains available from **View > Reset Shell** even while a
  shell plugin is active.

---

## 7. Plugin manager

The manager is a normal tool window:

- **Installed** - name, id, version, type, author, enabled state, trust state,
  permission badges, package path, and last error.
- **Actions** - enable, disable, unload (native), remove, reveal in Finder,
  view log, re-read manifest.
- **Install** - drag a `.plugin` onto the window or choose **Install Plugin**.
- **Trust modal** - identity, version, SHA-256, every requested permission with
  an explanation, and Allow Once / Always Allow / Block.
- **Developer** - open plugin folders, reload declarative plugins, copy the load
  log, and show the host API version.

Removing a plugin deletes its extracted cache and trust record but never
touches the original `.plugin` file.

---

## 8. Discovery

Search order:

1. `~/Library/Application Support/CasioEmuMsvc/Plugins/*.plugin`
2. `<app bundle>/plugins/*.plugin` (read-only built-ins)
3. `<app bundle>/../plugins/*.plugin` for portable builds

If the same id appears twice, the user folder wins and the duplicate is logged.
A workspace `.plugins` folder is reserved for a later release and is not read by
v1.

---

## 9. Examples

The repository ships one example per type under `examples/plugins/`:

| Folder | Type | Demonstrates |
| --- | --- | --- |
| `theme-vscode-dark` | `theme` | Token JSON, dark palette |
| `inject-hello` | `inject` | Named payload with address/bytes |
| `layout-compact` | `ui_revamp` | Declarative tool/sidebar layout |
| `tool-clock` | `tool` | Native `UIWindow` |
| `tab-notes` | `tab` | Native tab with per-session notes |

Each example has a `README.md` with build and packaging commands.

---

## 10. Validation errors

The loader rejects a package with a stable error code and a readable message:

| Code | Meaning |
| --- | --- |
| `manifest.missing` | `plugin.json` not at the root. |
| `manifest.invalid_json` | Parse error; includes line/column. |
| `manifest.schema` | Unsupported `schema`. |
| `manifest.field` | Missing/invalid required field. |
| `manifest.id` | Invalid or duplicate id. |
| `manifest.version` | Invalid semver. |
| `manifest.type` | Unknown type. |
| `manifest.permission` | Unknown permission. |
| `manifest.platform` | Unsupported platform. |
| `manifest.min_host` | Host is older than `min_host`. |
| `package.path` | Unsafe path, symlink, or absolute path. |
| `package.sha256` | Hash mismatch with the trust record. |
| `native.entry` | Missing dylib or wrong architecture. |
| `native.load` | `dlopen` failed. |
| `native.symbol` | `fPluginLoad` missing. |
| `native.api` | API version mismatch. |
| `plugin.blocked` | User blocked this hash. |

Errors are shown in the manager and appended to the load log with the package
path and hash.
