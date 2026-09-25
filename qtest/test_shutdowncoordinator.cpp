#include "test_shutdowncoordinator.h"
#include "fakesessionstore.h"
#include "fakeclock.h"
#include "shutdowncoordinator.h"
#include <QCoreApplication>
#include <QtTest>

using TestCommon::createSettingsFile;
using TestCommon::fixedNow;

// ============================================================================
// D15/F1 (part 3 + part 4, B2): this suite's tests call Start() and never
// call onTick() themselves, so the scheduled midnight stop -- now driven
// purely from the heartbeat (C5), with no dedicated QTimer of its own --
// cannot fire on its own during any test's run regardless of the real wall
// clock. Historically (before C5) Start() armed a real QTimer for
// 23:59:59.500 that could fire mid-suite near real local midnight; that
// class of flake is gone.
//
// Every test that calls Start() still gets clock_, a fixture-owned FakeClock
// pinned to fixedNow() (local noon), via init()/cleanup() below, so
// segment_start_time and any assertions about elapsed time stay independent
// of the real wall clock. The one deliberate exception,
// test_A4_forced_shutdown_with_unresolved_discard_writes_no_marker(), pins
// its own pre-midnight anchor on purpose (it tests the cross-midnight
// watchdog itself) and does not use clock_.
// ============================================================================

void ShutdownCoordinatorTest::init()
{
    clock_ = new FakeClock(fixedNow());
}

void ShutdownCoordinatorTest::cleanup()
{
    delete clock_;
    clock_ = nullptr;
}

// ============================================================================
// Test G — happy path: stop, flush, marker
// ============================================================================

void ShutdownCoordinatorTest::test_G_happy_path_stop_flush_marker()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);
    QVERIFY(tracker.getOngoingDuration().has_value());

    fakeDb.callLog.clear();

    coordinator.run();

    QVERIFY(!tracker.getOngoingDuration().has_value());
    QVERIFY(fakeDb.callLog.contains("commitSession"));
    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    int commitIdx = fakeDb.callLog.indexOf("commitSession");
    int markerIdx = fakeDb.callLog.indexOf("setLastCleanShutdownMarker");
    int flushIdx  = fakeDb.callLog.indexOf("flushToDisc");
    // C9a: run() is now fully synchronous (no event-loop pump between the
    // drain and finishDrain()) -- assert the whole sequence's order:
    // drain (commitSession) then the marker, then the FULL barrier.
    QVERIFY2(commitIdx < markerIdx && markerIdx < flushIdx,
             "run() must be synchronous: commitSession, then marker, then barrier, in that order");
}

// ============================================================================
// Test H — idempotent: second run() is a no-op
// ============================================================================

void ShutdownCoordinatorTest::test_H_idempotent_second_run_is_noop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);

    coordinator.run();

    fakeDb.callLog.clear();

    coordinator.run();

    QVERIFY(!fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));
}

// ============================================================================
// Test K — cancelShutdown() re-arms the guard so a second real shutdown
// flushes again (A4: a cancelled OS shutdown must not permanently consume
// the guard); D5/R15-002: it also clears the tracker's terminal flag so the
// application is an ordinary usable stopped session, with no new Start
// required first.
// ============================================================================

void ShutdownCoordinatorTest::test_K_cancelShutdown_allows_second_shutdown()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);

    coordinator.run();
    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(tracker.wasShutDown());

    // Simulate WM_ENDSESSION(FALSE): the OS shutdown was cancelled.
    coordinator.cancelShutdown();
    QVERIFY(!tracker.wasShutDown());

    // User keeps working: a fresh session starts.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock_->advance(20);

    fakeDb.callLog.clear();
    coordinator.run();

    QCOMPARE(fakeDb.callLog.count("flushToDisc"), 1);
}

// ============================================================================
// Test M — timer already stopped before run(): the coordinator's single
// markCleanShutdown() call still fires (D6), marker before flush like every
// other clean-drain case -- there is no separate "already stopped" branch
// anymore.
// ============================================================================

void ShutdownCoordinatorTest::test_M_marker_before_flush_when_already_stopped()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);

    // TimeTracker never started -> already in Mode::None, no pending payload:
    // drainForShutdown() reports clean immediately.
    fakeDb.callLog.clear();

    coordinator.run();

    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QVERIFY2(fakeDb.callLog.lastIndexOf("setLastCleanShutdownMarker") < fakeDb.callLog.lastIndexOf("flushToDisc"),
             "marker must precede flush (D6: markCleanShutdown writes the marker, then the barrier)");
}

// ============================================================================
// C9b: shutdown is terminal until cancelShutdown() -- a Start dispatched
// directly, or queued to fire after run() returns, is refused. A second
// run() just returns the cached result -- no extra marker/flush call.
// ============================================================================

void ShutdownCoordinatorTest::test_C9b_start_refused_after_run_direct_and_queued()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);

    const DrainResult first = coordinator.run();
    QVERIFY(first.clean());
    QVERIFY(tracker.wasShutDown());

    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);
    const int commitCountAfterShutdown = fakeDb.callLog.count("commitSession");

    // Act: a direct Start after shutdown is refused.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.wasShutDown());
    QCOMPARE(transitionedSpy.count(), 0);
    QCOMPARE(fakeDb.callLog.count("commitSession"), commitCountAfterShutdown);

    // Act: a Start queued to fire on the next event-loop turn is also refused.
    QTimer::singleShot(0, [&tracker]() {
        tracker.useTimerViaButton(Button::Start);
    });
    QCoreApplication::processEvents();
    QVERIFY(tracker.isStopped());
    QCOMPARE(transitionedSpy.count(), 0);

    // Act: a second run() returns the cached result -- no extra work.
    fakeDb.callLog.clear();
    const DrainResult second = coordinator.run();

    // Assert
    QVERIFY(second.clean());
    QVERIFY(!fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));
}

// ============================================================================
// C9a: run() no longer pumps the event loop, so a Start queued (via
// QTimer::singleShot(0, ...)) before run() is called must not be dispatched
// by run() itself -- only once the caller's own event loop turns does it
// fire, and C9b's terminal guard then refuses it.
// ============================================================================

void ShutdownCoordinatorTest::test_C9a_run_does_not_dispatch_start_queued_before_run()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);

    // Arrange: queue a Start before run() is called.
    bool startDispatched = false;
    QTimer::singleShot(0, [&tracker, &startDispatched]() {
        startDispatched = true;
        tracker.useTimerViaButton(Button::Start);
    });

    // Act: run() itself must not dispatch the queued slot -- it no longer
    // pumps the event loop.
    const DrainResult result = coordinator.run();

    // Assert: the queued Start has not fired yet, and run()'s own drain
    // stopped the tracker cleanly.
    QVERIFY(!startDispatched);
    QVERIFY(tracker.isStopped());
    QVERIFY(result.clean());

    // Spy created after run()'s own stop transition, so it only observes
    // the queued Start's (refused) attempt below.
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);
    const int commitCountBefore = fakeDb.callLog.count("commitSession");

    // Act: only now does the caller's own event loop turn dispatch it.
    QCoreApplication::processEvents();

    // Assert: it fired, but C9b's terminal guard refuses it -- no new
    // transition and no new commitSession.
    QVERIFY(startDispatched);
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.wasShutDown());
    QCOMPARE(transitionedSpy.count(), 0);
    QCOMPARE(fakeDb.callLog.count("commitSession"), commitCountBefore);

    // Assert: a second run() returns the cached clean result -- no new
    // marker/flush call.
    fakeDb.callLog.clear();
    const DrainResult second = coordinator.run();
    QVERIFY(second.clean());
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QVERIFY(!fakeDb.callLog.contains("flushToDisc"));
}

// ============================================================================
// A3 (R11-001): run()/retryDrain() consume the typed DrainResult. Clean-
// marker eligibility comes from that result, not from mode_ == None.
// ============================================================================

void ShutdownCoordinatorTest::test_A3_run_incomplete_then_retryDrain_succeeds()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);

    // Act: the store fails, so run()'s own drain is incomplete.
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    const DrainResult first = coordinator.run();
    QVERIFY(!first.clean());
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    // Act: the store recovers; the user clicks Retry.
    fakeDb.callLog.clear();
    fakeDb.commitSessionResult = SessionStoreResult::success();
    const DrainResult second = coordinator.retryDrain();

    // Assert: resolved, flushed, and the marker is now written.
    QVERIFY(second.clean());
    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));
}

void ShutdownCoordinatorTest::test_A3_run_incomplete_then_retryDrain_fails()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);

    fakeDb.commitSessionResult = SessionStoreResult::transient("still failing");
    const DrainResult first = coordinator.run();
    QVERIFY(!first.clean());

    // Act: Retry, but the store still fails.
    fakeDb.callLog.clear();
    const DrainResult second = coordinator.retryDrain();

    // Assert: still incomplete, no marker written.
    QVERIFY(!second.clean());
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    // Cleanup so destruction does not find an unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(coordinator.retryDrain().clean());
}

void ShutdownCoordinatorTest::test_A3_run_already_stopped_no_pending_is_noop_success()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);

    // TimeTracker never started -> already Mode::None, no pending payload.
    const DrainResult result = coordinator.run();

    QVERIFY(result.clean());
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));
}

void ShutdownCoordinatorTest::test_A3_direct_mode_failure_emits_no_warning_and_no_marker()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);
    QSignalSpy warnSpy(&tracker, &TimeTracker::userWarning);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);

    // Act: OS-forced shutdown; the store fails. Per policy, a forced
    // shutdown attempts once and exits with no warning and no clean marker.
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    const DrainResult result = coordinator.run();

    // Assert
    QVERIFY(!result.clean());
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QCOMPARE(warnSpy.count(), 0);

    // Cleanup so destruction does not find an unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(coordinator.retryDrain().clean());
}

// ============================================================================
// A4 (R05-001): an unresolved checkpoint-discard intent must block clean
// marking exactly like a pending commit -- including the explicit
// best-effort termination exception: an OS-forced shutdown attempts once
// and, on failure, exits with no warning and no clean marker.
// ============================================================================

void ShutdownCoordinatorTest::test_A4_forced_shutdown_with_unresolved_discard_writes_no_marker()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // ~TimeTracker() drain note in test_timer.cpp).
    FakeClock clock(preMidnight);
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);

    // The engine crosses midnight and deliberately rejects the segment, but
    // the store keeps failing the discard.
    fakeDb.discardCheckpointResult = SessionStoreResult::transient("injected discard failure");
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime));
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop_dbg(clock.now()));
    QVERIFY(tracker.hasPendingDiscards_dbg());

    fakeDb.callLog.clear();

    // Act: OS-forced shutdown attempts once; per policy it shows no warning
    // on failure, but it must also NOT write a clean marker -- the discard
    // intent is still unresolved.
    const DrainResult result = coordinator.run();

    // Assert
    QVERIFY(!result.clean());
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    // Cleanup so destruction does not find an unresolved intent.
    fakeDb.discardCheckpointResult = SessionStoreResult::success();
    QVERIFY(coordinator.retryDrain().clean());
}

// ============================================================================
// D5 (R15-002): cancelShutdown() is one lifecycle transition owned by
// ShutdownCoordinator that atomically re-arms its own guard and clears
// TimeTracker's terminal flag -- History must accept a save right after
// cancellation, with no new Start in between.
// ============================================================================

void ShutdownCoordinatorTest::test_D5_cancelShutdown_makes_history_available_without_new_start()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    // Arrange: an active session shut down as if by WM_QUERYENDSESSION.
    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);
    coordinator.run();
    QVERIFY(tracker.wasShutDown());

    // Act: WM_ENDSESSION(FALSE) -- the shutdown was cancelled.
    coordinator.cancelShutdown();

    // Assert: the tracker's own terminal flag is already clear -- no Start
    // needed. (History's actual per-dialog save gate is shutdownEpoch(),
    // covered by qtest/test_historydialog.cpp's D5 cases; this test is a
    // ShutdownCoordinator/TimeTracker contract check, not a History proxy.)
    QVERIFY(!tracker.wasShutDown());
    QVERIFY(tracker.isStopped());
}

// ============================================================================
// D6 (R05-002/R15-004): clean marking (marker write + FULL barrier) is one
// durable store operation, attempted only when the drain itself is clean.
// Medium-2: an incomplete drain still bypasses the clean MARKER but must
// still run the durability barrier on its own -- plan section 2 promises
// existing durable checkpoints remain "available for best-effort startup
// recovery", which requires this fsync.
// ============================================================================

void ShutdownCoordinatorTest::test_D6_incomplete_drain_skips_marker_but_still_barriers()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);

    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    fakeDb.callLog.clear();

    const DrainResult result = coordinator.run();

    QVERIFY(!result.clean());
    QVERIFY2(fakeDb.callLog.contains("flushToDisc"),
             "Medium-2: the durability barrier must still run for an "
             "incomplete drain -- only the clean marker is bypassed");
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    // Cleanup so destruction does not find an unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(coordinator.retryDrain().clean());
}

// High-1 (R05-002): a marker write failure on an otherwise-clean drain must
// downgrade the result to non-clean AND must never reach the durability
// barrier -- SessionStore::markCleanShutdown()'s contract is that the
// barrier is attempted only after a successful marker write. Regression bar
// (per review): reverting finishDrain()'s downgrade to a log-only warning
// must turn this test red.
void ShutdownCoordinatorTest::test_D6_marker_failure_is_not_clean_and_skips_barrier()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);

    fakeDb.setMarkerResult = SessionStoreResult::transient("injected marker failure");
    fakeDb.callLog.clear();

    // Act
    const DrainResult result = coordinator.run();

    // Assert: not clean, but the failure is attributed to the marker only --
    // session data itself was drained successfully (Medium-3).
    QVERIFY(!result.clean());
    QVERIFY(result.markerOnlyFailure);
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QVERIFY2(!fakeDb.callLog.contains("flushToDisc"),
             "no barrier attempted after a failed marker write");
}

// High-1/M1 (R05-002): the durability-barrier half of markCleanShutdown()
// failing (marker write itself succeeded) must also report non-clean, and
// is NOT a markerOnlyFailure -- the marker WAS committed; only the fsync
// meant to cover it (and the session data) failed, which is a genuine
// durability failure, not "the record was never written". Presenting it as
// markerOnlyFailure would falsely reassure the user in exactly the case
// where durability is known to have failed (M1).
void ShutdownCoordinatorTest::test_D6_barrier_failure_after_successful_marker_is_not_clean()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);

    fakeDb.flushToDiscResult = SessionStoreResult::transient("injected barrier failure");
    fakeDb.callLog.clear();

    // Act
    const DrainResult result = coordinator.run();

    // Assert: marker WAS written (it precedes the barrier), but the whole
    // result is still non-clean since the barrier that must cover it failed
    // -- and this must NOT be reported as markerOnlyFailure (M1).
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QVERIFY(!result.clean());
    QVERIFY(!result.markerOnlyFailure);
}

// High-1 (R05-002/R05-005): Disabled history's marker write must not be
// treated as a failure -- markCleanShutdown() returning Disabled keeps the
// drain clean, exactly like every other Disabled no-op in this codebase.
void ShutdownCoordinatorTest::test_D6_disabled_history_marker_keeps_drain_clean()
{
    // Arrange: FakeSessionStore's returned result is purely test-configured
    // (it does not read Settings itself, unlike SqliteSessionStore) --
    // inject Disabled directly on setMarkerResult to model history_days == 0.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([this]() { return clock_->now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock_->advance(20);
    fakeDb.setMarkerResult = SessionStoreResult::disabled();
    fakeDb.callLog.clear();

    // Act
    const DrainResult result = coordinator.run();

    // Assert
    QVERIFY(result.clean());
}

void ShutdownCoordinatorTest::test_D5_cancelShutdown_while_already_stopped()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);

    // Arrange: shutdown runs on a tracker that was never started.
    coordinator.run();
    QVERIFY(tracker.wasShutDown());

    // Act
    coordinator.cancelShutdown();

    // Assert
    QVERIFY(!tracker.wasShutDown());
    QVERIFY(tracker.isStopped());
}
