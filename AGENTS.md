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
`test_historydialog.cpp`).

**Running tests:**
```bash
cd qtest
qmake-qt5 && make -j4
./qtest
```

All tests shall use the "Arrange, Act, Assert" pattern.

**Expected output:** All tests pass with zero `QWARN` messages. The zero-`QWARN` gate and qtest abort-truncation diagnostic are enforced on Linux only; a Windows run prints `NOTE: QWARN enforcement is disabled` and its exit status reflects test failures or aborts but not warning-only runs.

