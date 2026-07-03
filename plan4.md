# plan4 — Cross-cutting review findings (10-reviewer synthesis)

Scope agreed: all Section A correctness findings **except A6 and A7** (dropped),
plus all Section C nits. Section B (design debt) is appended at the end as
"Points to discuss later" — do not implement without discussion.

Items marked **[×N]** were flagged independently by N reviewers.

---

## Section A — Correctness / data-integrity fixes

### A1. Repair the broken Windows build (stale MSVC project files)
- `uTimer.vcxproj:190-223` — `historyeditsession.cpp/.h` missing from the MSVC
  project → link failure on any Windows build. Fix: add
  `<ClCompile Include="historyeditsession.cpp" />` and
  `<ClInclude Include="historyeditsession.h" />`.
- `uTimer.vcxproj:143` — Debug|Win32 pins `stdcpp14` while the code needs C++17
  (`std::optional` in types.h, structured binding at `sqlitesessionstore.cpp:596`).
  The moc_predefs custom-build steps (vcxproj:228,235) also pass `-std:c++14`.
  Fix: `stdcpp17` in the Debug ClCompile group and both moc_predefs commands.
- `qtest/qtest.vcxproj:104-130` — Windows test project misses four suites
  (`test_historyeditsession`, `test_history_commit`, `test_persistence_contract`,
  `test_contract_matrix`) plus `..\historyeditsession.cpp`. Fix: mirror
  `qtest.pro` SOURCES/HEADERS into ClCompile/QtMoc/ClInclude.
- `qtest/qtest.vcxproj:69-86` — Debug|Win32 lacks `AdditionalIncludeDirectories`
  (`$(SolutionDir)`) and `LanguageStandard`. Fix: duplicate from Release group.
- `uTimer.sln:22-23` — solution builds qtest in Release, but 9 test files use
  `#ifndef QT_NO_DEBUG` probes and cannot compile there. Fix: remove qtest's
  Release Build.0 entry (Linux pins `CONFIG += debug` in qtest.pro:3).
- Guard against future drift: add a trivial script test that compares `.pro`
  SOURCES lists against `.vcxproj` ClCompile lists.

### A2. Suspend-time miscounting — unify on wall clock **[×3]**
Three clocks disagree across suspend (Linux `CLOCK_MONOTONIC` stops during
suspend; Windows QPC does not):
- `lockstatewatcher.cpp:182,193` — `lock_timer_` (QElapsedTimer) doesn't advance
  during suspend on Linux: lock 17:00 → suspend → resume next morning → quick
  unlock ⇒ `LongOngoingLock` never fires; the whole locked night is credited as
  Activity. Fix: record the lock instant as wall-clock `QDateTime lock_time_`
  and test `lock_time_.msecsTo(QDateTime::currentDateTime()) >= backpauseMsec`
  in `shouldEmitLongLock` (keep the Unknown guard).
- `timer.cpp:570-617` — `backpauseTimer` converts a *fixed* window ending at
  `now`, assuming the event fired exactly at threshold: lock 10:00 → suspend
  10:05 → wake 14:00 → event ~14:10 ⇒ `[start…13:55]` persisted as Activity
  (~4 h of suspended time credited as work). Fix: start the pause at the real
  (wall-clock) lock time passed along with the event, not `now − backpause`.
- `timer.cpp:810-832` — display totals use `timer_.elapsed()` while persistence
  uses wall clock (`timer.cpp:1235` switched deliberately); GUI and DB disagree
  after suspend until stop. Fix: compute the ongoing portion from
  `session_.segment_start_time.msecsTo(now)` — i.e. implement
  `getActiveTime/getPauseTime` via `snapshot().activeMsec()/pauseMsec()` — and
  retire `timer_`'s timing role.

### A3. Midnight-watchdog discard leaks the unfinalized checkpoint row **[×2]**
`timer.cpp:1174-1204` (`discardCrossMidnightOngoingAndStop`) discards the
in-flight segment but its checkpoint row stays `is_finalized = 0`. Two opposite
failures:
- Restart soon after: `recoverStartupCheckpoints` (`sqlitesessionstore.cpp:1525`)
  finalizes the row — history silently regains time the engine deliberately
  discarded, and the clean marker suppresses the notification.
- Restart >24 h later: the row is dropped as stale — checkpointed pre-midnight
  work is permanently lost with no crash having occurred.

Fix: on watchdog discard (and any deliberate discard of a checkpointed
segment), delete or finalize the segment's checkpoint row immediately — e.g. a
new `SessionStore::discardCheckpoint(segmentId)` called from the stop/discard
path. The discard must have an observable side effect (checkpoint cleanup, log
line), not silence.

### A4. Cancelled Windows shutdown permanently consumes the shutdown guard **[×2]**
`mainwin.cpp:220-248` + `shutdowncoordinator.cpp:24-27,59` —
`WM_QUERYENDSESSION` runs the full shutdown (stops the timer, sets
`shutdown_completed_ = true`); if the user cancels the OS shutdown
(`WM_ENDSESSION` wParam=FALSE) the app keeps running, but the next real
shutdown skips the entire save path; Windows then terminates the process
without unwinding `main()`, so everything since the last 5-min checkpoint is
lost. Variant: an open HistoryDialog survives this sequence and a later OK
re-saves finalized rows as unfinalized into a stopped Timer.
Fix: add `ShutdownCoordinator::reset()` and call it on `WM_ENDSESSION(FALSE)`;
consider not stopping the timer until `WM_ENDSESSION(TRUE)`; have
`HistoryDialog::saveChanges()` abort when the engine was shut down.

### A5. Exact-pattern debounce permanently misses Lock on one noisy sample
`lockstatewatcher.cpp:148-153` — the buffer must equal exactly `[U,U,L,L,L]`;
one flaky sample during the transition means Lock is never emitted for the
entire lock session (no autopause; checkpoints keep running while locked); the
mirror case emits phantom Unlocks. The test comment at
`qtest/test_lockstatewatcher.cpp:9` says "majority" — the intended semantics —
but the code is exact-match.
Fix: hysteresis on a debounced level — track `debounced_locked_`, flip to
Locked when the last 3 samples are all Locked, flip to Unlocked when the last 3
are all Unlocked, emit Lock/Unlock on the flip. Guarantees Lock/Unlock
alternation and recovery from arbitrary noise.

### A8. Backward clock jump + HistoryDialog OK silently stops the engine **[×2]**
`timer.cpp:1007` (`getOngoingDuration_locked` returns nullopt when
`segment_start_time >= now`) + `historyeditsession.cpp:171-176` (clears the
pending ongoing when the engine reports none) + `timer.cpp:955-962`
(no-ongoing commit → `stopped(EditApplied)`). An NTP/DST step back plus an OK
click ends the running session and drops the in-flight time.
Fix: distinguish "engine stopped" from "ongoing temporarily invalid" —
`refreshOngoing` should check `timer.isStopped()` before clearing; or
`getOngoingDuration_locked` returns a zero-length-capped ongoing for the
clock-back case (keep hiding only the cross-midnight case, which the watchdog
owns).

### A9. Merge-save demotes finalized history rows; stale policy then deletes them
`historyeditsession.cpp:220-241` (memory-interval re-route) +
`sqlitesessionstore.cpp:1560-1568` (stale drop). When normalization merges a DB
history row with a memory row, the merged row is routed to the session bucket
and written `is_finalized = 0`; crash + relaunch >24 h later ⇒
previously-finalized work is silently deleted by the stale-orphan policy.
Fix: finalize completed session rows on save (only the ongoing row stays
unfinalized), or make the stale policy finalize-not-drop for multi-minute
orphans.

### A10. Clicking OK with zero edits can rewrite and merge DB history
`historyeditsession.cpp:208-210` — `buildSavePayload` normalizes all pages
unconditionally; adjacent same-type rows with <500 ms gaps (or ≤50 ms
near-duplicates) from any past day get merged and the whole DB is rewritten via
delete-all `replaceAll`, on a no-op visit, behind a prompt that misleadingly
says "overlapping".
Fix: skip the save entirely when no `pendingTimelines_` entry differs from its
page's original rows.

### A11. Crash-recovery notification compares against the *oldest* finalized orphan
`sqlitesessionstore.cpp:1605-1617` — an old overlap-rejected orphan covered by
the marker suppresses the notification for a *newer genuine crash*.
Fix: compare the marker against the **newest** finalized orphan's end (`max`
instead of `min`); notify iff any finalized orphan ends after the marker.

### A12. Empty `segment_id` row bricks debug builds and corrupts release upserts
`sqlitesessionstore.cpp:833` calls `SegmentId::fromString()` which `Q_ASSERT`s
non-empty (`types.h:34`); `checkSchemaOnStartup()` does not validate the
`CHECK(length(segment_id) > 0)` constraint, so an old/hand-edited DB
assert-aborts every `loadDurations()` in debug builds, and in release the empty
id flows into `UPDATE … WHERE segment_id = ''`, matching every empty-id row.
Fix: treat an empty segment_id as a corrupt row (skip + `result.skipped++`) in
`loadDurations()`, and the same guard in `loadUnfinalizedCheckpoints()`
(line 1259).

### A13. HealthMonitor warnings fire once per app lifetime, not per session
`healthmonitor.cpp:26-30` + `mainwin.cpp:183` — `reset()` runs only in
`MainWin::start()`; a long-running tray app warns on Monday and never again,
contradicting architecture.md's "at most once per session".
Fix: connect `Timer::started(false)` (fresh session) or `Timer::stopped` to
`health_monitor_->reset()` in MainWin.

### A14. Remaining confirmed bugs (small fixes)
- **No single-instance guard** — a second instance's startup recovery
  (`sqlitesessionstore.cpp:1525`) finalizes the first instance's *live*
  checkpoint row; the first instance's next `saveCheckpoint` UPDATE matches
  nothing, falls to INSERT, hits UNIQUE → CallerBug. `createBackup`'s
  close-copy-reopen (line 499) can also copy a torn snapshot. Fix: lock file at
  startup and refuse a second instance (or scope recovery to rows older than a
  grace period).
- **`commitEditedTimeline` half-adopts at `mode_ == None`** —
  `timer.cpp:935-954`: `adoptOngoingSegment` sets `segment_id`/start time while
  the engine stays stopped — the state the header documents as impossible.
  Fix: either start the engine (`mode_ = desired`, emit `started(false)`) or
  refuse/ignore the ongoing when `mode_ == None`; don't half-adopt.
- **Shutdown guard set too late / pump re-entrancy** —
  `shutdowncoordinator.cpp:22-59`: set `shutdown_completed_ = true` immediately
  after the guard check; a queued START click dispatched inside
  `pumpEvents(150)` can restart the timer after `shutdown()` — drain events
  before calling `timetracker_.shutdown()` or re-check/re-stop after the pump.
- **`boot_time_sec_` unbounded** — `settings.cpp:56`: only numeric key with no
  `qBound`; garbage values silently void the boot credit (rejected as
  cross-midnight). Fix: `qBound(0u, …, 3600u)` (or similar cap).
- **Backward clock resurrects future-dated orphans** —
  `sqlitesessionstore.cpp:1562`: `endTime.msecsTo(now)` is negative for a
  future-dated orphan, so it passes the stale check and gets finalized. Fix:
  also drop orphans with `endTime > now + slack`.
- **Data dir = executable dir** — `apppaths.h:12-14`: installing to
  Program Files / /usr/bin ⇒ writability check fails ⇒ `Inaccessible` ⇒ hard
  startup failure with no user remedy. Decide: document the portable-only
  constraint in README, or fall back to `QStandardPaths::AppDataLocation` when
  the app dir is not writable.

Dropped by decision: A6 (RDP `WTS_CURRENT_SESSION`), A7 (Linux logind
`LockedHint` functional probe).

---

## Section C — Nits (naming, comments, hygiene)

- Class `Timer` shadows `QTimer` and every call site already dodges the name
  (`time_tracker`, `timetracker_`) — rename class to `TimeTracker`, files to
  timetracker.h/cpp.
- `lazyClose()` closes eagerly (`sqlitesessionstore.cpp:225-230`); rename to
  `closeConnection()` and fix the self-contradictory probe comment
  (`sqlitesessionstore.h:131-133`).
- `saveCheckpointInternal` (`timer.cpp:1227`) encodes the held-mutex
  precondition only in a comment while the sibling uses the `_locked` suffix
  (`getOngoingDuration_locked`) — rename to `saveCheckpoint_locked` (update the
  `_dbg` probe).
- `snapshot()` vs `getCurrentDurations()` (`timer.h:281-284`) differ only via
  comments — rename the latter to `completedTimeline()` (or fold into
  `snapshot(bool includeOngoing)`).
- Comments keyed to unresolvable task IDs — `T5.1/T5.2/T5.3` (timer.h:123-125),
  `T8` (timer.cpp:384), `T10` (timer.cpp:453), `T1+T9` (timer.cpp:462), `S5`
  (sqlitesessionstore.cpp:639) — strip the codes, keep the plain-language
  rationale.
- `historydialog.cpp:7,13` — file header twice names a nonexistent
  `commitChanges()` (method is `saveChanges()`); doc blocks at :315-324 and
  :556-564 sit on the wrong functions (`addReadOnlyRow` / `presetSplitDialog`
  instead of `updateTable` / `onSplitRow`) — fix the name, move both blocks.
- `maybeReanchorCheckpoint` failure logs are prefixed
  `"[CHECKPOINT] replaceCurrentDurations: …"` (`timer.cpp:890-897`) — stale
  caller name; change prefix to `maybeReanchorCheckpoint:`.
- `mainwin.cpp:2` header overstates ("Application entry point and
  orchestrator" — it is Qt glue per doc/architecture.md); `shutdown()`'s 3-step
  doc block (`mainwin.cpp:186-196`) describes a one-line delegate — rewrite
  both.
- `hasEntriesForDate` (`sqlitesessionstore.cpp:898-905`) is the only store
  method without an explicit `history_days_to_keep_ == 0` guard — add it to
  match the siblings (return `Unknown`).
- `flushToDisc` (`sqlitesessionstore.cpp:1186`) promotes `synchronous=FULL` and
  never restores NORMAL despite the "temporarily promotes" doc — restore NORMAL
  after the flush or fix the comment.
- `pumpEvents` busy-spins for the full 150 ms (`shutdowncoordinator.cpp:14-20`)
  — use `QEventLoop` + `QTimer::singleShot`.
- Logger rotation runs only in a destructor that never executes on crash or
  Windows `WM_ENDSESSION` termination (`logger.cpp:45-72`); the file grows
  unbounded meanwhile — rotate on startup instead.
- `setupUnixSignalHandlers()` return value ignored (`main.cpp:93`) — log on
  failure.
- Repo hygiene: committed `databasemanager.o` in the root; stray zero-byte
  `qtest/sqlitesessionstore.cpp`; `uTimer.sqlite*` backup artifacts left in
  qtest/ by test runs.
- doc/persistence.md describes the pre-refactor seam (marker "written by
  ShutdownCoordinator"; reconciliation decided by Timer via interface methods)
  — both moved; update the doc.

---

## Recommended implementation order

Ranked by impact ÷ effort; items grouped so that files touched together land
together. Each step should end green (build + full qtest run, zero QWARN).

**Phase 1 — Make both platforms buildable again**
1. **A1** — repair the MSVC project files (vcxproj entries, `stdcpp17`, qtest
   suites, drop qtest Release from the .sln) and add the .pro↔.vcxproj parity
   script. Trivial mechanical work, unblocks Windows entirely, and every later
   fix should be verified against both build systems.

**Phase 2 — Clock seam (promoted from B11): the regression-test enabler**
2. Inject a now-provider into `Timer` (e.g. `std::function<QDateTime()>`
   defaulting to `QDateTime::currentDateTime()`, or a `now` parameter on the
   entry points that don't already take one) and wire `qtest/fakeclock.h` into
   the Timer tests. Add the analogous seam to `LockStateWatcher` (wall-clock
   lock time + a stubbed query result probe) so lock/suspend scenarios are
   drivable too.
   Scope note: this is ONLY the clock/query seam — the SessionState
   encapsulation and StateGuard removal from B11 stay in "points to discuss
   later". While here, de-flake the worst offenders: the midnight-sensitive
   tests that arm the real scheduled stop, and the ~7 s of `qWait` sleeps
   (seed backdated checkpoint rows via `rawDb_dbg()` instead).
   Every fix in Phases 3–5 must land WITH a regression test that reproduces
   the bug through this seam first (red), then passes with the fix (green).

**Phase 3 — The two silent-miscounting families (core product promise)**
3. **A2** — wall-clock unification: lock timestamp in LockStateWatcher,
   backpause anchored at the real lock time, display totals from
   `segment_start_time`. Highest data-correctness impact for small, localized
   edits. Regression tests: fake-clock suspend gap between lock and
   LongOngoingLock → backpause starts at lock time; display total equals
   persisted total across a simulated suspend.
4. **A5** — debounce hysteresis. Same file as the A2 lock-watcher change; do
   it in the same sitting to touch `lockstatewatcher.cpp` once. Regression
   tests: noisy transition (`U,U,L,U,L,L,L,…`) still emits exactly one Lock;
   Lock/Unlock strictly alternate; LongOngoingLock fires at most once per
   lock session (drive `update()` via the stubbed query probe).
5. **A3** — `discardCheckpoint(segmentId)` on watchdog/deliberate discard.
   Small store call + one call site; closes both the resurrection and the
   silent-loss path. Prerequisite for discussing B2 later. Regression test:
   checkpoint written → fake clock crosses midnight → watchdog discard →
   assert zero `is_finalized = 0` rows remain.

**Phase 4 — Shutdown data-loss window**
6. **A4** — `ShutdownCoordinator::reset()` on `WM_ENDSESSION(FALSE)`, plus the
   two A14 coordinator hardenings that belong to the same file: set
   `shutdown_completed_` immediately after the guard check, and drain/re-check
   around `pumpEvents`. Regression tests: run → reset → run flushes twice;
   nested run() during the pump flushes once; also add the missing
   coordinator-marker-branch test (timer already stopped → marker written
   after flush) flagged by the test review.
7. **A13** — HealthMonitor reset per session. One `connect()` line; slot it
   here because it is MainWin wiring like A4's caller. Regression test:
   warning fires in session 1, timer stopped/restarted, warning fires again
   in session 2.

**Phase 5 — Recovery & history-edit edge cases**
8. **A11** — newest-orphan marker comparison (`max` instead of `min`).
   Regression test: two orphans, marker covers the older only → notify.
9. **A12** — empty-`segment_id` rows treated as corrupt at load. Regression
   test: seed an empty-id row via `rawDb_dbg()` → `loadDurations()` skips it
   (`skipped` incremented), no assert.
10. **A9** — stop demoting finalized rows on merge-save (or finalize completed
    session rows on save). Decide the approach before coding; it interacts
    with A3's cleanup. Regression test: merge-save → crash-simulate →
    recovery >24 h later (fake clock) → the merged row survives.
11. **A10** — no-edit OK becomes a true no-op (skip save when nothing
    changed). Also removes the misleading "overlapping" prompt on gap merges.
    Regression test: adjacent rows with a <500 ms gap survive an editless OK
    byte-for-byte.
12. **A8** — backward-clock-jump guard in `refreshOngoing` /
    `getOngoingDuration_locked`. Same files as A9/A10; do after them so the
    history-edit test updates land once. Regression test: fake clock steps
    back 2 min while running → dialog OK → engine still running, no
    `stopped(EditApplied)`.

**Phase 6 — Remaining small fixes (rest of A14)**
13. Single-instance lock file; `commitEditedTimeline` half-adopt at None;
    `boot_time_sec_` bound; future-dated-orphan drop; data-dir decision
    (likely just a README note). Each with a matching test where the seam
    allows (half-adopt and future-dated-orphan are fake-clock testable).

**Phase 7 — Section C nits**
14. Behavior-adjacent nits first (`hasEntriesForDate` disabled guard,
    `flushToDisc` NORMAL restore, logger rotation on startup, `pumpEvents`
    event-loop rewrite, signal-handler failure log) — each is a tiny
    verifiable change.
15. Comment/doc fixes (task-ID codes, stale `commitChanges` header, misplaced
    doc blocks, log prefix, MainWin header, doc/persistence.md drift) and repo
    hygiene (`databasemanager.o`, stray zero-byte file, qtest artifacts) —
    zero risk, any time.
16. **Renames last** (`Timer`→`TimeTracker`, `lazyClose`→`closeConnection`,
    `saveCheckpoint_locked`, `completedTimeline`) — they churn many lines, so
    doing them after all bug fixes keeps the earlier diffs reviewable.

---

## Points to discuss later (Section B — design debt, do not implement yet)

1. **Store error vocabulary fragmentation [×3]** — sqlitesessionstore.cpp uses
   ~8 result shapes (`SessionStoreResult`; raw bool at :629/:1270/:1487;
   `optional<MarkerResult>` with a nested tri-state at :1451;
   `optional<deque>` at :1216; plus LoadResult.status, BackupResult,
   SchemaStatus, StartupRecoveryResult.ok). Worst instance:
   `finalizeIfNoOverlap`'s bool conflates overlap-skip / already-finalized /
   SQL error, so `reconcileUnfinalizedCheckpoints:1403-1411` reports transient
   DB errors upward as policy drops. Proposal: one `Category` vocabulary for
   private helpers; `{Finalized, OverlapSkipped, Error}` enum for
   `finalizeIfNoOverlap`; convert `saveDurations` to `SessionStoreResult`.
2. **Clean-shutdown marker has two writers [×4]** — `timer.cpp:656` writes on
   every stop; `shutdowncoordinator.cpp:46` writes only when
   `canCleanMark && !stopCalled` (the field exists solely to encode "marker
   already written elsewhere"); the coordinator branch is untested.
   ⚠ Caution: the per-stop marker is what suppresses false "recovered N
   seconds" notifications for orphans covered by mid-session clean stops
   (`sqlitesessionstore.cpp:1595-1617`); coordinator-only writes would change
   recovery semantics. Proposed resolution: keep the per-stop write as owner,
   drop the coordinator's duplicate branch and the `stopCalled` handshake —
   but only after A3 is fixed so the marker can't launder discarded segments.
3. **HistoryEditSession leaks its model mutably** — `historyeditsession.h:86`
   non-const `pendingTimelines()`; dialog lambdas
   (`historydialog.cpp:418-486`) and `onSplitRow:639-650` mutate the model
   directly and must remember `registerSplitChild` or origin re-splitting
   silently corrupts. Proposal: `toggleSegmentType` / `setOngoingType` /
   `splitSegment` methods on the session, origin registration inside;
   `applySplit_dbg` collapses onto the same method.
4. **Dialog-open deferral is a hand-rolled special-case queue** —
   `dialog_open_` checked in 5 places with two ad-hoc pending slots and
   priority rules visible only in `endExclusiveEdit`
   (`timer.cpp:752-798, 1274-1290`). Proposal: one
   `deferIfEditing(DeferredEvent)` gate + a single `optional<DeferredEvent>`
   slot with the supersede rules beside it.
5. **Duplicate signal channels with a documented trap** — `timer.h:343-356`:
   lock-resume fires both `started(true)` and `modeChanged(LockResume)`;
   correctness rests on a "GUI must not double-act" comment. Proposal: three
   cause-carrying signals (`started(PauseCause)`, `paused(PauseCause)`,
   `stopped(StopReason)`); delete `modeChanged`.
6. **Timer↔UI wiring is split-brain + dead dependency** — ContentWidget holds
   `Timer&` and wires Timer→GUI internally (`contentwidget.cpp:49-59`), but
   button→Timer goes through relay signals wired in `mainwin.cpp:50-53`;
   `MainWin::db_` (`mainwin.h:22`) is never used. Proposal: wire buttons
   directly in ContentWidget, delete the relay slots/signals; drop `db` from
   MainWin's constructor.
7. **`getDurationsHistory()` pass-through with temporal coupling [×2]** —
   `timer.cpp:982-995` forwards `loadDurations()` and caches skipped/repaired
   for a later getter with a hidden call-order protocol. Also:
   **`LoadResult.repaired` is never set anywhere**, yet the UI banner reports
   "N rows auto-repaired" (`historydialog.cpp:263`). Proposal: return
   `LoadResult` directly (rename `loadHistoryFromStore()`), delete
   `getLastHistoryLoadStats()`; fix or drop the "repaired" claim.
8. **Three names for one operation [×3]** — `commit()`,
   `replaceCurrentDurations()`, `commitEditedTimeline()`
   (`timer.cpp:971-980`). Proposal: delete the first two; point
   `replaceCurrentDurations_dbg` at `commitEditedTimeline(Timeline(d, o))`.
9. **`accept()`/`setResult()` handshake [×2]** —
   `historydialog.cpp:89-101,119-122` routes control flow through QDialog's
   result register purely for a test-only guard. Proposal: delete the
   `result()` guard from `saveChanges()`; `accept()` becomes
   `if (saveChanges()) QDialog::accept();`.
10. **Error-code sniffing** — `sqlitesessionstore.cpp:1003-1022` string-matches
    native code `"19"` to detect finalized-row conflicts. Proposal: query
    `is_finalized` inside the open transaction; single
    `INSERT … ON CONFLICT(segment_id) DO UPDATE … WHERE is_finalized = 0`.
11. **Timer's debug surface ≈ its private state [×2]** — ~18 `_dbg` probes
    (`timer.h:361-420`) freeze internals into white-box tests.
    NOTE: the clock-seam half of this finding (now-provider injection,
    fakeclock wiring) has been **promoted to Phase 2** of the implementation
    order above; what remains to discuss here is the structural half:
    make `SessionState` fields private so the compiler enforces what
    `StateGuard` polices, then delete StateGuard, and shrink the `_dbg` probe
    set by asserting through `snapshot()` + FakeSessionStore call recording.
    ⚠ Caution (test reviewer): don't delete the guard *before* the
    encapsulation — it is currently the suite's safety net.
12. **Store header exports implementation vocabulary** — `TransactionMode`,
    `BackupResult`, public `saveDurations`/`updateDurationsById`
    (`sqlitesessionstore.h:18-59`) bypass the SessionStore seam. Proposal:
    move enums into the .cpp anonymous namespace, make the helpers private;
    tests seed via `commitSession`/`replaceAll` or `rawDb_dbg()`.
13. **Dead/unreachable code** — `classifyMerge` branches 2-3 unreachable after
    `normalized()`'s sort (`timeline.cpp:159-165`); `maybeReanchorCheckpoint`
    can never fire from the production dialog flow (`saveChanges()` runs while
    `dialog_open_` is true — the documented re-anchor contract is false in
    practice; decide: move the re-anchor into `endExclusiveEdit()` or delete
    it with `EditCommitResult`); `setDurationType` (`timer.cpp:846`) has no
    production caller; ongoing-row split guard at `historydialog.cpp:591-594`
    unreachable; `applyMergeDecision`'s `curr_end` local unused
    (`timeline.cpp:196`).
14. **`retryUnsavedDurations` bool + state re-inspection** —
    `timer.cpp:489-490` re-derives the outcome from mutated state
    (`!session_.has_unsaved_data`). Proposal: return
    `RetryOutcome {NothingToRetry, Saved, KeepUnsavedAndContinue, Abort}`.
15. **Page-0 representation mismatch** — `Page.durations` includes the ongoing
    row only for page 0, forcing the off-by-one at
    `historydialog.cpp:377-398`. Proposal: store completed-only for all pages;
    keep the "+1" only in the title count in `buildFromTimer`.
16. **`useTimerViaLockEvent` fall-through control flow** —
    `timer.cpp:752-798`: Unlock is handled in two places (the :767 block
    deliberately does not return and falls through to :792); a future `return`
    silently kills auto-resume. Proposal: `switch (event)` dispatching to
    `onLockDetected` / `onUnlockDetected` / `onLongLock`, each owning its
    complete behavior including the autopause gate.
