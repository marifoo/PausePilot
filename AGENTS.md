# uTimer Project Overview

uTimer is a lightweight, cross-platform (Windows & Linux) time tracking application designed for remote work. Its primary feature is automatic pause detection when the computer is locked, ensuring accurate tracking of active work time. It features a robust architecture for data integrity, handling system crashes and daily session boundaries.

## Architecture and Software Design

The code of uTimer follows a the ideas outlined in John Ousterhouts book "Philosophy of Software Design". The design and architecture are documented in the `doc` subfolder.

## Build Commands Linux

```bash
# Build (requires Qt5)
qmake-qt5 && make -j4

# Clean build
make clean

# Full clean (removes generated files)
make distclean
```

The project uses Qt5 qmake. Build output goes to `build/` directory. Executable is `uTimer`.

## Testing

Tests are in `qtest/` using Qt Test Framework. `qtest/main.cpp` is the runner
that aggregates per-component suites; each component has its own
`test_<component>.cpp` (e.g. `test_timer.cpp`, `test_database.cpp`,
`test_historydialog.cpp`). There are two runners: `qtest`, the 19-suite
component aggregate under a strict zero-`QWARN` policy, and `apptest`, the
app-shell composition suite that links `mainwin.cpp`, `contentwidget.cpp`,
`startuprecovery.cpp` and `unixsignalbridge.cpp` (which `qtest` deliberately
does not link) and spawns the production binary. Both must pass before a
change is considered verified.

**Running tests:**
```bash
cd qtest
qmake-qt5 && make -j4
./qtest
```

`apptest` builds against a sibling production binary, so build that first:
```bash
# 1. build the production binary first -- apptest spawns it
qmake-qt5 && make -j4

# 2. build and run the app-shell suite
cd apptest
qmake-qt5 && make -j4
QT_QPA_PLATFORM=offscreen ./apptest
```

Step 1 is mandatory: `apptest.pro` bakes the production binary's path in at
qmake time (`UTIMER_PRODUCTION_BINARY`, relative to `$$OUT_PWD/..`), so
without a sibling `uTimer` build it fails at run time with
`could not start production binary "<path>"`; set `UTIMER_BIN` to point at a
binary built elsewhere. `QT_QPA_PLATFORM=offscreen` is what the runner is
written for -- `apptest` realizes real top-level widgets and
`apptest/main.cpp` allow-lists exactly two offscreen-QPA warnings, so other
QPA plugins open real windows and aren't headless-safe.

All tests shall use the "Arrange, Act, Assert" pattern.

**Expected output:** `qtest` passes with zero `QWARN` messages; `apptest`
tolerates only its two allow-listed offscreen-QPA `QWARN`s. The zero-`QWARN`
gate and qtest abort-truncation diagnostic are enforced on Linux only; a
Windows run prints `NOTE: QWARN enforcement is disabled` and its exit status
reflects test failures or aborts but not warning-only runs.

