#include "test_shutdowncoordinator.h"
#include "fakesessionstore.h"
#include "shutdowncoordinator.h"
#include <QtTest>

using TestCommon::createSettingsFile;

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
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.getOngoingDuration().has_value());

    fakeDb.callLog.clear();

    coordinator.run(ShutdownMode::DrainEvents);

    QVERIFY(!tracker.getOngoingDuration().has_value());
    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    int flushIdx  = fakeDb.callLog.lastIndexOf("flushToDisc");
    int markerIdx = fakeDb.callLog.lastIndexOf("setLastCleanShutdownMarker");
    // stopTimer (inside shutdown) writes the marker; the coordinator writes
    // flushToDisc afterward — so marker must precede flush here.
    QVERIFY2(markerIdx < flushIdx, "marker must precede flush (written by stopTimer)");
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
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    coordinator.run(ShutdownMode::DrainEvents);

    fakeDb.callLog.clear();

    coordinator.run(ShutdownMode::DrainEvents);

    QVERIFY(!fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));
}

// ============================================================================
// Test J — pump drains queued events (regression for midnight wrap)
// ============================================================================

void ShutdownCoordinatorTest::test_J_pump_drains_queued_events()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);

    // Queue a zero-delay slot; it only fires if the event loop is pumped.
    bool slotRan = false;
    QTimer::singleShot(0, [&slotRan]() { slotRan = true; });

    // run(DrainEvents) calls pumpEvents(150) before flushToDisc.
    coordinator.run(ShutdownMode::DrainEvents);

    QVERIFY2(slotRan, "pumpEvents must drain queued events");
}

// ============================================================================
// Test I — force-direct path skips retry loop (flushToDisc still happens)
// ============================================================================

void ShutdownCoordinatorTest::test_I_force_direct_skips_retry_loop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.getOngoingDuration().has_value());

    fakeDb.callLog.clear();

    // The Direct path must not block for 150 ms (no processEvents pump).
    // We measure elapsed time: run(Direct) with FakeDb completes well under 50 ms.
    QElapsedTimer elapsed;
    elapsed.start();
    coordinator.run(ShutdownMode::Direct);
    qint64 elapsedMs = elapsed.elapsed();

    QVERIFY2(elapsedMs < 100, qPrintable(QString("force-direct took %1 ms; expected < 100 ms").arg(elapsedMs)));
    QVERIFY(!tracker.getOngoingDuration().has_value());
    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QVERIFY2(fakeDb.callLog.lastIndexOf("setLastCleanShutdownMarker") < fakeDb.callLog.lastIndexOf("flushToDisc"),
             "marker must precede flush (written by stopTimer)");
}

// ============================================================================
// Test K — reset() re-arms the guard so a second real shutdown flushes again
// (A4: a cancelled OS shutdown must not permanently consume the guard)
// ============================================================================

void ShutdownCoordinatorTest::test_K_reset_allows_second_shutdown()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    coordinator.run(ShutdownMode::DrainEvents);
    QVERIFY(fakeDb.callLog.contains("flushToDisc"));

    // Simulate WM_ENDSESSION(FALSE): the OS shutdown was cancelled.
    coordinator.reset();

    // User keeps working: a fresh session starts.
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    fakeDb.callLog.clear();
    coordinator.run(ShutdownMode::DrainEvents);

    QVERIFY2(fakeDb.callLog.contains("flushToDisc"),
             "reset() must re-enable the full save path for the next shutdown");
}

// ============================================================================
// Test L — a re-entrant run() dispatched inside pumpEvents() flushes only once
// (A14: shutdown_completed_ is set immediately after the guard check)
// ============================================================================

void ShutdownCoordinatorTest::test_L_nested_run_during_pump_flushes_once()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    fakeDb.callLog.clear();

    // Queue a re-entrant run() that fires while the outer run() is pumping.
    QTimer::singleShot(0, [&coordinator]() {
        coordinator.run(ShutdownMode::DrainEvents);
    });

    coordinator.run(ShutdownMode::DrainEvents);

    QCOMPARE(fakeDb.callLog.count("flushToDisc"), 1);
}

// ============================================================================
// Test M — timer already stopped before run(): the coordinator (not stopTimer)
// writes the clean marker, and it must land AFTER the flush.
// ============================================================================

void ShutdownCoordinatorTest::test_M_marker_after_flush_when_already_stopped()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);

    // TimeTracker never started → already in Mode::None. shutdown() does not call
    // stopTimer, so the coordinator's own marker branch fires.
    fakeDb.callLog.clear();

    coordinator.run(ShutdownMode::DrainEvents);

    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QVERIFY2(fakeDb.callLog.lastIndexOf("setLastCleanShutdownMarker") > fakeDb.callLog.lastIndexOf("flushToDisc"),
             "marker must follow flush (written by the coordinator, not stopTimer)");
}

// ============================================================================
// Test N — a START click dispatched inside pumpEvents() restarts the timer;
// the coordinator must re-stop it so no live segment survives shutdown.
// ============================================================================

void ShutdownCoordinatorTest::test_N_restart_during_pump_is_restopped()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    // Queue a START click that fires inside pumpEvents(150), after shutdown().
    QTimer::singleShot(0, [&tracker]() {
        tracker.useTimerViaButton(Button::Start);
    });

    coordinator.run(ShutdownMode::DrainEvents);

    QVERIFY2(tracker.isStopped(),
             "a timer restarted during the shutdown pump must be re-stopped");
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
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    // Act: the store fails, so run()'s own drain is incomplete.
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    const DrainResult first = coordinator.run(ShutdownMode::DrainEvents);
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
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    fakeDb.commitSessionResult = SessionStoreResult::transient("still failing");
    const DrainResult first = coordinator.run(ShutdownMode::DrainEvents);
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
    const DrainResult result = coordinator.run(ShutdownMode::DrainEvents);

    QVERIFY(result.clean());
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));
}

void ShutdownCoordinatorTest::test_A3_direct_and_drainevents_modes_produce_same_result()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    FakeSessionStore fakeDbDrainEvents;
    TimeTracker trackerDrainEvents(settings, fakeDbDrainEvents);
    ShutdownCoordinator coordinatorDrainEvents(trackerDrainEvents, fakeDbDrainEvents);
    trackerDrainEvents.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    fakeDbDrainEvents.commitSessionResult = SessionStoreResult::transient("injected failure");
    const DrainResult drainEventsResult = coordinatorDrainEvents.run(ShutdownMode::DrainEvents);

    FakeSessionStore fakeDbDirect;
    TimeTracker trackerDirect(settings, fakeDbDirect);
    ShutdownCoordinator coordinatorDirect(trackerDirect, fakeDbDirect);
    trackerDirect.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    fakeDbDirect.commitSessionResult = SessionStoreResult::transient("injected failure");
    const DrainResult directResult = coordinatorDirect.run(ShutdownMode::Direct);

    // Assert: identical persistence outcome regardless of pump mode.
    QCOMPARE(directResult.clean(), drainEventsResult.clean());
    QCOMPARE(directResult.category, drainEventsResult.category);
    QVERIFY(trackerDrainEvents.isStopped());
    QVERIFY(trackerDirect.isStopped());

    // Cleanup so destruction does not find an unresolved commit.
    fakeDbDrainEvents.commitSessionResult = SessionStoreResult::success();
    QVERIFY(coordinatorDrainEvents.retryDrain().clean());
    fakeDbDirect.commitSessionResult = SessionStoreResult::success();
    QVERIFY(coordinatorDirect.retryDrain().clean());
}

void ShutdownCoordinatorTest::test_A3_direct_mode_failure_emits_no_warning_and_no_marker()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);
    QSignalSpy warnSpy(&tracker, &TimeTracker::userWarning);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    // Act: OS-forced shutdown; the store fails. Per policy, a forced
    // shutdown attempts once and exits with no warning and no clean marker.
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    const DrainResult result = coordinator.run(ShutdownMode::Direct);

    // Assert
    QVERIFY(!result.clean());
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QCOMPARE(warnSpy.count(), 0);

    // Cleanup so destruction does not find an unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(coordinator.retryDrain().clean());
}
