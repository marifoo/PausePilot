#include "test_timer.h"
#include "fakesessionstore.h"
#include "fakeclock.h"
#include <QtTest>
#include <QSqlQuery>
#include <algorithm>

using TestCommon::createSettingsFile;
using TestCommon::fixedNow;
using TestCommon::sumDurations;

void TimerTest::initTestCase()
{
    QVERIFY(data_dir_.isValid());
    TestCommon::setDataDirOverride(data_dir_.path());
    db_path_ = QDir(data_dir_.path()).filePath("uTimer.sqlite");
}

void TimerTest::cleanupTestCase()
{
    TestCommon::clearDataDirOverride();
}

void TimerTest::test_timer_start_transitions_to_active()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert
    QVERIFY(tracker.isActive());
    QVERIFY(tracker.getOngoingDuration().has_value());
}

void TimerTest::test_timer_pause_captures_duration_and_keeps_ongoing()
{
    // Arrange: reach Activity mode first (setup, not the behavior under test)
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Act
    tracker.useTimerViaButton(Button::Pause);

    // Assert: durations captured and checkpoint id reset
    QVERIFY(tracker.isPaused());
    QVERIFY(tracker.completedTimeline().completed().size() >= 1);
    QVERIFY(tracker.getOngoingDuration().has_value());
}

void TimerTest::test_timer_resume_from_pause_transitions_to_active()
{
    // Arrange: reach Pause mode first (setup, not the behavior under test)
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Pause);

    // Act
    tracker.useTimerViaButton(Button::Start);

    // Assert
    QVERIFY(tracker.isActive());
}

void TimerTest::test_timer_manual_checkpoint_preserves_ongoing_duration()
{
    // Arrange: reach Activity mode first (setup, not the behavior under test)
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    // D15/F1: deterministic clock so Start's segment_start_time and the
    // checkpoint end below are guaranteed same-day -- see the house
    // convention at test_timer.cpp:78.
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(100); // Ensure elapsed > 0

    // Act
    tracker.saveCheckpoint_locked_dbg(clock.now());

    // Assert: the checkpoint is actually written to the store (savedCheckpoints
    // is the real teeth here -- getOngoingDuration() alone depends only on
    // mode_/segment_start_time and does not depend on saveCheckpoint_locked's
    // success path, so it would pass even if the write were a no-op).
    QCOMPARE(fakeDb.savedCheckpoints.size(), static_cast<size_t>(1));
    QVERIFY(tracker.getOngoingDuration().has_value());
}

void TimerTest::test_timer_stop_transitions_to_stopped()
{
    // Arrange: reach Activity mode first (setup, not the behavior under test)
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Act
    tracker.useTimerViaButton(Button::Stop);

    // Assert: move to None
    QVERIFY(tracker.isStopped());
    // (segment_id.isEmpty() after Stop is not asserted here: mode_ == None
    // implies it, since clearSegment() -- which clears segment_id -- runs
    // straight-line with the mode_ = Mode::None assignment at
    // timetracker.cpp:853-854, the only site reached by a Stop.)
}

void TimerTest::test_timer_backpause_resets_checkpoint_and_splits()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 1); // 60s
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // ~TimeTracker() drain note elsewhere in this file).
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);

    // Move now 120s past the Start anchor, reaching the same 120s elapsed
    // segment the removed sessionState_dbg() write forced directly.
    clock.advance(120ll * 1000ll);

    // C12b: backpauseTimer requires a captured lock instant; anchor it
    // 60s before now so the expected ~120s Activity+Pause total still holds
    // (equivalent to the old now-minus-threshold fallback this test relied on).
    tracker.backpauseTimer_dbg(clock.now(), clock.now().addSecs(-60));
    QVERIFY(tracker.isPaused());
    QVERIFY(tracker.getOngoingDuration().has_value());
    QVERIFY(tracker.completedTimeline().completed().size() >= 2);
    // Sum should equal ~120s
    qint64 total = sumDurations(tracker.completedTimeline().completed(), DurationType::Activity) +
                   sumDurations(tracker.completedTimeline().completed(), DurationType::Pause);
    QVERIFY(total >= 120000 - 2000); // allow minor tolerance
}


void TimerTest::test_timer_lock_events_checkpoint_and_resume()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    tracker.useTimerViaLockEvent(LockEvent::Lock);
    QVERIFY(tracker.isLocked_dbg());
    tracker.useTimerViaLockEvent(LockEvent::Unlock);
    QVERIFY(!tracker.isLocked_dbg());
}

// C12c: the checkpoint interval is a fixed 5 minutes at the engine, not a
// setting -- confirm both the configured interval and that the timer is
// actually running in Activity.
void TimerTest::test_C12c_checkpoint_timer_interval_is_fixed_five_minutes()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);

    QCOMPARE(tracker.checkpointTimerIntervalMsec_dbg(), 5 * 60 * 1000);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());
}

// C12c: a pre-existing ini with the now-removed key, including the legacy
// "0 = off" value, must not disable checkpoints -- the cadence is fixed at
// the engine and no longer reads this setting at all.
void TimerTest::test_C12c_legacy_zero_interval_setting_no_longer_disables_checkpoints()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 0);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);

    QCOMPARE(tracker.checkpointTimerIntervalMsec_dbg(), 5 * 60 * 1000);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    // Settings' constructor must have removed the obsolete key from the ini.
    QSettings verify(settingsPath, QSettings::IniFormat);
    QVERIFY(!verify.contains("uTimer/checkpoint_interval_minutes"));

    // A legacy non-zero value (e.g. 60) must likewise have no effect on the
    // fixed cadence, and must also be removed from the ini.
    QTemporaryDir tempDir2;
    QVERIFY(tempDir2.isValid());
    QString settingsPath2 = createSettingsFile(tempDir2.path(), 7);
    QSettings writer2(settingsPath2, QSettings::IniFormat);
    writer2.setValue("uTimer/checkpoint_interval_minutes", 60);
    writer2.sync();

    Settings settings2(settingsPath2);
    FakeSessionStore fakeDb2;
    TimeTracker tracker2(settings2, fakeDb2);
    tracker2.useTimerViaButton(Button::Start);

    QCOMPARE(tracker2.checkpointTimerIntervalMsec_dbg(), 5 * 60 * 1000);
    QVERIFY(tracker2.isCheckpointTimerActive_dbg());

    QSettings verify2(settingsPath2, QSettings::IniFormat);
    QVERIFY(!verify2.contains("uTimer/checkpoint_interval_minutes"));
}

// C12c: the periodic checkpoint timer must not run outside Activity -- it
// stops on Pause and on Stop.
void TimerTest::test_C12c_checkpoint_timer_stops_in_pause_and_after_stop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());
    QVERIFY(!tracker.isCheckpointTimerActive_dbg());

    // No periodic checkpoint write while paused, even if manually triggered
    // (as the stopped QTimer would have, had it still been running).
    const size_t savedBeforeTrigger = fakeDb.savedCheckpoints.size();
    tracker.triggerCheckpoint_dbg();
    QCOMPARE(fakeDb.savedCheckpoints.size(), savedBeforeTrigger);

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.isCheckpointTimerActive_dbg());
}

// C12c: the immediate lock-instant checkpoint attempt (onLockDetected) is
// retained unconditionally -- it no longer depends on any configurable
// interval, so a Lock while Active must still write a checkpoint row.
void TimerTest::test_C12c_lock_writes_immediate_checkpoint()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(1000);

    QCOMPARE(fakeDb.savedCheckpoints.size(), static_cast<size_t>(0));

    tracker.useTimerViaLockEvent(LockEvent::Lock);

    QVERIFY(tracker.isLocked_dbg());
    QCOMPARE(fakeDb.savedCheckpoints.size(), static_cast<size_t>(1));

    // No further periodic checkpoints while locked -- even a manually
    // triggered tick (as the QTimer would fire) is suppressed.
    clock.advance(60ll * 1000ll);
    tracker.triggerCheckpoint_dbg();
    QCOMPARE(fakeDb.savedCheckpoints.size(), static_cast<size_t>(1));
}

void TimerTest::test_timer_ongoing_duration()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // Initially stopped
    QCOMPARE(tracker.getOngoingDuration(), std::nullopt);

    // Start -> Activity
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10); // Ensure time advances so segment_start_time < now
    auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QCOMPARE(ongoing->type, DurationType::Activity);
    
    // Pause -> Pause
    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);
    ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QCOMPARE(ongoing->type, DurationType::Pause);

    // Stop -> None
    tracker.useTimerViaButton(Button::Stop);
    QCOMPARE(tracker.getOngoingDuration(), std::nullopt);
}

void TimerTest::test_timer_checkpoints_paused()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    // Checkpoints run on the engine's fixed cadence; here we trigger manually.
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);
    
    // Must be in Activity to save checkpoints
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(100); // Ensure elapsed > 0

    // Pause checkpoints
    tracker.beginExclusiveEdit();
    tracker.triggerCheckpoint_dbg(); // Should be ignored
    
    // Check DB: no unfinalized checkpoint entries yet
    QVERIFY(db.ensureOpen_dbg());
    QSqlQuery query(db.rawDb_dbg());
    QVERIFY(query.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
    db.closeConnection_dbg();

    // Resume and trigger
    tracker.endExclusiveEdit();
    tracker.triggerCheckpoint_dbg();

    // Check DB: should now have one unfinalized checkpoint entry
    QVERIFY(db.ensureOpen_dbg());
    QVERIFY(query.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    db.closeConnection_dbg();
}

void TimerTest::test_timer_retry_append_failure_then_success_preserves_segments()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);
    SqliteSessionStore db2(settings);
    QVERIFY(db2.ensureOpen_dbg());

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    // Force stop save to fail and keep data in memory for retry.
    // Close the connection first so the next ensureOpen() re-tries the file open
    // and fails on the read-only permission.  (With a long-lived connection the
    // file descriptor is open with write permission; making the file read-only
    // afterwards doesn't revoke that permission until the connection is closed.)
    db.closeConnection_dbg();
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
    tracker.useTimerViaButton(Button::Stop);

    QVERIFY(tracker.hasPendingCommit_dbg());
    std::deque<TimeDuration> unsavedCopy = tracker.pendingCommitPayload_dbg();

    // Retry while DB is still blocked -> must stay unsaved.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Restore DB and retry again -> previously unsaved rows should be appended.
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
    tracker.useTimerViaButton(Button::Stop);
    tracker.useTimerViaButton(Button::Start);

    auto loaded = db2.loadDurations();

    for (const auto& d : unsavedCopy) {
        bool found = false;
        for (const auto& row : loaded) {
            if (row.type == d.type && row.startTime == d.startTime && row.endTime == d.endTime && row.duration() == d.duration()) {
                found = true;
                break;
            }
        }
        QVERIFY(found);
    }
}

void TimerTest::test_timer_retry_failure_keeps_unsaved_state_and_durations()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);
    SqliteSessionStore db2(settings);
    QVERIFY(db2.ensureOpen_dbg());

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    // Force first stop save to fail.  Close the connection before making the
    // file read-only so that ensureOpen() re-tries and fails on the permission.
    db.closeConnection_dbg();
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
    tracker.useTimerViaButton(Button::Stop);

    QVERIFY(tracker.hasPendingCommit_dbg());

    // Retry still fails -> must still be unsaved with data intact.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Cleanup permissions for following tests, then resolve the pending
    // payload via the same idempotent drain a real shutdown would use (A3):
    // an unresolved pending commit at destruction with no prior drain call
    // is flagged as an orchestration bug.
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
    QVERIFY(tracker.drainForShutdown().clean());
}

// ============================================================================
// SessionState transition tests
// ============================================================================

void TimerTest::test_replaceAll_success_clears_unsaved_cache()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Arrange: a pending commit via a real failed Stop persist (behavior, not
    // direct field injection).
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Act: replaceAll succeeds (default result), so the DB is now authoritative.
    const SessionStoreResult result = tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt));

    QVERIFY(result.ok());
    QVERIFY(!tracker.hasPendingCommit_dbg());
    QVERIFY(tracker.pendingCommitPayload_dbg().empty());
}

void TimerTest::test_replaceAll_failure_keeps_unsaved_cache()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Arrange: a pending commit via a real failed Stop persist.
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Act: replaceAll itself also fails.
    fakeDb.replaceDurationsResult = SessionStoreResult::transient("injected failure");
    const SessionStoreResult result = tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt));

    QVERIFY(!result.ok());
    QVERIFY(result.category == SessionStoreResult::TransientError);
    QVERIFY(tracker.hasPendingCommit_dbg());
    QVERIFY(!tracker.pendingCommitPayload_dbg().empty());

    // Cleanup (A3): resolve the pending payload via the same idempotent
    // drain a real shutdown would use, so destruction does not find an
    // unresolved commit with no prior drain call.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

void TimerTest::test_session_state_start_to_pause_transition()
{
    // Arrange: Start -> Activity, then pause, then stop -- FakeClock drives
    // time instead of real waits.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Act: start
    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);

    // Assert: Activity mode, valid segment
    QVERIFY(tracker.isActive());
    QVERIFY(tracker.getOngoingDuration().has_value());
    const SegmentId activitySegId = tracker.getOngoingDuration()->segment_id;

    // Act: pause
    tracker.useTimerViaButton(Button::Pause);

    // Assert: Pause mode, new segment id (different from Activity)
    QVERIFY(tracker.isPaused());
    QVERIFY(tracker.getOngoingDuration().has_value());
    QVERIFY(tracker.getOngoingDuration()->segment_id != activitySegId);

    const SegmentId pauseSegId = tracker.getOngoingDuration()->segment_id;

    // Act: stop
    clock.advance(30ll * 1000ll);
    tracker.useTimerViaButton(Button::Stop);

    // Assert: the committed timeline holds exactly the Activity and Pause
    // segments, each under its own id and with its own type. FakeSessionStore
    // does not dedupe re-submissions of the same segment_id the way the real
    // store's upsert does (documented in fakesessionstore.h), so identify
    // segments by the deduped id set rather than by raw row count.
    QCOMPARE(fakeDb.committedSegmentIds.size(), 2);
    QVERIFY(fakeDb.committedSegmentIds.contains(activitySegId.toString()));
    QVERIFY(fakeDb.committedSegmentIds.contains(pauseSegId.toString()));
    const auto activityRow = std::find_if(fakeDb.storedDurations.begin(), fakeDb.storedDurations.end(),
        [&](const TimeDuration& d) { return d.segment_id == activitySegId; });
    const auto pauseRow = std::find_if(fakeDb.storedDurations.begin(), fakeDb.storedDurations.end(),
        [&](const TimeDuration& d) { return d.segment_id == pauseSegId; });
    QVERIFY(activityRow != fakeDb.storedDurations.end());
    QVERIFY(pauseRow != fakeDb.storedDurations.end());
    QCOMPARE(activityRow->type, DurationType::Activity);
    QCOMPARE(pauseRow->type, DurationType::Pause);
}

void TimerTest::test_session_state_pause_to_activity_transition()
{
    // Arrange: Start -> Pause -> Resume
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(10ll * 1000ll);
    tracker.useTimerViaButton(Button::Pause);
    clock.advance(10ll * 1000ll);

    QVERIFY(tracker.getOngoingDuration().has_value());
    const SegmentId pauseSegId = tracker.getOngoingDuration()->segment_id;
    // FakeSessionStore does not dedupe re-submissions of the same segment_id
    // the way the real store's upsert does, so track distinct committed
    // segments through the deduped id set rather than raw row count.
    const int committedBeforeResume = fakeDb.committedSegmentIds.size();
    QVERIFY(!fakeDb.committedSegmentIds.contains(pauseSegId.toString()));

    // Act: resume (Start from Pause)
    tracker.useTimerViaButton(Button::Start);

    // Assert: Activity mode, new segment id, pause duration committed
    QVERIFY(tracker.isActive());
    QVERIFY(tracker.getOngoingDuration().has_value());
    QVERIFY(tracker.getOngoingDuration()->segment_id != pauseSegId);
    QCOMPARE(fakeDb.committedSegmentIds.size(), committedBeforeResume + 1);
    QVERIFY(fakeDb.committedSegmentIds.contains(pauseSegId.toString()));
    const auto pauseRow = std::find_if(fakeDb.storedDurations.begin(), fakeDb.storedDurations.end(),
        [&](const TimeDuration& d) { return d.segment_id == pauseSegId; });
    QVERIFY(pauseRow != fakeDb.storedDurations.end());
    QCOMPARE(pauseRow->type, DurationType::Pause);
}

void TimerTest::test_session_state_stop_clears_segment()
{
    // Arrange: Start -> Stop
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(10ll * 1000ll);
    QVERIFY(tracker.getOngoingDuration().has_value());

    // Act
    tracker.useTimerViaButton(Button::Stop);

    // Assert: None mode, no pending commit (durations cleared, saved to DB)
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.hasPendingCommit_dbg());
}

void TimerTest::test_session_state_adopted_segment_start_time_anchors_committed_row()
{
    // Arrange: Start an Activity segment, then commit a History-style edit
    // that moves ITS start time earlier (same segment_id, same type -- just
    // a moved anchor), the way HistoryDialog::saveChanges() does via
    // TimeTracker::commitEditedTimeline(). This drives
    // SessionState::adoptOngoingSegment() through its real public entry
    // point rather than a _dbg seam.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    QVERIFY(tracker.getOngoingDuration().has_value());
    const SegmentId segId = tracker.getOngoingDuration()->segment_id;

    // Act: edit the ongoing segment's start time to 20 minutes earlier than
    // t0 and commit it, as a History edit would.
    const QDateTime editedStart = t0.addSecs(-20 * 60);
    const TimeDuration editedOngoing = TimeDuration::fromLiveSession(
        DurationType::Activity, editedStart, clock.now(), segId);
    const TimeTracker::EditCommitResult commitResult =
        tracker.commitEditedTimeline(Timeline({}, editedOngoing));
    QVERIFY(commitResult.ok);

    // Advance further and stop, so the ongoing segment is finalized and
    // written to the store.
    clock.advance(5ll * 60ll * 1000ll);
    const QDateTime stopTime = clock.now();
    tracker.useTimerViaButton(Button::Stop);

    // Assert: the committed row's startTime (and thus its length) reflects
    // the edited anchor, not the original t0 start -- read from the store,
    // not a _dbg seam.
    const auto row = std::find_if(fakeDb.storedDurations.begin(), fakeDb.storedDurations.end(),
        [&](const TimeDuration& d) { return d.segment_id == segId; });
    QVERIFY(row != fakeDb.storedDurations.end());
    QCOMPARE(row->type, DurationType::Activity);
    QCOMPARE(row->startTime, editedStart);
    QCOMPARE(row->duration(), editedStart.msecsTo(stopTime));
}

// ============================================================================
// Boot time + hasEntriesForDate tri-state tests (T18)
// ============================================================================

void TimerTest::test_boot_time_not_added_when_history_disabled()
{
    // When history is disabled (history_days_to_keep = 0), hasEntriesForDate
    // returns Unknown. startTimer must NOT add boot time in this case,
    // because we can't confirm whether entries already exist for today.
    // Adding boot time unconditionally would cause double-counting on every
    // subsequent start of the day.

    // Arrange
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 0);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // Act: start timer twice (simulating a second start on the same day)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Stop);
    QTest::qWait(10);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: no boot time was added on either start, because history is
    // disabled and hasEntriesForDate returns Unknown.
    // The only durations should be from actual timer running, not boot time.
    qint64 totalActivity = sumDurations(tracker.completedTimeline().completed(), DurationType::Activity);
    // Boot time would be 30000ms. If it were added, total would be >= 30000.
    // Since we only ran for ~10ms, it should be much less.
    QVERIFY2(totalActivity < 5000,
             "Boot time should not be added when history is disabled (DB returns Unknown)");

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_boot_time_added_once_on_empty_db()
{
    // When history is enabled and the DB is empty for today, boot time
    // should be added on the first startTimer of the day.

    // Arrange
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    // D15/F1 (part 3, B1): Start computes bootStart = now - boot_time_seconds
    // (timetracker.cpp:646). On the real clock, within 30s of local midnight
    // that lands on the previous civil day and addDuration() rejects the
    // cross-midnight interval as CrossMidnight -- boot time silently isn't
    // credited and this assertion fails. Pin to fixedNow() (local noon), far
    // from that window. FakeClock declared before TimeTracker so it outlives
    // it (see the outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Act: start the timer (first session today, empty DB)
    tracker.useTimerViaButton(Button::Start);
    clock.advance(10);

    // Assert: boot time (30s = 30000ms) was added as an Activity segment
    qint64 totalActivity = sumDurations(tracker.completedTimeline().completed(), DurationType::Activity);
    QVERIFY2(totalActivity >= 29000,
             "Boot time should be added when DB is empty for today");

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_boot_time_not_added_when_db_has_entries_for_today()
{
    // When history is enabled and the DB already has entries for today,
    // boot time must NOT be added (to avoid double-counting).

    // Arrange: seed the DB with an entry for today
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);

    // Anchored to a fixed local noon-ish civil day (not real now), so the
    // seed and the tracker's "today" can never straddle local midnight --
    // see the house convention at test_timer.cpp:78 and git show 5e63eae.
    const QDateTime anchor(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);

    // Use a separate SqliteSessionStore to seed an entry for today
    {
        SqliteSessionStore seeder(settings);
        std::deque<TimeDuration> durations;
        durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, anchor.addSecs(-3600), anchor.addSecs(-3540)));
        QVERIFY(seeder.commitSession(Timeline(durations, std::nullopt)).ok());
    }

    SqliteSessionStore db(settings);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(anchor);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Act: start the timer (DB already has entries for today)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: no boot time added. Only the in-progress segment should exist.
    qint64 totalActivity = sumDurations(tracker.completedTimeline().completed(), DurationType::Activity);
    QVERIFY2(totalActivity < 5000,
             "Boot time should not be added when DB already has entries for today");

    tracker.useTimerViaButton(Button::Stop);
}

// ============================================================================
// Pause persistence crash safety (T19)
// ============================================================================

void TimerTest::test_pause_row_persisted_immediately_on_resume()
{
    // Verify that when the user resumes from Pause (startTimer from Pause),
    // the completed Pause duration is immediately persisted to the DB as a
    // finalized row. A simulated crash (skipping stopTimer) should not lose
    // the Pause entry — the reloaded history must show the Pause row.

    // Arrange
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);

    {
        SqliteSessionStore db(settings);
        TimeTracker tracker(settings, db);

        // Start -> Activity
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(20);

        // Pause -> records Activity segment, transitions to Pause
        tracker.useTimerViaButton(Button::Pause);
        QTest::qWait(50);

        // Resume -> completes Pause segment and persists it immediately (T19)
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(20);

        // Simulate crash: destroy tracker without calling stopTimer.
        // Prevents the destructor's drain from gracefully finalizing the
        // segment, so it survives as an unfinalized orphan.
        tracker.simulateCrashWithoutShutdown_dbg();
    }

    // Assert: re-read the DB and verify the Pause row survived the crash.
    SqliteSessionStore db(settings);
    auto loaded = db.loadDurations();

    // We expect at least one Activity row (from the finalizeActivityToPause
    // call in pauseTimer) and one Pause row (from startTimer's immediate
    // persist). The second Activity segment may or may not be present
    // depending on checkpoint timing, but the Pause must be there.
    bool hasPause = false;
    bool hasActivity = false;
    for (const auto& d : loaded.durations) {
        if (d.type == DurationType::Pause) {
            hasPause = true;
        }
        if (d.type == DurationType::Activity) {
            hasActivity = true;
        }
    }
    QVERIFY2(hasPause, "Pause row must survive a crash after resume (T19)");
    QVERIFY2(hasActivity, "Activity row must be present in history after pause+resume");
}

// ============================================================================
// Midnight forced-stop and cross-midnight discard tests
// ============================================================================

void TimerTest::test_addduration_appends_single_same_day_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    QDateTime start = fixedNow().addSecs(-60);
    QDateTime end = fixedNow();

    // Act
    tracker.addDuration_dbg(DurationType::Activity, start, end);

    // Assert
    const auto completed = tracker.completedTimeline().completed();
    QCOMPARE(completed.size(), static_cast<size_t>(1));
    QCOMPARE(completed[0].type, DurationType::Activity);
    QCOMPARE(completed[0].startTime, start);
    QCOMPARE(completed[0].endTime, end);
}

void TimerTest::test_addduration_discards_cross_midnight_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    QDate yesterday = QDate::currentDate().addDays(-1);
    QDateTime start(yesterday, QTime(23, 59, 58, 0));
    QDateTime end = start.addSecs(5); // crosses midnight

    // Act
    tracker.addDuration_dbg(DurationType::Activity, start, end);

    // Assert: cross-midnight segment was silently discarded
    QVERIFY(tracker.completedTimeline().completed().empty());
}

void TimerTest::test_addduration_discards_zero_and_negative_duration()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    QDateTime now = QDateTime::currentDateTime();

    // Act: zero duration
    tracker.addDuration_dbg(DurationType::Activity, now, now);
    // Act: negative duration
    tracker.addDuration_dbg(DurationType::Activity, now, now.addSecs(-10));

    // Assert
    QVERIFY(tracker.completedTimeline().completed().empty());
}

// 9.1: all three DurationCreateError causes (InvalidTimestamp, NonPositive, CrossMidnight)
// must discard the segment in addDuration. This test covers the InvalidTimestamp path,
// which was previously lumped under the catch-all [MIDNIGHT] log tag.
void TimerTest::test_addduration_discards_invalid_timestamp()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    QDateTime valid = QDateTime::currentDateTime();
    QDateTime invalid; // default-constructed QDateTime is invalid

    // Invalid start
    tracker.addDuration_dbg(DurationType::Activity, invalid, valid);
    QVERIFY(tracker.completedTimeline().completed().empty());

    // Invalid end
    tracker.addDuration_dbg(DurationType::Activity, valid, invalid);
    QVERIFY(tracker.completedTimeline().completed().empty());
}

void TimerTest::test_useTimerViaButton_force_stops_on_cross_midnight_ongoing()
{
    // Arrange: start yesterday just before midnight via the clock seam,
    // then advance the clock into today -- the real sleep-through-midnight
    // transition, instead of writing segment_start_time directly.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.setNow(QDateTime::currentDateTime());

    // Act: try to stop — discardCrossMidnightOngoingAndStop should fire first
    tracker.useTimerViaButton(Button::Stop);

    // Assert: engine is in None, no cross-midnight row in DB.
    // (!tracker.sessionState_dbg().segment_start_time.isValid() is not
    // asserted here: mode_ is assigned Mode::None at exactly two sites in
    // the codebase -- stopTimer() (timetracker.cpp:853), immediately
    // followed by session_.clearSegment() at :854, and
    // commitEditedTimeline()'s no-ongoing branch (timetracker.cpp:1206),
    // preceded by clearSegment() at :1204 -- and clearSegment()
    // (timetracker.cpp:240-248) clears segment_start_time in the same
    // straight-line body. The midnight watchdog delegates teardown to
    // stopTimer() (discardCrossMidnightOngoingAndStop(), timetracker.cpp:1601)
    // so it is covered by the :853/:854 pair; initializeFromStore()
    // (timetracker.cpp:445-453) never touches mode_ or session_; and
    // commitEditedTimeline()'s stopped branch (timetracker.cpp:1193-1199)
    // refuses to adopt a segment while mode_ == None. So isStopped()
    // implies !segment_start_time.isValid() on every path -- enforcing
    // this centrally is P5e's job (checkDurationInvariants()).)
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.isAutopausePendingResume_dbg());
    // FakeSessionStore captures writes; no duration should have been written
    // with a cross-midnight segment (the in-flight segment is discarded)
    for (const auto& d : fakeDb.storedDurations) {
        QVERIFY(!isCrossMidnight(d.startTime, d.endTime));
    }
}

void TimerTest::test_useTimerViaButton_pause_event_is_dropped_when_cross_midnight()
{
    // Arrange: simulate cross-midnight state via the clock seam -- start
    // yesterday just before midnight, then advance the clock into today --
    // instead of writing segment_start_time directly.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.setNow(QDateTime::currentDateTime());

    // Act: user presses Pause — but the guard should intercept and force-stop
    tracker.useTimerViaButton(Button::Pause);

    // Assert: engine is in None (not Pause).
    // (!tracker.sessionState_dbg().segment_start_time.isValid() is not
    // asserted here for the same reason as in
    // test_useTimerViaButton_force_stops_on_cross_midnight_ongoing above --
    // mode_ is assigned Mode::None only alongside clearSegment() (see that
    // test's comment for the full chain of sites), so isStopped() implies
    // !segment_start_time.isValid() on every path. Enforcing this centrally
    // is P5e's job (checkDurationInvariants()).)
    QVERIFY(tracker.isStopped());
}

void TimerTest::test_useTimerViaLockEvent_unlock_does_not_restart_after_cross_midnight_discard()
{
    // Arrange: simulate cross-midnight with autopause_pending_resume_ set.
    // Drive Pause mode with a segment anchored just before midnight through
    // real transitions -- Start yesterday evening, Pause at 23:59:58 (still
    // same-day), then advance the clock into today -- instead of writing
    // mode_/segment_start_time directly.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    const QDate yesterday = QDate::currentDate().addDays(-1);
    FakeClock clock(QDateTime(yesterday, QTime(23, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.setNow(QDateTime(yesterday, QTime(23, 59, 58, 0), Qt::LocalTime));
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());
    tracker.setAutopausePendingResume_dbg(true);
    clock.setNow(QDateTime::currentDateTime());

    // Act: Unlock event arrives — guard should discard and not restart
    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    // Assert: stayed in None, did not auto-restart
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.isAutopausePendingResume_dbg());
}

void TimerTest::test_saveCheckpoint_locked_does_not_write_cross_midnight_row()
{
    // Arrange: start yesterday just before midnight via the clock seam,
    // instead of writing segment_start_time directly. No event loop is
    // spun before the checkpoint call below, so the real checkpoint QTimer
    // (armed by Start) cannot fire first (trap: it would force-stop the
    // segment before the Act below gets to).
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Act: checkpoint fires with "now" on the far side of midnight
    // (simulating late checkpoint after sleep)
    clock.setNow(QDateTime::currentDateTime());
    const QDateTime now = clock.now();
    tracker.saveCheckpoint_locked_dbg(now);

    // Assert: engine stopped, no checkpoint written to DB
    QVERIFY(tracker.isStopped());
    QCOMPARE(fakeDb.savedCheckpoints.size(), static_cast<size_t>(0));
}

void TimerTest::test_stop_clears_was_active_before_autopause()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.setAutopausePendingResume_dbg(true);

    // Act
    tracker.useTimerViaButton(Button::Stop);

    // Assert
    QVERIFY(!tracker.isAutopausePendingResume_dbg());
    QVERIFY(tracker.isStopped());
}

void TimerTest::test_startTimer_drops_cross_midnight_boot_time_entry()
{
    // A boot_time_sec large enough that bootStart falls on the previous day.
    // addDuration should silently discard it and the user still enters Activity.
    //
    // A14: boot_time_sec_ is now capped at 3600s (1h), so the fake clock is
    // parked shortly after midnight (00:30) rather than relying on a
    // multi-hour boot time to reach across the day boundary.

    // Arrange
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    // Boot time at the (post-A14) cap; combined with "now" just after midnight
    // below, bootStart still lands on the previous day.
    writer.setValue("uTimer/boot_time_seconds", 3600); // 1 hour (capped max)
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(0, 30, 0), Qt::LocalTime));
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Act
    tracker.useTimerViaButton(Button::Start);

    // Assert: cross-midnight boot entry was dropped; no completed durations
    // (the ongoing segment is fresh from startTimer)
    QCOMPARE(tracker.completedTimeline().completed().size(), static_cast<size_t>(0));
    QVERIFY(tracker.isActive());

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_getOngoingDuration_returns_nullopt_when_cross_midnight()
{
    // Arrange: start yesterday just before midnight via the clock seam,
    // then advance the clock into today -- the real sleep-through-midnight
    // transition, instead of writing segment_start_time directly.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.setNow(QDateTime::currentDateTime());

    // Act
    auto result = tracker.getOngoingDuration();

    // Assert: cross-midnight transient is hidden from observers
    QVERIFY(!result.has_value());
}

// ============================================================================
// TimeTracker public surface regression guard
// ============================================================================

void TimerTest::test_D_timetracker_public_surface_regression()
{
    // Construct a TimeTracker and exercise every public method to confirm none
    // were accidentally removed. This is a regression guard — it would fail
    // to compile if a method disappeared from the header.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    QDateTime now = QDateTime::currentDateTime();

    // Exercise every public method (slots + regular)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Accessors
    qint64 active = tracker.getActiveTime();
    QVERIFY(active >= 0);

    qint64 pause = tracker.getPauseTime();
    QVERIFY(pause >= 0);

    const auto& durations = tracker.completedTimeline();
    (void)durations;

    auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value()); // timer is running

    qint64 recovered = tracker.getStartupRecoveredMsec();
    QCOMPARE(recovered, static_cast<qint64>(0)); // fresh db, nothing to recover

    bool showNotification = tracker.shouldShowStartupRecoveryNotification();
    (void)showNotification;

    // Mutators
    tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    tracker.useTimerViaButton(Button::Stop);

    // DB write helpers
    fakeDb.callLog.clear();
    tracker.updateDurationsInDB_dbg();
    tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt));

    // loadHistoryFromStore
    auto history = tracker.loadHistoryFromStore();
    (void)history;

    // lock event path
    tracker.useTimerViaLockEvent(LockEvent::Lock);
    tracker.useTimerViaLockEvent(LockEvent::Unlock);
}

void TimerTest::test_E_boot_time_gate_entries_yes_skips_boot_time()
{
    // Test E (Yes branch): DB says entries exist for today → boot time skipped.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    fakeDb.entriesForDateResult = EntriesForDateResult::Yes;
    TimeTracker tracker(settings, fakeDb);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: no boot-time segment was added (session_.durations is empty
    // at start because there are no completed segments yet)
    qint64 totalActivity = 0;
    const auto completedNow = tracker.completedTimeline();
    for (const auto& d : completedNow.completed())
        if (d.type == DurationType::Activity)
            totalActivity += d.duration();
    QVERIFY2(totalActivity < 5000,
             "Boot time must not be added when DB reports Yes for today");

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_E_boot_time_gate_entries_no_adds_boot_time()
{
    // Test E (No branch): DB says no entries for today → boot time added.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    fakeDb.entriesForDateResult = EntriesForDateResult::No;
    // D15/F1 (part 3, B1): see test_boot_time_added_once_on_empty_db() above
    // -- Start's bootStart = now - boot_time_seconds can cross local midnight
    // on the real clock. FakeClock declared before TimeTracker so it outlives
    // it (see the outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Act
    tracker.useTimerViaButton(Button::Start);
    clock.advance(10);

    // Assert: a 30-second boot-time Activity segment was prepended
    qint64 totalActivity = 0;
    const auto completedNow = tracker.completedTimeline();
    for (const auto& d : completedNow.completed())
        if (d.type == DurationType::Activity)
            totalActivity += d.duration();
    QVERIFY2(totalActivity >= 29000,
             "Boot time must be added when DB reports No entries for today");

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_E_boot_time_gate_entries_unknown_skips_boot_time()
{
    // Test E (Unknown branch): DB result is Unknown (e.g., history disabled)
    // → boot time skipped to avoid double-counting.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    fakeDb.entriesForDateResult = EntriesForDateResult::Unknown;
    TimeTracker tracker(settings, fakeDb);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: no boot-time segment added
    qint64 totalActivity = 0;
    const auto completedNow = tracker.completedTimeline();
    for (const auto& d : completedNow.completed())
        if (d.type == DurationType::Activity)
            totalActivity += d.duration();
    QVERIFY2(totalActivity < 5000,
             "Boot time must not be added when DB reports Unknown for today");

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_E_boot_time_inmemory_start_date_skips_boot_time()
{
    // Regression guard for #3: in-memory check uses
    // DayValidity::overlapsLocalDate(d.startTime, d.endTime, today) (D1), not a
    // private startTime/endTime.date() comparison. DB says No entries, but an
    // in-memory completed cross-midnight segment starting today and ending
    // tomorrow overlaps today and must block boot time from being added.

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 30);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    fakeDb.entriesForDateResult = EntriesForDateResult::No;
    TimeTracker tracker(settings, fakeDb);

    // Pre-seed a cross-midnight completed segment: startTime is today, endTime is
    // tomorrow. It overlaps today (DayValidity::overlapsLocalDate), so the
    // production check must find it and block boot time from being added.
    QDateTime now = QDateTime::currentDateTime();
    QDateTime startToday = QDateTime(now.date(), QTime(23, 0, 0));
    QDateTime endTomorrow = startToday.addSecs(3600); // crosses midnight
    // seedCompletedDuration_dbg() kept deliberately: no public API can inject
    // a cross-midnight *completed* segment directly into session_.durations
    // -- addDuration() discards it by design (TimeDuration::create returns
    // CrossMidnight, covered separately by
    // test_addduration_discards_cross_midnight_segment), so this in-memory
    // precondition is unreachable through any real transition.
    // Hazard: this pre-seeds a cross-midnight completed row directly, and
    // Timeline's constructor runs assertSameDayInvariant() under
    // #ifndef QT_NO_DEBUG (timeline.cpp:10-17), which Q_ASSERTs
    // !isCrossMidnight(...) per completed row (timeline.cpp:108-113). It is
    // safe only because Start's None branch reaches
    // session_.resetForNewSession() (timetracker.cpp:644) below and wipes
    // the pre-seed before completedTimeline() reads it; if Start ever
    // returns earlier -- fatal_state_latched_ (:604), !retryUnsavedDurations()
    // (:612), or the second latch check (:621) -- the pre-seed survives and
    // the read below aborts the whole qtest binary instead of failing one
    // test.
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, startToday, endTomorrow));

    // Act: start the timer (DB says No, but in-memory entry exists for today)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: boot time was NOT added (in-memory start-date check fired).
    // (See the pre-seed comment above: this completedTimeline() read is the
    // Q_ASSERT hazard site -- it is only safe because resetForNewSession()
    // already wiped the pre-seeded cross-midnight row.)
    qint64 totalActivity = 0;
    const auto completedNow = tracker.completedTimeline();
    for (const auto& d : completedNow.completed())
        if (d.type == DurationType::Activity && d.duration() >= 25000)
            totalActivity += d.duration();
    QVERIFY2(totalActivity < 29000,
             "Boot time must not be added when in-memory entry has startTime == today");

    tracker.useTimerViaButton(Button::Stop);
}

// ============================================================================
// Stop persists state via commitSession only
// ============================================================================

/**
 * X: Stop persists state via commitSession only.
 *
 * After a start→stop cycle, the FakeSessionStore's callLog must contain
 * "commitSession" for the stop write.
 */
void TimerTest::test_X_stop_persists_via_commitSession_only()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    fakeDb.callLog.clear(); // record only the stop-path writes
    tracker.useTimerViaButton(Button::Stop);

    // Assert: commitSession was called
    QVERIFY2(fakeDb.callLog.contains("commitSession"),
             "Stop must call commitSession to persist the session");
}

// WP5: a Lock arriving while History is open revokes the edit immediately,
// then runs the ordinary no-dialog lock checkpoint and, once
// LongOngoingLock/Unlock arrive, the ordinary backpause/resume -- exactly as
// if History had never been open.
void TimerTest::test_lock_with_history_open_revokes_then_backpauses_normally()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 15);
    writer.sync();
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Arrange
    tracker.useTimerViaButton(Button::Start);
    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());

    QSignalSpy revokedSpy(&tracker, &TimeTracker::exclusiveEditRevoked);
    const QDateTime lockTime(QDate(2025, 6, 1), QTime(9, 20, 0), Qt::LocalTime);
    clock.setNow(lockTime);

    // Act: Lock arrives while History is open.
    tracker.useTimerViaLockEvent(LockEvent::Lock);

    // Assert: revoked, a lock checkpoint saved, the dialog closed.
    QCOMPARE(revokedSpy.count(), 1);
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(fakeDb.callLog.contains("saveCheckpoint"));
    QVERIFY(tracker.isActive());

    // Act: LongOngoingLock matures the backpause.
    const QDateTime maturedTime(QDate(2025, 6, 1), QTime(9, 35, 0), Qt::LocalTime);
    clock.setNow(maturedTime);
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, lockTime);

    // Assert: A [09:00, 09:20] and P [09:20, 09:35] both completed -- the
    // backpause retroactively accounts the locked interval up to "now"
    // (09:35), then a fresh Pause segment starts ongoing from there.
    QVERIFY(tracker.isPaused());
    const auto completedNow = tracker.completedTimeline();
    const auto& d = completedNow.completed();
    bool foundActivity = false, foundPause = false;
    for (const auto& seg : d) {
        if (seg.type == DurationType::Activity && seg.startTime == t0 && seg.endTime == lockTime)
            foundActivity = true;
        if (seg.type == DurationType::Pause && seg.startTime == lockTime && seg.endTime == maturedTime)
            foundPause = true;
    }
    QVERIFY2(foundActivity, "Expected completed Activity segment 09:00->09:20");
    QVERIFY2(foundPause, "Expected completed Pause segment 09:20->09:35");
    const auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QCOMPARE(ongoing->type, DurationType::Pause);
    QCOMPARE(ongoing->startTime, maturedTime);

    // Act: Unlock resumes.
    const QDateTime unlockTime(QDate(2025, 6, 1), QTime(9, 50, 0), Qt::LocalTime);
    clock.setNow(unlockTime);
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);
    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    // Assert: LockResume.
    QVERIFY(tracker.isActive());
    QCOMPARE(transitionedSpy.count(), 1);
    QCOMPARE(transitionedSpy.first().first().value<TimeTracker::Transition>().cause,
             TimeTracker::TransitionCause::LockResume);

    tracker.useTimerViaButton(Button::Stop);
}

// WP5: a Lock arriving while History is open and the timer is stopped still
// revokes the edit, but writes nothing -- there is no Activity segment to
// checkpoint.
void TimerTest::test_lock_with_history_open_while_stopped_revokes_without_writes()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Arrange: timer stopped, History open.
    QVERIFY(tracker.isStopped());
    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());

    QSignalSpy revokedSpy(&tracker, &TimeTracker::exclusiveEditRevoked);
    fakeDb.callLog.clear();

    // Act
    tracker.useTimerViaLockEvent(LockEvent::Lock);

    // Assert: revoked, dialog closed, still stopped, nothing written.
    QCOMPARE(revokedSpy.count(), 1);
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isStopped());
    QVERIFY(!fakeDb.callLog.contains("saveCheckpoint"));
    QVERIFY(fakeDb.storedDurations.empty());
}

// WP5 lag window: History can only be open while unlocked, so the only way a
// LongOngoingLock/Unlock revokes is the lag between an already-debounced
// Unlock and the moment it is processed. Arranged with no dialog first (Lock,
// then LongOngoingLock, matching the plan's heartbeat order), then History
// opens while Paused with a resume pending; the Unlock that follows revokes
// History and resumes normally instead of HEAD's silently-dropped Unlock.
void TimerTest::test_unlock_with_history_open_revokes_and_resumes_pending_autopause()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 15);
    writer.sync();
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Arrange: no dialog yet. Lock, then the matured LongOngoingLock -- the
    // timer backpauses into Pause with a resume pending.
    tracker.useTimerViaButton(Button::Start);
    const QDateTime lockTime(QDate(2025, 6, 1), QTime(9, 20, 0), Qt::LocalTime);
    clock.setNow(lockTime);
    tracker.useTimerViaLockEvent(LockEvent::Lock);
    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(9, 35, 0), Qt::LocalTime));
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, lockTime);
    QVERIFY(tracker.isPaused());
    QVERIFY(tracker.isAutopausePendingResume_dbg());

    // History opens in the lag window (unlocked in the OS, still Paused
    // here), then the debounced Unlock finally arrives.
    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());
    QSignalSpy revokedSpy(&tracker, &TimeTracker::exclusiveEditRevoked);
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    const QDateTime unlockTime(QDate(2025, 6, 1), QTime(9, 50, 0), Qt::LocalTime);
    clock.setNow(unlockTime);

    // Act
    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    // Assert: revoked, and back in Activity via LockResume -- HEAD stays
    // Paused with the Unlock silently dropped.
    QCOMPARE(revokedSpy.count(), 1);
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isActive());
    QCOMPARE(transitionedSpy.count(), 1);
    QCOMPARE(transitionedSpy.first().first().value<TimeTracker::Transition>().cause,
             TimeTracker::TransitionCause::LockResume);

    tracker.useTimerViaButton(Button::Stop);
}

// ============================================================================
// WP5: end-to-end check that a lock -> auto-pause -> unlock interval starting
// while History is open is recorded unchanged. The LongOngoingLock revokes
// History immediately (no replay any more), so the ordinary no-dialog code
// produces Activity[start->lock], Pause[lock->unlock], then a fresh Activity
// segment anchored at the true unlock time.
// ============================================================================

void TimerTest::test_lock_interval_with_history_open_is_recorded_at_true_timestamps()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 15);
    writer.sync();
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    tracker.beginExclusiveEdit();

    const QDateTime lockTime(QDate(2025, 6, 1), QTime(9, 20, 0), Qt::LocalTime);
    clock.setNow(lockTime);
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, lockTime);

    const QDateTime unlockTime(QDate(2025, 6, 1), QTime(10, 30, 0), Qt::LocalTime);
    clock.setNow(unlockTime);
    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    const QDateTime dialogCloseTime(QDate(2025, 6, 1), QTime(10, 35, 0), Qt::LocalTime);
    clock.setNow(dialogCloseTime);
    tracker.endExclusiveEdit();

    // Bind before iterating: completedTimeline() returns by value and
    // completed() returns a reference into it, so a range-for over the
    // chained call would bind to a freed temporary (see the dangling-
    // reference trap noted in P5b, test_timer.cpp / commit a58dfea).
    const auto completedNow = tracker.completedTimeline();
    const auto& d = completedNow.completed();
    bool foundActivity = false;
    bool foundPause = false;
    for (const auto& seg : d) {
        if (seg.type == DurationType::Activity && seg.startTime == t0 && seg.endTime == lockTime)
            foundActivity = true;
        if (seg.type == DurationType::Pause && seg.startTime == lockTime && seg.endTime == unlockTime)
            foundPause = true;
    }
    QVERIFY2(foundActivity, "Expected completed Activity segment 09:00->09:20");
    QVERIFY2(foundPause, "Expected completed Pause segment 09:20->10:30");

    // Engine ended in Activity, ongoing segment anchored at the true unlock
    // time (10:30), not at dialog-close time (10:35) — the post-unlock work
    // stays Activity.
    QVERIFY(tracker.isActive());
    const auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QCOMPARE(ongoing->startTime, unlockTime);

    tracker.useTimerViaButton(Button::Stop);
}

// ============================================================================
// Pause-merge removal: unpause creates new Pause segment with fresh id
// ============================================================================

void TimerTest::test_T1_unpause_creates_new_pause_segment_with_fresh_id()
{
    // Verify that resuming from Pause (Pause→Activity) creates a new Pause
    // segment whose segment_id is different from the preceding Activity
    // segment_id. The old bug reused the Activity segment_id for the Pause row,
    // corrupting the DB. Each segment must have its own unique id.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    const auto ongoingActivity = tracker.getOngoingDuration();
    QVERIFY(ongoingActivity.has_value());
    const SegmentId activitySegId = ongoingActivity->segment_id;

    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);
    const auto ongoingPause = tracker.getOngoingDuration();
    QVERIFY(ongoingPause.has_value());
    const SegmentId pauseSegId = ongoingPause->segment_id;
    const size_t dursBefore = tracker.completedTimeline().completed().size();

    // Act: resume from Pause
    tracker.useTimerViaButton(Button::Start);

    // Assert: a new Pause segment was appended (not merged into the Activity)
    QVERIFY(tracker.completedTimeline().completed().size() > dursBefore);
    // The Pause segment just recorded must have its own id — neither empty,
    // nor the same as the preceding Activity's id. Bind the Timeline to a
    // named local before taking a reference into it: completedTimeline()
    // returns by value and completed() returns a reference into that
    // temporary (see the dangling-reference trap noted in P5b, commit
    // a58dfea).
    const auto completedAfterResume = tracker.completedTimeline();
    const TimeDuration& pauseEntry = completedAfterResume.completed().back();
    QCOMPARE(pauseEntry.type, DurationType::Pause);
    // (pauseEntry.segment_id.isEmpty() is not asserted: TimeDuration's ctor
    // (types.h:150) mints a fresh id whenever the one passed in is empty, so
    // segment_id can never be empty on any TimeDuration, completed or
    // ongoing — the check could not fail.)
    QVERIFY2(pauseEntry.segment_id != activitySegId,
             "Pause segment must not reuse the Activity segment_id (T1 regression)");
    // The new Activity's segment_id must also be fresh
    const auto ongoingAfterResume = tracker.getOngoingDuration();
    QVERIFY(ongoingAfterResume.has_value());
    QVERIFY2(ongoingAfterResume->segment_id != pauseSegId,
             "New Activity segment must have a different id from the Pause segment");

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_T9_new_activity_segment_after_unpause_has_activity_type()
{
    // Verify that after resuming from Pause the engine is in Activity mode
    // and the new ongoing segment type is Activity (not Pause or anything else).
    // The old Pause-merge branch could cause type confusion.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: engine is in Activity mode
    QVERIFY(tracker.isActive());

    // The ongoing duration must be Activity type
    auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QCOMPARE(ongoing->type, DurationType::Activity);

    // All completed Pause entries in durations must have type Pause,
    // and all Activity entries must have type Activity — no type corruption.
    // Bind the Timeline to a named local before iterating: completedTimeline()
    // returns by value and completed() returns a reference into that
    // temporary (see the dangling-reference trap noted in P5b, commit
    // a58dfea).
    const auto completedNow = tracker.completedTimeline();
    for (const auto& d : completedNow.completed()) {
        QVERIFY(d.type == DurationType::Activity || d.type == DurationType::Pause);
        // (d.segment_id.isEmpty() is not asserted here: TimeDuration's ctor
        // (types.h:150) mints a fresh id whenever the one passed in is
        // empty, so segment_id can never be empty on any TimeDuration —
        // the check could not fail.)
    }

    tracker.useTimerViaButton(Button::Stop);
}

// ============================================================================
// Autopause flag cleared in startTimer
// ============================================================================

void TimerTest::test_T10_was_active_cleared_on_start_from_none()
{
    // Verify that startTimer from None always clears autopause_pending_resume_,
    // even when that flag was set before the call (e.g. by a previous lock event).

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Pre-set the flag to simulate a stale autopause state
    tracker.setAutopausePendingResume_dbg(true);

    // Act: start from None
    tracker.useTimerViaButton(Button::Start);

    // Assert: flag is cleared regardless of prior value
    QVERIFY2(!tracker.isAutopausePendingResume_dbg(),
             "autopause_pending_resume_ must be false after startTimer from None");

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_T10_was_active_cleared_on_start_from_pause()
{
    // Verify that startTimer from Pause always clears autopause_pending_resume_.

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Pause);

    // Simulate stale autopause flag (as if a LongOngoingLock set it before the pause)
    tracker.setAutopausePendingResume_dbg(true);

    // Act: resume from Pause
    tracker.useTimerViaButton(Button::Start);

    // Assert: flag is cleared
    QVERIFY2(!tracker.isAutopausePendingResume_dbg(),
             "autopause_pending_resume_ must be false after startTimer from Pause");

    tracker.useTimerViaButton(Button::Stop);
}

// ============================================================================
// Destructor ordering: crash-absence smoke test
// ============================================================================

void TimerTest::test_T8_destructor_does_not_crash_while_active()
{
    // Verify that destroying a TimeTracker while it is actively running (and its
    // checkpointTimer_ could in theory fire) does not crash or deadlock.
    //
    // The ordering fix (T8) stops and disconnects checkpointTimer_ before
    // acquiring the mutex in ~TimeTracker(). Without this fix, a queued slot
    // firing during destruction could deadlock on the recursive mutex or
    // access already-freed members.
    //
    // Direct unit-testing of destructor ordering is not feasible (we cannot
    // reliably force a slot to fire exactly during destruction without a
    // race condition in the test itself). This test instead serves as an
    // absence-of-crash guard: if the fix is missing and a slot fires in the
    // wrong window, this test may crash or deadlock during CI. The FakeSessionStore
    // is used so no real DB I/O is needed.

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;

    {
        TimeTracker tracker(settings, fakeDb);
        tracker.useTimerViaButton(Button::Start);
        QTest::qWait(10);
        // tracker destroyed here while in Activity mode with checkpointTimer_ running
    }

    // If we reach here without a crash or deadlock, T8 is correct.
    QVERIFY(true);
}

// ============================================================================
// beginExclusiveEdit / endExclusiveEdit + commitEditedTimeline checkpoint guard
// ============================================================================

// T2: commitEditedTimeline must NOT call saveCheckpoint while dialog_open_ is true.
void TimerTest::test_T2_commitEditedTimeline_skips_checkpoint_while_dialog_open()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Build a plausible ongoing segment for commitEditedTimeline to adopt.
    const QDateTime start = fixedNow().addSecs(-60);
    const QDateTime end   = fixedNow();
    TimeDuration ongoing = TimeDuration::fromLiveSession(DurationType::Activity, start, end, SegmentId::fromString("seg-001"));

    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());

    fakeDb.callLog.clear();

    // commitEditedTimeline with a valid ongoing segment — checkpoint must be suppressed.
    tracker.commitEditedTimeline(Timeline({}, ongoing));

    QCOMPARE(fakeDb.callLog.count("saveCheckpoint"), 0);

    tracker.endExclusiveEdit();
}

// C6: after endExclusiveEdit(), a subsequent commitEditedTimeline resumes normal writes.
void TimerTest::test_C6_commitEditedTimeline_writes_checkpoint_after_endExclusiveEdit()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    const QDateTime start = fixedNow().addSecs(-60);
    const QDateTime end   = fixedNow();
    TimeDuration ongoing = TimeDuration::fromLiveSession(DurationType::Activity, start, end, SegmentId::fromString("seg-002"));

    tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();
    QVERIFY(!tracker.isDialogOpen_dbg());

    fakeDb.callLog.clear();

    // After endExclusiveEdit, dialog_open_ is false — checkpoint write must proceed.
    tracker.commitEditedTimeline(Timeline({}, ongoing));

    QCOMPARE(fakeDb.callLog.count("saveCheckpoint"), 1);
}

void TimerTest::test_S12_marker_error_skips_reconciliation()
{
    // When recoverStartupCheckpoints() returns a TransientError/FatalError
    // status (e.g. marker read error), TimeTracker must treat recovered_msec
    // as 0 and must NOT show a notification. The store is responsible for
    // skipping reconciliation on error; TimeTracker only acts on the domain
    // facts returned when the status is Success/Disabled (okOrDisabled()).

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Simulate store-level failure (marker error or other critical failure).
    fakeDb.startupRecoveryResult = StartupRecoveryResult::failure(SessionStoreResult::TransientError, "marker read failed");

    TimeTracker tracker(settings, fakeDb);
    tracker.initializeFromStore();

    // No milliseconds recovered; notification suppressed.
    QCOMPARE(tracker.getStartupRecoveredMsec(), (qint64)0);
    QVERIFY(!tracker.shouldShowStartupRecoveryNotification());
    QVERIFY(fakeDb.callLog.contains("recoverStartupCheckpoints"));
}

// QF1 (pre-D10): pinned that an explicit user pause is distinguishable from
// lock-driven autopause even though both used to land in different signals
// (paused() vs modeChanged()). D10 replaced that two-signal protocol with one
// cause-carrying transitioned() event; the same distinction is now pinned via
// t.cause rather than "which signal fired".
void TimerTest::test_QF1_explicit_pause_is_ButtonPause_cause()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);
    tracker.useTimerViaButton(Button::Start);

    QSignalSpy spy(&tracker, &TimeTracker::transitioned);

    tracker.useTimerViaButton(Button::Pause);

    QCOMPARE(spy.count(), 1);
    const auto t = spy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.to, TimeTracker::Mode::Pause);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::ButtonPause);
}

void TimerTest::test_QF1_explicit_resume_is_ButtonStart_cause()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    tracker.useTimerViaButton(Button::Pause);

    QSignalSpy spy(&tracker, &TimeTracker::transitioned);

    tracker.useTimerViaButton(Button::Start);

    QCOMPARE(spy.count(), 1);
    const auto t = spy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Pause);
    QCOMPARE(t.to, TimeTracker::Mode::Activity);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::ButtonStart);
}

void TimerTest::test_QF1_lock_autopause_is_LockAutopause_cause()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);

    QSignalSpy spy(&tracker, &TimeTracker::transitioned);

    // C12b: LongOngoingLock requires a captured lock instant.
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, QDateTime::currentDateTime());

    QCOMPARE(spy.count(), 1);
    const auto t = spy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.to, TimeTracker::Mode::Pause);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::LockAutopause);
}

void TimerTest::test_QF1_lock_resume_is_LockResume_cause_emitted_once()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    // C12b: LongOngoingLock requires a captured lock instant.
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, QDateTime::currentDateTime());
    QVERIFY(tracker.isPaused());

    QSignalSpy spy(&tracker, &TimeTracker::transitioned);

    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    // Exactly one emission for one real transition (R10-002's original
    // point: lock resume used to require two signals from two call sites).
    QCOMPARE(spy.count(), 1);
    const auto t = spy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Pause);
    QCOMPARE(t.to, TimeTracker::Mode::Activity);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::LockResume);
}

// Issue #1: committing an ongoing segment with a different type must align mode_.
void TimerTest::test_commit_ongoing_type_edit_changes_mode()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // 1. Start in Activity mode.
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    QVERIFY(tracker.isActive());

    // Capture the live ongoing segment's id and start time so the committed
    // segment is a plausible replacement (same identity, different type).
    auto liveOngoing = tracker.getOngoingDuration();
    QVERIFY(liveOngoing.has_value());
    QCOMPARE(liveOngoing->type, DurationType::Activity);

    // 2. Build an ongoing TimeDuration with the same id/start but type = Pause,
    //    simulating what HistoryDialog would commit after the user flips the checkbox.
    const QDateTime start = liveOngoing->startTime;
    const QDateTime end   = QDateTime::currentDateTime();
    TimeDuration editedOngoing = TimeDuration::fromLiveSession(
        DurationType::Pause, start, end, liveOngoing->segment_id);

    tracker.commitEditedTimeline(Timeline({}, editedOngoing));

    // 3. mode_ must be Pause after the commit.
    QVERIFY(tracker.isPaused());

    // getOngoingDuration() derives type from mode_, so it should now return Pause.
    auto postCommit = tracker.getOngoingDuration();
    QVERIFY(postCommit.has_value());
    QCOMPARE(postCommit->type, DurationType::Pause);

    // 4. Stop the timer and confirm the persisted row is Pause, not Activity.
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());

    // The completed segment written to fakeDb on stop should be a Pause row.
    bool foundPause = false;
    for (const auto& d : fakeDb.storedDurations) {
        if (d.segment_id == liveOngoing->segment_id && d.type == DurationType::Pause)
            foundPause = true;
    }
    QVERIFY2(foundPause, "persisted row should be Pause after type-edit commit");
}

// commitEditedTimeline returns an EditCommitResult and aligns mode_
// with the type of the edited ongoing segment.
void TimerTest::test_commitEditedTimeline_mode_aligns_with_edited_ongoing_type()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Start in Activity mode.
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    QVERIFY(tracker.isActive());

    // Build a Pause-typed ongoing segment with the same segment id/start as the
    // live Activity segment, simulating a type flip in HistoryDialog.
    auto liveOngoing = tracker.getOngoingDuration();
    QVERIFY(liveOngoing.has_value());
    TimeDuration editedOngoing = TimeDuration::fromLiveSession(
        DurationType::Pause,
        liveOngoing->startTime,
        QDateTime::currentDateTime(),
        liveOngoing->segment_id);

    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    // commitEditedTimeline must return success and switch mode_ to Pause.
    const auto result = tracker.commitEditedTimeline(Timeline({}, editedOngoing));
    QVERIFY(result.ok);
    QVERIFY(tracker.isPaused());

    // getOngoingDuration() derives type from mode_, so it should now be Pause.
    auto postCommit = tracker.getOngoingDuration();
    QVERIFY(postCommit.has_value());
    QCOMPARE(postCommit->type, DurationType::Pause);

    // D10: exactly one transitioned(Activity->Pause, EditApplied) event --
    // a History edit flipping the ongoing type must not leave the UI stale.
    QCOMPARE(transitionedSpy.count(), 1);
    const auto t = transitionedSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Activity);
    QCOMPARE(t.to, TimeTracker::Mode::Pause);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::EditApplied);
}

// D10: the mirror direction -- an edited ongoing segment flipped from Pause
// to Activity must align mode_ AND emit exactly one transitioned(Pause->
// Activity, EditApplied), not silently leave the button reading CONTINUE.
void TimerTest::test_D10_editApplied_pause_to_activity_emits_transitioned()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());

    auto liveOngoing = tracker.getOngoingDuration();
    QVERIFY(liveOngoing.has_value());
    QCOMPARE(liveOngoing->type, DurationType::Pause);
    TimeDuration editedOngoing = TimeDuration::fromLiveSession(
        DurationType::Activity,
        liveOngoing->startTime,
        QDateTime::currentDateTime(),
        liveOngoing->segment_id);

    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    const auto result = tracker.commitEditedTimeline(Timeline({}, editedOngoing));

    QVERIFY(result.ok);
    QVERIFY(tracker.isActive());
    QCOMPARE(transitionedSpy.count(), 1);
    const auto t = transitionedSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Pause);
    QCOMPARE(t.to, TimeTracker::Mode::Activity);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::EditApplied);
}

// M7: guard against re-arming timer during exclusive edit
void TimerTest::test_M7_edit_to_activity_during_exclusive_edit_does_not_arm_timer()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());

    // Begin exclusive edit (simulates HistoryDialog open)
    tracker.beginExclusiveEdit();

    auto liveOngoing = tracker.getOngoingDuration();
    QVERIFY(liveOngoing.has_value());
    QCOMPARE(liveOngoing->type, DurationType::Pause);
    TimeDuration editedOngoing = TimeDuration::fromLiveSession(
        DurationType::Activity,
        liveOngoing->startTime,
        QDateTime::currentDateTime(),
        liveOngoing->segment_id);

    const auto result = tracker.commitEditedTimeline(Timeline({}, editedOngoing));

    QVERIFY(result.ok);
    QVERIFY(tracker.isActive());
    QVERIFY2(!tracker.isCheckpointTimerActive_dbg(), "commitEditedTimeline must not re-arm checkpoints while an exclusive edit is in progress");

    // Resume exclusive edit
    tracker.endExclusiveEdit();
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.useTimerViaButton(Button::Stop);
}

// Disabled history: the store returns Disabled (a no-op) for commitSession.
// Disabled must NOT be treated as a save failure: stopping must not retain
// bogus unsaved data, must not bump the retry-failure counter, and must not
// emit a user warning.
void TimerTest::test_disabled_history_stop_keeps_clean_state_no_warning()
{
    // Arrange: real store with history disabled (commitSession returns Disabled).
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 0));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);
    QSignalSpy warnSpy(&tracker, &TimeTracker::userWarning);

    // Act: run and stop a session that produces a completed Activity segment.
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Stop);

    // Assert: Disabled was folded into success, so no failure side effects.
    QCOMPARE(warnSpy.count(), 0);
    QCOMPARE(tracker.hasPendingCommit_dbg(), false);
}

// commitEditedTimeline re-anchors a checkpoint for an ongoing Activity segment.
// With history disabled, saveCheckpoint returns Disabled, which must be
// reported as success, not EditCommitResult::failure.
void TimerTest::test_disabled_history_commitEditedTimeline_succeeds()
{
    // Arrange: real store with history disabled; default checkpoint interval is
    // non-zero so the checkpoint path inside commitEditedTimeline is exercised.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 0));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    QVERIFY(tracker.isActive());

    // Keep the ongoing segment Activity so maybeReanchorCheckpoint runs saveCheckpoint.
    auto liveOngoing = tracker.getOngoingDuration();
    QVERIFY(liveOngoing.has_value());
    TimeDuration editedOngoing = TimeDuration::fromLiveSession(
        DurationType::Activity,
        liveOngoing->startTime,
        QDateTime::currentDateTime(),
        liveOngoing->segment_id);

    // Act + Assert: disabled checkpoint write is a no-op, so commit succeeds.
    const auto result = tracker.commitEditedTimeline(Timeline({}, editedOngoing));
    QVERIFY(result.ok);

    tracker.useTimerViaButton(Button::Stop);
}

// Item B: commitEditedTimeline must clear the segment anchor and stop the engine
// when the edited timeline has no ongoing segment and mode_ != None.
void TimerTest::test_commitEditedTimeline_clears_anchor_when_no_ongoing()
{
    // Arrange: timer running in Activity mode with a valid segment anchor.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    QVERIFY(tracker.isActive());
    // (!segment_id.isEmpty() is not asserted while running: beginNewSegment()
    // unconditionally mints a fresh id (timetracker.cpp:233), so the check
    // could not fail once isActive() is established above.)
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    // Build an edited timeline with completed segments but NO ongoing segment.
    QDateTime now = fixedNow();
    std::deque<TimeDuration> completed;
    auto seg = TimeDuration::create(DurationType::Activity, now.addSecs(-60), now);
    QVERIFY(seg.has_value());
    completed.push_back(seg.value());
    Timeline editedTimeline(completed, std::nullopt);

    // Set up spies: the clear path must emit exactly one transition, to None.
    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    // Act: commit a timeline with no ongoing segment while mode_ != None.
    const auto commitResult = tracker.commitEditedTimeline(editedTimeline);

    // Assert: commit succeeded.
    QVERIFY(commitResult.ok);

    // (segment_id.isEmpty() / !segment_start_time.isValid() are not asserted
    // here: this commit takes the "no ongoing, mode_ != None" branch
    // (timetracker.cpp:1200-1207), which calls session_.clearSegment()
    // (:1204) straight-line before assigning mode_ = Mode::None (:1206) --
    // one of exactly two sites in production code that ever assigns
    // Mode::None, each paired the same way (the other is stopTimer(),
    // timetracker.cpp:853-854). So isStopped() below already implies both
    // checks; they could not fail.)

    // Mode is None.
    QVERIFY2(tracker.isStopped(),
             "mode_ must be None after commit with no ongoing segment");

    // Checkpoint timer is not running.
    QVERIFY2(!tracker.isCheckpointTimerActive_dbg(),
             "checkpoint timer must be stopped after commit with no ongoing segment");

    // getOngoingDuration() returns nullopt.
    QVERIFY2(!tracker.getOngoingDuration().has_value(),
             "getOngoingDuration() must return nullopt after anchor is cleared");

    // stopped(EditApplied) was emitted exactly once; transitioned() confirms
    // it was the ONE real transition for this commit (no other mode change).
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::EditApplied);
    QCOMPARE(transitionedSpy.count(), 1);
    QCOMPARE(transitionedSpy.first().first().value<TimeTracker::Transition>().to,
             TimeTracker::Mode::None);
}

// Item B: same clear-on-no-ongoing behavior when the engine is in Pause mode.
void TimerTest::test_commitEditedTimeline_clears_anchor_when_no_ongoing_from_pause()
{
    // Arrange: timer running, then paused, so mode_ == Pause with a valid anchor.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());
    // (!segment_id.isEmpty() is not asserted while running: beginNewSegment()
    // unconditionally mints a fresh id (timetracker.cpp:233), so the check
    // could not fail once isPaused() is established above.)

    // Build an edited timeline with completed segments but NO ongoing segment.
    QDateTime now = fixedNow();
    std::deque<TimeDuration> completed;
    auto seg = TimeDuration::create(DurationType::Activity, now.addSecs(-60), now);
    QVERIFY(seg.has_value());
    completed.push_back(seg.value());
    Timeline editedTimeline(completed, std::nullopt);

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    // Act: commit a timeline with no ongoing segment while mode_ == Pause.
    const auto commitResult = tracker.commitEditedTimeline(editedTimeline);

    // Assert: commit succeeded and the anchor is fully cleared.
    QVERIFY(commitResult.ok);
    // (segment_id.isEmpty() / !segment_start_time.isValid() are not asserted
    // here: this commit takes the "no ongoing, mode_ != None" branch
    // (timetracker.cpp:1200-1207), which calls session_.clearSegment()
    // (:1204) straight-line before assigning mode_ = Mode::None (:1206) --
    // one of exactly two sites in production code that ever assigns
    // Mode::None, each paired the same way (the other is stopTimer(),
    // timetracker.cpp:853-854). So isStopped() below already implies both
    // checks; they could not fail.)
    QVERIFY2(tracker.isStopped(),
             "mode_ must be None after commit with no ongoing segment");
    QVERIFY2(!tracker.isCheckpointTimerActive_dbg(),
             "checkpoint timer must be stopped after commit with no ongoing segment");
    QVERIFY2(!tracker.getOngoingDuration().has_value(),
             "getOngoingDuration() must return nullopt after anchor is cleared");

    // stopped(EditApplied) was emitted exactly once; transitioned() confirms
    // it was the ONE real transition, correctly from Pause to None.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::EditApplied);
    QCOMPARE(transitionedSpy.count(), 1);
    const auto t = transitionedSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Pause);
    QCOMPARE(t.to, TimeTracker::Mode::None);
}

// A14: commitEditedTimeline must not half-adopt an ongoing segment while the
// engine is already stopped (mode_ == None). Adopting segment_id/start time
// without also flipping mode_ would leave a state SessionState documents as
// impossible (segment_id "Empty when the timer is stopped"). The fix refuses
// the ongoing entirely: engine stays stopped, anchor stays clear, no signal.
void TimerTest::test_commitEditedTimeline_ignores_ongoing_when_already_stopped()
{
    // Arrange: engine never started -- mode_ == None, no segment anchor.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime start(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(start);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.getOngoingDuration().has_value());

    // Build an edited timeline with an ongoing segment (as if HistoryDialog
    // still held one from before the engine stopped), but mode_ is None now.
    TimeDuration editedOngoing = TimeDuration::fromLiveSession(
        DurationType::Activity, start, start.addSecs(600));

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    // Act: commit while mode_ == None.
    const auto result = tracker.commitEditedTimeline(Timeline({}, editedOngoing));

    // Assert: commit still reports success; the ongoing segment is ignored.
    QVERIFY(result.ok);
    QVERIFY2(tracker.isStopped(), "engine must remain stopped, not half-started");
    // This test is the one documented exception the seam-removal series does
    // not convert: no public observable exists for the A14 impossible state.
    // checkDurationInvariants()'s invariant 4 (timetracker.cpp:~303) asserts
    // centrally that mode_ == Mode::None implies segment_id/segment_start_time
    // are clear, and it is unconditionally called at the end of
    // commitEditedTimeline() (timetracker.cpp:~1224) regardless of which
    // branch ran -- but every production reader of the anchor (isStopped(),
    // getOngoingDuration()) gates on mode_ first, so neither can distinguish
    // a correctly-refused half-adopt from a corrupted one, and the
    // invariant's only report channel is a qWarning that is NOT enforced on
    // Windows (qtest/main.cpp's message handler gates QWARN-as-failure inside
    // #ifndef _WIN32). If the "already stopped" branch (timetracker.cpp:
    // ~1206-1212) were corrupted to call adoptOngoingSegment() instead of
    // refusing, segment_id/segment_start_time would become non-empty while
    // mode_ stayed None -- exactly the "impossible state" A14 guards against
    // -- and nothing but these two direct reads would catch it on MSVC.
    QVERIFY2(tracker.sessionState_dbg().segment_id.isEmpty(),
             "segment_id must stay empty -- refused, not half-adopted");
    QVERIFY2(!tracker.sessionState_dbg().segment_start_time.isValid(),
             "segment_start_time must stay invalid -- refused, not half-adopted");
    QVERIFY2(!tracker.getOngoingDuration().has_value(),
             "getOngoingDuration() must return nullopt: mode_ == None");

    // No mode-transition signal fires at all: the engine state did not change.
    QCOMPARE(stoppedSpy.count(), 0);
    QCOMPARE(transitionedSpy.count(), 0);
}

// Phase 2 clock-seam enabler: prove the injected now-provider fully controls the
// engine's clock. Start the engine at a fixed fake instant, advance the fake
// clock by 90 minutes with NO real sleep, and assert the ongoing duration is
// computed from the fake clock (start/end/duration exact to the millisecond).
void TimerTest::test_seam_fake_clock_drives_ongoing_duration()
{
    // Arrange: inject a controllable clock.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore db;

    const QDateTime start(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(start);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Act: start the engine (segment anchored at the fake now), then jump ahead.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    const auto ongoingAtStart = tracker.getOngoingDuration();
    QVERIFY(ongoingAtStart.has_value());
    QCOMPARE(ongoingAtStart->startTime, start);

    const qint64 ninetyMinutesMs = 90ll * 60ll * 1000ll;
    clock.advance(ninetyMinutesMs);

    // Assert: the ongoing segment reflects the advanced fake clock — no sleeping.
    const auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QCOMPARE(ongoing->startTime, start);
    QCOMPARE(ongoing->endTime, start.addMSecs(ninetyMinutesMs));
    QCOMPARE(ongoing->duration(), ninetyMinutesMs);
}

void TimerTest::test_A2_backpause_anchors_at_lock_time_across_suspend()
{
    // Arrange: autopause on, 15 min threshold, fake clock.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 15); // 900s backpause
    writer.sync();
    Settings settings(settingsPath);
    FakeSessionStore db;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Act: Activity from 09:00; desktop locks at 10:00.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    const QDateTime lockInstant(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::LocalTime);
    clock.setNow(lockInstant);
    tracker.useTimerViaLockEvent(LockEvent::Lock, lockInstant);

    // Machine suspends; LongOngoingLock only surfaces at 14:00 after resume,
    // carrying the real lock instant (10:00).
    const QDateTime resumeInstant(QDate(2025, 6, 1), QTime(14, 0, 0), Qt::LocalTime);
    clock.setNow(resumeInstant);
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, lockInstant);

    // Assert: Activity is exactly 09:00→10:00 (1h) and Pause 10:00→14:00 (4h);
    // none of the suspended interval leaks into Activity.
    QVERIFY(tracker.isPaused());
    // Bind the Timeline to a named local before taking a reference into it:
    // completedTimeline() returns by value and completed() returns a
    // reference into that temporary (see the dangling-reference trap noted
    // in P5b, commit a58dfea).
    const auto completedNow = tracker.completedTimeline();
    const auto& d = completedNow.completed();
    const qint64 activeMs = sumDurations(d, DurationType::Activity);
    const qint64 pauseMs  = sumDurations(d, DurationType::Pause);
    QCOMPARE(activeMs, 60ll * 60ll * 1000ll);       // 1 hour
    QCOMPARE(pauseMs,  4ll * 60ll * 60ll * 1000ll);  // 4 hours

    // The Pause segment must start at the lock instant, not now−backpause.
    bool foundPauseAtLock = false;
    for (const auto& seg : d)
        if (seg.type == DurationType::Pause && seg.startTime == lockInstant)
            foundPauseAtLock = true;
    QVERIFY(foundPauseAtLock);
}

void TimerTest::test_A2_display_total_matches_wallclock_across_suspend()
{
    // Arrange: fake clock; the monotonic timer_ will not advance with it.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore db;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Act: start Activity, then jump the wall clock two hours (simulated suspend).
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    const qint64 twoHoursMs = 2ll * 60ll * 60ll * 1000ll;
    clock.advance(twoHoursMs);

    // Assert: the displayed Active total tracks wall-clock elapsed, matching what
    // persistence would compute from segment_start_time — not the frozen timer_.
    QCOMPARE(tracker.getActiveTime(), twoHoursMs);
    QCOMPARE(tracker.getPauseTime(), 0ll);
}

// Counts unfinalized (is_finalized = 0) checkpoint rows in the store's DB.
static int countUnfinalizedRows(SqliteSessionStore& db)
{
    db.ensureOpen_dbg();
    QSqlQuery q(db.rawDb_dbg());
    q.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0");
    q.next();
    return q.value(0).toInt();
}

void TimerTest::test_A3_watchdog_discard_deletes_checkpoint_row()
{
    // Arrange: real store so we can inspect checkpoint rows, plus a fake clock.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Start Activity and write a checkpoint before midnight.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(5ll * 60ll * 1000ll); // 23:55
    tracker.saveCheckpoint_locked_dbg(clock.now());
    QCOMPARE(countUnfinalizedRows(db), 1);

    // Act: cross midnight — the watchdog discards the in-flight segment.
    const QDateTime postMidnight(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime);
    clock.setNow(postMidnight);
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop_dbg(postMidnight));
    QVERIFY(tracker.isStopped());

    // Assert: the orphaned checkpoint row is gone — nothing for recovery to
    // resurrect and nothing for the stale policy to silently drop.
    QCOMPARE(countUnfinalizedRows(db), 0);
}

void TimerTest::test_C9b_start_refused_after_shutdown_until_cancel()
{
    // Arrange: start, stop, then shutdown() -- shutdown_called_ set.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());

    tracker.shutdown();
    QVERIFY(tracker.wasShutDown());

    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);
    const QStringList callLogBefore = fakeDb.callLog;

    // Act: both Start entry points must be refused while shutdown_called_ is set.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.wasShutDown());
    QCOMPARE(transitionedSpy.count(), 0);
    QCOMPARE(fakeDb.callLog, callLogBefore);

    tracker.onStartPausePressed();
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.wasShutDown());
    QCOMPARE(transitionedSpy.count(), 0);
    QCOMPARE(fakeDb.callLog, callLogBefore);

    // Act: cancelShutdown() re-arms the tracker -- Start now works.
    tracker.cancelShutdown();
    tracker.useTimerViaButton(Button::Start);

    // Assert
    QVERIFY(tracker.isActive());
    QCOMPARE(transitionedSpy.count(), 1);
}

void TimerTest::test_C9b_start_refused_does_not_retry_pending_commit()
{
    // Arrange: a Stop-time commit failure leaves a pending payload, then
    // shutdown() marks the tracker terminal without resolving it -- a
    // refused Start must not touch that pending payload (mirrors D5's
    // cancelShutdown pending-commit test, but for a refused Start).
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.hasPendingCommit_dbg());
    tracker.shutdown();
    QVERIFY(tracker.wasShutDown());
    const int commitAttemptsBeforeStart = fakeDb.callLog.count("commitSession");

    // Act
    tracker.useTimerViaButton(Button::Start);

    // Assert: refused Start left the pending commit exactly as it was.
    QVERIFY(tracker.hasPendingCommit_dbg());
    QCOMPARE(fakeDb.callLog.count("commitSession"), commitAttemptsBeforeStart);
    QVERIFY(tracker.isStopped());

    // Cleanup: resolve the payload so destruction finds no unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

void TimerTest::test_C9b_lock_resume_refused_after_shutdown()
{
    // Arrange: autopause enabled, running, shutdown() called, then a
    // pending lock-resume arrives -- must be refused just like button Start.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    tracker.shutdown();
    QVERIFY(tracker.wasShutDown());
    QVERIFY(tracker.isStopped());

    tracker.setAutopausePendingResume_dbg(true);
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);
    const QStringList callLogBefore = fakeDb.callLog;

    // Act
    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    // Assert
    QVERIFY(tracker.isStopped());
    QCOMPARE(transitionedSpy.count(), 0);
    QCOMPARE(fakeDb.callLog, callLogBefore);
}

// F2: shutdown() after a machine slept past midnight must discard the
// stale checkpoint of the ongoing (cross-midnight, discarded) segment —
// otherwise recoverStartupCheckpoints() would resurrect it on next launch.
void TimerTest::test_F2_shutdown_after_midnight_discards_checkpoint()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(60ll * 1000ll); // elapse a minute so the checkpoint isn't zero-length
    tracker.triggerCheckpoint_dbg();
    QVERIFY(fakeDb.callLog.contains("saveCheckpoint"));
    const auto ongoingAtCheckpoint = tracker.getOngoingDuration();
    QVERIFY(ongoingAtCheckpoint.has_value());
    const SegmentId checkpointSegmentId = ongoingAtCheckpoint->segment_id;

    // Act: machine "wakes" well past midnight; shutdown() stops the engine.
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime));
    tracker.shutdown();

    // Assert: the orphaned checkpoint row was discarded.
    QVERIFY(fakeDb.callLog.contains("discardCheckpoint"));
    const bool stillPresent = std::any_of(fakeDb.savedCheckpoints.begin(), fakeDb.savedCheckpoints.end(),
        [&](const FakeSessionStore::CheckpointRecord& cp) { return cp.segmentId == checkpointSegmentId; });
    QVERIFY(!stillPresent);
}

// ============================================================================
// WP2 (midnight_close_plan.md): the engine revokes an open History edit at
// the midnight boundary, then runs its normal no-dialog code. The notice
// text mirrors kHistoryRevokedNotice in timetracker.cpp.
// ============================================================================

namespace {
const QString kWP2RevokedNotice =
    "History was closed (screen lock, sleep, midnight or shutdown); any unsaved changes were discarded.";
}

// N1: a scheduled stop revokes an open edit and credits Activity through the
// captured cutoff, exactly as it would with no dialog.
void TimerTest::test_N1_scheduled_stop_revokes_edit_and_credits_activity_through_cutoff()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(60ll * 1000ll); // 23:51, so the checkpoint isn't zero-length
    tracker.triggerCheckpoint_dbg();
    QVERIFY(fakeDb.callLog.contains("saveCheckpoint"));

    tracker.beginExclusiveEdit();
    const QDateTime cutoff(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(cutoff);

    QSignalSpy revokedSpy(&tracker, &TimeTracker::exclusiveEditRevoked);
    QSignalSpy warnSpy(&tracker, &TimeTracker::userWarning);
    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    fakeDb.callLog.clear();

    // Act
    tracker.onTick(clock.now());

    // Assert
    QCOMPARE(revokedSpy.count(), 1);
    QVERIFY(!tracker.isDialogOpen_dbg());
    QCOMPARE(warnSpy.count(), 1);
    QCOMPARE(warnSpy.first().first().toString(), kWP2RevokedNotice);
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightScheduled);
    QVERIFY(fakeDb.callLog.contains("commitSession"));
    QVERIFY(!fakeDb.callLog.contains("discardCheckpoint"));
    QCOMPARE(fakeDb.storedDurations.size(), size_t(1));
    const auto& seg = fakeDb.storedDurations.front();
    QCOMPARE(seg.type, DurationType::Activity);
    QCOMPARE(seg.startTime, t0);
    QCOMPARE(seg.endTime, cutoff);
}

// N2 (*HEAD-red*): the Pause variant of N1.
void TimerTest::test_N2_scheduled_stop_revokes_edit_and_credits_pause_through_cutoff()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(5ll * 60ll * 1000ll); // 23:55
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());
    const QDateTime pauseStart = clock.now();
    fakeDb.storedDurations.clear(); // ignore the crash-safety commit finalizePauseTransition just made

    tracker.beginExclusiveEdit();
    const QDateTime cutoff(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(cutoff);
    fakeDb.callLog.clear();

    // Act
    tracker.onTick(clock.now());

    // Assert
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isStopped());
    QVERIFY(!fakeDb.callLog.contains("discardCheckpoint"));
    bool foundActivity = false, foundPause = false;
    for (const auto& seg : fakeDb.storedDurations) {
        if (seg.type == DurationType::Activity && seg.startTime == t0 && seg.endTime == pauseStart)
            foundActivity = true;
        if (seg.type == DurationType::Pause && seg.startTime == pauseStart && seg.endTime == cutoff)
            foundPause = true;
    }
    QVERIFY2(foundActivity, "Expected the pre-pause Activity segment to survive");
    QVERIFY2(foundPause, "Expected the Pause segment credited exactly through the cutoff");
}

// N3 (*HEAD-red*): a watchdog tick at 00:10 with the dialog open revokes the
// edit and discards the whole cross-midnight segment, exactly as the
// non-deferred watchdog policy already does.
void TimerTest::test_N3_watchdog_tick_revokes_edit_and_discards_segment()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    tracker.triggerCheckpoint_dbg();
    QVERIFY(fakeDb.callLog.contains("saveCheckpoint"));

    tracker.beginExclusiveEdit();
    const QDateTime postMidnight(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime);
    clock.setNow(postMidnight);
    fakeDb.callLog.clear();

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    // Act: the real 100ms heartbeat slot.
    tracker.onTick(postMidnight);

    // Assert
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isStopped());
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightWatchdog);
    QVERIFY(fakeDb.callLog.contains("discardCheckpoint"));
    QVERIFY(!fakeDb.callLog.contains("commitSession"));
    QVERIFY(fakeDb.storedDurations.empty());
}

// N4 (*HEAD-red*): a Lock event at 00:05 with the dialog open revokes the
// edit and stops via the same cross-midnight discard.
void TimerTest::test_N4_lock_event_revokes_edit_and_stops()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    tracker.beginExclusiveEdit();
    const QDateTime postMidnight(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime);
    clock.setNow(postMidnight);

    // Act
    tracker.useTimerViaLockEvent(LockEvent::Lock);

    // Assert
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isStopped());
    QVERIFY(fakeDb.storedDurations.empty());
}

// N5 (*HEAD-red*): a LongOngoingLock/Unlock interval that happens while the
// dialog is open is still recorded normally by the time the scheduled stop
// later revokes the edit. This test must stay unchanged through WP5 (when the
// lock events themselves start revoking immediately, no deferral involved).
void TimerTest::test_N5_scheduled_stop_after_autopause_revokes_and_credits_all_segments()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 15);
    writer.sync();
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    tracker.beginExclusiveEdit();

    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    const QDateTime lockTime(QDate(2025, 6, 1), QTime(23, 20, 0), Qt::LocalTime);
    clock.setNow(lockTime);
    tracker.useTimerViaLockEvent(LockEvent::Lock);
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, lockTime);

    const QDateTime unlockTime(QDate(2025, 6, 1), QTime(23, 40, 0), Qt::LocalTime);
    clock.setNow(unlockTime);
    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    const QDateTime cutoff(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(cutoff);

    // Act: WP5 means the Lock above already revoked History and ran the
    // lock/autopause bookkeeping as ordinary no-dialog code, so this fire is
    // just the scheduled stop -- no deferred correction is involved any more.
    tracker.onTick(clock.now());

    // Assert: rows
    QVERIFY(tracker.isStopped());
    bool foundPreLockActivity = false, foundPause = false, foundPostUnlockActivity = false;
    for (const auto& seg : fakeDb.storedDurations) {
        if (seg.type == DurationType::Activity && seg.startTime == t0 && seg.endTime == lockTime)
            foundPreLockActivity = true;
        if (seg.type == DurationType::Pause && seg.startTime == lockTime && seg.endTime == unlockTime)
            foundPause = true;
        if (seg.type == DurationType::Activity && seg.startTime == unlockTime && seg.endTime == cutoff)
            foundPostUnlockActivity = true;
    }
    QVERIFY2(foundPreLockActivity, "Expected completed Activity segment 23:00->23:20");
    QVERIFY2(foundPause, "Expected completed Pause segment 23:20->23:40");
    QVERIFY2(foundPostUnlockActivity, "Expected Activity segment 23:40->cutoff");

    // Assert: transitioned causes in order.
    QCOMPARE(transitionedSpy.count(), 3);
    QCOMPARE(transitionedSpy.at(0).first().value<TimeTracker::Transition>().cause,
             TimeTracker::TransitionCause::LockAutopause);
    QCOMPARE(transitionedSpy.at(1).first().value<TimeTracker::Transition>().cause,
             TimeTracker::TransitionCause::LockResume);
    QCOMPARE(transitionedSpy.at(2).first().value<TimeTracker::Transition>().cause,
             TimeTracker::TransitionCause::MidnightScheduled);
}

// N5b (*HEAD-red*): History stays open while still locked (no Unlock) right
// up to the scheduled stop. The LongOngoingLock is still "still locked" at
// that point, so the scheduled stop finalizes a Pause segment through the cutoff.
void TimerTest::test_N5b_scheduled_stop_while_still_locked_revokes_and_stops_paused()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 15);
    writer.sync();
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    tracker.beginExclusiveEdit();

    const QDateTime lockTime(QDate(2025, 6, 1), QTime(23, 20, 0), Qt::LocalTime);
    clock.setNow(lockTime);
    tracker.useTimerViaLockEvent(LockEvent::Lock);

    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(23, 35, 0), Qt::LocalTime));
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, lockTime);

    // No Unlock before the scheduled stop.
    const QDateTime cutoff(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(cutoff);

    // Act
    tracker.onTick(clock.now());

    // Assert
    QVERIFY(tracker.isStopped());
    bool foundActivity = false, foundPause = false;
    for (const auto& seg : fakeDb.storedDurations) {
        if (seg.type == DurationType::Activity && seg.startTime == t0 && seg.endTime == lockTime)
            foundActivity = true;
        if (seg.type == DurationType::Pause && seg.startTime == lockTime && seg.endTime == cutoff)
            foundPause = true;
    }
    QVERIFY2(foundActivity, "Expected completed Activity segment 23:00->23:20");
    QVERIFY2(foundPause, "Expected the Pause interval to cover [23:20, cutoff] contiguously");

    // A later Unlock does not restart the (now stopped) timer.
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime));
    tracker.useTimerViaLockEvent(LockEvent::Unlock);
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.isAutopausePendingResume_dbg());
}

// N6 (*HEAD-red*; the Medium; tick-first): a watchdog tick fires before an
// Unlock arrives. WP5 means the Lock itself already revoked History (no
// deferral): the Lock checkpoint and the LongOngoingLock backpause run as
// ordinary no-dialog code, producing completed Activity and Pause segments.
// The overnight tick then discards only the still-ongoing Pause segment that
// crosses midnight, exactly as the no-dialog watchdog would. HEAD instead
// resumes Activity at 08:00.
void TimerTest::test_N6_watchdog_tick_before_unlock_revokes_and_discards_whole_segment()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 15);
    writer.sync();
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(21, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    tracker.beginExclusiveEdit();

    const QDateTime lockTime(QDate(2025, 6, 1), QTime(22, 5, 0), Qt::LocalTime);
    clock.setNow(lockTime);
    tracker.useTimerViaLockEvent(LockEvent::Lock);

    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(22, 20, 0), Qt::LocalTime));
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, lockTime);

    const QDateTime nextMorning(QDate(2025, 6, 2), QTime(8, 0, 0), Qt::LocalTime);
    clock.setNow(nextMorning);

    // Act
    tracker.onTick(nextMorning);
    tracker.useTimerViaLockEvent(LockEvent::Unlock);
    tracker.endExclusiveEdit();

    // Assert
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.isAutopausePendingResume_dbg());
    bool foundActivity = false, foundPause = false;
    for (const auto& seg : fakeDb.storedDurations) {
        QVERIFY2(seg.endTime.date() == t0.date(), "No row may end after midnight");
        if (seg.type == DurationType::Activity && seg.startTime == t0 && seg.endTime == lockTime)
            foundActivity = true;
        if (seg.type == DurationType::Pause && seg.startTime == lockTime
            && seg.endTime == QDateTime(QDate(2025, 6, 1), QTime(22, 20, 0), Qt::LocalTime))
            foundPause = true;
    }
    // WP5: the Lock closed History immediately, so this matches the
    // no-dialog outcome exactly -- the evening's Activity and the Pause up
    // to the LongOngoingLock maturation are stored; only the still-ongoing
    // Pause is discarded by the overnight watchdog.
    QVERIFY2(foundActivity, "Expected completed Activity segment 21:00->22:05");
    QVERIFY2(foundPause, "Expected completed Pause segment 22:05->22:20");
}

// N7 (*HEAD-red*; the Medium): once a scheduled stop has already revoked and
// committed the cutoff segment, a later shutdown() must not re-touch it.
void TimerTest::test_N7_shutdown_after_revoked_scheduled_stop_leaves_segment_unchanged()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    tracker.beginExclusiveEdit();

    const QDateTime cutoff(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(cutoff);
    tracker.onTick(clock.now());
    QVERIFY(tracker.isStopped());

    // Act
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(1, 0, 0), Qt::LocalTime));
    tracker.shutdown();

    // Assert
    QCOMPARE(fakeDb.storedDurations.size(), size_t(1));
    const auto& seg = fakeDb.storedDurations.front();
    QCOMPARE(seg.type, DurationType::Activity);
    QCOMPARE(seg.startTime, t0);
    QCOMPARE(seg.endTime, cutoff);
}

// N8: a heartbeat tick before the 23:59:59.500 cutoff must not revoke or stop.
void TimerTest::test_N8_before_cutoff_tick_does_not_revoke_or_stop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    tracker.beginExclusiveEdit();

    const QDateTime early(QDate(2025, 6, 1), QTime(23, 59, 58, 0), Qt::LocalTime);
    clock.setNow(early);

    QSignalSpy revokedSpy(&tracker, &TimeTracker::exclusiveEditRevoked);

    // Act
    tracker.onTick(clock.now());

    // Assert
    QCOMPARE(revokedSpy.count(), 0);
    QVERIFY(tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isActive());
}

// N9: after N1, a second endExclusiveEdit() (the guard's own harmless
// repeat) changes nothing.
void TimerTest::test_N9_repeated_endExclusiveEdit_after_revoke_changes_nothing()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    tracker.beginExclusiveEdit();
    const QDateTime cutoff(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(cutoff);

    QSignalSpy revokedSpy(&tracker, &TimeTracker::exclusiveEditRevoked);
    QSignalSpy warnSpy(&tracker, &TimeTracker::userWarning);

    tracker.onTick(clock.now());
    QVERIFY(tracker.isStopped());
    QCOMPARE(revokedSpy.count(), 1);
    QCOMPARE(warnSpy.count(), 1);
    const size_t storedAfterFirst = fakeDb.storedDurations.size();
    const int callsAfterFirst = fakeDb.callLog.size();

    // Act
    tracker.endExclusiveEdit();

    // Assert: no new revoke, no new notice, no new persistence.
    QCOMPARE(revokedSpy.count(), 1);
    QCOMPARE(warnSpy.count(), 1);
    QVERIFY(tracker.isStopped());
    QCOMPARE(fakeDb.storedDurations.size(), storedAfterFirst);
    QCOMPARE(fakeDb.callLog.size(), callsAfterFirst);
}

// ========================================================================
// WP4 (midnight_close_plan.md): revoke History when the date moves forward
// ========================================================================

// S1: timer stopped, begin edit at 23:50 on 2025-06-01, tick at 00:00:00.100
// on 2025-06-02. Assert: revoked once, one notice, still stopped, no writes,
// dialog closed.
void TimerTest::test_S1_date_forward_tick_revokes_edit_while_stopped()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Arrange: start, advance 1 min, then stop
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(60ll * 1000ll);
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());

    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());

    // Move to next day at 00:00:00.100
    const QDateTime nextDay(QDate(2025, 6, 2), QTime(0, 0, 0, 100), Qt::LocalTime);
    clock.setNow(nextDay);

    QSignalSpy revokedSpy(&tracker, &TimeTracker::exclusiveEditRevoked);
    QSignalSpy warnSpy(&tracker, &TimeTracker::userWarning);
    fakeDb.callLog.clear();

    // Act: trigger a tick (which calls DayBoundaryWatcher::tick)
    tracker.onTick(nextDay);

    // Assert
    QCOMPARE(revokedSpy.count(), 1);
    QVERIFY(!tracker.isDialogOpen_dbg());
    QCOMPARE(warnSpy.count(), 1);
    QCOMPARE(warnSpy.first().first().toString(), kWP2RevokedNotice);
    QVERIFY(tracker.isStopped());
    // No store writes should have happened
    QVERIFY(!fakeDb.callLog.contains("commitSession"));
    QVERIFY(!fakeDb.callLog.contains("discardCheckpoint"));
}

// S2: one test function that runs stopped and running variants in sequence.
// Same-day tick at 23:59:59.400 does not revoke in either (spy count 0,
// dialog still open).
void TimerTest::test_S2_same_day_tick_does_not_revoke_stopped_or_running()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Stopped variant
    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());

    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());

    // Move to 23:59:59.400 same day
    const QDateTime sameDay(QDate(2025, 6, 1), QTime(23, 59, 59, 400), Qt::LocalTime);
    clock.setNow(sameDay);

    QSignalSpy revokedSpy(&tracker, &TimeTracker::exclusiveEditRevoked);

    // Act: tick
    tracker.onTick(sameDay);

    // Assert: no revoke, dialog still open
    QCOMPARE(revokedSpy.count(), 0);
    QVERIFY(tracker.isDialogOpen_dbg());

    // Running variant: fresh tracker
    FakeSessionStore fakeDb2;
    FakeClock clock2(t0);
    TimeTracker tracker2(settings, fakeDb2);
    tracker2.setNowProvider_dbg([&clock2]() { return clock2.now(); });

    tracker2.useTimerViaButton(Button::Start);
    QVERIFY(tracker2.isActive());

    tracker2.beginExclusiveEdit();
    QVERIFY(tracker2.isDialogOpen_dbg());

    clock2.setNow(sameDay);

    QSignalSpy revokedSpy2(&tracker2, &TimeTracker::exclusiveEditRevoked);

    // Act: tick
    tracker2.onTick(sameDay);

    // Assert: no revoke, dialog still open
    QCOMPARE(revokedSpy2.count(), 0);
    QVERIFY(tracker2.isDialogOpen_dbg());
}

// S3: a backward date step does not revoke. Begin edit on 2025-06-02, tick
// with clock at 2025-06-01. Assert: spy 0, dialog still open.
void TimerTest::test_S3_backward_date_step_does_not_revoke()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 2), QTime(10, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());

    // Move backward to previous day
    const QDateTime prevDay(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    clock.setNow(prevDay);

    QSignalSpy revokedSpy(&tracker, &TimeTracker::exclusiveEditRevoked);

    // Act: tick
    tracker.onTick(prevDay);

    // Assert: no revoke, dialog still open
    QCOMPARE(revokedSpy.count(), 0);
    QVERIFY(tracker.isDialogOpen_dbg());
}

// F2: an on-time stop (same day) must NOT discard the checkpoint — it gets
// finalized by commitSession's upsert instead.
void TimerTest::test_F2_ontime_stop_does_not_discard_checkpoint()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(60ll * 1000ll); // elapse a minute so the checkpoint isn't zero-length
    tracker.triggerCheckpoint_dbg();
    QVERIFY(fakeDb.callLog.contains("saveCheckpoint"));
    fakeDb.callLog.clear();

    // Act: stop the same day, before midnight.
    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(23, 59, 0), Qt::LocalTime));
    tracker.useTimerViaButton(Button::Stop);

    // Assert: no discard; the completed segment was committed instead.
    QVERIFY(!fakeDb.callLog.contains("discardCheckpoint"));
    QVERIFY(fakeDb.callLog.contains("commitSession"));
}

// F3: pauseTimer's finalizePauseTransition persists immediately for crash
// safety, but ignored the result. A failure there must feed the same
// unsaved-data bookkeeping stopTimer() already uses.
void TimerTest::test_F3_pause_failure_marks_unsaved()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(60ll * 1000ll); // elapse a minute so the segment isn't zero-length

    // Act: fail the persistence write that finalizePauseTransition performs.
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Pause);

    // Assert: the failure is not swallowed - it is retained for retry.
    QVERIFY(tracker.isPaused());
    QVERIFY(tracker.hasPendingCommit_dbg());
    QVERIFY(!tracker.pendingCommitPayload_dbg().empty());

    // Cleanup (A3): resolve the pending payload via the same idempotent
    // drain a real shutdown would use, so destruction does not find an
    // unresolved commit with no prior drain call.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// F3: backpauseTimer (via a LongOngoingLock) also finalizes through
// finalizePauseTransition and must get the same treatment as an explicit pause.
void TimerTest::test_F3_backpause_failure_marks_unsaved()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    const QDateTime lockInstant(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::LocalTime);
    clock.setNow(lockInstant);

    // Act: fail the persistence write that finalizePauseTransition performs.
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, lockInstant);

    // Assert: the failure is not swallowed - it is retained for retry.
    QVERIFY(tracker.isPaused());
    QVERIFY(tracker.hasPendingCommit_dbg());
    QVERIFY(!tracker.pendingCommitPayload_dbg().empty());

    // Cleanup (A3): resolve the pending payload via the same idempotent
    // drain a real shutdown would use, so destruction does not find an
    // unresolved commit with no prior drain call.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// F3: a later successful resume-from-Pause write must clear the unsaved flag
// that a prior pause failure set.
void TimerTest::test_F3_recovery_after_pause_failure_clears_unsaved()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Act: DB recovers; resuming from Pause persists successfully.
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::success();
    tracker.useTimerViaButton(Button::Start);

    // Assert: the retained unsaved data was cleared by the successful write.
    QVERIFY(!tracker.hasPendingCommit_dbg());
    QVERIFY(tracker.pendingCommitPayload_dbg().empty());
}

// ============================================================================
// A2 (R16-001/R09-001): PendingCommit replaces the retry flag/cache/counter
// with one immutable-payload lifecycle. There is no retry threshold.
// ============================================================================

void TimerTest::test_A2_first_submit_outcome_data()
{
    QTest::addColumn<int>("category");
    QTest::addColumn<bool>("expectPendingAfterStop");
    QTest::addColumn<bool>("expectStartPermitted");

    QTest::newRow("Success")        << static_cast<int>(SessionStoreResult::Success)        << false << true;
    QTest::newRow("Disabled")       << static_cast<int>(SessionStoreResult::Disabled)        << false << true;
    QTest::newRow("TransientError") << static_cast<int>(SessionStoreResult::TransientError)  << true  << false;
    QTest::newRow("FatalError")     << static_cast<int>(SessionStoreResult::FatalError)      << true  << false;
}

// Table-driven (A2 packet table, "first submit"/"operation" rows): the
// outcome of a single persist attempt at Stop determines whether a pending
// commit remains and whether a subsequent Start is permitted. Start is
// attempted with the SAME store outcome still configured, so a failing
// category is retried exactly once and refused - never "continued" with a
// warning.
void TimerTest::test_A2_first_submit_outcome()
{
    QFETCH(int, category);
    QFETCH(bool, expectPendingAfterStop);
    QFETCH(bool, expectStartPermitted);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);

    const auto cat = static_cast<SessionStoreResult::Category>(category);
    const bool isFailure = (cat == SessionStoreResult::TransientError || cat == SessionStoreResult::FatalError);
    fakeDb.commitSessionResult = {cat, isFailure ? QStringLiteral("injected failure") : QString()};
    tracker.useTimerViaButton(Button::Stop);

    QCOMPARE(tracker.hasPendingCommit_dbg(), expectPendingAfterStop);

    // Act: attempt Start with the same store outcome still configured.
    tracker.useTimerViaButton(Button::Start);

    QCOMPARE(tracker.isActive(), expectStartPermitted);
    QCOMPARE(tracker.isStopped(), !expectStartPermitted);
    // A refused Start must never silently drop the pending payload.
    QCOMPARE(tracker.hasPendingCommit_dbg(), !expectStartPermitted);

    // Cleanup (A3): resolve any pending payload via the same idempotent
    // drain a real shutdown would use, so destruction does not find an
    // unresolved commit with no prior drain call.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// A single Start-time retry that still fails must retain the exact same
// payload (same segment IDs/content) - never replace it with newer mutable
// session contents - and must refuse Start.
void TimerTest::test_A2_retry_failure_retains_same_payload_and_refuses_start()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.hasPendingCommit_dbg());
    const auto payloadBefore = tracker.pendingCommitPayload_dbg();
    QVERIFY(!payloadBefore.empty());

    // Act: one Start-time retry attempt, still failing.
    tracker.useTimerViaButton(Button::Start);

    // Assert: refused, payload retained byte-for-byte.
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.hasPendingCommit_dbg());
    const auto payloadAfter = tracker.pendingCommitPayload_dbg();
    QCOMPARE(payloadAfter.size(), payloadBefore.size());
    for (size_t i = 0; i < payloadBefore.size(); ++i) {
        QCOMPARE(payloadAfter[i].segment_id.toString(), payloadBefore[i].segment_id.toString());
        QCOMPARE(payloadAfter[i].startTime, payloadBefore[i].startTime);
        QCOMPARE(payloadAfter[i].endTime, payloadBefore[i].endTime);
    }

    // Cleanup (A3): resolve the pending payload via the same idempotent
    // drain a real shutdown would use, so destruction does not find an
    // unresolved commit with no prior drain call.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// A single Start-time retry that succeeds clears the payload and permits Start.
void TimerTest::test_A2_retry_success_clears_payload_and_permits_start()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Act: store recovers; the Start-time retry now succeeds.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    tracker.useTimerViaButton(Button::Start);

    // Assert: pending cleared, Start permitted.
    QVERIFY(tracker.isActive());
    QVERIFY(!tracker.hasPendingCommit_dbg());
}

// Regression for R09-001: there is no retry counter to go stale. Three
// independent, non-consecutive fail/recover cycles must each behave exactly
// like a first failure - a single retry either unblocks Start or refuses it;
// nothing ever escalates or refuses permanently.
void TimerTest::test_A2_no_stale_failure_state_across_intermittent_successes()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    QSignalSpy warnSpy(&tracker, &TimeTracker::userWarning);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    for (int cycle = 0; cycle < 3; ++cycle) {
        tracker.useTimerViaButton(Button::Start);
        QVERIFY(tracker.isActive());
        clock.advance(60ll * 1000ll);

        // Fail the Stop persist -> pending commit.
        fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
        tracker.useTimerViaButton(Button::Stop);
        QVERIFY(tracker.isStopped());
        QVERIFY(tracker.hasPendingCommit_dbg());

        // Start while still failing: must be refused, exactly like a first-ever failure.
        tracker.useTimerViaButton(Button::Start);
        QVERIFY(tracker.isStopped());
        QVERIFY(tracker.hasPendingCommit_dbg());

        // Fix the store: the single retry now succeeds.
        fakeDb.commitSessionResult = SessionStoreResult::success();
        tracker.useTimerViaButton(Button::Start);
        QVERIFY(tracker.isActive());
        QVERIFY(!tracker.hasPendingCommit_dbg());

        clock.advance(60ll * 1000ll);
        tracker.useTimerViaButton(Button::Stop);
        QVERIFY(!tracker.hasPendingCommit_dbg());
    }

    // No message ever mentions an escalating "consecutive failure" count.
    for (int i = 0; i < warnSpy.count(); ++i) {
        const QString msg = warnSpy.at(i).at(0).toString();
        QVERIFY(!msg.contains("consecutive", Qt::CaseInsensitive));
    }
}

// Repeated failed Start attempts while a payload is pending must neither
// begin tracking nor mutate the payload retained from the earlier session.
void TimerTest::test_A2_repeated_failed_start_neither_starts_nor_mutates_payload()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.hasPendingCommit_dbg());
    const auto payloadBefore = tracker.pendingCommitPayload_dbg();

    // Act: repeatedly attempt Start while the store keeps failing.
    for (int i = 0; i < 3; ++i) {
        tracker.useTimerViaButton(Button::Start);
        QVERIFY(tracker.isStopped());
        QVERIFY(!tracker.isActive());
    }

    // Assert: never began tracking, payload never mutated.
    QVERIFY(tracker.isStopped());
    const auto payloadAfter = tracker.pendingCommitPayload_dbg();
    QCOMPARE(payloadAfter.size(), payloadBefore.size());
    for (size_t i = 0; i < payloadBefore.size(); ++i) {
        QCOMPARE(payloadAfter[i].segment_id.toString(), payloadBefore[i].segment_id.toString());
    }

    // Cleanup (A3): resolve the pending payload via the same idempotent
    // drain a real shutdown would use, so destruction does not find an
    // unresolved commit with no prior drain call.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

void TimerTest::test_F1_loadHistoryFromStore_forwards_status()
{
    // F1: loadHistoryFromStore() must forward the store's LoadResult (status
    // included) instead of discarding the status and returning just the
    // (empty-on-failure) deque — the contract violation the rest of F1 chain
    // followed from.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.loadDurationsResult.status = SessionStoreResult::TransientError;
    TimeTracker tracker(settings, fakeDb);

    const LoadResult result = tracker.loadHistoryFromStore();

    QCOMPARE(result.status, SessionStoreResult::TransientError);
}

// ============================================================================
// A3 (R11-001): drainForShutdown() retries a pending commit payload
// independent of tracker mode. Invariant: shutdown cannot report clean while
// a commit payload exists only in memory.
// ============================================================================

void TimerTest::test_A3_drain_already_stopped_no_pending_is_noop_success()
{
    // Arrange: a tracker that was never started.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Act
    const DrainResult result = tracker.drainForShutdown();

    // Assert
    QVERIFY(result.clean());
    QVERIFY(!tracker.hasPendingCommit_dbg());
}

void TimerTest::test_A3_drain_retries_preexisting_pending_payload_while_stopped()
{
    // Arrange: an already-stopped tracker with a payload retained from an
    // earlier failed Stop (mode_ == None throughout -- the R11-001 scenario:
    // the old shutdown() path never retried this).
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.hasPendingCommit_dbg());
    const int commitsBefore = fakeDb.callLog.count("commitSession");

    // Act: the store recovers; drainForShutdown() retries without a Start.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    const DrainResult result = tracker.drainForShutdown();

    // Assert: resolved, exactly one retry attempt, no residual pending state.
    QVERIFY(result.clean());
    QVERIFY(!tracker.hasPendingCommit_dbg());
    QCOMPARE(fakeDb.callLog.count("commitSession") - commitsBefore, 1);
}

void TimerTest::test_A3_drain_retry_failure_keeps_incomplete_and_payload()
{
    // Arrange: same setup, but the store keeps failing.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("still failing");
    tracker.useTimerViaButton(Button::Stop);
    const auto payloadBefore = tracker.pendingCommitPayload_dbg();

    // Act
    const DrainResult result = tracker.drainForShutdown();

    // Assert: no clean marker eligibility, payload retained byte-for-byte,
    // the failure category/message are preserved.
    QVERIFY(!result.clean());
    QCOMPARE(result.category, SessionStoreResult::TransientError);
    QCOMPARE(result.message, QString("still failing"));
    QVERIFY(tracker.hasPendingCommit_dbg());
    const auto payloadAfter = tracker.pendingCommitPayload_dbg();
    QCOMPARE(payloadAfter.size(), payloadBefore.size());
    for (size_t i = 0; i < payloadBefore.size(); ++i) {
        QCOMPARE(payloadAfter[i].segment_id.toString(), payloadBefore[i].segment_id.toString());
    }

    // Cleanup so destruction does not find an unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

void TimerTest::test_A3_drain_running_session_stop_commit_fails_then_drain_succeeds()
{
    // Arrange: a running session whose live segment has never been stopped.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(60ll * 1000ll);

    // Act: the store fails at drain time, so the inline Stop-time commit
    // inside drainForShutdown() fails on this first call.
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    const DrainResult firstAttempt = tracker.drainForShutdown();
    QVERIFY(!firstAttempt.clean());
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.hasPendingCommit_dbg());

    // The store recovers; a second drainForShutdown() call (what a user
    // Retry performs) resolves the retained payload.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    const DrainResult secondAttempt = tracker.drainForShutdown();

    // Assert
    QVERIFY(secondAttempt.clean());
    QVERIFY(!tracker.hasPendingCommit_dbg());
}

void TimerTest::test_A3_drain_running_session_stop_commit_fails_then_drain_fails()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");

    // Act: two drain attempts, store never recovers.
    const DrainResult firstAttempt = tracker.drainForShutdown();
    const DrainResult secondAttempt = tracker.drainForShutdown();

    // Assert: both incomplete, engine stopped, payload retained.
    QVERIFY(!firstAttempt.clean());
    QVERIFY(!secondAttempt.clean());
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Cleanup so destruction does not find an unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

void TimerTest::test_A3_drain_idempotent_repeated_calls_after_success()
{
    // Arrange: a resolved drain (no pending payload).
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    QVERIFY(tracker.drainForShutdown().clean());
    const int commitsAfterFirstDrain = fakeDb.callLog.count("commitSession");

    // Act: a second call after success.
    const DrainResult second = tracker.drainForShutdown();

    // Assert: still Complete, and no additional commit was submitted (no
    // duplicate/resubmit of an already-committed segment).
    QVERIFY(second.clean());
    QCOMPARE(fakeDb.callLog.count("commitSession"), commitsAfterFirstDrain);
}

void TimerTest::test_A3_drain_idempotent_repeated_calls_after_failure()
{
    // Arrange: a failed drain leaves a pending payload.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("first failure message");
    const DrainResult first = tracker.drainForShutdown();
    const auto payloadAfterFirst = tracker.pendingCommitPayload_dbg();
    const int commitsAfterFirst = fakeDb.callLog.count("commitSession");

    // Act: a second call. The store now fails with a DIFFERENT message, which
    // also proves the second call really reached the store rather than
    // replaying a cached result.
    fakeDb.commitSessionResult = SessionStoreResult::transient("second failure message");
    const DrainResult second = tracker.drainForShutdown();

    // Assert: the second call genuinely re-attempted the store (a cached-result
    // implementation that skipped it would leave this delta at 0)...
    QCOMPARE(fakeDb.callLog.count("commitSession") - commitsAfterFirst, 1);
    // ...and it did not lose the payload identity...
    QVERIFY(!first.clean());
    QVERIFY(!second.clean());
    const auto payloadAfterSecond = tracker.pendingCommitPayload_dbg();
    QCOMPARE(payloadAfterSecond.size(), payloadAfterFirst.size());
    for (size_t i = 0; i < payloadAfterFirst.size(); ++i) {
        QCOMPARE(payloadAfterSecond[i].segment_id.toString(), payloadAfterFirst[i].segment_id.toString());
    }
    // Each call returns that attempt's latest outcome.
    QCOMPARE(first.message, QString("first failure message"));
    QCOMPARE(second.message, QString("second failure message"));

    // Cleanup so destruction does not find an unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

void TimerTest::test_A3_destructor_is_noop_after_successful_drain()
{
    // Arrange: fakeDb outlives tracker (mirrors the real ownership order:
    // TimeTracker must be destroyed before the store) so the callLog can be
    // inspected after the tracker's destructor has run.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    int commitsAfterDrain = 0;
    {
        const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
        FakeClock clock(t0);
        TimeTracker tracker(settings, fakeDb);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

        tracker.useTimerViaButton(Button::Start);
        clock.advance(60ll * 1000ll);
        // Act: a full drain, as ShutdownCoordinator would perform it.
        QVERIFY(tracker.drainForShutdown().clean());
        commitsAfterDrain = fakeDb.callLog.count("commitSession");
        // tracker goes out of scope here -- its destructor's own
        // drainForShutdown() call runs against an already-resolved state.
    }

    // Assert: the destructor did not resubmit/duplicate the committed
    // segment (no additional commitSession call beyond the explicit drain).
    QCOMPARE(fakeDb.callLog.count("commitSession"), commitsAfterDrain);
}

// ============================================================================
// A4 (R05-001): durable checkpoint-discard intent.
// ============================================================================

void TimerTest::test_A4_discard_transient_failure_retains_intent_and_blocks_clean_mark()
{
    // Arrange: an Activity session that crosses midnight.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    const auto ongoingAtStart = tracker.getOngoingDuration();
    QVERIFY(ongoingAtStart.has_value());
    const SegmentId segId = ongoingAtStart->segment_id;

    // The store's discard call keeps failing transiently.
    fakeDb.discardCheckpointResult = SessionStoreResult::transient("injected discard failure");
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime));

    // Act: the watchdog discards the ongoing (cross-midnight) segment.
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop_dbg(clock.now()));

    // Assert: the intent is retained by its stable segment id, and clean
    // marking is forbidden while it is unresolved.
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.hasPendingDiscards_dbg());
    const auto pendingIds = tracker.pendingDiscardIds_dbg();
    QVERIFY(std::find(pendingIds.begin(), pendingIds.end(), segId) != pendingIds.end());
    QVERIFY(!tracker.drainForShutdown().clean());

    // Cleanup so destruction does not find an unresolved intent.
    fakeDb.discardCheckpointResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

void TimerTest::test_A4_discard_fatal_failure_retains_intent_and_blocks_clean_mark()
{
    // Arrange: same setup, but the store fails Fatal instead of Transient.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);

    fakeDb.discardCheckpointResult = SessionStoreResult::fatal("injected fatal discard failure");
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime));

    // Act
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop_dbg(clock.now()));

    // Assert: retained exactly like Transient (C4 owns latching a fatal
    // tracker-unavailable state; A4 only guarantees retention here).
    QVERIFY(tracker.hasPendingDiscards_dbg());
    QVERIFY(!tracker.drainForShutdown().clean());

    // Cleanup.
    fakeDb.discardCheckpointResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

void TimerTest::test_A4_discard_disabled_resolves_intent_without_mutating_checkpoints()
{
    // Arrange: a failed discard leaves an intent pending and a checkpoint
    // record still present in the fake.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    tracker.triggerCheckpoint_dbg();
    const size_t checkpointsBefore = fakeDb.savedCheckpoints.size();
    QVERIFY(checkpointsBefore > 0);

    fakeDb.discardCheckpointResult = SessionStoreResult::transient("injected discard failure");
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime));
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop_dbg(clock.now()));
    QVERIFY(tracker.hasPendingDiscards_dbg());

    // Act: history storage becomes (contractually) disabled -- a deliberate
    // no-op success, not a real deletion.
    fakeDb.discardCheckpointResult = SessionStoreResult::disabled();
    QVERIFY(tracker.drainForShutdown().clean());

    // Assert: the intent resolved (Disabled is a contractual no-op success)
    // without creating unresolvable work, but the fake's checkpoint records
    // were NOT mutated -- Disabled must never mutate state.
    QVERIFY(!tracker.hasPendingDiscards_dbg());
    QCOMPARE(fakeDb.savedCheckpoints.size(), checkpointsBefore);
}

void TimerTest::test_A4_start_retries_pending_discard_without_blocking_start()
{
    // Arrange: a failed discard leaves an intent pending while stopped.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);

    fakeDb.discardCheckpointResult = SessionStoreResult::transient("injected discard failure");
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime));
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop_dbg(clock.now()));
    QVERIFY(tracker.hasPendingDiscards_dbg());

    // Act: the store recovers, then the user presses Start.
    fakeDb.discardCheckpointResult = SessionStoreResult::success();
    tracker.useTimerViaButton(Button::Start);

    // Assert: Start was NOT blocked by the unresolved discard intent, and
    // the retry performed at Start resolved it.
    QVERIFY(tracker.isActive());
    QVERIFY(!tracker.hasPendingDiscards_dbg());
}

void TimerTest::test_A4_drain_retries_pending_discard_then_succeeds()
{
    // Arrange: a failed discard leaves an intent pending while stopped.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);

    fakeDb.discardCheckpointResult = SessionStoreResult::transient("injected discard failure");
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime));
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop_dbg(clock.now()));
    QVERIFY(!tracker.drainForShutdown().clean());

    // Act: the store recovers; drainForShutdown() retries without a Start.
    fakeDb.discardCheckpointResult = SessionStoreResult::success();
    const DrainResult result = tracker.drainForShutdown();

    // Assert
    QVERIFY(result.clean());
    QVERIFY(!tracker.hasPendingDiscards_dbg());
}

void TimerTest::test_A4_drain_incomplete_while_discard_unresolved()
{
    // Arrange: a failed discard leaves an intent pending while stopped.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);

    fakeDb.discardCheckpointResult = SessionStoreResult::transient("injected discard failure");
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime));
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop_dbg(clock.now()));

    // Act: the store keeps failing.
    const DrainResult result = tracker.drainForShutdown();

    // Assert: no clean-marker eligibility, intent still retained.
    QVERIFY(!result.clean());
    QVERIFY(tracker.hasPendingDiscards_dbg());

    // Cleanup so destruction does not find an unresolved intent.
    fakeDb.discardCheckpointResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// ============================================================================
// C4 (R05-006): FatalError latches TimeTracker into one unavailable state.
// ============================================================================

// A Fatal periodic checkpoint failure stops tracking immediately (through the
// normal stopTimer()/pending-commit lifecycle) and shows exactly one modal
// error.
void TimerTest::test_C4_fatal_checkpoint_failure_latches_and_stops_tracking()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    QSignalSpy fatalSpy(&tracker, &TimeTracker::fatalError);
    QSignalSpy warnSpy(&tracker, &TimeTracker::userWarning);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(60ll * 1000ll);
    fakeDb.saveCheckpointResult = SessionStoreResult::fatal("injected fatal checkpoint failure");

    // Act
    tracker.triggerCheckpoint_dbg();

    // Assert: tracking stopped, latch set, exactly one modal error (C4 fix
    // round 2 / HIGH-1: fatalError, not the tray-routed userWarning).
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.isFatalLatched());
    QCOMPARE(fatalSpy.count(), 1);
    QCOMPARE(warnSpy.count(), 0);
}

// Once latched, Start is refused even after the store recovers -- only a
// fresh TimeTracker instance (a restart) can clear the latch.
void TimerTest::test_C4_fatal_latch_disables_start_even_after_store_recovers()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.saveCheckpointResult = SessionStoreResult::fatal("injected fatal checkpoint failure");
    tracker.triggerCheckpoint_dbg();
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.isFatalLatched());

    // Act: the store recovers, but Start is attempted on the SAME instance.
    fakeDb.saveCheckpointResult = SessionStoreResult::success();
    fakeDb.commitSessionResult = SessionStoreResult::success();
    tracker.useTimerViaButton(Button::Start);

    // Assert: still refused -- the latch survives store recovery.
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.isActive());
    QVERIFY(tracker.isFatalLatched());
}

// Two independent Fatal failures across different operations must show
// exactly one modal error in total, not one per failure.
void TimerTest::test_C4_fatal_error_shows_dialog_exactly_once()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    QSignalSpy fatalSpy(&tracker, &TimeTracker::fatalError);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.saveCheckpointResult = SessionStoreResult::fatal("first fatal failure");
    tracker.triggerCheckpoint_dbg();
    QVERIFY(tracker.isFatalLatched());
    QCOMPARE(fatalSpy.count(), 1);

    // Act: a second, independent Fatal failure from a DIFFERENT operation
    // (a discard retry during drainForShutdown -- not blocked by the
    // Start-latch check, since tracking is already stopped).
    tracker.injectPendingDiscard_dbg(SegmentId::mint());
    fakeDb.discardCheckpointResult = SessionStoreResult::fatal("second fatal failure");
    tracker.drainForShutdown();

    // Assert: still exactly one modal error in total.
    QCOMPARE(fatalSpy.count(), 1);

    // Cleanup so destruction does not find an unresolved intent.
    fakeDb.discardCheckpointResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// A transient periodic checkpoint failure is silent (no dialog) and tracking
// continues; the very next checkpoint interval retries through the same
// call path and succeeds once the store recovers.
void TimerTest::test_C4_transient_checkpoint_failure_is_silent_and_tracking_continues()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    QSignalSpy warnSpy(&tracker, &TimeTracker::userWarning);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.saveCheckpointResult = SessionStoreResult::transient("injected transient checkpoint failure");

    // Act: the periodic checkpoint fails transiently.
    tracker.triggerCheckpoint_dbg();

    // Assert: tracking continues, no dialog, no latch.
    QVERIFY(tracker.isActive());
    QVERIFY(!tracker.isFatalLatched());
    QCOMPARE(warnSpy.count(), 0);

    // Act: the store recovers; the next ordinary checkpoint interval retries
    // through the exact same call path and succeeds.
    fakeDb.saveCheckpointResult = SessionStoreResult::success();
    clock.advance(60ll * 1000ll);
    tracker.triggerCheckpoint_dbg();

    // Assert: still active, still no dialog, no counter/backoff prevented the retry.
    QVERIFY(tracker.isActive());
    QCOMPARE(warnSpy.count(), 0);

    // Cleanup.
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.drainForShutdown().clean());
}

// C4 finding 1: replaceAll()'s Fatal used to reach the caller (HistoryDialog)
// directly and never latch fatal_state_latched_ at all -- the ONLY surviving
// Fatal in the concrete store (its post-backup reopen) was therefore
// unreachable in production. Assert it now latches and refuses Start exactly
// like a Fatal from any other operation.
void TimerTest::test_C4_fatal_replaceAll_latches_and_refuses_subsequent_start()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    QSignalSpy fatalSpy(&tracker, &TimeTracker::fatalError);

    // Act: replaceAll (as HistoryDialog would call it) returns Fatal.
    fakeDb.replaceDurationsResult = SessionStoreResult::fatal("injected fatal replaceAll failure");
    const SessionStoreResult r = tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt));

    // Assert: the caller still sees the Fatal result (HistoryDialog needs it
    // to know the save failed), AND it latched the tracker with exactly one
    // modal error.
    QCOMPARE(r.category, SessionStoreResult::FatalError);
    QVERIFY(tracker.isFatalLatched());
    QCOMPARE(fatalSpy.count(), 1);

    // Act: Start is attempted afterward, even though the store recovers.
    fakeDb.replaceDurationsResult = SessionStoreResult::success();
    tracker.useTimerViaButton(Button::Start);

    // Assert: refused -- the latch survives.
    QVERIFY(!tracker.isActive());
    QVERIFY(tracker.isStopped());
}

// C4 finding 2: enterFatalState() used to call stopTimer() re-entrantly from
// deep inside an in-progress transition. On the Pause->Activity resume, the
// nested stop set mode_ = None and emitted stopped() BEFORE startTimer()'s
// own code continued on to `emit started(true)` -- leaving observers with
// started() AFTER stopped(), the stuck-running-UI bug. The fix stops at a
// safe point after the transition completes instead.
void TimerTest::test_C4_fatal_pauseresume_commit_stops_without_started_after_stopped()
{
    // Arrange: get into Pause mode.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    // Act: resume from Pause; committing the just-completed Pause segment fails Fatal.
    fakeDb.commitSessionResult = SessionStoreResult::fatal("injected fatal commit on resume");
    clock.advance(10ll * 1000ll);
    tracker.useTimerViaButton(Button::Start);

    // Assert: latched and cleanly stopped, not stuck showing Activity.
    QVERIFY(tracker.isFatalLatched());
    QVERIFY(tracker.isStopped());
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.last().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::FatalStoreError);
    // The stuck-UI bug: no second (started-like) transition must follow the
    // stop -- exactly one transitioned() event for this whole operation.
    QCOMPARE(transitionedSpy.count(), 1);
    // D10 finding 3: mode_ was internally mutated to Activity before the
    // nested fatal stop fired, but observers never saw that Activity
    // transition announced -- the emitted `from` must be the mode they
    // actually last saw (Pause), not the impossible Activity->None.
    const auto t = transitionedSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Pause);
    QCOMPARE(t.to, TimeTracker::Mode::None);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::FatalStoreError);

    // Cleanup: latched, so no further Start; resolve the retained payload
    // so the destructor doesn't find an unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    tracker.drainForShutdown();
}

// C4 finding 2: same defect on the Activity->Pause transition
// (finalizePauseTransition, shared by pauseTimer/backpauseTimer). The nested
// stop used to fire BEFORE session_.beginNewSegment(now), leaving
// segment_id/segment_start_time set while mode_ == None -- the state
// SessionState documents as impossible. The fix stops only after the pause
// transition has reached a fully consistent state.
void TimerTest::test_C4_fatal_pause_transition_ends_stopped_not_impossible_state()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    // Act: pause; committing the just-completed Activity segment fails Fatal.
    fakeDb.commitSessionResult = SessionStoreResult::fatal("injected fatal commit on pause");
    clock.advance(60ll * 1000ll);
    tracker.useTimerViaButton(Button::Pause);

    // Assert: latched and cleanly stopped -- never the impossible state of
    // segment_id/segment_start_time set while mode_ == None -- and no
    // separate pause-like transition for a mode that never actually landed.
    QVERIFY(tracker.isFatalLatched());
    QVERIFY(tracker.isStopped());
    // (segment_id.isEmpty() / !segment_start_time.isValid() are not asserted
    // here: mode_ is assigned Mode::None at exactly two sites in production
    // code -- stopTimer() (timetracker.cpp:853), straight-line followed by
    // session_.clearSegment() (:854), and commitEditedTimeline's no-ongoing
    // branch (:1206), preceded by clearSegment() (:1204) -- so isStopped()
    // above already implies both checks; they could not fail.)
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.last().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::FatalStoreError);
    QCOMPARE(transitionedSpy.count(), 1);
    // D10 finding 3: mode_ was internally mutated to Pause before the nested
    // fatal stop fired, but that Pause was never announced -- `from` must be
    // Activity (what observers actually last saw), not the impossible Pause->None.
    const auto t = transitionedSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Activity);
    QCOMPARE(t.to, TimeTracker::Mode::None);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::FatalStoreError);

    // Cleanup: latched, so no further Start; resolve the retained payload
    // so the destructor doesn't find an unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    tracker.drainForShutdown();
}

// C4 finding 2: enterFatalState()'s old nested stopTimer() consumed the
// cross-midnight transition out from under discardCrossMidnightOngoingAndStop()'s
// own unconditional stopTimer(now, MidnightWatchdog) call, which then became a
// no-op (mode_ already None) -- so observers saw StopReason::FatalStoreError
// instead of MidnightWatchdog. The fix (enterFatalState() no longer stops at
// all) restores the watchdog's own stop as the one that actually fires.
void TimerTest::test_C4_fatal_during_midnight_discard_still_reports_midnightwatchdog()
{
    // Arrange: Start, Pause, Resume before midnight so session_.durations has
    // completed segments to persist when the watchdog fires.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 0, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(23, 10, 0), Qt::LocalTime));
    tracker.useTimerViaButton(Button::Pause);
    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(23, 20, 0), Qt::LocalTime));
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    QVERIFY(!tracker.completedTimeline().completed().empty());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    // Act: the watchdog fires after midnight; the completed-segment commit
    // it attempts along the way fails Fatal.
    fakeDb.commitSessionResult = SessionStoreResult::fatal("injected fatal commit during midnight discard");
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime));
    QVERIFY(tracker.discardCrossMidnightOngoingAndStop_dbg(clock.now()));

    // Assert: latched (Fatal was handled), but the watchdog's OWN stop still
    // wins the reported reason.
    QVERIFY(tracker.isFatalLatched());
    QVERIFY(tracker.isStopped());
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.last().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightWatchdog);

    // Cleanup: latched, so no further Start; resolve the retained payload
    // so the destructor doesn't find an unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    tracker.drainForShutdown();
}

// C4 fix round 2 (HIGH-2): a CallerBug on stop-commit must NOT be dropped.
// Round-1 special-cased CallerBug to clear the pending payload and write the
// clean-shutdown marker as if the data had been persisted -- silently
// destroying an entire session's tracked time. The plan's policy (section 2)
// is that CallerBug falls through the normal okOrDisabled() path: retained
// for retry, no clean marker, surfaced via the normal Retry/Exit-Anyway
// drain dialog at shutdown -- exactly like any other failure category.
void TimerTest::test_C4_callerBug_commit_payload_is_retained_not_dropped()
{
    // Arrange: a running session with something to persist on Stop.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);

    // Act: Stop persists the session, but the store rejects it as a
    // deterministic caller bug (e.g. a reversed/cross-day payload).
    fakeDb.commitSessionResult = SessionStoreResult::callerBug("injected caller bug on commit");
    tracker.useTimerViaButton(Button::Stop);

    // Assert: retained for retry -- NOT dropped -- and no clean marker was
    // written for data that was never actually persisted.
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.hasPendingCommit_dbg());
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    // The store recovers with an ordinary success result -- the normal A2
    // retry-on-Start path resolves it like any other retained payload.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    QVERIFY(!tracker.hasPendingCommit_dbg());
    tracker.useTimerViaButton(Button::Stop);

    QVERIFY(tracker.drainForShutdown().clean());
}

// C4 fix round 3 (F6): unlike the previous test, the store's CallerBug does
// NOT recover -- it is deterministic (e.g. a reversed/cross-day payload),
// so retrying the identical retained payload must keep failing the same way
// and Start must stay refused for as long as that holds.
void TimerTest::test_C4_callerBug_commit_keeps_start_refused_while_persistent()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);

    fakeDb.commitSessionResult = SessionStoreResult::callerBug("injected caller bug on commit");
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Act: the store's CallerBug result never changes -- Start is attempted
    // twice, and the retained payload is retried against the same rejection
    // each time.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(!tracker.isActive());
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.hasPendingCommit_dbg());

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(!tracker.isActive());
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.hasPendingCommit_dbg());
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    // Cleanup: only the store recovering (analogous to Exit Anyway ending
    // the process) resolves it -- drain so destruction finds no unresolved
    // payload.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// C4 fix round 2 (MEDIUM-HIGH-3): a CallerBug on a pending checkpoint-discard
// retry must NOT be resolved -- round-1 dropped the intent, silently
// unblocking the clean-shutdown marker for a checkpoint row that was never
// actually removed from storage. It must retain the intent exactly like any
// other failure category.
void TimerTest::test_C4_callerBug_discard_retry_is_retained_not_resolved()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Arrange: a pending discard intent that a retry will hit.
    tracker.injectPendingDiscard_dbg(SegmentId::mint());
    fakeDb.discardCheckpointResult = SessionStoreResult::callerBug("injected caller bug on discard");

    // Act: drainForShutdown() is a normal persistence/discard-retry opportunity.
    const DrainResult result = tracker.drainForShutdown();

    // Assert: intent retained, drain incomplete, clean marker blocked.
    QVERIFY(tracker.hasPendingDiscards_dbg());
    QVERIFY(!result.clean());

    // Cleanup so destruction does not find an unresolved intent.
    fakeDb.discardCheckpointResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// C4 fix round 2 (HIGH-1): enterFatalState() must emit exactly one
// fatalError (routed to a modal QMessageBox::critical by MainWin) and must
// NOT emit userWarning for the fatal transition itself -- userWarning is a
// tray balloon/status-bar message, not the "one modal error" C4/R05-006
// requires. Repeated fatal failures must not repeat either signal.
void TimerTest::test_C4_fatalError_emitted_once_userWarning_not_emitted_for_fatal()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    QSignalSpy fatalSpy(&tracker, &TimeTracker::fatalError);
    QSignalSpy warnSpy(&tracker, &TimeTracker::userWarning);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.saveCheckpointResult = SessionStoreResult::fatal("first fatal failure");
    tracker.triggerCheckpoint_dbg();

    QCOMPARE(fatalSpy.count(), 1);
    QCOMPARE(warnSpy.count(), 0);

    // A second, independent Fatal failure must not repeat either signal.
    tracker.injectPendingDiscard_dbg(SegmentId::mint());
    fakeDb.discardCheckpointResult = SessionStoreResult::fatal("second fatal failure");
    tracker.drainForShutdown();

    QCOMPARE(fatalSpy.count(), 1);
    QCOMPARE(warnSpy.count(), 0);

    // Cleanup.
    fakeDb.discardCheckpointResult = SessionStoreResult::success();
    tracker.drainForShutdown();
}

// C4 fix round 3 (F1): retryUnsavedDurations() opens with
// retryPendingDiscards(), which can latch fatal_state_latched_ via a Fatal
// discardCheckpoint() -- with no pending commit payload, retryUnsavedDurations()
// then returns true and startTimer()'s None branch would fall through to
// `mode_ = Mode::Activity` unless the latch is re-checked right after. Assert
// Start does NOT begin tracking when this happens.
void TimerTest::test_C4_start_refused_when_discard_retry_latches_with_no_pending_commit()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    // Arrange: a pending discard intent, no pending commit payload, and the
    // tracker not latched yet.
    tracker.injectPendingDiscard_dbg(SegmentId::mint());
    QVERIFY(!tracker.hasPendingCommit_dbg());
    QVERIFY(!tracker.isFatalLatched());
    fakeDb.discardCheckpointResult = SessionStoreResult::fatal("injected fatal discard failure");

    // Act
    tracker.useTimerViaButton(Button::Start);

    // Assert: latch is now set, but tracking must NOT have started -- mode_
    // stayed None throughout (the latch happens before any mode mutation),
    // so no transition of any kind was ever emitted.
    QVERIFY(tracker.isFatalLatched());
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.isActive());
    QCOMPARE(transitionedSpy.count(), 0);

    // Cleanup so destruction does not find an unresolved intent.
    fakeDb.discardCheckpointResult = SessionStoreResult::success();
    tracker.drainForShutdown();
}


// D2 (R11-003): a backward wall-clock step (NTP/manual/DST) makes `now`
// precede the live segment's captured start. Stopping must still discard
// only the now-reversed CLOSING interval (as TimeDuration::create's
// NonPositive check already does) and nothing else: no row reaches the
// store for it (zero DB mutation), no discard intent is registered (HIGH-A:
// that would authorize deleting a valid checkpoint prefix -- see the
// dedicated "never_discards_checkpoint" test and the real-SQLite recovery
// test), and -- because there is nothing left unresolved for this process
// to retry (MEDIUM-B; see the D2 packet's deviation note) -- a later drain
// can still clean-mark. D6: stopTimer() itself writes no marker at all
// anymore (that is now exclusively ShutdownCoordinator's job via the
// store's markCleanShutdown()), so this proves the drain-eligibility side
// of the old claim via drainForShutdown().clean() instead.
void TimerTest::test_D2_stop_backward_clock_step_zero_mutation_drain_still_clean()
{
    // Arrange: running session, clock then steps backward within the same day.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    fakeDb.callLog.clear();
    clock.setNow(t0.addSecs(-600)); // backward step: now precedes segment start
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    // Act
    tracker.useTimerViaButton(Button::Stop);

    // Assert: zero mutation, nothing pending, stopTimer wrote no marker
    // itself, and a subsequent drain (what ShutdownCoordinator performs)
    // is clean -- nothing this reversal left behind blocks clean marking.
    QVERIFY(tracker.isStopped());
    QVERIFY(fakeDb.storedDurations.empty());
    QVERIFY(!tracker.hasPendingDiscards_dbg());
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QVERIFY(tracker.drainForShutdown().clean());

    // D10: the backward clock step drops only the closing interval -- Stop
    // itself is still one ordinary transition, correctly ButtonStop.
    QCOMPARE(transitionedSpy.count(), 1);
    const auto t = transitionedSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Activity);
    QCOMPARE(t.to, TimeTracker::Mode::None);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::ButtonStop);
}

// D2/HIGH-A (round-2 review): a backward-clock Stop must never register a
// checkpoint-discard intent -- doing so (as an earlier implementation did,
// reusing the A4 cross-midnight mechanism) authorizes deleting a valid,
// already-checkpointed prefix of the same segment along with the genuinely-
// invalid closing delta. Proves discardCheckpoint() is never even called
// for the reversal case, unlike the cross-midnight case which legitimately
// discards the whole segment (see the existing A4/cross-midnight tests).
void TimerTest::test_D2_stop_backward_clock_step_never_discards_checkpoint()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    fakeDb.callLog.clear();

    // Act: backward step, then Stop.
    clock.setNow(t0.addSecs(-600));
    tracker.useTimerViaButton(Button::Stop);

    // Assert: stopped, no discard was ever attempted, nothing is pending,
    // and Start is not blocked (MEDIUM-B) -- there is no retryable payload
    // and no intent to resolve.
    QVERIFY(tracker.isStopped());
    QVERIFY(!fakeDb.callLog.contains("discardCheckpoint"));
    QVERIFY(!tracker.hasPendingDiscards_dbg());
    QVERIFY(!tracker.hasPendingCommit_dbg());

    clock.setNow(t0.addSecs(3600));
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
}

// D2 (R11-003): a backward step that lands on the PREVIOUS calendar date
// must not be misclassified as a cross-midnight watchdog condition -- it is
// an ordinary reversed interval like the same-day case, reported with the
// button's own StopReason, not MidnightWatchdog.
void TimerTest::test_D2_stop_rejects_backward_clock_step_across_date_not_misclassified()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Backward step onto the PRIOR calendar date.
    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime));

    // Act
    tracker.useTimerViaButton(Button::Stop);

    // Assert: stopped via the button's own reason, not the watchdog's --
    // this is a reversed interval, not a genuine cross-midnight condition.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::ButtonStop);
    QVERIFY(tracker.isStopped());
    QVERIFY(fakeDb.storedDurations.empty());
}

// D2 (R11-003): the same backward step at Pause must not mutate any state
// at all -- refusing the whole transition (rather than capping `now` to the
// Activity segment's own start, which still re-anchors the new Pause
// segment there) is what prevents real Activity time from later being
// persisted as Pause once that segment ends normally.
void TimerTest::test_D2_pause_refuses_backward_clock_step_no_state_mutated()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    const auto ongoingAtStart = tracker.getOngoingDuration();
    QVERIFY(ongoingAtStart.has_value());
    const SegmentId segId = ongoingAtStart->segment_id;

    clock.setNow(t0.addSecs(-600)); // backward step: now precedes segment start

    // Act
    tracker.useTimerViaButton(Button::Pause);

    // Assert: refused outright -- still Activity, same segment identity and
    // start, nothing persisted. getOngoingDuration() stays populated across
    // the reversed clock: getOngoingDuration_locked() caps a reversed-clock
    // read at segment_start_time (a zero-length duration) rather than
    // returning nullopt.
    QVERIFY(tracker.isActive());
    const auto ongoingAfterRefusedPause = tracker.getOngoingDuration();
    QVERIFY(ongoingAfterRefusedPause.has_value());
    QCOMPARE(ongoingAfterRefusedPause->startTime, t0);
    QCOMPARE(ongoingAfterRefusedPause->segment_id, segId);
    QVERIFY(fakeDb.storedDurations.empty());

    // Once wall time is sane again, Pause proceeds normally.
    clock.setNow(t0.addSecs(300));
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());
    const auto ongoingAfterPause = tracker.getOngoingDuration();
    QVERIFY(ongoingAfterPause.has_value());
    QCOMPARE(ongoingAfterPause->startTime, t0.addSecs(300));
}

// D2/MEDIUM-6 (R11-003): backpauseTimer -- the AUTOMATIC lock-driven pause
// path -- is exposed to the exact same anchor-regression risk as the manual
// Pause path when an NTP correction arrives during/soon after a lock. It
// must refuse the same way.
void TimerTest::test_D2_backpause_refuses_backward_clock_step_no_state_mutated()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    const auto ongoingAtStart = tracker.getOngoingDuration();
    QVERIFY(ongoingAtStart.has_value());
    const SegmentId segId = ongoingAtStart->segment_id;

    // A lock fires at t0+60s; by the time the LongOngoingLock event is
    // processed, a backward NTP step has moved `now` before the lock time.
    const QDateTime lockTime = t0.addSecs(60);
    const QDateTime nowAtEvent = t0.addSecs(30); // before lockTime

    // Act
    tracker.backpauseTimer_dbg(nowAtEvent, lockTime);

    // Assert: refused -- still Activity, nothing mutated.
    QVERIFY(tracker.isActive());
    const auto ongoingAfterRefusedBackpause = tracker.getOngoingDuration();
    QVERIFY(ongoingAfterRefusedBackpause.has_value());
    QCOMPARE(ongoingAfterRefusedBackpause->startTime, t0);
    QCOMPARE(ongoingAfterRefusedBackpause->segment_id, segId);
    QVERIFY(fakeDb.storedDurations.empty());
}

// C12b: backpauseTimer_dbg (the direct debug entry point) with a null
// lockTime -- production callers never do this; useTimerViaLockEvent
// already rejects a timestamp-less LongOngoingLock before it can reach
// backpauseTimer -- must be a pure no-op.
void TimerTest::test_C12b_backpauseTimer_dbg_invalid_lockTime_is_noop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    const SegmentId segId = tracker.getOngoingDuration()->segment_id;

    clock.advance(60000);

    // Act: direct debug call with an invalid (default) lockTime.
    tracker.backpauseTimer_dbg(clock.now(), QDateTime());

    // Assert: no mutation at all -- still Activity, same segment, nothing
    // stored.
    QVERIFY(tracker.isActive());
    const auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QCOMPARE(ongoing->startTime, t0);
    QCOMPARE(ongoing->segment_id, segId);
    QVERIFY(fakeDb.storedDurations.empty());
}

// C12b: Settings allows the autopause threshold anywhere from 0 to 99
// minutes, but that threshold no longer participates in backpauseTimer at
// all -- the Activity segment always ends exactly at the captured lockTime.
// Cover both boundary values to show the (now-removed) 1s-1h clamp cannot
// resurface.
void TimerTest::test_C12b_backpause_anchors_at_lockTime_for_threshold_boundaries()
{
    for (int thresholdMinutes : {0, 99}) {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString settingsPath = createSettingsFile(tempDir.path(), 7);
        QSettings writer(settingsPath, QSettings::IniFormat);
        writer.setValue("uTimer/autopause_enabled", true);
        writer.setValue("uTimer/autopause_threshold_minutes", thresholdMinutes);
        writer.sync();
        Settings settings(settingsPath);
        FakeSessionStore fakeDb;

        const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
        FakeClock clock(t0);
        TimeTracker tracker(settings, fakeDb);
        tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
        tracker.useTimerViaButton(Button::Start);

        clock.advance(120000); // now = t0 + 120s
        const QDateTime lockTime = t0.addSecs(90); // an arbitrary captured instant

        // Act
        tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, lockTime);

        // Assert: the Activity segment ends exactly at lockTime, regardless
        // of the threshold setting.
        QVERIFY(tracker.isPaused());
        const Timeline timeline = tracker.completedTimeline();
        bool foundActivityEndingAtLockTime = false;
        for (const auto& d : timeline.completed()) {
            if (d.type == DurationType::Activity && d.endTime == lockTime) {
                foundActivityEndingAtLockTime = true;
            }
        }
        QVERIFY(foundActivityEndingAtLockTime);
    }
}

// D2 (R11-003): a DST fall-back is, from the engine's point of view, just
// another backward wall-clock step -- Pause must refuse it the same way.
void TimerTest::test_D2_pause_refuses_dst_fallback_backward_step()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Europe/Berlin 2025 fall-back: 03:00 CEST -> 02:00 CET on Oct 26.
    // Model it directly as the wall-clock repeat it produces: the segment
    // starts at the post-transition reading (02:30 CET) and a later read
    // lands back at the pre-transition reading (02:30 CEST), i.e. an hour
    // earlier in UTC terms -- the same shape as a manual backward step.
    const QDateTime t0(QDate(2025, 10, 26), QTime(2, 30, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    clock.setNow(t0.addSecs(-1800)); // the repeated, earlier-reading 02:00 CET

    // Act
    tracker.useTimerViaButton(Button::Pause);

    // Assert: refused, exactly like any other backward step.
    QVERIFY(tracker.isActive());
    const auto ongoingAfterRefusedPause = tracker.getOngoingDuration();
    QVERIFY(ongoingAfterRefusedPause.has_value());
    QCOMPARE(ongoingAfterRefusedPause->startTime, t0);
}

// D2 (R11-003): a DST spring-forward is a FORWARD step and must not be
// mistaken for a reversal -- Pause proceeds normally.
void TimerTest::test_D2_pause_proceeds_normally_through_dst_springforward()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Europe/Berlin 2025 spring-forward: 02:00 CET jumps to 03:00 CEST on
    // Mar 30. Model the forward jump directly: 01:30 -> 03:15 local.
    const QDateTime t0(QDate(2025, 3, 30), QTime(1, 30, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    const QDateTime afterJump(QDate(2025, 3, 30), QTime(3, 15, 0), Qt::LocalTime);
    clock.setNow(afterJump);

    // Act
    tracker.useTimerViaButton(Button::Pause);

    // Assert: an ordinary Pause -- new segment anchored at the (later, not
    // reversed) current time.
    QVERIFY(tracker.isPaused());
    const auto ongoingAfterPause = tracker.getOngoingDuration();
    QVERIFY(ongoingAfterPause.has_value());
    QCOMPARE(ongoingAfterPause->startTime, afterJump);
}

// D2 (R11-004): a backward wall-clock step before the 23:59:59.500 cutoff
// must not stop the engine early; a later heartbeat that actually reaches
// the cutoff then stops exactly once, at the cutoff.
void TimerTest::test_D2_scheduled_stop_backward_wall_step_before_cutoff_then_stops_once_at_cutoff()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Act: the clock steps backward, still well before the cutoff.
    const QDateTime early(QDate(2025, 6, 1), QTime(8, 0, 0), Qt::LocalTime);
    clock.setNow(early);
    tracker.onTick(clock.now());

    // Assert: no stop.
    QCOMPARE(stoppedSpy.count(), 0);
    QVERIFY(tracker.isActive());

    // Act: a later tick reaches the cutoff.
    const QDateTime cutoff(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(cutoff);
    tracker.onTick(clock.now());

    // Assert: stops now, exactly once, at the cutoff.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightScheduled);
    QVERIFY(tracker.isStopped());
}

// D2 (R11-004): a heartbeat tick landing after the 23:59:59.500 cutoff, but
// still on the SAME civil date (e.g. a small forward jump/late tick), must
// still stop exactly at the cutoff -- not the later `now` -- so the
// committed endpoint is always exactly the cutoff.
void TimerTest::test_D2_scheduled_stop_late_same_day_applies_captured_boundary()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Act: a heartbeat tick lands slightly after the cutoff, same civil date.
    const QDateTime late(QDate(2025, 6, 1), QTime(23, 59, 59, 900), Qt::LocalTime);
    clock.setNow(late);
    tracker.onTick(clock.now());

    // Assert: stopped, and the committed endpoint is exactly the cutoff --
    // not the late `now`.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightScheduled);
    QVERIFY(tracker.isStopped());
    const QDateTime boundary(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    QVERIFY(!fakeDb.storedDurations.empty());
    QCOMPARE(fakeDb.storedDurations.back().endTime, boundary);
}

// D2/MEDIUM-5 (R11-004): a heartbeat tick landing on a FURTHER civil date
// (the canonical cause is suspend across midnight) must NOT credit
// [segment_start, cutoff] -- that would "invent" unattended work time,
// contradicting plan §2's conservative watchdog policy. The scheduled-stop
// check only applies while `now` is still on the segment's own start date,
// so a tick that has already rolled onto a later date takes only the
// watchdog's whole-segment discard, never credit through the prior day's
// cutoff.
void TimerTest::test_D2_scheduled_stop_late_across_date_defers_to_watchdog_discard()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Act: a forward jump (e.g. resume from suspend) lands on the next date.
    // A single tick there both skips the scheduled-stop check (different
    // local date) and runs the watchdog, which discards the whole segment.
    const QDateTime late(QDate(2025, 6, 2), QTime(1, 30, 0), Qt::LocalTime);
    clock.setNow(late);
    tracker.onTick(late);

    // Assert: discarded via the watchdog, not credited through the boundary.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightWatchdog);
    QVERIFY(tracker.isStopped());
}

// ============================================================================
// M6: offset-aware cutoff construction across a DST spring-forward day: the
// cutoff is built from `now`'s own instant (toLocalTime()), so it must stop
// at the correct 23:59:59.500 CEST wall-clock instant, not an offset-blind
// miscalculation.
// ============================================================================

void TimerTest::test_M6_scheduled_stop_offset_aware_across_spring_dst()
{
    // Arrange: Set TZ to Europe/Berlin and construct all QDateTimes after.
    struct TzGuard {
        QByteArray orig;
        TzGuard() : orig(qgetenv("TZ")) {}
        ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
    } tzGuard;
    qputenv("TZ", "Europe/Berlin");
    tzset();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Start at 2025-03-30 01:00 CET (before the spring-forward jump to CEST).
    const QDateTime t0(QDate(2025, 3, 30), QTime(1, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QVERIFY(stoppedSpy.isValid());

    // Act: a heartbeat tick at 23:59:59.500 CEST, the same local date.
    const QDateTime cutoff(QDate(2025, 3, 30), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(cutoff);
    tracker.onTick(clock.now());

    // Assert: stops exactly at that cutoff.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightScheduled);
    QVERIFY(tracker.isStopped());
    QVERIFY(!fakeDb.storedDurations.empty());
    QCOMPARE(fakeDb.storedDurations.back().endTime, cutoff);
}

// ============================================================================
// C5: Start/Pause/unpause pressed AT OR AFTER the nominal 23:59:59.500
// cutoff. The segment's own start is at/after the cutoff, so the stop must
// happen at the first later heartbeat's `now`, never at a cutoff earlier
// than the segment's own start, and a same-instant tick must do nothing.
// ============================================================================

void TimerTest::test_C5_start_after_cutoff_stops_at_first_later_tick_not_before_own_start()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 59, 59, 600), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    // Act: a same-instant tick does nothing.
    tracker.onTick(clock.now());
    QCOMPARE(stoppedSpy.count(), 0);
    QVERIFY(tracker.isActive());

    // Act: a later tick, still the same civil date, stops.
    const QDateTime later(QDate(2025, 6, 1), QTime(23, 59, 59, 700), Qt::LocalTime);
    clock.setNow(later);
    tracker.onTick(clock.now());

    // Assert: stopped at `later`, never at a cutoff before the segment's
    // own start (23:59:59.600).
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightScheduled);
    QVERIFY(tracker.isStopped());
    QVERIFY(!fakeDb.storedDurations.empty());
    QCOMPARE(fakeDb.storedDurations.back().startTime, t0);
    QCOMPARE(fakeDb.storedDurations.back().endTime, later);
}

void TimerTest::test_C5_pause_after_cutoff_stops_at_first_later_tick_not_before_own_start()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    const QDateTime pauseStart(QDate(2025, 6, 1), QTime(23, 59, 59, 600), Qt::LocalTime);
    clock.setNow(pauseStart);
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    // Act: a same-instant tick does nothing.
    tracker.onTick(clock.now());
    QCOMPARE(stoppedSpy.count(), 0);
    QVERIFY(tracker.isPaused());

    // Act: a later tick, still the same civil date, stops.
    const QDateTime later(QDate(2025, 6, 1), QTime(23, 59, 59, 700), Qt::LocalTime);
    clock.setNow(later);
    tracker.onTick(clock.now());

    // Assert: stopped at `later`, never at a cutoff before the Pause
    // segment's own start.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightScheduled);
    QVERIFY(tracker.isStopped());
    bool foundPause = false;
    for (const auto& seg : fakeDb.storedDurations) {
        if (seg.type == DurationType::Pause && seg.startTime == pauseStart && seg.endTime == later)
            foundPause = true;
    }
    QVERIFY2(foundPause, "Expected the Pause segment credited through `later`, not an earlier cutoff");
}

void TimerTest::test_C5_unpause_after_cutoff_stops_at_first_later_tick_not_before_own_start()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(23, 30, 0), Qt::LocalTime));
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());

    const QDateTime unpauseStart(QDate(2025, 6, 1), QTime(23, 59, 59, 600), Qt::LocalTime);
    clock.setNow(unpauseStart);
    tracker.useTimerViaButton(Button::Start); // unpause
    QVERIFY(tracker.isActive());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    // Act: a same-instant tick does nothing.
    tracker.onTick(clock.now());
    QCOMPARE(stoppedSpy.count(), 0);
    QVERIFY(tracker.isActive());

    // Act: a later tick, still the same civil date, stops.
    const QDateTime later(QDate(2025, 6, 1), QTime(23, 59, 59, 700), Qt::LocalTime);
    clock.setNow(later);
    tracker.onTick(clock.now());

    // Assert: stopped at `later`, never at a cutoff before the resumed
    // Activity segment's own start.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightScheduled);
    QVERIFY(tracker.isStopped());
    bool foundResumedActivity = false;
    for (const auto& seg : fakeDb.storedDurations) {
        if (seg.type == DurationType::Activity && seg.startTime == unpauseStart && seg.endTime == later)
            foundResumedActivity = true;
    }
    QVERIFY2(foundResumedActivity, "Expected the resumed Activity segment credited through `later`");
}

// C5: a Qt::OffsetFromUTC now-provider must have its cutoff computed from
// the instant's true local calendar date (via toLocalTime()), not from the
// offset representation's own raw date/time fields.
void TimerTest::test_C5_offset_from_utc_now_provider_computes_cutoff_by_local_date()
{
    struct TzGuard {
        QByteArray orig;
        TzGuard() : orig(qgetenv("TZ")) {}
        ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
    } tzGuard;
    qputenv("TZ", "Europe/Berlin"); // CET, UTC+1 in January -- no DST ambiguity.
    tzset();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // This instant is 2025-01-16 00:00:00 CET (system local), even though
    // its own OffsetFromUTC(0) fields read 2025-01-15 23:00:00.
    const QDateTime t0(QDate(2025, 1, 15), QTime(23, 0, 0), Qt::OffsetFromUTC, 0);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    // This instant is 2025-01-16 23:59:59.500 CET -- the correct cutoff for
    // the segment's true local start date (2025-01-16) -- expressed via the
    // same OffsetFromUTC(0) representation.
    const QDateTime cutoff(QDate(2025, 1, 16), QTime(22, 59, 59, 500), Qt::OffsetFromUTC, 0);
    clock.setNow(cutoff);
    tracker.onTick(clock.now());

    // Assert: stops exactly at that cutoff instant.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightScheduled);
    QVERIFY(tracker.isStopped());
    QVERIFY(!fakeDb.storedDurations.empty());
    QCOMPARE(fakeDb.storedDurations.back().endTime, cutoff);
}

// C5: the half-open isCrossMidnight rule -- a tick at exactly local
// 00:00:00 (the exclusive end of the segment's start date) may revoke an
// open History edit (forward-date check) but must not yet discard the
// segment; the first tick strictly beyond midnight does.
void TimerTest::test_C5_tick_at_exact_midnight_does_not_discard_next_tick_does()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());

    QSignalSpy revokedSpy(&tracker, &TimeTracker::exclusiveEditRevoked);
    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    // Act: a tick at exactly the exclusive end of the start date.
    const QDateTime exactMidnight(QDate(2025, 6, 2), QTime(0, 0, 0), Qt::LocalTime);
    clock.setNow(exactMidnight);
    tracker.onTick(clock.now());

    // Assert: the forward-date History revoke ran, but the segment is not
    // discarded -- exactly at the boundary is still valid, half-open.
    QCOMPARE(revokedSpy.count(), 1);
    QCOMPARE(stoppedSpy.count(), 0);
    QVERIFY(tracker.isActive());

    // Act: the first tick strictly beyond midnight.
    const QDateTime pastMidnight(QDate(2025, 6, 2), QTime(0, 0, 0, 100), Qt::LocalTime);
    clock.setNow(pastMidnight);
    tracker.onTick(clock.now());

    // Assert: now discarded via the watchdog.
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightWatchdog);
    QVERIFY(tracker.isStopped());
}

// C5: repeated ticks after the cutoff must yield exactly one stopped()
// emission -- no duplicate stop.
void TimerTest::test_C5_repeated_ticks_after_cutoff_yield_exactly_one_stop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    const QDateTime cutoff(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(cutoff);
    tracker.onTick(clock.now());
    QCOMPARE(stoppedSpy.count(), 1);
    QVERIFY(tracker.isStopped());

    // Act: repeated ticks at and after the cutoff.
    tracker.onTick(clock.now());
    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(23, 59, 59, 600), Qt::LocalTime));
    tracker.onTick(clock.now());
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime));
    tracker.onTick(clock.now());

    // Assert: still exactly one stopped() emission.
    QCOMPARE(stoppedSpy.count(), 1);
}

// ============================================================================
// D5 (R15-002): cancelShutdown() atomically clears the tracker's terminal
// flag after a cancelled OS shutdown, without reconstructing/resuming the
// ended session and without disturbing pending persistence.
// ============================================================================

void TimerTest::test_D5_cancelShutdown_after_active_shutdown_stays_stopped_not_resumed()
{
    // Arrange: an active session that shutdown() closes (e.g. WM_QUERYENDSESSION).
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore db;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    tracker.shutdown();
    QVERIFY(tracker.wasShutDown());
    QVERIFY(tracker.isStopped());
    const qint64 activeTimeAfterShutdown = tracker.getActiveTime();

    // Act: the OS shutdown is cancelled.
    tracker.cancelShutdown();

    // Assert: terminal state clears, but the engine remains an ordinary
    // stopped session -- no live/ongoing segment is reconstructed or resumed.
    QVERIFY(!tracker.wasShutDown());
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.getOngoingDuration().has_value());
    QCOMPARE(tracker.getActiveTime(), activeTimeAfterShutdown);
}

void TimerTest::test_D5_cancelShutdown_while_already_stopped()
{
    // Arrange: shutdown() runs on a tracker that was never started.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore db;
    TimeTracker tracker(settings, db);

    tracker.shutdown();
    QVERIFY(tracker.wasShutDown());
    QVERIFY(tracker.isStopped());

    // Act
    tracker.cancelShutdown();

    // Assert
    QVERIFY(!tracker.wasShutDown());
    QVERIFY(tracker.isStopped());
}

void TimerTest::test_D5_cancelShutdown_retains_pending_commit_for_next_start()
{
    // Arrange: a Stop-time commit failure leaves a pending payload, then
    // shutdown() (e.g. via ShutdownCoordinator::run()) marks the tracker
    // terminal without resolving it.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.hasPendingCommit_dbg());
    tracker.shutdown();
    QVERIFY(tracker.wasShutDown());
    const int commitAttemptsBeforeCancel = fakeDb.callLog.count("commitSession");

    // Act
    tracker.cancelShutdown();

    // Assert: cancellation neither loses nor retries the payload itself --
    // A2/A3 still own resolving it, at the next Start.
    QVERIFY(!tracker.wasShutDown());
    QVERIFY(tracker.hasPendingCommit_dbg());
    QCOMPARE(fakeDb.callLog.count("commitSession"), commitAttemptsBeforeCancel);

    // Cleanup: resolve the payload so destruction finds no unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// ============================================================================
// D10 (R10-001/R10-002): one typed input dispatcher, and one cause-carrying
// transition event sufficient for a UI reducer.
// ============================================================================

void TimerTest::test_D10_transitioned_emits_one_event_per_real_transition_data()
{
    QTest::addColumn<int>("scenario");
    QTest::addColumn<TimeTracker::Mode>("expectedFrom");
    QTest::addColumn<TimeTracker::Mode>("expectedTo");
    QTest::addColumn<TimeTracker::TransitionCause>("expectedCause");

    QTest::newRow("button_start_from_none")
        << 0 << TimeTracker::Mode::None << TimeTracker::Mode::Activity << TimeTracker::TransitionCause::ButtonStart;
    QTest::newRow("button_pause_from_activity")
        << 1 << TimeTracker::Mode::Activity << TimeTracker::Mode::Pause << TimeTracker::TransitionCause::ButtonPause;
    QTest::newRow("button_resume_from_pause")
        << 2 << TimeTracker::Mode::Pause << TimeTracker::Mode::Activity << TimeTracker::TransitionCause::ButtonStart;
    QTest::newRow("button_stop_from_activity")
        << 3 << TimeTracker::Mode::Activity << TimeTracker::Mode::None << TimeTracker::TransitionCause::ButtonStop;
    QTest::newRow("button_stop_from_pause")
        << 4 << TimeTracker::Mode::Pause << TimeTracker::Mode::None << TimeTracker::TransitionCause::ButtonStop;
    QTest::newRow("lock_autopause_from_activity")
        << 5 << TimeTracker::Mode::Activity << TimeTracker::Mode::Pause << TimeTracker::TransitionCause::LockAutopause;
    QTest::newRow("lock_resume_from_pause")
        << 6 << TimeTracker::Mode::Pause << TimeTracker::Mode::Activity << TimeTracker::TransitionCause::LockResume;
}

void TimerTest::test_D10_transitioned_emits_one_event_per_real_transition()
{
    QFETCH(int, scenario);
    QFETCH(TimeTracker::Mode, expectedFrom);
    QFETCH(TimeTracker::Mode, expectedTo);
    QFETCH(TimeTracker::TransitionCause, expectedCause);

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    if (scenario == 5 || scenario == 6) {
        QSettings writer(settingsPath, QSettings::IniFormat);
        writer.setValue("uTimer/autopause_enabled", true);
        writer.setValue("uTimer/autopause_threshold_minutes", 15);
        writer.sync();
    }
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    switch (scenario) {
    case 0: // already None
    case 3: break;
    case 1:
    case 4:
        tracker.useTimerViaButton(Button::Start);
        if (scenario == 4) tracker.useTimerViaButton(Button::Pause);
        break;
    case 2:
        tracker.useTimerViaButton(Button::Start);
        tracker.useTimerViaButton(Button::Pause);
        break;
    case 5:
        tracker.useTimerViaButton(Button::Start);
        break;
    case 6:
        tracker.useTimerViaButton(Button::Start);
        tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, clock.now());
        break;
    }
    if (scenario == 3) tracker.useTimerViaButton(Button::Start);

    QCOMPARE(tracker.isActive(), expectedFrom == TimeTracker::Mode::Activity);
    QCOMPARE(tracker.isPaused(), expectedFrom == TimeTracker::Mode::Pause);
    QCOMPARE(tracker.isStopped(), expectedFrom == TimeTracker::Mode::None);

    QSignalSpy spy(&tracker, &TimeTracker::transitioned);

    // Act
    switch (scenario) {
    case 0:
    case 2: tracker.useTimerViaButton(Button::Start); break;
    case 1: tracker.useTimerViaButton(Button::Pause); break;
    case 3:
    case 4: tracker.useTimerViaButton(Button::Stop); break;
    case 5: tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, clock.now()); break;
    case 6: tracker.useTimerViaLockEvent(LockEvent::Unlock); break;
    }

    // Assert
    QCOMPARE(spy.count(), 1);
    const auto t = spy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, expectedFrom);
    QCOMPARE(t.to, expectedTo);
    QCOMPARE(t.cause, expectedCause);
}

void TimerTest::test_D10_noop_inputs_emit_no_transitioned_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("pause_from_none") << 0;
    QTest::newRow("pause_from_pause") << 1;
    QTest::newRow("start_from_activity") << 2;
    QTest::newRow("stop_from_none") << 3;
}

void TimerTest::test_D10_noop_inputs_emit_no_transitioned()
{
    QFETCH(int, scenario);

    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    Button action = Button::Pause;
    switch (scenario) {
    case 0: action = Button::Pause; break;
    case 1:
        tracker.useTimerViaButton(Button::Start);
        tracker.useTimerViaButton(Button::Pause);
        action = Button::Pause;
        break;
    case 2:
        tracker.useTimerViaButton(Button::Start);
        action = Button::Start;
        break;
    case 3:
        action = Button::Stop;
        break;
    }

    QSignalSpy spy(&tracker, &TimeTracker::transitioned);

    // Act
    tracker.useTimerViaButton(action);

    // Assert
    QCOMPARE(spy.count(), 0);
}

void TimerTest::test_D10_onStartPausePressed_delegates_to_button_dispatch_data()
{
    QTest::addColumn<TimeTracker::Mode>("startMode");
    QTest::newRow("from_none") << TimeTracker::Mode::None;
    QTest::newRow("from_pause") << TimeTracker::Mode::Pause;
    QTest::newRow("from_activity") << TimeTracker::Mode::Activity;
}

void TimerTest::test_D10_onStartPausePressed_delegates_to_button_dispatch()
{
    QFETCH(TimeTracker::Mode, startMode);

    // Arrange: two independently-arranged trackers in the identical
    // precondition mode, one driven via the production toggle slot, the
    // other via the direct button dispatcher.
    QTemporaryDir tempDirA;
    QTemporaryDir tempDirB;
    QVERIFY(tempDirA.isValid());
    QVERIFY(tempDirB.isValid());
    Settings settingsA(createSettingsFile(tempDirA.path(), 7));
    Settings settingsB(createSettingsFile(tempDirB.path(), 7));
    FakeSessionStore dbA;
    FakeSessionStore dbB;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clockA(t0);
    FakeClock clockB(t0);

    TimeTracker trackerToggle(settingsA, dbA);
    trackerToggle.setNowProvider_dbg([&clockA]() { return clockA.now(); });
    TimeTracker trackerButton(settingsB, dbB);
    trackerButton.setNowProvider_dbg([&clockB]() { return clockB.now(); });

    if (startMode != TimeTracker::Mode::None) {
        trackerToggle.useTimerViaButton(Button::Start);
        trackerButton.useTimerViaButton(Button::Start);
    }
    if (startMode == TimeTracker::Mode::Pause) {
        trackerToggle.useTimerViaButton(Button::Pause);
        trackerButton.useTimerViaButton(Button::Pause);
    }

    QSignalSpy toggleSpy(&trackerToggle, &TimeTracker::transitioned);
    QSignalSpy buttonSpy(&trackerButton, &TimeTracker::transitioned);

    // Act
    trackerToggle.onStartPausePressed();
    const Button equivalent = (startMode == TimeTracker::Mode::Activity) ? Button::Pause : Button::Start;
    trackerButton.useTimerViaButton(equivalent);

    // Assert: identical transition, identical resulting mode.
    QCOMPARE(toggleSpy.count(), buttonSpy.count());
    QCOMPARE(toggleSpy.count(), 1);
    const auto a = toggleSpy.first().first().value<TimeTracker::Transition>();
    const auto b = buttonSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(a.from, b.from);
    QCOMPARE(a.to, b.to);
    QCOMPARE(a.cause, b.cause);
    QCOMPARE(trackerToggle.isActive(), trackerButton.isActive());
    QCOMPARE(trackerToggle.isPaused(), trackerButton.isPaused());
    QCOMPARE(trackerToggle.isStopped(), trackerButton.isStopped());
}

void TimerTest::test_D10_longlock_disabled_autopause_dialog_open_is_dropped_not_deferred()
{
    // Arrange: autopause explicitly disabled, Activity running, History
    // dialog open.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", false);
    writer.sync();
    Settings settings(settingsPath);
    QVERIFY(!settings.isAutopauseEnabled());
    FakeSessionStore fakeDb;
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    tracker.beginExclusiveEdit();

    // Act: a LongOngoingLock arrives while the dialog is open, autopause off.
    const QDateTime lockTime(QDate(2025, 6, 1), QTime(9, 30, 0), Qt::LocalTime);
    clock.setNow(lockTime);
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, lockTime);

    // Assert: dropped outright -- exactly the non-dialog-open behavior --
    // not captured for a later replay that would resurrect it if autopause
    // is subsequently re-enabled. The dialog is also already revoked (WP5),
    // since any real lock-watcher event closes it before this handler runs.
    QVERIFY(!tracker.isDialogOpen_dbg());

    tracker.endExclusiveEdit();
    QVERIFY(tracker.isActive());
}

// C12b: a LongOngoingLock with no captured timestamp is a malformed
// external/test event -- production's LockStateWatcher always stamps a real
// instant -- and must be rejected before ANY of useTimerViaLockEvent's other
// effects: no History revoke, no autopause mutation, no midnight discard.
void TimerTest::test_C12b_invalid_longlock_no_timestamp_is_ignored_no_revoke()
{
    // Arrange: Active, autopause enabled, History dialog open.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.sync();
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());

    QSignalSpy revokeSpy(&tracker, &TimeTracker::exclusiveEditRevoked);
    QSignalSpy warnSpy(&tracker, &TimeTracker::userWarning);

    // Act: a malformed LongOngoingLock -- default-constructed lockTime.
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock);

    // Assert: entirely ignored -- no revoke, no warning, dialog still open,
    // still Active, nothing stored.
    QCOMPARE(revokeSpy.count(), 0);
    QCOMPARE(warnSpy.count(), 0);
    QVERIFY(tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isActive());
    QVERIFY(fakeDb.storedDurations.empty());

    tracker.endExclusiveEdit();

    // A later real Unlock must not restart -- pending-resume was never set
    // by the ignored malformed event.
    QSignalSpy spy(&tracker, &TimeTracker::transitioned);
    tracker.useTimerViaLockEvent(LockEvent::Unlock);
    QCOMPARE(spy.count(), 0);
    QVERIFY(tracker.isActive());
}

// Round-2 review (finding 1): a live scheduled midnight stop, driven through
// the real heartbeat tick (TimeTracker::onTick), must emit
// transitioned(..., MidnightScheduled) exactly once -- not mis-mapped to
// MidnightWatchdog or dropped entirely.
void TimerTest::test_D10_midnight_scheduled_live_emits_transitioned()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    const QDateTime boundary(QDate(2025, 6, 1), QTime(23, 59, 59, 500), Qt::LocalTime);
    clock.setNow(boundary);
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);
    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    // Act: the real heartbeat tick fires at the boundary.
    tracker.onTick(clock.now());

    // Assert
    QVERIFY(tracker.isStopped());
    QCOMPARE(transitionedSpy.count(), 1);
    const auto t = transitionedSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Activity);
    QCOMPARE(t.to, TimeTracker::Mode::None);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::MidnightScheduled);
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightScheduled);
}

// Round-2 review (finding 1): a live watchdog cross-midnight discard, driven
// through the REAL production onTick() slot (not the discardCrossMidnightOngoingAndStop_dbg
// bypass other watchdog tests use), must emit transitioned(..., MidnightWatchdog)
// exactly once -- not mis-mapped to MidnightScheduled.
void TimerTest::test_D10_midnight_watchdog_live_emits_transitioned()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    const QDateTime postMidnight(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::LocalTime);
    clock.setNow(postMidnight);
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);
    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    // Act: the real 100ms heartbeat slot detects the missed boundary.
    tracker.onTick(postMidnight);

    // Assert
    QVERIFY(tracker.isStopped());
    QCOMPARE(transitionedSpy.count(), 1);
    const auto t = transitionedSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Activity);
    QCOMPARE(t.to, TimeTracker::Mode::None);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::MidnightWatchdog);
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::MidnightWatchdog);
}

// Round-2 review (finding 1): shutdown()/drainForShutdown() must emit
// transitioned(..., Shutdown) -- not mis-mapped to ButtonStop by either the
// cause derivation or the legacy stopped() StopReason translation.
void TimerTest::test_D10_shutdown_emits_transitioned()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);
    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    // Act
    tracker.shutdown();

    // Assert
    QVERIFY(tracker.isStopped());
    QCOMPARE(transitionedSpy.count(), 1);
    const auto t = transitionedSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Activity);
    QCOMPARE(t.to, TimeTracker::Mode::None);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::Shutdown);
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::Shutdown);
}

// Round-2 review (finding 1): a Fatal commit failure during a Button::Stop
// press must NOT relabel the transition's cause as FatalStoreError --
// stopTimer() never nests through stopIfLatched() (unlike startTimer's Pause
// branch and finalizePauseTransition); the button's own reason is what
// actually happened, while the latch still takes effect for the NEXT Start.
void TimerTest::test_D10_fatal_commit_on_stop_keeps_buttonstop_cause()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(60ll * 1000ll); // non-zero duration so commitSession is actually called

    fakeDb.commitSessionResult = SessionStoreResult::fatal("injected fatal commit on stop");
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    // Act
    tracker.useTimerViaButton(Button::Stop);

    // Assert: latched for the future, but THIS transition truthfully
    // reports what the user actually did.
    QVERIFY(tracker.isFatalLatched());
    QVERIFY(tracker.isStopped());
    QCOMPARE(transitionedSpy.count(), 1);
    const auto t = transitionedSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(t.from, TimeTracker::Mode::Activity);
    QCOMPARE(t.to, TimeTracker::Mode::None);
    QCOMPARE(t.cause, TimeTracker::TransitionCause::ButtonStop);

    // Cleanup: latched, so no further Start; resolve the retained payload.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    tracker.drainForShutdown();
}

// Round-2 review (finding 1, M8b): the delegation test above only exercises
// the three ordinary mode rows -- a hand-rolled toggle body that dropped
// useTimerViaButton()'s cross-midnight-discard guard would still pass all
// three. Cover it explicitly: an ongoing segment that has crossed midnight
// must be force-stopped by onStartPausePressed() exactly as by
// useTimerViaButton(), regardless of which button the toggle would have
// picked.
void TimerTest::test_D10_onStartPausePressed_cross_midnight_discard_matches_button_dispatch()
{
    // Arrange: two trackers, each with an Activity segment that has silently
    // crossed midnight (started yesterday, "now" is today).
    QTemporaryDir tempDirA;
    QTemporaryDir tempDirB;
    QVERIFY(tempDirA.isValid());
    QVERIFY(tempDirB.isValid());
    Settings settingsA(createSettingsFile(tempDirA.path(), 7));
    Settings settingsB(createSettingsFile(tempDirB.path(), 7));
    FakeSessionStore dbA;
    FakeSessionStore dbB;
    const QDateTime yesterday(QDate(2025, 6, 1), QTime(23, 0, 0), Qt::LocalTime);
    const QDateTime today(QDate(2025, 6, 2), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clockA(yesterday);
    FakeClock clockB(yesterday);

    TimeTracker trackerToggle(settingsA, dbA);
    trackerToggle.setNowProvider_dbg([&clockA]() { return clockA.now(); });
    TimeTracker trackerButton(settingsB, dbB);
    trackerButton.setNowProvider_dbg([&clockB]() { return clockB.now(); });

    trackerToggle.useTimerViaButton(Button::Start);
    trackerButton.useTimerViaButton(Button::Start);
    clockA.setNow(today);
    clockB.setNow(today);

    QSignalSpy toggleSpy(&trackerToggle, &TimeTracker::transitioned);
    QSignalSpy buttonSpy(&trackerButton, &TimeTracker::transitioned);

    // Act: toggle would have picked Pause (mode_ is Activity), but the
    // cross-midnight guard must force a stop instead, exactly like Button::Pause
    // routed through useTimerViaButton() would.
    trackerToggle.onStartPausePressed();
    trackerButton.useTimerViaButton(Button::Pause);

    // Assert: both forced to None via the discard path, not Pause.
    QVERIFY(trackerToggle.isStopped());
    QVERIFY(trackerButton.isStopped());
    QCOMPARE(toggleSpy.count(), buttonSpy.count());
    QCOMPARE(toggleSpy.count(), 1);
    const auto a = toggleSpy.first().first().value<TimeTracker::Transition>();
    const auto b = buttonSpy.first().first().value<TimeTracker::Transition>();
    QCOMPARE(a.to, TimeTracker::Mode::None);
    QCOMPARE(a.cause, TimeTracker::TransitionCause::MidnightWatchdog);
    QCOMPARE(a.cause, b.cause);
    QCOMPARE(a.to, b.to);
}

void TimerTest::test_shutdown_with_history_open_revokes_before_stopping()
{
    // Arrange: event recording vector declared before the tracker (lifetime rule).
    QStringList events;
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime));
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore db;
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Start and begin exclusive edit.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());

    // Connect to record event order.
    QObject::connect(&tracker, &TimeTracker::exclusiveEditRevoked, [&events]() {
        events.append("revoked");
    });
    QObject::connect(&tracker, &TimeTracker::transitioned, [&events](const TimeTracker::Transition& t) {
        if (t.to == TimeTracker::Mode::None) {
            events.append(QString("transitioned_none_%1").arg(int(t.cause)));
        }
    });
    QSignalSpy userWarningSpy(&tracker, &TimeTracker::userWarning);

    // Act: shutdown.
    tracker.shutdown();

    // Assert: revoked once, before transition to None with cause Shutdown.
    QCOMPARE(events.count(), 2);
    QCOMPARE(events.at(0), "revoked");
    QCOMPARE(events.at(1), QString("transitioned_none_%1").arg(int(TimeTracker::TransitionCause::Shutdown)));
    QVERIFY(!tracker.isDialogOpen_dbg());
    QCOMPARE(userWarningSpy.count(), 1);

    // Act: second shutdown.
    userWarningSpy.clear();
    events.clear();
    tracker.shutdown();

    // Assert: no additional revoke.
    QCOMPARE(events.count(), 0);
    QCOMPARE(userWarningSpy.count(), 0);
}

// ============================================================================
// WP3 (step6_plan_revised.md): beginExclusiveEdit() context, per-date
// eligibility (HistoryEligibility::isEditable) and saveHistoryEdit().
// ============================================================================

void TimerTest::test_WP3_today_readonly_while_active_and_paused_past_and_future_editable()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Act: open while Active.
    const HistoryEditContext ctxActive = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert: Today is read-only, a past date and a future-dated page are editable.
    QVERIFY(!ctxActive.isEditable(today));
    QVERIFY(ctxActive.isEditable(today.addDays(-1)));
    QVERIFY(ctxActive.isEditable(today.addDays(1)));

    // Arrange: Pause.
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());

    // Act: open while Pause.
    const HistoryEditContext ctxPaused = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert: Today is still read-only in Pause; past/future stay editable.
    QVERIFY(!ctxPaused.isEditable(today));
    QVERIFY(ctxPaused.isEditable(today.addDays(-1)));
    QVERIFY(ctxPaused.isEditable(today.addDays(1)));
}

void TimerTest::test_WP3_today_editable_after_accepted_stop()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(3600000);
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());
    QVERIFY(!fakeDb.storedDurations.empty());

    // Act
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert
    QVERIFY(ctx.isEditable(today));
}

void TimerTest::test_WP3_today_readonly_stopped_empty_history()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    QVERIFY(tracker.isStopped());

    // Act
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert: stopped, but no finalized row fully inside today -> read-only.
    QVERIFY(!ctx.isEditable(today));
}

void TimerTest::test_WP3_today_readonly_transient_error_load()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.loadDurationsResult.status = SessionStoreResult::TransientError;
    const QDate today(2025, 6, 15);
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    QVERIFY(tracker.isStopped());

    // Act
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert
    QCOMPARE(ctx.stored.status, SessionStoreResult::TransientError);
    QVERIFY(!ctx.isEditable(today));
    QVERIFY(!ctx.isEditable(today.addDays(-1)));
}

void TimerTest::test_WP3_today_readonly_only_cross_midnight_continuation()
{
    // Arrange: a legacy finalized row spanning local midnight into today.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    const QDateTime legacyStart(today.addDays(-1), QTime(23, 30, 0), Qt::LocalTime);
    const QDateTime legacyEnd(today, QTime(0, 30, 0), Qt::LocalTime);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, legacyStart, legacyEnd));
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    QVERIFY(tracker.isStopped());

    // Act
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert: the only "today" row is a continuation, not fully inside today.
    QVERIFY(!ctx.isEditable(today));
}

void TimerTest::test_WP3_today_readonly_disabled_history_load()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.loadDurationsResult.status = SessionStoreResult::Disabled;
    const QDate today(2025, 6, 15);
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    QVERIFY(tracker.isStopped());

    // Act
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert
    QCOMPARE(ctx.stored.status, SessionStoreResult::Disabled);
    QVERIFY(!ctx.isEditable(today));
}

void TimerTest::test_WP3_today_readonly_failed_stop_with_pending_commit()
{
    // Arrange: a Stop whose persistence fails leaves a pending commit
    // payload covering today.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.commitSessionResult = SessionStoreResult::transient("boom");
    const QDate today(2025, 6, 15);
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(3600000);
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Act
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert: the unpersisted pending-commit row overlaps today.
    QVERIFY(!ctx.pendingCommit.empty());
    QVERIFY(!ctx.isEditable(today));

    // Cleanup: shutdown() so destruction finds no unresolved orchestration bug.
    tracker.shutdown();
}

void TimerTest::test_WP3_today_readonly_leftover_unfinalized_row()
{
    // Arrange: an unfinalized checkpoint row sits on today, with no other
    // activity at all.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    const QDateTime cpStart(today, QTime(8, 0, 0), Qt::LocalTime);
    const QDateTime cpEnd(today, QTime(8, 5, 0), Qt::LocalTime);
    fakeDb.historyUnfinalizedRows.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, cpStart, cpEnd));
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    QVERIFY(tracker.isStopped());

    // Act
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert
    QVERIFY(!ctx.isEditable(today));
}

void TimerTest::test_WP3_failed_midnight_stop_then_open_next_day_yesterday_readonly_today_unaffected()
{
    // Arrange: an Activity segment fails to persist at the 23:59:59.500
    // scheduled stop, leaving a pending-commit payload for yesterday.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate yesterday(2025, 6, 15);
    const QDate today(2025, 6, 16);
    FakeClock clock(QDateTime(yesterday, QTime(23, 50, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    fakeDb.commitSessionResult = SessionStoreResult::transient("boom");
    clock.setNow(QDateTime(yesterday, QTime(23, 59, 59, 500), Qt::LocalTime));
    tracker.onTick(clock.now());
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Arrange: a finalized row fully inside today, so today's own permission
    // (independent of the leftover pending commit) can be observed as true.
    const QDateTime todayStart(today, QTime(7, 0, 0), Qt::LocalTime);
    const QDateTime todayEnd(today, QTime(8, 0, 0), Qt::LocalTime);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, todayStart, todayEnd));
    clock.setNow(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));

    // Act
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert: yesterday is blocked by the leftover pending commit; today is
    // unaffected by it (editable on its own, ordinary merits).
    QVERIFY(!ctx.isEditable(yesterday));
    QVERIFY(ctx.isEditable(today));

    // Cleanup: shutdown() so destruction finds no unresolved orchestration bug.
    tracker.shutdown();
}

void TimerTest::test_WP3_ongoing_end_time_equals_openedAt_single_clock_read()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(120000);

    // Act
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    // Assert: the ongoing row's end is exactly the one clock read this
    // context was built from.
    QVERIFY(ctx.live.ongoing().has_value());
    QCOMPARE(ctx.live.ongoing()->endTime, ctx.openedAt);
    QCOMPARE(ctx.openedAt, clock.now());
}

void TimerTest::test_WP3_saveHistoryEdit_forwards_snapshot_and_days_state_untouched()
{
    // Arrange: a stopped tracker with a finalized past-day row.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    const QDate pastDate(2025, 6, 10);
    const QDateTime pastStart(pastDate, QTime(9, 0, 0), Qt::LocalTime);
    const QDateTime pastEnd(pastDate, QTime(10, 0, 0), Qt::LocalTime);
    const TimeDuration pastRow =
        TimeDuration::fromPersistedRow(DurationType::Activity, pastStart, pastEnd);
    fakeDb.storedDurations.push_back(pastRow);
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    QVERIFY(tracker.isStopped());

    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    QVERIFY(ctx.isEditable(pastDate));

    HistoryDayChange day;
    day.date = pastDate;
    day.dayStartUtc = DayValidity::localDayStart(pastDate).toUTC();
    day.dayEndUtc = DayValidity::localDayEnd(pastDate).toUTC();
    day.upserts.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, pastStart, pastEnd, pastRow.segment_id));
    const std::vector<HistoryDayChange> days{day};

    const auto modeBefore = tracker.isActive() ? TimeTracker::Mode::Activity
        : tracker.isPaused() ? TimeTracker::Mode::Pause : TimeTracker::Mode::None;
    const auto durationsBefore = tracker.sessionState_dbg().durations;
    const bool pendingBefore = tracker.hasPendingCommit_dbg();

    // Act
    const SessionStoreResult result = tracker.saveHistoryEdit(ctx, days);

    // Assert: the exact snapshot and days reached the store unchanged.
    QVERIFY(result.ok());
    QCOMPARE(fakeDb.lastHistorySaveSnapshot.size(), ctx.stored.rows.size());
    for (size_t i = 0; i < ctx.stored.rows.size(); ++i) {
        QCOMPARE(fakeDb.lastHistorySaveSnapshot[i].segmentId, ctx.stored.rows[i].segmentId);
        QCOMPARE(fakeDb.lastHistorySaveSnapshot[i].startUtc, ctx.stored.rows[i].startUtc);
        QCOMPARE(fakeDb.lastHistorySaveSnapshot[i].endUtc, ctx.stored.rows[i].endUtc);
        QCOMPARE(fakeDb.lastHistorySaveSnapshot[i].isFinalized, ctx.stored.rows[i].isFinalized);
    }
    QCOMPARE(fakeDb.lastHistorySaveDays.size(), (size_t)1);
    QCOMPARE(fakeDb.lastHistorySaveDays[0].date, day.date);
    QCOMPARE(fakeDb.lastHistorySaveDays[0].dayStartUtc, day.dayStartUtc);
    QCOMPARE(fakeDb.lastHistorySaveDays[0].dayEndUtc, day.dayEndUtc);
    QCOMPARE(fakeDb.lastHistorySaveDays[0].upserts.size(), (size_t)1);
    QCOMPARE(fakeDb.lastHistorySaveDays[0].upserts[0].segment_id, pastRow.segment_id);

    // Assert: engine memory/mode/pending-commit are untouched by the save.
    const auto modeAfter = tracker.isActive() ? TimeTracker::Mode::Activity
        : tracker.isPaused() ? TimeTracker::Mode::Pause : TimeTracker::Mode::None;
    QCOMPARE(modeAfter, modeBefore);
    QCOMPARE(tracker.sessionState_dbg().durations.size(), durationsBefore.size());
    QCOMPARE(tracker.hasPendingCommit_dbg(), pendingBefore);
}

void TimerTest::test_WP3_saveHistoryEdit_date_not_editable_at_open_is_callerbug_no_store_call()
{
    // Arrange: Today is not editable while Active.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    QVERIFY(!ctx.isEditable(today));

    HistoryDayChange day;
    day.date = today;
    day.dayStartUtc = DayValidity::localDayStart(today).toUTC();
    day.dayEndUtc = DayValidity::localDayEnd(today).toUTC();
    const std::vector<HistoryDayChange> days{day};
    const int saveCallsBefore = fakeDb.callLog.count("saveHistoryDays");

    // Act
    const SessionStoreResult result = tracker.saveHistoryEdit(ctx, days);
    tracker.endExclusiveEdit();

    // Assert
    QCOMPARE(result.category, SessionStoreResult::CallerBug);
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), saveCallsBefore);
}

void TimerTest::test_WP3_saveHistoryEdit_backward_clock_date_change_is_conflict_no_store_call()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    const QDate pastDate(2025, 6, 10);
    fakeDb.storedDurations.push_back(TimeDuration::fromPersistedRow(
        DurationType::Activity, QDateTime(pastDate, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(pastDate, QTime(10, 0, 0), Qt::LocalTime)));
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    QVERIFY(tracker.isStopped());

    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    QVERIFY(ctx.isEditable(pastDate));

    HistoryDayChange day;
    day.date = pastDate;
    day.dayStartUtc = DayValidity::localDayStart(pastDate).toUTC();
    day.dayEndUtc = DayValidity::localDayEnd(pastDate).toUTC();
    const std::vector<HistoryDayChange> days{day};

    // Act: the wall clock steps backward to a previous local date before Save.
    clock.setNow(QDateTime(today.addDays(-1), QTime(9, 0, 0), Qt::LocalTime));
    const int saveCallsBefore = fakeDb.callLog.count("saveHistoryDays");
    const SessionStoreResult result = tracker.saveHistoryEdit(ctx, days);
    tracker.endExclusiveEdit();

    // Assert
    QCOMPARE(result.category, SessionStoreResult::Conflict);
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), saveCallsBefore);
}

void TimerTest::test_WP3_saveHistoryEdit_timezone_change_is_conflict_no_store_call()
{
    // Arrange: TZ restored on scope exit (test_dayvalidity.cpp's pattern).
    struct TzGuard {
        QByteArray orig;
        TzGuard() : orig(qgetenv("TZ")) {}
        ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
    } tzGuard;
    qputenv("TZ", "Etc/UTC");
    tzset();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    const QDate pastDate(2025, 6, 10);
    fakeDb.storedDurations.push_back(TimeDuration::fromPersistedRow(
        DurationType::Activity, QDateTime(pastDate, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(pastDate, QTime(10, 0, 0), Qt::LocalTime)));
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    QVERIFY(tracker.isStopped());

    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    QVERIFY(ctx.isEditable(pastDate));

    HistoryDayChange day;
    day.date = pastDate;
    // Captured under Etc/UTC, exactly as the real caller would at open.
    day.dayStartUtc = DayValidity::localDayStart(pastDate).toUTC();
    day.dayEndUtc = DayValidity::localDayEnd(pastDate).toUTC();
    const std::vector<HistoryDayChange> days{day};

    // Act: the process timezone changes before Save (the local date stays
    // the same, but the local day's UTC span shifts).
    qputenv("TZ", "America/Los_Angeles");
    tzset();
    const int saveCallsBefore = fakeDb.callLog.count("saveHistoryDays");
    const SessionStoreResult result = tracker.saveHistoryEdit(ctx, days);
    tracker.endExclusiveEdit();

    // Assert
    QCOMPARE(result.category, SessionStoreResult::Conflict);
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), saveCallsBefore);
}

void TimerTest::test_WP3_saveHistoryEdit_after_endExclusiveEdit_is_conflict_no_store_call()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    const QDate pastDate(2025, 6, 10);
    fakeDb.storedDurations.push_back(TimeDuration::fromPersistedRow(
        DurationType::Activity, QDateTime(pastDate, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(pastDate, QTime(10, 0, 0), Qt::LocalTime)));
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    QVERIFY(tracker.isStopped());

    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    QVERIFY(ctx.isEditable(pastDate));
    tracker.endExclusiveEdit();

    HistoryDayChange day;
    day.date = pastDate;
    day.dayStartUtc = DayValidity::localDayStart(pastDate).toUTC();
    day.dayEndUtc = DayValidity::localDayEnd(pastDate).toUTC();
    const std::vector<HistoryDayChange> days{day};
    const int saveCallsBefore = fakeDb.callLog.count("saveHistoryDays");

    // Act
    const SessionStoreResult result = tracker.saveHistoryEdit(ctx, days);

    // Assert
    QCOMPARE(result.category, SessionStoreResult::Conflict);
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), saveCallsBefore);
}

void TimerTest::test_WP3_saveHistoryEdit_after_revoke_is_conflict_no_store_call()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    const QDate today(2025, 6, 15);
    const QDate pastDate(2025, 6, 10);
    fakeDb.storedDurations.push_back(TimeDuration::fromPersistedRow(
        DurationType::Activity, QDateTime(pastDate, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(pastDate, QTime(10, 0, 0), Qt::LocalTime)));
    FakeClock clock(QDateTime(today, QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    QVERIFY(tracker.isStopped());

    const HistoryEditContext ctx = tracker.beginExclusiveEdit();
    QVERIFY(ctx.isEditable(pastDate));

    // Act: shutdown revokes the open edit (dialog_open_ -> false) before Save.
    tracker.shutdown();
    QVERIFY(!tracker.isDialogOpen_dbg());

    HistoryDayChange day;
    day.date = pastDate;
    day.dayStartUtc = DayValidity::localDayStart(pastDate).toUTC();
    day.dayEndUtc = DayValidity::localDayEnd(pastDate).toUTC();
    const std::vector<HistoryDayChange> days{day};
    const int saveCallsBefore = fakeDb.callLog.count("saveHistoryDays");

    const SessionStoreResult result = tracker.saveHistoryEdit(ctx, days);

    // Assert
    QCOMPARE(result.category, SessionStoreResult::Conflict);
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), saveCallsBefore);
}
