# Working on uTimer

## Architecture and design

Follow the principles in John Ousterhout's *A Philosophy of Software Design*.
Start with [the documentation index](doc/README.md), then read the relevant
architecture, runtime, or persistence document before changing those areas.
Keep design documentation aligned with changes to module responsibilities
or behavior; class and function details belong beside the code.

## Build and validation references

[README.md](README.md#for-contributors) is the source of truth for contributor
build dependencies and CI commands. Do not duplicate those instructions here.
The executable definitions of the checks are
[the CI workflow](.github/workflows/ci.yml) and the scripts in `scripts/`.

Prefer `scripts/ci-linux.sh` for local Linux validation: it builds the
production application and both test runners in a matching shadow tree.
When working from Windows, use a Linux environment such as WSL for
Linux-specific behavior and warning enforcement. Report any checks that
could not be run rather than treating a Windows-only run as Linux verification.

qmake puts the executable in the target's build directory. The production
project's `build/` setting places intermediate objects there; it does not
move the executable into that subdirectory. The CI scripts choose their own
shadow build directories explicitly.

When adding or removing source/header files, update both the relevant
`.pro` and `.vcxproj` lists and run `scripts/check_vcxproj_parity.py`.
For workflow or shell-script changes, run actionlint with ShellCheck and
check standalone scripts with ShellCheck too.

## Test structure and verification

Both Qt test runners must pass before a code change is considered verified:

- `qtest/main.cpp` aggregates 19 component suites. Add component tests to
  the appropriate `qtest/test_<component>.cpp` suite.
- `apptest/main.cpp` runs application-shell composition and production-process
  tests. It links `mainwin.cpp`, `contentwidget.cpp`, `startuprecovery.cpp`,
  and `unixsignalbridge.cpp`, which the component runner deliberately omits.

`apptest` requires a built production executable. Its qmake definition embeds
`UTIMER_PRODUCTION_BINARY` relative to the parent of the test build directory
(`$$OUT_PWD/..`). Build production first in that parent, or set `UTIMER_BIN`
at test run time to an executable built elsewhere.

Run GUI tests with `QT_QPA_PLATFORM=offscreen`. On Linux, `qtest` permits no
printed `QWARN`s; `apptest` permits only the two existing offscreen-QPA
warning patterns in its runner. Preserve these gates. Windows disables
warning enforcement and prints a note; test failures still affect exit status.
The component runner also reports aborted or incomplete aggregate runs.

Use Arrange, Act, Assert in tests. Keep tests independent of the host desktop
state, inherited descriptor layout, and process scheduling:

- Use fake clocks and stores where the test does not require real integration.
- Wait for observable completion rather than fixed delays. Database file
  creation does not prove startup has finished; database snapshot tests wait
  for the production readiness diagnostic with file logging enabled.
- For descriptor-exhaustion tests, reserve descriptor gaps explicitly and use
  scope guards to release reservations and restore resource limits on early
  assertion returns. Keep Linux-only system-call coverage platform-guarded.
- Source-reading tests must resolve files through the build-provided source
  root, not the current working directory or an assumed in-tree build layout.

For changes to persistence, shutdown, or object lifetimes, run the sanitizer
checks as well as the normal test suites. Preserve sanitizer report checks
for child processes and the Clazy correctness gate; do not broadly suppress
diagnostics to make CI pass.
