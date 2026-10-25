# Architecture

PausePilot is a single-process Qt desktop application. There are **no threads of
our own** — everything runs on the Qt event loop driven by a 100 ms heartbeat
`QTimer` constructed in `main.cpp`. The components below are plain C++ objects
owned on the stack of `main()`; they communicate through Qt signals/slots
wired in the same function.

## Component overview

```mermaid
graph TD
    Heartbeat["QTimer (100 ms)<br/>main.cpp"]
    Settings["Settings"]
    LockWatcher["LockStateWatcher"]
    Store["SqliteSessionStore<br/><i>implements SessionStore</i>"]
    TimeTracker["TimeTracker<br/>(engine)"]
    Shutdown["ShutdownCoordinator"]
    MainWin["MainWin"]
    Content["ContentWidget"]
    Health["HealthMonitor"]
    History["HistoryDialog"]

    Heartbeat --> MainWin
    Heartbeat --> TimeTracker
    Heartbeat --> LockWatcher

    LockWatcher --> TimeTracker

    TimeTracker --> Store
    TimeTracker --> MainWin
    TimeTracker --> Content

    MainWin --> TimeTracker
    MainWin --> Content
    MainWin --> Health
    MainWin --> Shutdown

    Shutdown --> TimeTracker
    Shutdown --> Store

    Content --> TimeTracker
    Content --> History

    History --> TimeTracker

    Settings -.read.-> TimeTracker
    Settings -.read.-> Store
    Settings -.read.-> LockWatcher
    Settings -.read.-> MainWin
    Settings -.read.-> Health
    Settings -.read/write.-> Content
```

Each box in one or two lines:

- **`Settings`** — thin wrapper around the `user-settings.ini` file. Read-only
  reference passed to every component that needs configuration.
- **`LockStateWatcher`** — abstracts OS lock detection (see below). Emits
  `desktopLockEvent(LockEvent)` on lock/unlock/long-lock transitions.
- **`SessionStore`** — abstract persistence interface. The concrete
  implementation `SqliteSessionStore` is the only one in the production binary;
  the test harness substitutes `FakeSessionStore` (see `qtest/`). See
  `persistence.md` for the storage model.
- **`TimeTracker`** — the timing engine and state machine. Owns the session in
  memory, drives checkpoints, talks to `SessionStore` for persistence, and
  emits `transitioned`, `stopped`, `userWarning` for the GUI to
  follow.
- **`ShutdownCoordinator`** — runs the shutdown sequence (see
  `runtime-behaviour.md`). Holds references to `TimeTracker` and `SessionStore`;
  does not depend on any GUI class.
- **`MainWin`** — the top-level Qt window. Owns the tray icon, the
  `ContentWidget`, and the `HealthMonitor`. Wires the Qt-side signals to the
  engine and delegates to `ShutdownCoordinator`.
- **`ContentWidget`** — the central widget: time labels, Start/Pause/Stop
  buttons, history/autopause/pin/min-to-tray buttons. Connects directly to
  `TimeTracker::transitioned()` to update its own display, constructs and
  executes `HistoryDialog` on its own stack when History is opened, and
  writes the autopause/pin-to-top settings itself via its `Settings&`
  reference (not read-only).
- **`HealthMonitor`** — watches accumulated active / pause time per session
  and emits `warningTriggered` if a configurable threshold is exceeded.
- **`HistoryDialog`** — modal editor for the persisted timeline; opens
  lazily from `ContentWidget`. Listens for `TimeTracker::exclusiveEditRevoked()`
  and closes, discarding unsaved edits, when the engine closes History due to
  lock, sleep, midnight, shutdown, or any mode change. Builds its model
  (`HistoryEditSession::buildFromContext()`) from the open-time
  `HistoryEditContext` `TimeTracker::beginExclusiveEdit()` captures, and saves
  a per-date diff (`buildScopedSave()`) via `TimeTracker::saveHistoryEdit()`
  (H5/WP5) rather than a whole-table replace; see "History saves" in
  `doc/persistence.md`.

## The `SessionStore` abstraction

`SessionStore` (in `sessionstore.h`) is a pure-virtual interface listing every
DB operation `TimeTracker` (or `ShutdownCoordinator`) actually calls. `TimeTracker`
holds a `SessionStore&` reference, not a `SqliteSessionStore&`. The seam exists
for one reason: **testability**. In `qtest/` the engine runs against an
in-memory `FakeSessionStore` that records calls and lets tests inject failure
modes without touching disk or SQLite. The interface is small and chosen to
expose *intent* rather than SQL: `commitSession`,
`loadDurations`, `hasEntriesForDate`, `saveCheckpoint`, `discardCheckpoint`,
`checkSchemaOnStartup`, `flushToDisc`, `markCleanShutdown`,
`loadHistoryRows`, `saveHistoryDays`, and `recoverStartupCheckpoints`.
The last one is the sole recovery entry point:
it decides and applies orphan disposition and clean-marker consumption
entirely inside `SqliteSessionStore` (see `persistence.md`); `TimeTracker`
only consumes the `StartupRecoveryResult` it returns.

## What each module hides

| Module             | Hides from its callers                                                                              |
| ------------------ | --------------------------------------------------------------------------------------------------- |
| `TimeTracker`      | The Activity/Pause/None FSM, the midnight policy, the checkpoint cadence, and `SessionState` mutations |
| `SessionStore`     | SQLite, transactions, the `is_finalized` flag, segment-id/orphan bookkeeping, retention             |
| `Timeline`         | Segment normalization, ordering, and the ongoing-segment splice in `activeMsec`/`pauseMsec`         |
| `LockStateWatcher` | The OS API (polled `WTSQuerySessionInformation` on Windows, several D-Bus backends on Linux) and the debounce buffer |
| `HealthMonitor`    | The "show each warning at most once per session" thresholds                                        |
| `MainWin`          | Tray, window flags, message boxes, native event plumbing                                            |

## Domain vs Qt-glue

The application logic lives in **`TimeTracker`**, **`SessionStore`**, **`Timeline`**,
and **`HealthMonitor`** — these compile and run without a `QApplication` and
are exercised that way by the test harness. **`MainWin`** and
**`ContentWidget`** are pure Qt glue: tray icon, layouts, widget signals,
signal-to-slot routing. When you are looking for *behaviour*, start in the
first group; when you are looking for *layout or wiring*, start in the second.

## Single-rule, single-site reference

Each cross-cutting rule is owned by one class. When a question comes up, go
straight to the owner:

| Concern                                    | Owner                                |
| ------------------------------------------ | ------------------------------------ |
| Midnight policy (cross-day cutoff)         | `TimeTracker` via `DayBoundaryWatcher`     |
| Autopause / lock-driven auto-resume        | `TimeTracker` (signalled via `transitioned`)|
| Checkpoint cadence                         | `TimeTracker`                        |
| Orphan checkpoint reconciliation (decision and mechanics) | `SqliteSessionStore`  |
| Clean-shutdown marker                      | `ShutdownCoordinator` + `SessionStore` |
| Database schema, migrations, retention     | `SqliteSessionStore`                 |
| OS lock detection                          | `LockStateWatcher`                   |
| Shutdown sequence (orchestration)          | `ShutdownCoordinator`                |
| Health warnings (long activity / pause)    | `HealthMonitor`                      |

## Lock detection

`LockStateWatcher` is the single boundary between the application and the OS's
notion of "the desktop is locked". On Windows it polls
`WTSQuerySessionInformation` (querying the current session, not the physical
console, so RDP sessions behave correctly) on each 100 ms heartbeat tick. On
Linux it tries several D-Bus backends in order — `systemd-logind`, the
freedesktop screensaver, the GNOME screensaver, then the KDE screensaver —
and uses the first one whose real per-session query succeeds; a failing
active backend triggers a rate-limited (exponential-backoff) rescan of the
chain rather than a scan on every tick. The exact order and method names live
in `lockstatewatcher.cpp` (`LinuxLockBackend`). Both platforms feed a raw
`LockQueryResult` (`Locked`/`Unlocked`/`Unknown`) into a 5-sample debounce
buffer that flips its debounced level — and emits `Lock`/`Unlock` — only once
the last 3 samples agree; a query failure (`Unknown`) leaves the buffer
untouched rather than being treated as an unlock. `LongOngoingLock` is the
separate "the lock has now been held long enough to be a real pause" signal,
timed off wall-clock lock duration, which is what triggers the backpause
behaviour in `TimeTracker`.

## Signal/slot wiring

The connections set up at the bottom of `main()`, plus the button wiring
`MainWin::setupCentralWidget` sets up when it constructs `ContentWidget`, are:

```mermaid
sequenceDiagram
    autonumber
    participant QT as QTimer (100ms)
    participant LW as LockStateWatcher
    participant MW as MainWin
    participant TT as TimeTracker
    participant CW as ContentWidget
    participant HD as HistoryDialog

    QT->>MW: timeout → onTick()
    QT->>TT: timeout → onTick(now)
    QT->>LW: timeout → update()
    LW->>TT: desktopLockEvent(LockEvent)<br/>→ useTimerViaLockEvent
    CW->>TT: startPausePressed()<br/>→ onStartPausePressed
    CW->>TT: stopPressed()<br/>→ onStopPressed
    TT-->>MW: userWarning(QString)<br/>→ showUserWarning
    TT-->>MW: stopped(StopReason)<br/>→ health_monitor_.reset
    TT-->>CW: transitioned(Transition)<br/>→ onTransitioned
    TT-->>HD: exclusiveEditRevoked()<br/>→ onExclusiveEditRevoked
```

`MainWin::onAboutToQuit` is wired separately to
`QCoreApplication::aboutToQuit`; it delegates to `ShutdownCoordinator`. On
Linux, a `QSocketNotifier` reading from a `socketpair` translates
`SIGTERM`/`SIGINT`/`SIGHUP` into the same shutdown call — see
`runtime-behaviour.md`.
