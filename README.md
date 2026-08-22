# TsFileViewer

Qt GUI viewer for TsFiles: tree model (COMAC/DPR TsFileArchive output) and
table model (Apache upstream), including mixed files. Read-only: open a
`.tsfile`, browse devices/tables/parameters, inspect values in a table and
plot them with QCustomPlot.

## Layout

- `Src/` — viewer sources (MainWindow / models / data layer)
- `Src/TableFixture.cpp` — console helper that writes a table-model tsfile
  for manual testing (COMAC archives are tree-only)
- `3rd/tsfile` — submodule, Apache tsfile C++ tree (static link, no tools/tests)
- `3rd/qcustomplot` — submodule, QCustomPlot 2.1.1 CMake library mirror

## Build

Presets: `msvc-release` / `msvc-debug` (Ninja). Windows (VS2019 x64, Qt 5.15.2
via `QTDIR`):

```bat
git submodule update --init
_build.bat              :: msvc-release (default)
_build.bat msvc-debug
```

Or manually:

```bat
call vcvars64.bat
cmake --preset msvc-release
cmake --build --preset msvc-release
```

Linux is expected to work (Qt5 + Ninja + GCC; the CMake side has the
`WIN32`/`else()` splits for lib naming and flags) but only Windows presets
are provided and tested.

Output: `Build\Release\bin\TsFileViewer.exe` (+ `qcustomplot.dll` copied
next to it; Qt DLLs must be on PATH or deployed separately — `QTDIR\bin`).

## Notes

- The embedded tsfile is configured via ExternalProject because its
  sub-CMakeLists assume they are the top-level project (`CMAKE_SOURCE_DIR`).
- Windows only: `zlibstatic[d].lib` switches by config; `tsfile.lib` keeps
  its name in both.
- Non-ASCII paths: the upstream tsfile reader opens paths via CRT `::open()`
  in the active code page; `Src/PathBridge.h` bridges UTF-8 (Qt) paths to an
  ACP-safe form (8.3 short path, then lossless ACP transcoding). On POSIX it
  is a no-op.
