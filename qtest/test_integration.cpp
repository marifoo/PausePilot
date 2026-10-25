#include "test_integration.h"
#include "fakesessionstore.h"
#include "fakeclock.h"
#include "shutdowncoordinator.h"
#include <QtTest>
#include <QSignalSpy>
#ifdef Q_OS_UNIX
#include <time.h>
#endif

using TestCommon::createSettingsFile;
using TestCommon::fixedNow;
using TestCommon::fixedNowUtc;
using TestCommon::mk;
using TestCommon::sumDurations;

void IntegrationTest::initTestCase()
{
    QVERIFY(data_dir_.isValid());
    TestCommon::setDataDirOverride(data_dir_.path());
    db_path_ = QDir(data_dir_.path()).filePath("uTimer.sqlite");
}

void IntegrationTest::cleanupTestCase()
{
    TestCommon::clearDataDirOverride();
}

void IntegrationTest::test_integration_checkpoint_recovery_on_restart()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    SegmentId orphanSegmentId;
    
    {
        // First session: Start timer and save checkpoint
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);

        // Drive the clock via the seam: no real sleep needed to age the checkpoint
        // past the 1 s minimum-recoverable-orphan threshold.
        // FakeClock declared before TimeTracker: see outlives-tracker note above
        FakeClock clock(fixedNow().addSecs(-10));
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

        tracker.useTimerViaButton(Button::Start);
        clock.advance(1200);

        // Manually save checkpoint
        tracker.saveCheckpoint_locked_dbg(clock.now());
        QVERIFY(tracker.getOngoingDuration().has_value());
        auto orphans = db.loadUnfinalizedCheckpoints_dbg();
        QCOMPARE(orphans.size(), static_cast<size_t>(1));
        QCOMPARE(orphans[0].segment_id, tracker.getOngoingDuration()->segment_id);
        orphanSegmentId = orphans[0].segment_id;

        // Simulate an unclean shutdown: keep checkpoint row, skip graceful
        // stop. D11: no behavioral seam reaches this -- destructing the
        // tracker while mode_ is still Activity/Pause runs the destructor's
        // own last-resort drainForShutdown(), which gracefully finalizes the
        // segment (see ~TimeTracker(), timetracker.cpp:455-482) instead of
        // leaving it as the unfinalized orphan checkpoint this test needs to
        // exercise startup recovery against. simulateCrashWithoutShutdown_dbg()
        // makes the destructor's drain a no-op (mode already None, no pending
        // commit/discard), matching what a real process crash would leave behind.
        tracker.simulateCrashWithoutShutdown_dbg();

        // Simulate crash (tracker destroyed without stopping)
    }
    
    {
        // Second session: startup reconciliation finalizes orphan checkpoint.
        // D15/F1: recoverStartupCheckpoints() policy-drops any orphan whose
        // endTime is strictly older than the History retention cutoff
        // (now.addDays(-history_days_to_keep_), sqlitesessionstore.cpp) --
        // the checkpoint above was written relative to fixedNow(), so "now"
        // here must be pinned the same way rather than left as the real wall
        // clock, or the checkpoint could read as expired depending on the
        // real date.
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        FakeClock clock(fixedNow());
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.initializeFromStore();
        QVERIFY(tracker.getStartupRecoveredMsec() > 0);

        SqliteSessionStore db2(settings);
        auto loaded = db2.loadDurations();
        QCOMPARE(loaded.size(), (size_t)1); // Reconciled orphan recovered
        QCOMPARE(loaded[0].type, DurationType::Activity);

        // Reconciliation must keep row identity by finalizing in place.
        QVERIFY(db.ensureOpen_dbg());
        QSqlQuery query(db.rawDb_dbg());
        query.prepare("SELECT is_finalized FROM durations WHERE segment_id = :segment_id");
        query.bindValue(":segment_id", orphanSegmentId.toString());
        QVERIFY(query.exec());
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        db.closeConnection_dbg();
    }
}

void IntegrationTest::test_integration_orphan_reconciliation_is_idempotent()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        // FakeClock declared before TimeTracker: see outlives-tracker note above
        FakeClock clock(fixedNow().addSecs(-10));
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.useTimerViaButton(Button::Start);
        clock.advance(1200);  // seam-driven aging, no real sleep
        tracker.saveCheckpoint_locked_dbg(clock.now());
        QVERIFY(tracker.getOngoingDuration().has_value());
        {
            auto orphans = db.loadUnfinalizedCheckpoints_dbg();
            QCOMPARE(orphans.size(), static_cast<size_t>(1));
            QCOMPARE(orphans[0].segment_id, tracker.getOngoingDuration()->segment_id);
        }
        // D11: see the crash-simulation note in
        // test_integration_checkpoint_recovery_on_restart() above.
        tracker.simulateCrashWithoutShutdown_dbg();
    }

    {
        // D15/F1: pin "now" to fixedNow() -- see the same comment in
        // test_integration_checkpoint_recovery_on_restart() above.
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        FakeClock clock(fixedNow());
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.initializeFromStore();
        QCOMPARE(tracker.getStartupRecoveredMsec() >= 1, true);
    }

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        FakeClock clock(fixedNow());
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.initializeFromStore();
        QCOMPARE(tracker.getStartupRecoveredMsec(), static_cast<qint64>(0));
        SqliteSessionStore db2(settings);
        auto loaded = db2.loadDurations();
        QCOMPARE(loaded.size(), static_cast<size_t>(1));
    }
}

// C1/C3 step 4: the old kMinRecoverableOrphanDurationMs/kOrphanStaleAgeMs
// filters are gone. A positive orphan is recovered regardless of how short
// or how old it is, as long as its endTime is still inside configured
// History retention (7 days here) relative to recovery's own captured
// "now" -- pinned via fixedNow()/FakeClock rather than the real wall clock,
// so the test does not depend on when it happens to run.
void IntegrationTest::test_integration_orphan_reconciliation_recovers_short_and_stale_but_retained()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    QVERIFY(db.ensureOpen_dbg());

    const QDateTime now = fixedNowUtc();

    // Positive 500 ms orphan, otherwise unremarkable.
    {
        QSqlQuery insert(db.rawDb_dbg());
        const QDateTime start = now.addSecs(-10);
        const QDateTime end = start.addMSecs(500);
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)"
        );
        insert.bindValue(":segment_id", SegmentId::mint().toString());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        insert.bindValue(":start_utc", start.toString(Qt::ISODateWithMs));
        insert.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
        QVERIFY(insert.exec());
    }

    // 2-day-old 30 s orphan -- more than 24h old, but well inside the 7-day
    // retention window.
    {
        QSqlQuery insert(db.rawDb_dbg());
        const QDateTime start = now.addDays(-2);
        const QDateTime end = start.addSecs(30);
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)"
        );
        insert.bindValue(":segment_id", SegmentId::mint().toString());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        insert.bindValue(":start_utc", start.toString(Qt::ISODateWithMs));
        insert.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
        QVERIFY(insert.exec());
    }

    db.closeConnection_dbg();

    SqliteSessionStore db2(settings);
    // FakeClock declared before TimeTracker: see outlives-tracker note above.
    FakeClock clock(now);
    TimeTracker tracker(settings, db2);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.initializeFromStore();
    QCOMPARE(tracker.getStartupRecoveredMsec(), static_cast<qint64>(30500));

    // loadDurations() only ever returns is_finalized = 1 rows, so finding
    // both here already proves both were finalized by recovery.
    auto loaded = db.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(2));
}

void IntegrationTest::test_integration_orphan_reconciliation_marker_present_is_silent()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        // FakeClock declared before TimeTracker: see outlives-tracker note above
        FakeClock clock(fixedNow().addSecs(-10));
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.useTimerViaButton(Button::Start);
        clock.advance(1200);  // seam-driven aging, no real sleep
        tracker.saveCheckpoint_locked_dbg(clock.now());
        QVERIFY(tracker.getOngoingDuration().has_value());
        {
            auto orphans = db.loadUnfinalizedCheckpoints_dbg();
            QCOMPARE(orphans.size(), static_cast<size_t>(1));
            QCOMPARE(orphans[0].segment_id, tracker.getOngoingDuration()->segment_id);
        }
        // D11: kept deliberately -- see the crash-simulation note in
        // test_integration_checkpoint_recovery_on_restart() above.
        tracker.simulateCrashWithoutShutdown_dbg();
    }

    {
        // D15/F1: the marker must be >= the newest finalized orphan's end
        // time (fixedNow()-based, see above) to suppress the recovery
        // notification -- a real "now" here is unrelated to that anchor
        // whenever the wall clock isn't near noon.
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        QVERIFY(db.setLastCleanShutdownMarker_dbg(fixedNow()).ok());
    }

    {
        // D15/F1: pin "now" to fixedNow() -- see the same comment in
        // test_integration_checkpoint_recovery_on_restart() above.
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        FakeClock clock(fixedNow());
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.initializeFromStore();
        QVERIFY(tracker.getStartupRecoveredMsec() > 0);
        QVERIFY(!tracker.shouldShowStartupRecoveryNotification());
    }
}

void IntegrationTest::test_integration_orphan_reconciliation_marker_absent_shows_notification()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        // FakeClock declared before TimeTracker: see outlives-tracker note above
        FakeClock clock(fixedNow().addSecs(-10));
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.useTimerViaButton(Button::Start);
        clock.advance(1200);  // seam-driven aging, no real sleep
        tracker.saveCheckpoint_locked_dbg(clock.now());
        QVERIFY(tracker.getOngoingDuration().has_value());
        {
            auto orphans = db.loadUnfinalizedCheckpoints_dbg();
            QCOMPARE(orphans.size(), static_cast<size_t>(1));
            QCOMPARE(orphans[0].segment_id, tracker.getOngoingDuration()->segment_id);
        }
        // D11: kept deliberately -- see the crash-simulation note in
        // test_integration_checkpoint_recovery_on_restart() above.
        tracker.simulateCrashWithoutShutdown_dbg();
    }

    {
        // D15/F1: pin "now" to fixedNow() -- see the same comment in
        // test_integration_checkpoint_recovery_on_restart() above.
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        FakeClock clock(fixedNow());
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.initializeFromStore();
        QVERIFY(tracker.getStartupRecoveredMsec() > 0);
        QVERIFY(tracker.shouldShowStartupRecoveryNotification());
    }
}

void IntegrationTest::test_integration_memory_db_consistency()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614). Anchored to a
    // fixed local civil time, not real now, so this test cannot go
    // cross-midnight and trip getOngoingDuration()'s nullopt-on-cross-
    // midnight behavior (timetracker.cpp:1272-1273) -- see the house
    // convention at test_timer.cpp:78 and git show 5e63eae.
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Start -> Activity for 50ms
    tracker.useTimerViaButton(Button::Start);
    clock.advance(50);

    // Pause
    tracker.useTimerViaButton(Button::Pause);
    auto memoryDurations = tracker.completedTimeline();
    QVERIFY(memoryDurations.completed().size() >= 1);

    // Resume -> Activity for 50ms
    tracker.useTimerViaButton(Button::Start);
    clock.advance(50);

    // Save checkpoint
    tracker.saveCheckpoint_locked_dbg(clock.now());
    QVERIFY(tracker.getOngoingDuration().has_value());
    {
        auto orphans = db.loadUnfinalizedCheckpoints_dbg();
        QCOMPARE(orphans.size(), static_cast<size_t>(1));
        QCOMPARE(orphans[0].segment_id, tracker.getOngoingDuration()->segment_id);
    }

    // Stop
    tracker.useTimerViaButton(Button::Stop);
    
    // Load from DB
    SqliteSessionStore db2(settings);
    auto dbDurations = db2.loadDurations();
    
    // Total durations should match (memory + checkpoint)
    qint64 memoryTotal = sumDurations(memoryDurations.completed(), DurationType::Activity) +
                        sumDurations(memoryDurations.completed(), DurationType::Pause);
    qint64 dbTotal = sumDurations(dbDurations.durations, DurationType::Activity) +
                    sumDurations(dbDurations.durations, DurationType::Pause);
    
    // Allow small tolerance for timing differences
    QVERIFY(qAbs(memoryTotal - dbTotal) < 200);
}

void IntegrationTest::test_integration_retention_cleanup_preserves_current()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 2)); // Keep 2 days
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNowUtc();

    // Insert data from 5 days ago (should be deleted)
    std::deque<TimeDuration> old;
    old.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addDays(-5), now.addDays(-5).addSecs(60)));
    QVERIFY(manager.commitSession(Timeline(old, std::nullopt)).ok());
    
    // Insert data from today (should be kept)
    std::deque<TimeDuration> current;
    current.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-90)));
    QVERIFY(manager.commitSession(Timeline(current, std::nullopt)).ok());
    
    // Cleanup runs in checkSchemaOnStartup(), not in the constructor.
    SqliteSessionStore manager2(settings);
    QCOMPARE(manager2.checkSchemaOnStartup(), SchemaStatus::Ready);

    auto loaded = manager2.loadDurations();
    QVERIFY(loaded.size() >= 1); // Current day preserved
    
    // Verify old entries are gone
    bool hasOldEntry = false;
    for (const auto& d : loaded) {
        if (d.endTime.date() == now.addDays(-5).date()) {
            hasOldEntry = true;
            break;
        }
    }
    QVERIFY(!hasOldEntry);
}

void IntegrationTest::test_integration_duplicate_prevention()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime start = fixedNow();
    QDateTime end = start.addSecs(10);
    
    // Insert same segment twice via update-by-id; second call updates same row.
    std::deque<TimeDuration> durations;
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));
    
    QVERIFY(manager.commitSession(Timeline(durations, std::nullopt)).ok());
    // D7 (R03-002): duration is now derived from the endpoints, so moving
    // endTime is all that's needed -- no separate repair assignment.
    durations.front().endTime = end.addSecs(10);
    QVERIFY(manager.commitSession(Timeline(durations, std::nullopt)).ok());
    
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
}

void IntegrationTest::test_integration_empty_database_operations()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Operations on empty database should succeed. commitSession() with an
    // empty Timeline early-outs before touching the DB (its result is
    // unconditionally Success, so asserting .ok() on it proves nothing --
    // D13e), so this only checks that a query against a genuinely empty
    // database reports the correct empty state.
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)0);

    QCOMPARE(manager.hasEntriesForDate(QDate::currentDate()), EntriesForDateResult::No);
}

// ============================================================================
// Shutdown sequence: stop → flush → clean-shutdown marker
// ============================================================================

void IntegrationTest::test_F_shutdown_sequence_stop_flush_marker()
{
    // Test F: shutdown sequence end-to-end through ShutdownCoordinator.
    //
    // ShutdownCoordinator::run() owns the sequence:
    //   1. timetracker_.drainForShutdown()  — stops the engine and retries any
    //                                         pending commit/discard intent (A3/A4)
    //   2. db_.markCleanShutdown(), iff the drain was clean (D6) — one store
    //      operation that writes the marker then runs the FULL durability
    //      barrier (setLastCleanShutdownMarker() then flushToDisc(), in that
    //      order, inside FakeSessionStore's/SqliteSessionStore's own composition)
    //
    // This test drives the production owner rather than reproducing the
    // sequence by hand.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    // D15/F1 (part 4, B2 class): the scheduled midnight stop is driven
    // purely from the heartbeat (TimeTracker::onTick), which this test
    // never calls, so it cannot fire during this test's run.
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    // Start the timer so there is something to stop.
    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);
    QVERIFY(tracker.isActive());

    // Clear the call log so the assertion only sees the shutdown sequence.
    fakeDb.callLog.clear();

    // Act — drive the production lifecycle owner.
    coordinator.run();

    // Assert: engine stopped.
    QVERIFY(tracker.isStopped());

    // flushToDisc and setLastCleanShutdownMarker must both appear.
    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    // D6: markCleanShutdown() writes the marker, then only on success runs
    // the FULL barrier -- so marker must always precede flush.
    int flushIdx  = fakeDb.callLog.lastIndexOf("flushToDisc");
    int markerIdx = fakeDb.callLog.lastIndexOf("setLastCleanShutdownMarker");
    QVERIFY2(markerIdx < flushIdx,
             "setLastCleanShutdownMarker must precede flushToDisc (D6: marker then barrier)");
}

// ============================================================================
// DayBoundaryWatcher regression: cross-midnight behavior
// ============================================================================

/**
 * Normal same-day session: discardCrossMidnightOngoingAndStop() does not fire.
 */
void IntegrationTest::test_5_0a_normal_same_day_no_discard()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Start with a same-day segment_start_time (current real time is fine).
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Directly call the internal helper with a same-day "now".
    const QDateTime sameDay = QDateTime::currentDateTime();
    QVERIFY(!tracker.discardCrossMidnightOngoingAndStop_dbg(sameDay));

    // Engine must still be in Activity.
    QVERIFY(tracker.isActive());
}

/**
 * Cross-midnight ongoing segment is discarded; same-day completed segments
 * that accumulated before midnight are preserved and flushed to the DB.
 */
void IntegrationTest::test_5_0a_cross_midnight_ongoing_discarded_completed_preserved()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Arrange: start the timer yesterday (via the clock seam), add a
    // completed yesterday-same-day segment, then advance the clock into
    // today so the ongoing segment (still anchored to yesterday) crosses
    // local midnight -- the real transition path a sleep-through-midnight
    // takes, instead of writing segment_start_time directly.
    const QDateTime yesterdayStart(QDate::currentDate().addDays(-1), QTime(20, 0, 0), Qt::LocalTime);
    // FakeClock declared before TimeTracker so it outlives it: the tracker
    // is left in Activity at scope exit, and ~TimeTracker()'s
    // drainForShutdown() calls back into the now-provider, which captures
    // &clock by reference.
    FakeClock clock(yesterdayStart);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Insert a completed segment via direct addDuration() so we control the
    // timestamps and guarantee a positive duration independent of wall-clock.
    const QDateTime segStart = yesterdayStart.addSecs(60);
    const QDateTime segEnd   = yesterdayStart.addSecs(69);
    tracker.addDuration_dbg(DurationType::Activity, segStart, segEnd);
    QVERIFY(!tracker.completedTimeline().completed().empty());
    const size_t completedCount = tracker.completedTimeline().completed().size();

    // Simulate sleep through midnight: advance the clock into today.
    clock.setNow(QDateTime(QDate::currentDate(), QTime(9, 0, 0), Qt::LocalTime));

    // Act: call the internal helper with "now" (today).
    const QDateTime today = clock.now();
    fakeDb.callLog.clear();
    const bool fired = tracker.discardCrossMidnightOngoingAndStop_dbg(today);

    // Assert: helper fired, engine stopped, completed segments were committed.
    QVERIFY(fired);
    QVERIFY(tracker.isStopped());
    QVERIFY(fakeDb.callLog.contains("commitSession"));
    // The completed segments must NOT be in the completed timeline any more
    // (they were flushed and resetForNewSession was called).
    QCOMPARE(tracker.completedTimeline().completed().size(), static_cast<size_t>(0));
    (void)completedCount; // used to verify state before discard
}

/**
 * Repeated calls to discardCrossMidnightOngoingAndStop() are idempotent:
 * the second call returns false and makes no DB writes.
 */
void IntegrationTest::test_5_0a_discard_is_idempotent()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime yesterdayStart(QDate::currentDate().addDays(-1), QTime(20, 0, 0), Qt::LocalTime);
    // FakeClock declared before TimeTracker: see outlives-tracker note above
    // (test_5_0a_cross_midnight_ongoing_discarded_completed_preserved, ~line 558).
    FakeClock clock(yesterdayStart);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Advance the clock into today: the segment started yesterday.
    clock.setNow(QDateTime(QDate::currentDate(), QTime(9, 0, 0), Qt::LocalTime));

    const QDateTime today = clock.now();
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop_dbg(today));
    QVERIFY(tracker.isStopped());

    // Second call must return false (engine already stopped).
    fakeDb.callLog.clear();
    QVERIFY(!tracker.discardCrossMidnightOngoingAndStop_dbg(today));
    QVERIFY(!fakeDb.callLog.contains("commitSession"));
}

void IntegrationTest::test_integration_backpause_db_update()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614). Anchored to a
    // fixed local civil time, not real now, so this test cannot go
    // cross-midnight and trip discardCrossMidnightOngoingAndStop() inside
    // saveCheckpoint_locked_dbg() below -- see the house convention at
    // test_timer.cpp:78 and git show 5e63eae.
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Start activity
    tracker.useTimerViaButton(Button::Start);
    clock.advance(100);

    // Save checkpoint
    tracker.saveCheckpoint_locked_dbg(clock.now());
    QVERIFY(tracker.getOngoingDuration().has_value());
    auto checkpointOrphans = db.loadUnfinalizedCheckpoints_dbg();
    SegmentId checkpointSegmentId = checkpointOrphans.empty() ? SegmentId{} : checkpointOrphans[0].segment_id;
    QVERIFY(!checkpointSegmentId.isEmpty());

    // Simulate lock
    tracker.useTimerViaLockEvent(LockEvent::Lock);

    // Checkpoint row should still be present and unchanged in the store
    // (lock doesn't reset its identity).
    QVERIFY(tracker.getOngoingDuration().has_value());
    auto lockOrphans = db.loadUnfinalizedCheckpoints_dbg();
    QCOMPARE(lockOrphans.size(), static_cast<size_t>(1));
    QVERIFY(!lockOrphans[0].segment_id.isEmpty());
    QCOMPARE(lockOrphans[0].segment_id.toString(), checkpointSegmentId.toString());

    // Simulate long ongoing lock (triggers backpause). C12b: LongOngoingLock
    // requires a captured lock instant.
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, clock.now());

    // Backpause transitions to Pause and starts a fresh segment identity.
    // The Pause segment isn't checkpointed (saveCheckpoint_locked only
    // writes while mode_ == Activity), so the DB can't observe this.
    auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QVERIFY(ongoing->segment_id.toString() != checkpointSegmentId.toString());
    
    // Verify durations were updated in DB
    SqliteSessionStore db2(settings);
    auto loaded = db2.loadDurations();
    QVERIFY(loaded.size() >= 1);
}

// C12b: production wiring end-to-end -- LockStateWatcher's real
// desktopLockEvent signal (which always carries a captured wall-clock lock
// instant; see test_lockstatewatcher.cpp's coverage of that timestamp)
// connected into TimeTracker::useTimerViaLockEvent. The Activity segment
// must end exactly at the watcher's captured instant, not at some
// now-relative fallback.
void IntegrationTest::test_C12b_watcher_propagates_lockTime_to_backpause_anchor()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 1); // 60s
    writer.sync();
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    LockStateWatcher watcher(settings);
    watcher.setNowProvider_dbg([&clock]() { return clock.now(); });
    QObject::connect(&watcher, &LockStateWatcher::desktopLockEvent,
                      &tracker, &TimeTracker::useTimerViaLockEvent);

    std::deque<LockQueryResult> script = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked };
    watcher.setQueryProvider_dbg([&script](const QDateTime &) {
        LockQueryResult r = script.front();
        script.pop_front();
        return r;
    });

    tracker.useTimerViaButton(Button::Start);

    // Debounce Lock (3rd Locked sample), then mature LongOngoingLock via a
    // suspend-sized jump, exactly as test_lockstatewatcher.cpp's own H3/A2
    // coverage does.
    watcher.update();
    clock.advance(100);
    watcher.update();
    clock.advance(100);
    watcher.update(); // Lock, stamped at the first Locked sample: t0+200

    const QDateTime expectedLockTime = t0.addMSecs(200);

    clock.advance(settings.getBackpauseMsec() + 60000);
    watcher.update();
    clock.advance(100);
    watcher.update(); // LongOngoingLock, stamped at the same first Locked sample

    // Assert: the Activity segment ends exactly at the watcher's captured
    // lock instant.
    QVERIFY(tracker.isPaused());
    const Timeline timeline = tracker.completedTimeline();
    bool foundActivityEndingAtLockTime = false;
    for (const auto& d : timeline.completed()) {
        if (d.type == DurationType::Activity && d.endTime == expectedLockTime) {
            foundActivityEndingAtLockTime = true;
        }
    }
    QVERIFY(foundActivityEndingAtLockTime);
}

// ============================================================================
// DayBoundaryWatcher: scheduled vs watchdog midnight-stop signals
// ============================================================================

/**
 * Test Y — Engine-driven scheduled stop.
 *
 * Advance FakeClock to the intended 23:59:59.500 boundary, arm at that same
 * instant, and drive the timer-fired callback directly (no real wait).
 * Assert:
 *   - stopped(MidnightScheduled) is emitted exactly once.
 *   - Engine reaches Mode::None.
 *   - Same-day completed segments were committed to the fake DB.
 */
void IntegrationTest::test_Y_engine_scheduled_stop_emits_MidnightScheduled()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Pin the engine clock to mid-day so the scheduled stop (purely
    // heartbeat-driven now) does not interfere before we tick below.
    // FakeClock declared before TimeTracker: see outlives-tracker note above
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Start the engine (Activity mode).
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Add a completed same-day segment so we can verify it is preserved.
    const QDateTime segStart = clock.now().addSecs(-30);
    const QDateTime segEnd   = clock.now().addSecs(-5);
    tracker.addDuration_dbg(DurationType::Activity, segStart, segEnd);
    QVERIFY(!tracker.completedTimeline().completed().empty());
    fakeDb.callLog.clear();

    // Spy on the stopped() signal.
    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Advance the fake clock to the boundary and drive the real heartbeat
    // tick directly (C5): no real wait.
    const QDateTime boundary(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(boundary);
    tracker.onTick(boundary);

    // Assert: exactly one stopped signal, with MidnightScheduled reason.
    QCOMPARE(stoppedSpy.count(), 1);
    const auto reason = stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>();
    QCOMPARE(reason, TimeTracker::StopReason::MidnightScheduled);

    // Engine must be stopped.
    QVERIFY(tracker.isStopped());

    // Completed segments must have been committed to the DB.
    QVERIFY(fakeDb.callLog.contains("commitSession"));
}

/**
 * Test Z — Engine-driven watchdog.
 *
 * With FakeClock jumped past midnight (no scheduled stop fired), the next
 * onTick() call emits stopped(MidnightWatchdog).
 */
void IntegrationTest::test_Z_engine_watchdog_emits_MidnightWatchdog()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Simulate sleep through midnight: start yesterday (via the clock seam),
    // then advance the clock into today before ticking.
    const QDateTime yesterdayStart(QDate::currentDate().addDays(-1), QTime(20, 0, 0), Qt::LocalTime);
    // FakeClock declared before TimeTracker: see outlives-tracker note above
    // (test_5_0a_cross_midnight_ongoing_discarded_completed_preserved, ~line 558).
    FakeClock clock(yesterdayStart);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    clock.setNow(QDateTime(QDate::currentDate(), QTime(9, 0, 0), Qt::LocalTime));

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Simulate the 100 ms heartbeat with today's "now".
    tracker.onTick(clock.now());

    // Engine must have stopped and emitted MidnightWatchdog.
    QCOMPARE(stoppedSpy.count(), 1);
    const auto reason = stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>();
    QCOMPARE(reason, TimeTracker::StopReason::MidnightWatchdog);
    QVERIFY(tracker.isStopped());
}

/**
 * Test AA — No duplicate stop.
 *
 * With the scheduled stop and the watchdog both driven purely from the same
 * heartbeat (C5), repeated onTick() calls at the cutoff, later same-day
 * instants, and into the next date must still yield exactly one stopped()
 * signal.
 */
void IntegrationTest::test_AA_no_duplicate_stop_signal()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Pin the engine clock to mid-day.
    // FakeClock declared before TimeTracker: see outlives-tracker note above
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Act: a tick at the cutoff stops the engine.
    const QDateTime cutoff(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(cutoff);
    tracker.onTick(clock.now());
    QCOMPARE(stoppedSpy.count(), 1);
    QVERIFY(tracker.isStopped());

    // Act: further ticks -- a later same-day instant, and into the next
    // date -- must not produce a second stop.
    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(23, 59, 59, 900), Qt::LocalTime));
    tracker.onTick(clock.now());
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime));
    tracker.onTick(clock.now());

    // Assert: still exactly one emission.
    QCOMPARE(stoppedSpy.count(), 1);
    QVERIFY(tracker.isStopped());
}

/**
 * Test AB — scheduleMidnightStop is gone.
 *
 * Verify that the symbol scheduleMidnightStop no longer appears in mainwin.h.
 * Resolve the source independently of the test binary's build directory.
 */
void IntegrationTest::test_AB_scheduleMidnightStop_is_gone()
{
    const QString mainwinHPath =
        QDir(QStringLiteral(PAUSEPILOT_SOURCE_DIR)).filePath("mainwin.h");
    QFile mainwinH(mainwinHPath);
    QVERIFY2(mainwinH.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable("Cannot open mainwin.h: " + mainwinHPath));
    const QString content = QString::fromUtf8(mainwinH.readAll());
    mainwinH.close();
    QVERIFY2(!content.contains("scheduleMidnightStop"),
             "scheduleMidnightStop should not appear in mainwin.h");
}

/**
 * recoverStartupCheckpoints overlap policy.
 *
 * Seeds two orphan rows against a real SqliteSessionStore: one that does not
 * overlap any finalised row (should be recovered) and one that does overlap
 * (should be dropped with its seconds not counted).
 */
void IntegrationTest::test_recoverStartupCheckpoints_overlap_rejected_not_counted()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);

    QVERIFY(db.ensureOpen_dbg());

    // Seed a finalised row 10:00 - 11:00 UTC.
    const QDateTime existingStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime existingEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 0, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 1)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", existingStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", existingEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }

    // Orphan A: 12:00 - 12:30 UTC (1800 s), no overlap → should be finalized.
    const QDateTime aStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime aEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 0)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", aStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", aEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }

    // Orphan B: 10:30 - 11:30 UTC, overlaps the finalised row → should be dropped.
    const QDateTime bStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 30, 0), Qt::UTC);
    const QDateTime bEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 30, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 0)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", bStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", bEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }
    db.closeConnection_dbg();

    // Use a 'now' close enough that neither orphan is outside History retention.
    const QDateTime now = QDateTime(QDate(2025, 1, 1), QTime(13, 0, 0), Qt::UTC).toLocalTime();
    StartupRecoveryResult result = db.recoverStartupCheckpoints(now);

    QVERIFY(result.ok());
    QCOMPARE(result.finalized_count, 1);    // A finalized
    QCOMPARE(result.recovered_msec, (qint64)1800000); // 30 min = 1,800,000 ms
    // No clean-shutdown marker → notify_user = true (unclean exit).
    QVERIFY(result.notify_user);
    // B's 3,600,000 ms must NOT appear in recovered_msec (overlap-rejected).
    QVERIFY(result.recovered_msec < 3600000);

    // B2 (R08-002): an overlap-rejected orphan is now physically deleted
    // (not left is_finalized=0 to recur every future launch).
    QVERIFY(db.ensureOpen_dbg());
    QSqlQuery checkB(db.rawDb_dbg());
    checkB.prepare("SELECT COUNT(*) FROM durations WHERE start_utc = :s AND is_finalized = 0");
    checkB.bindValue(":s", bStartUtc.toString(Qt::ISODateWithMs));
    QVERIFY(checkB.exec());
    QVERIFY(checkB.next());
    QCOMPARE(checkB.value(0).toInt(), 0);
}

/**
 * recoverStartupCheckpoints reports finalized and dropped counts.
 *
 * Verifies that finalized_count and dropped_count in StartupRecoveryResult
 * accurately reflect the number of rows finalized vs. orphaned/dropped.
 */
void IntegrationTest::test_recoverStartupCheckpoints_reports_finalized_and_dropped_counts()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);

    QVERIFY(db.ensureOpen_dbg());

    // Finalised blocker row 10:00 - 11:00 UTC (forces B to be overlap-dropped).
    const QDateTime blockerStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime blockerEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 0, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 1)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", blockerStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", blockerEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }

    // Orphan A: 12:00 - 12:30 UTC — no overlap, long enough, recent → finalized.
    const QDateTime aStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime aEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 0)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", aStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", aEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }

    // Orphan B: 10:30 - 11:30 UTC — overlaps blocker → overlap-dropped.
    const QDateTime bStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 30, 0), Qt::UTC);
    const QDateTime bEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 30, 0), Qt::UTC);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 0)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", bStartUtc.toString(Qt::ISODateWithMs));
        q.bindValue(":e", bEndUtc.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }
    db.closeConnection_dbg();

    const QDateTime now = QDateTime(QDate(2025, 1, 1), QTime(13, 0, 0), Qt::UTC).toLocalTime();
    StartupRecoveryResult result = db.recoverStartupCheckpoints(now);

    QVERIFY(result.ok());
    QCOMPARE(result.finalized_count, 1);         // A finalized
    QCOMPARE(result.policy_dropped_count, 1);    // B overlap-dropped (deleted, counted)
    QCOMPARE(result.recovered_msec, (qint64)1800000); // only A's 30 min
}

/**
 * TimeTracker correctly uses recovered_msec from recoverStartupCheckpoints.
 *
 * Verifies the TimeTracker → store boundary: TimeTracker passes the exact
 * recovered_msec from the store result directly to getStartupRecoveredMsec()
 * (B2: no seconds conversion or rounding happens at this boundary).
 */
void IntegrationTest::test_timer_startup_recovery_uses_store_recovered_msec()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Configure the store to report 60,900 ms recovered (e.g. A finalized, B
    // overlap-dropped) -- a non-round-second value to prove exact ms passthrough.
    fakeDb.startupRecoveryResult.status = SessionStoreResult::Success;
    fakeDb.startupRecoveryResult.recovered_msec = 60900;
    fakeDb.startupRecoveryResult.notify_user = true;

    TimeTracker tracker(settings, fakeDb);
    tracker.initializeFromStore();

    // TimeTracker should report exactly what the store returned.
    QCOMPARE(tracker.getStartupRecoveredMsec(), (qint64)60900);
    QVERIFY(tracker.shouldShowStartupRecoveryNotification());
}

// ============================================================================
// Step 19 (C9 timezone): parameterized timezone tests
// ============================================================================

namespace {
// RAII helper that saves the current TZ env-var and restores it on destruction.
struct TzGuard {
    std::optional<QByteArray> saved;

    TzGuard() {
        const char* tz = getenv("TZ");
        saved = tz ? std::make_optional(QByteArray(tz)) : std::nullopt;
    }

    void set(const QByteArray& tz) {
        qputenv("TZ", tz);
#ifdef Q_OS_UNIX
        tzset();
#endif
    }

    ~TzGuard() {
        if (saved)
            qputenv("TZ", *saved);
        else
            qunsetenv("TZ");
#ifdef Q_OS_UNIX
        tzset();
#endif
    }
};
} // anonymous namespace

/**
 * Data provider: three timezones exercised in hasEntriesForDate tests.
 */
void IntegrationTest::test_hasEntriesForDate_timezone_data()
{
    QTest::addColumn<QString>("timezone");
    QTest::newRow("UTC")              << "Etc/UTC";
    QTest::newRow("Europe/Berlin")    << "Europe/Berlin";
    QTest::newRow("America/Los_Angeles") << "America/Los_Angeles";
}

/**
 * For each timezone: insert a finalized row that starts exactly at noon on
 * 2025-06-15 local time, then ask hasEntriesForDate for that date.
 * Must return Yes regardless of timezone offset.
 */
void IntegrationTest::test_hasEntriesForDate_timezone()
{
    QFETCH(QString, timezone);

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));

    TzGuard guard;
    guard.set(timezone.toLatin1());

    // Construct noon on 2025-06-15 in the current (faked) local timezone.
    const QDate testDate(2025, 6, 15);
    const QDateTime localNoon(testDate, QTime(12, 0, 0), Qt::LocalTime);
    const QDateTime localNoonEnd = localNoon.addSecs(3600); // 1 hour session

    SqliteSessionStore db(settings);
    std::deque<TimeDuration> durations;
    durations.emplace_back(TimeDuration::fromPersistedRow(DurationType::Activity, localNoon, localNoonEnd));
    QVERIFY(db.commitSession(Timeline(durations, std::nullopt)).ok());

    // hasEntriesForDate must find this entry.
    QCOMPARE(db.hasEntriesForDate(testDate), EntriesForDateResult::Yes);

    // An adjacent date must not find it.
    QCOMPARE(db.hasEntriesForDate(testDate.addDays(1)), EntriesForDateResult::No);
    QCOMPARE(db.hasEntriesForDate(testDate.addDays(-1)), EntriesForDateResult::No);
}

/**
 * D1 (R16-002/R08-004): end-to-end proof that TimeTracker::start()'s
 * boot-credit gate benefits from hasEntriesForDate()'s overlap semantics, on
 * a Europe/Berlin DST-transition day (2025-03-30, local day is 23h). A
 * legacy row spanning the *previous* local midnight (out-of-contract, only
 * reachable via manual DB modification -- no PausePilot write path produces it)
 * already covers the first half hour of "today"; boot credit must not add a
 * second helping of that time.
 */
void IntegrationTest::test_bootCredit_suppressed_by_legacy_cross_midnight_row_on_dst_day()
{
    TzGuard guard;
    guard.set("Europe/Berlin");

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 30);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();
    Settings settings(settingsPath);

    // Legacy row: 2025-03-29 23:30 local -> 2025-03-30 00:30 local, spanning
    // the local midnight into the DST-transition day.
    const QDateTime legacyStart(QDate(2025, 3, 29), QTime(23, 30, 0), Qt::LocalTime);
    const QDateTime legacyEnd(QDate(2025, 3, 30), QTime(0, 30, 0), Qt::LocalTime);
    {
        SqliteSessionStore db(settings);
        // D11: this row deliberately crosses local midnight (the legacy,
        // out-of-contract shape under test), so it would be rejected by
        // commitSession()'s day-validity guard. Seed it via
        // scoped raw SQL (the established fixture pattern for legacy
        // cross-midnight rows) instead of a dbg wrapper.
        QVERIFY(db.ensureOpen_dbg());
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 1)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", legacyStart.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":e", legacyEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
        db.closeConnection_dbg();
    }

    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([]() {
        return QDateTime(QDate(2025, 3, 30), QTime(10, 0, 0), Qt::LocalTime);
    });

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    qint64 totalActivity = 0;
    const Timeline completed = tracker.completedTimeline();
    for (const auto& d : completed.completed())
        if (d.type == DurationType::Activity && d.duration() >= 25000)
            totalActivity += d.duration();
    QVERIFY2(totalActivity < 29000,
             "Boot time must not be added when a legacy cross-midnight row already "
             "overlaps today, even though it did not start today");
}

/**
 * Data provider: timezones for the midnight boundary reconciliation test.
 */
void IntegrationTest::test_midnight_boundary_timezone_data()
{
    QTest::addColumn<QString>("timezone");
    QTest::newRow("UTC")              << "Etc/UTC";
    QTest::newRow("Europe/Berlin")    << "Europe/Berlin";
    QTest::newRow("America/Los_Angeles") << "America/Los_Angeles";
}

/**
 * Midnight boundary: a session that ends at 23:59:59 local on 2025-06-14
 * must appear under 2025-06-14, not 2025-06-15 — even across DST offsets.
 * A session starting at 00:00:01 on 2025-06-15 belongs to 2025-06-15.
 */
void IntegrationTest::test_midnight_boundary_timezone()
{
    QFETCH(QString, timezone);

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));

    TzGuard guard;
    guard.set(timezone.toLatin1());

    // Entry A: ends one second before local midnight on 2025-06-14.
    const QDate day14(2025, 6, 14);
    const QDateTime endOfDay14(day14, QTime(23, 59, 59), Qt::LocalTime);
    const QDateTime startOfDay14 = endOfDay14.addSecs(-60);

    // Entry B: starts one second after local midnight → 2025-06-15.
    const QDate day15(2025, 6, 15);
    const QDateTime startOfDay15(day15, QTime(0, 0, 1), Qt::LocalTime);
    const QDateTime endOfDay15 = startOfDay15.addSecs(60);

    SqliteSessionStore db(settings);
    std::deque<TimeDuration> durations;
    durations.emplace_back(TimeDuration::fromPersistedRow(DurationType::Activity, startOfDay14, endOfDay14));
    durations.emplace_back(TimeDuration::fromPersistedRow(DurationType::Activity, startOfDay15, endOfDay15));
    QVERIFY(db.commitSession(Timeline(durations, std::nullopt)).ok());

    // Boundary: 2025-06-14 must see entry A; entry B must not bleed into it.
    QCOMPARE(db.hasEntriesForDate(day14), EntriesForDateResult::Yes);
    QCOMPARE(db.hasEntriesForDate(day15), EntriesForDateResult::Yes);

    // A date with no entries must return No.
    QCOMPARE(db.hasEntriesForDate(day14.addDays(-1)), EntriesForDateResult::No);
    QCOMPARE(db.hasEntriesForDate(day15.addDays(1)),  EntriesForDateResult::No);
}

// ============================================================================
// A3 (R11-001): Exit Anyway leaves a durable checkpoint untouched; a real
// SQLite reopen proves recovery finalizes only that durable portion.
// ============================================================================

void IntegrationTest::test_A3_exit_anyway_leaves_checkpoint_untouched_for_recovery()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    SegmentId checkpointSegmentId;

    {
        // Session 1: Start, durably checkpoint 20 s of Activity, then accrue
        // 5 more seconds that are never checkpointed. The Stop-time commit
        // (and the coordinator's own retry) fail -- simulating the user
        // choosing Exit Anyway: one drain attempt fails, no further retry,
        // process terminates.
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        // FakeClock declared before TimeTracker: see outlives-tracker note above
        FakeClock clock(fixedNow().addSecs(-30));
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

        tracker.useTimerViaButton(Button::Start);
        clock.advance(20000);
        tracker.saveCheckpoint_locked_dbg(clock.now());
        QVERIFY(tracker.getOngoingDuration().has_value());
        {
            auto orphans = db.loadUnfinalizedCheckpoints_dbg();
            QCOMPARE(orphans.size(), static_cast<size_t>(1));
            QCOMPARE(orphans[0].segment_id, tracker.getOngoingDuration()->segment_id);
            checkpointSegmentId = orphans[0].segment_id;
        }
        clock.advance(5000);

        // Force the drain's commit to fail: close the connection before
        // making the file read-only so ensureOpen() re-tries and fails on
        // the permission.
        db.closeConnection_dbg();
        QFile dbFile(db_path_);
        QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));

        ShutdownCoordinator coordinator(tracker, db);
        const DrainResult result = coordinator.run();

        QVERIFY(!result.clean());
        QVERIFY(tracker.hasPendingCommit_dbg());
        // Exit Anyway: no further retry. tracker/db/coordinator destruct
        // here with the store still unwritable, so the destructor's own
        // last-resort drain also fails silently (shutdown_called_ is
        // already true from run(), so no orchestration-bug assert fires).
        // The in-memory 5 s tail is genuinely lost.
    }

    // Restore permissions for the next process to open the DB normally.
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));

    // Session 2: real reopen. Startup recovery must finalize exactly the
    // durable 20 s checkpoint -- no more, no less -- and must not duplicate
    // it, proving Exit Anyway left the durable checkpoint untouched.
    // D15/F1: pin "now" to fixedNow() -- see the same comment in
    // test_integration_checkpoint_recovery_on_restart() above.
    Settings settings(settingsPath);
    SqliteSessionStore db2(settings);
    FakeClock reopenedClock(fixedNow());
    TimeTracker reopened(settings, db2);
    reopened.setNowProvider_dbg([&reopenedClock]() { return reopenedClock.now(); });
    reopened.initializeFromStore();

    QCOMPARE(reopened.getStartupRecoveredMsec(), static_cast<qint64>(20000));

    SqliteSessionStore db3(settings);
    auto loaded = db3.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded[0].segment_id.toString(), checkpointSegmentId.toString());
    QCOMPARE(loaded[0].duration(), static_cast<qint64>(20000));
}

// A4 (R05-001): end-to-end proof that a checkpoint-discard failure is
// durable across a real close/reopen -- the rejected segment can never be
// resurrected as credited history, even though the in-process discard call
// itself never succeeded before the process ended (Exit Anyway).
void IntegrationTest::test_A4_watchdog_discard_failure_survives_reopen_never_credited()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    {
        // Session 1: Start Activity before midnight, then the machine
        // "sleeps" past midnight. C3: the checkpoint row is seeded directly
        // via raw SQL (not through the engine's own saveCheckpoint call), so
        // the engine has no confirmed prefix and the cross-midnight close
        // still takes the whole-segment discard path -- this models a
        // "possibly persisted" write (e.g. an earlier ambiguous I/O) rather
        // than an engine-confirmed one. Shutdown finds the live segment
        // crossed midnight and deliberately rejects it, but the store's
        // row-delete step fails -- simulating a transient failure right at
        // the discard call. Exit Anyway: one drain attempt, no further
        // retry, process terminates.
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        const QDateTime preMidnight(QDate::currentDate(), QTime(23, 50, 0), Qt::LocalTime);
        // FakeClock declared before TimeTracker: see outlives-tracker note above
        FakeClock clock(preMidnight);
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

        tracker.useTimerViaButton(Button::Start);
        clock.advance(60000);
        QVERIFY(tracker.getOngoingDuration().has_value());
        const SegmentId checkpointSegmentId = tracker.getOngoingDuration()->segment_id;
        QVERIFY(db.ensureOpen_dbg());
        {
            QSqlQuery q(db.rawDb_dbg());
            q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                      "VALUES (:seg, 0, :s, :e, 0)");
            q.bindValue(":seg", checkpointSegmentId.toString());
            q.bindValue(":s", preMidnight.toUTC().toString(Qt::ISODateWithMs));
            q.bindValue(":e", clock.now().toUTC().toString(Qt::ISODateWithMs));
            QVERIFY(q.exec());
        }
        {
            auto orphans = db.loadUnfinalizedCheckpoints_dbg();
            QCOMPARE(orphans.size(), static_cast<size_t>(1));
            QCOMPARE(orphans[0].segment_id, checkpointSegmentId);
        }

        clock.setNow(QDateTime(preMidnight.date().addDays(1), QTime(0, 5, 0), Qt::LocalTime));
        db.setForceDiscardRowDeleteFailure_dbg(true);

        ShutdownCoordinator coordinator(tracker, db);
        const DrainResult result = coordinator.run();

        QVERIFY(!result.clean());
        QVERIFY(tracker.hasPendingDiscards_dbg());
        // tracker/db destruct here with the row-delete still failing -- the
        // destructor's own last-resort drain re-attempts and also fails
        // (shutdown_called_ is already true from run(), so no
        // orchestration-bug assert fires). The rejected segment is genuinely
        // NOT deleted yet in-process -- only its tombstone is durable.
    }

    // Session 2: real reopen. Startup recovery must delete the rejected row
    // -- never finalize/credit it -- and resolve the tombstone.
    Settings settings(settingsPath);
    SqliteSessionStore db2(settings);
    TimeTracker reopened(settings, db2);
    reopened.initializeFromStore();

    QCOMPARE(reopened.getStartupRecoveredMsec(), static_cast<qint64>(0));
    QVERIFY(!reopened.shouldShowStartupRecoveryNotification());
    QCOMPARE(reopened.getStartupPolicyDroppedCount(), 0);  // M4: an explicit discard must stay silent

    SqliteSessionStore db3(settings);
    auto loaded = db3.loadDurations();
    QVERIFY(loaded.empty());
}

// WP-C 1 (C3 compound-failure decision): a confirmed same-day prefix whose
// final same-ID close write fails must not be lost. A persistent SQLite
// trigger (unlike the one-shot commit-failure seam) blocks the finalizing
// UPDATE, so the watchdog tick leaves the prefix as a retained pending
// commit. Exit Anyway ends the process with the fault still in place; a
// real reopen with the trigger dropped must let ordinary startup recovery
// finalize exactly that persisted prefix -- no more, no less, and no
// cross-day row.
void IntegrationTest::test_C3_failed_prefix_finalization_then_crash_recovers_prefix()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    SegmentId segId;
    const QDateTime start(QDate(2025, 6, 1), QTime(23, 40, 0), Qt::LocalTime);
    const QDateTime checkpointEnd(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        // FakeClock declared before TimeTracker: see outlives-tracker note above
        FakeClock clock(start);
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

        tracker.useTimerViaButton(Button::Start);
        clock.setNow(checkpointEnd);
        tracker.triggerCheckpoint_dbg();
        QVERIFY(tracker.getOngoingDuration().has_value());
        segId = tracker.getOngoingDuration()->segment_id;
        {
            auto orphans = db.loadUnfinalizedCheckpoints_dbg();
            QCOMPARE(orphans.size(), static_cast<size_t>(1));
            QCOMPARE(orphans[0].segment_id, segId);
        }

        // Block the finalizing UPDATE with a persistent trigger.
        QVERIFY(db.ensureOpen_dbg());
        QSqlQuery trig(db.rawDb_dbg());
        QVERIFY(trig.exec(
            "CREATE TRIGGER block_finalize_c3_test BEFORE UPDATE ON durations "
            "BEGIN SELECT RAISE(ABORT, 'c3 test: blocked finalize'); END"
        ));

        const QDateTime nextTick(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime);
        clock.setNow(nextTick);
        tracker.onTick(nextTick);

        QVERIFY(tracker.isStopped());
        QVERIFY(tracker.hasPendingCommit_dbg());

        ShutdownCoordinator coordinator(tracker, db);
        const DrainResult result = coordinator.run();
        QVERIFY(!result.clean());
        // tracker/db destruct here with the finalizing UPDATE still blocked
        // -- the destructor's own last-resort drain re-attempts and also
        // fails (shutdown_called_ is already true from run(), so no
        // orchestration-bug assert fires). The confirmed prefix is
        // genuinely not yet finalized in-process, only durably checkpointed.
    }

    {
        // Drop the trigger via a fresh connection before the real reopen.
        Settings settings(settingsPath);
        SqliteSessionStore dbFix(settings);
        QVERIFY(dbFix.ensureOpen_dbg());
        QSqlQuery drop(dbFix.rawDb_dbg());
        QVERIFY(drop.exec("DROP TRIGGER block_finalize_c3_test"));
        dbFix.closeConnection_dbg();
    }

    {
        // Session 2: real reopen. Startup recovery must finalize exactly the
        // persisted 10-minute prefix -- nothing from the unverified tail,
        // and no cross-day row.
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        const QDateTime reopenNow(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime);
        FakeClock clock(reopenNow);
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.initializeFromStore();

        QCOMPARE(tracker.getStartupRecoveredMsec(), static_cast<qint64>(10 * 60 * 1000));
        QVERIFY(tracker.shouldShowStartupRecoveryNotification());

        SqliteSessionStore db2(settings);
        auto loaded = db2.loadDurations();
        QCOMPARE(loaded.size(), static_cast<size_t>(1));
        QCOMPARE(loaded[0].segment_id.toString(), segId.toString());
        QCOMPARE(loaded[0].startTime, start);
        QCOMPARE(loaded[0].endTime, checkpointEnd);
    }
}

// WP-C 2 (C3 compound-failure decision): with no confirmed prefix, the
// engine takes the discard path. A persistent trigger blocks the
// discard-intent INSERT itself, so neither this call nor the destructor's
// last-resort retry ever commits the tombstone before the process ends
// (Exit Anyway). A real reopen with the trigger dropped finds only a
// possibly-persisted, unfinalized, positive, same-day, non-overlapping row
// with no discard intent -- ordinary startup recovery cannot distinguish it
// from a genuine crash orphan, so it finalizes it. This is the accepted,
// rare overcount from the C3 compound-failure decision: no discard-safety
// guarantee is claimed before the intent commits, and startup gives only
// its ordinary recovered-work notice, not a special warning.
void IntegrationTest::test_C3_intent_insert_failure_then_crash_recovers_row_overcount_accepted()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    SegmentId segId;
    const QDateTime start(QDate(2025, 6, 1), QTime(23, 40, 0), Qt::LocalTime);
    const QDateTime orphanEnd(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        // FakeClock declared before TimeTracker: see outlives-tracker note above
        FakeClock clock(start);
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

        tracker.useTimerViaButton(Button::Start);
        clock.setNow(orphanEnd);
        QVERIFY(tracker.getOngoingDuration().has_value());
        segId = tracker.getOngoingDuration()->segment_id;

        // C3: no successful checkpoint -- raw-seed a same-ID unfinalized
        // orphan row directly, modeling a "possibly persisted" write (e.g.
        // an earlier ambiguous I/O) rather than an engine-confirmed one.
        QVERIFY(db.ensureOpen_dbg());
        {
            QSqlQuery q(db.rawDb_dbg());
            q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                      "VALUES (:seg, 0, :s, :e, 0)");
            q.bindValue(":seg", segId.toString());
            q.bindValue(":s", start.toUTC().toString(Qt::ISODateWithMs));
            q.bindValue(":e", orphanEnd.toUTC().toString(Qt::ISODateWithMs));
            QVERIFY(q.exec());
        }

        // Block the discard-intent INSERT itself with a persistent trigger.
        QSqlQuery trig(db.rawDb_dbg());
        QVERIFY(trig.exec(
            "CREATE TRIGGER block_intent_insert_c3_test BEFORE INSERT ON discard_intents "
            "BEGIN SELECT RAISE(ABORT, 'c3 test: blocked intent insert'); END"
        ));

        const QDateTime nextTick(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime);
        clock.setNow(nextTick);
        tracker.onTick(nextTick);

        QVERIFY(tracker.hasPendingDiscards_dbg());

        ShutdownCoordinator coordinator(tracker, db);
        const DrainResult result = coordinator.run();
        QVERIFY(!result.clean());
        // tracker/db destruct here with the intent INSERT still blocked --
        // the destructor's own last-resort drain re-attempts and also
        // fails. No discard intent was ever durably recorded for this
        // segment; only its unfinalized row is on disk.
    }

    {
        // Drop the trigger via a fresh connection before the real reopen.
        Settings settings(settingsPath);
        SqliteSessionStore dbFix(settings);
        QVERIFY(dbFix.ensureOpen_dbg());
        QSqlQuery drop(dbFix.rawDb_dbg());
        QVERIFY(drop.exec("DROP TRIGGER block_intent_insert_c3_test"));
        dbFix.closeConnection_dbg();
    }

    {
        // Session 2: real reopen. With no discard intent on record, ordinary
        // startup recovery cannot tell this row apart from a genuine crash
        // orphan, so it finalizes it -- the accepted overcount.
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        const QDateTime reopenNow(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime);
        FakeClock clock(reopenNow);
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.initializeFromStore();

        QCOMPARE(tracker.getStartupRecoveredMsec(), static_cast<qint64>(10 * 60 * 1000));
        QVERIFY(tracker.shouldShowStartupRecoveryNotification());
        QCOMPARE(tracker.getStartupPolicyDroppedCount(), 0);

        SqliteSessionStore db2(settings);
        auto loaded = db2.loadDurations();
        QCOMPARE(loaded.size(), static_cast<size_t>(1));
        QCOMPARE(loaded[0].segment_id.toString(), segId.toString());
        QCOMPARE(loaded[0].startTime, start);
        QCOMPARE(loaded[0].endTime, orphanEnd);

        QVERIFY(db.ensureOpen_dbg());
        QSqlQuery countIntents(db.rawDb_dbg());
        QVERIFY(countIntents.exec("SELECT COUNT(*) FROM discard_intents"));
        QVERIFY(countIntents.next());
        QCOMPARE(countIntents.value(0).toInt(), 0);
        db.closeConnection_dbg();
    }
}

/**
 * M4: the exact silent case the finding reports -- a committed recovery
 * that finalizes nothing and policy-drops a cross-day orphan (no discard
 * intent involved) must leave the old recovered-time channel closed while
 * opening the new policy-dropped-count channel, at the TimeTracker boundary
 * mainwin.cpp actually reads.
 */
void IntegrationTest::test_M4_tracker_reports_policy_dropped_count_for_cross_day_orphan()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);

    QVERIFY(db.ensureOpen_dbg());

    // Seed one cross-day unfinalized row (no discard intent): yesterday
    // 23:50 local to today 00:10 local, on a fixed calendar date.
    const QDate yesterday = QDate(2025, 6, 1);
    const QDate today     = QDate(2025, 6, 2);
    const QDateTime localStart(yesterday, QTime(23, 50, 0), Qt::LocalTime);
    const QDateTime localEnd(today, QTime(0, 10, 0), Qt::LocalTime);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 0)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", localStart.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":e", localEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }
    db.closeConnection_dbg();

    const QDateTime fixedNow(today, QTime(0, 20, 0), Qt::LocalTime);
    FakeClock clock(fixedNow);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.initializeFromStore();

    QCOMPARE(tracker.getStartupRecoveredMsec(), static_cast<qint64>(0));
    QVERIFY(!tracker.shouldShowStartupRecoveryNotification());
    QCOMPARE(tracker.getStartupPolicyDroppedCount(), 1);
}

/**
 * C1/C3 step 4: same tracker-boundary shape as the M4 cross-day case above,
 * but for an orphan whose endTime is strictly older than History retention
 * (retention = 7 days, orphan is 10 days old) rather than cross-day.
 */
void IntegrationTest::test_M4_tracker_reports_policy_dropped_count_for_expired_orphan()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);

    QVERIFY(db.ensureOpen_dbg());

    const QDateTime fixedNowUtc = QDateTime(QDate(2025, 6, 10), QTime(12, 0, 0), Qt::UTC);
    const QDateTime expiredStart = fixedNowUtc.addDays(-10);
    const QDateTime expiredEnd   = expiredStart.addSecs(30);
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 0)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", expiredStart.toString(Qt::ISODateWithMs));
        q.bindValue(":e", expiredEnd.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }
    db.closeConnection_dbg();

    FakeClock clock(fixedNowUtc);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.initializeFromStore();

    QCOMPARE(tracker.getStartupRecoveredMsec(), static_cast<qint64>(0));
    QVERIFY(!tracker.shouldShowStartupRecoveryNotification());
    QCOMPARE(tracker.getStartupPolicyDroppedCount(), 1);
}

// D2/HIGH-A (round-2 review): a backward wall-clock step must drop only the
// closing interval, never the already-checkpointed prefix of the same live
// segment -- that prefix is valid, positive, same-day, truthfully recorded
// data. A real close/reopen proves it: the checkpoint row survives Stop
// completely untouched (no discard, no finalize-in-place), and ordinary
// startup recovery (the SAME path that already finalizes any orphan not
// overlapping finalized history) picks it up on the next launch -- lossless
// and truthful, not gated by the clean marker.
void IntegrationTest::test_D2_real_sqlite_backward_clock_stop_preserves_checkpoint_for_recovery()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    SegmentId segId;
    const QDateTime t0(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::LocalTime);
    const QDateTime checkpointEnd = t0.addSecs(300); // 10:05 -- the "2h55m" stand-in

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);

        // FakeClock declared before TimeTracker: see outlives-tracker note above
        FakeClock clock(t0);
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.useTimerViaButton(Button::Start);
        QVERIFY(tracker.isActive());
        QVERIFY(tracker.getOngoingDuration().has_value());

        // A real checkpoint row is written for the recorded prefix.
        clock.setNow(checkpointEnd);
        tracker.triggerCheckpoint_dbg();
        {
            auto orphans = db.loadUnfinalizedCheckpoints_dbg();
            QCOMPARE(orphans.size(), static_cast<size_t>(1));
            QCOMPARE(orphans[0].segment_id, tracker.getOngoingDuration()->segment_id);
            segId = orphans[0].segment_id;
        }

        // Act: a routine backward wall-clock step, then Stop.
        clock.setNow(t0.addSecs(-600));
        tracker.useTimerViaButton(Button::Stop);
        QVERIFY(tracker.isStopped());

        // Assert: the checkpoint row STILL EXISTS, unfinalized, byte-for-
        // byte unchanged -- no discard was authorized for it.
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("SELECT COUNT(*), MIN(end_utc) FROM durations "
                  "WHERE segment_id = :seg AND is_finalized = 0");
        q.bindValue(":seg", segId.toString());
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
        QCOMPARE(QDateTime::fromString(q.value(1).toString(), Qt::ISODateWithMs), checkpointEnd.toUTC());
        QVERIFY(!tracker.hasPendingDiscards_dbg());
    }

    // Session 2: real reopen, shortly after (well within History retention)
    // so ordinary policy does not drop it. Startup recovery must finalize
    // the surviving checkpoint as real recovered time.
    Settings settings(settingsPath);
    SqliteSessionStore db2(settings);
    // FakeClock declared before TimeTracker: see outlives-tracker note above
    FakeClock reopenClock(checkpointEnd.addSecs(60));
    TimeTracker reopened(settings, db2);
    reopened.setNowProvider_dbg([&reopenClock]() { return reopenClock.now(); });
    reopened.initializeFromStore();

    QCOMPARE(reopened.getStartupRecoveredMsec(), t0.msecsTo(checkpointEnd));

    SqliteSessionStore db3(settings);
    auto loaded = db3.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);
    QCOMPARE(loaded[0].segment_id, segId);
    QCOMPARE(loaded[0].startTime.toUTC(), t0.toUTC());
    QCOMPARE(loaded[0].endTime.toUTC(), checkpointEnd.toUTC());
}

// D6 (R05-002/R15-004; carries forward D2/MEDIUM-7's marker-timestamp seam
// obligation, recorded as a deviation note in the D6 packet): the marker is
// now written exclusively by the store-level durable shutdown operation
// (SqliteSessionStore::markCleanShutdown(), invoked by ShutdownCoordinator),
// timestamped via the store's own injected now-provider rather than the
// real clock or any individual stop's captured `now`. Proves both facts
// against the real store: the injected clock is what gets persisted, and
// that persisted value survives closing and reopening the database file.
void IntegrationTest::test_D6_real_sqlite_shutdown_marker_uses_injected_clock_and_survives_reopen()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    const QDateTime shutdownTime(QDate(2025, 6, 1), QTime(9, 30, 0), Qt::LocalTime);

    {
        SqliteSessionStore db(settings);
        db.setNowProvider_dbg([&shutdownTime]() { return shutdownTime; });

        // FakeClock declared before TimeTracker: see outlives-tracker note above
        FakeClock clock(t0);
        TimeTracker tracker(settings, db);
        ShutdownCoordinator coordinator(tracker, db);

        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.useTimerViaButton(Button::Start);
        clock.setNow(shutdownTime);

        // Act: a real shutdown through the production owner.
        const DrainResult result = coordinator.run();
        QVERIFY(result.clean());

        // Assert: the marker holds the STORE's injected clock reading, not
        // the tracker's -- proving markCleanShutdown() captured its own
        // timestamp via now_provider_ rather than being handed one.
        QVERIFY(db.ensureOpen_dbg());
        QSqlQuery q(db.rawDb_dbg());
        QVERIFY(q.exec("SELECT value FROM app_settings WHERE key='last_clean_shutdown'"));
        QVERIFY(q.next());
        QCOMPARE(QDateTime::fromString(q.value(0).toString(), Qt::ISODateWithMs), shutdownTime.toUTC());
        db.closeConnection_dbg();
    }

    // Act: close and reopen -- a fresh store instance over the same file.
    SqliteSessionStore reopened(settings);
    QVERIFY(reopened.ensureOpen_dbg());
    QSqlQuery q2(reopened.rawDb_dbg());
    QVERIFY(q2.exec("SELECT value FROM app_settings WHERE key='last_clean_shutdown'"));
    QVERIFY(q2.next());

    // Assert: the marker durably survived the reopen with the same value.
    QCOMPARE(QDateTime::fromString(q2.value(0).toString(), Qt::ISODateWithMs), shutdownTime.toUTC());
    reopened.closeConnection_dbg();
}

// WP3 (step6_plan_revised.md): after a restart, Today is editable once a
// finalized row that startup recovery itself finalized lies fully inside
// today's span -- real SQLite end to end (see
// test_integration_checkpoint_recovery_on_restart() for the recovery half of
// this scenario).
void IntegrationTest::test_WP3_today_editable_after_restart_over_recovery_finalized_rows()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    {
        // First session: Start, checkpoint, then "crash" -- an unfinalized
        // orphan checkpoint is left in the DB for today.
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);
        FakeClock clock(fixedNow().addSecs(-10));
        TimeTracker tracker(settings, db);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

        tracker.useTimerViaButton(Button::Start);
        clock.advance(1200);
        tracker.saveCheckpoint_locked_dbg(clock.now());
        QVERIFY(tracker.getOngoingDuration().has_value());

        tracker.simulateCrashWithoutShutdown_dbg();
    }

    // Second session: startup recovery finalizes the orphan. The tracker is
    // never started -- it is stopped, matching an ordinary "open History
    // after restart" scenario.
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.initializeFromStore();
    QVERIFY(tracker.getStartupRecoveredMsec() > 0);
    QVERIFY(tracker.isStopped());

    // Act
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert: the recovery-finalized row lies fully inside today's span.
    QCOMPARE(ctx.stored.status, SessionStoreResult::Success);
    QVERIFY(ctx.isEditable(QDate::currentDate()));
}

// WP3 (step6_plan_revised.md): a backward-clock Stop (D2's scenario) leaves
// an unfinalized checkpoint row on Today, untouched by the Stop itself --
// real SQLite. That leftover row must make Today read-only.
void IntegrationTest::test_WP3_today_readonly_leftover_unfinalized_row_from_backward_clock_stop()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);

    const QDateTime t0(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::LocalTime);
    const QDateTime checkpointEnd = t0.addSecs(300);
    FakeClock clock(t0);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // A real checkpoint row is written for the recorded prefix.
    clock.setNow(checkpointEnd);
    tracker.triggerCheckpoint_dbg();

    // Act: a backward wall-clock step, then Stop -- the closing interval is
    // dropped but the already-checkpointed prefix survives untouched.
    clock.setNow(t0.addSecs(-600));
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());

    clock.setNow(t0.addSecs(3600));
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert
    QCOMPARE(ctx.stored.status, SessionStoreResult::Success);
    QVERIFY(!ctx.isEditable(t0.date()));
}
