# Bundled `.plugin` packages

Put ZIP-format plugin containers here. Gradle copies them into the APK under
`assets/plugins/`; `Game.checkAndExtractPluginAssets()` extracts them into the
app-private `files/plugins/` directory at first launch.

Each container must carry a `plugin.json` manifest (schema 1, `platforms` may
contain `android`, `type: "tool"` for native architecture plugins) and a native
binary named with the host platform tag, for example:

```
libcg100sh4-android-arm64-android.so
```

`libcg100sh4-android-any.so` and plain `libcg100sh4.so` are accepted as
fallbacks. Build the bundled CG100 plugin with:

```
plugins/cg100-sh4/build-android.sh
```
