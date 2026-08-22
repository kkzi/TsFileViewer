# TsFileViewer

Qt GUI viewer for tree-model TsFiles (COMAC/DPR TsFileArchive output).
Read-only: open a `.tsfile`, browse devices/parameters, inspect values in a
table and plot them with QCustomPlot.

## Layout

- `Src/` — viewer sources (MainWindow / models / data layer)
- `3rd/tsfile` — submodule, Apache tsfile C++ tree (static link, no tools/tests)
- `3rd/qcustomplot` — submodule, QCustomPlot 2.1.1 CMake library mirror

## Build (Windows, VS2019 x64, Qt 5.15.2 via `QTDIR`)

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

Output: `Build\Release\bin\TsFileViewer.exe` (+ `qcustomplot.dll` copied
next to it; Qt DLLs must be on PATH or deployed separately — `QTDIR\bin`).

Notes:
- The embedded tsfile is configured via ExternalProject because its
  sub-CMakeLists assume they are the top-level project (`CMAKE_SOURCE_DIR`).
- `zlibstatic[d].lib` switches by config; `tsfile.lib` keeps its name in both.
