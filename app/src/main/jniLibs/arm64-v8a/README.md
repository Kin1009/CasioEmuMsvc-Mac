# Prebuilt native libraries

Optional Gradle `jniLibs` directory. Libraries named with `plugin` or `sh4` in
the file name are picked up by the Android native plugin loader from the app's
`nativeLibraryDir` (e.g. `libcg100sh4_plugin.so`). The main emulator library
(`libmain.so`) is produced by the CMake `main` target and packaged by AGP.
