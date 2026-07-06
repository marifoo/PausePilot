#include "test_timer.h"
#include "fakesessionstore.h"
#include "fakeclock.h"
#include <QtTest>
#include <QSqlQuery>
#include <algorithm>

using TestCommon::createSettingsFile;
using TestCommon::sumDurations;

void TimerTest::initTestCase()
{
    db_path_ = QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite");
    if (QFile::exists(db_path_)) {
        db_backup_path_ = db_path_ + ".bak_test";
        QFile::remove(db_backup_path_);
        QVERIFY(QFile::rename(db_path_, db_backup_path_));
    }
}

void TimerTest::cleanupTestCase()
{
    if (!db_path_.isEmpty()) {
        QFile::remove(db_path_);
    }
    if (!db_backup_path_.isEmpty()) {
        QFile::remove(db_path_);
        QVERIFY(QFile::rename(db_backup_path_, db_path_));
    }
}

void TimerTest::test_timer_start_pause_resume_stop_and_checkpoints()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // Start -> Activity
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    QVERIFY(tracker.elapsedTimer_dbg().isValid());
    QVERIFY(tracker.isActive());
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());

    // Pause -> durations captured and checkpoint id reset
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());
    QVERIFY(tracker.sessionState_dbg().durations.size() >= 1);
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());

    // Resume -> Activity and timer restarted
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    tracker.elapsedTimer_dbg().invalidate();
    tracker.elapsedTimer_dbg().start();

    // Trigger checkpoint manually
    QTest::qWait(100); // Ensure elapsed > 0
    tracker.saveCheckpoint_locked_dbg(QDateTime::currentDateTime());
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());

    // Stop -> move to None and durations flushed
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.sessionState_dbg().segment_id.isEmpty());
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
    TimeTracker tracker(settings, db);
    tracker.useTimerViaButton(Button::Start);

    tracker.sessionState_dbg().segment_start_time = QDateTime::currentDateTime().addSecs(-120);
    tracker.elapsedTimer_dbg().invalidate();
    tracker.elapsedTimer_dbg().start();

    tracker.backpauseTimer_dbg(QDateTime::currentDateTime());
    QVERIFY(tracker.isPaused());
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
    QVERIFY(tracker.sessionState_dbg().durations.size() >= 2);
    // Sum should equal ~120s
    qint64 total = sumDurations(tracker.sessionState_dbg().durations, DurationType::Activity) +
                   sumDurations(tracker.sessionState_dbg().durations, DurationType::Pause);
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
    tracker.elapsedTimer_dbg().invalidate();
    tracker.elapsedTimer_dbg().start();
    tracker.useTimerViaLockEvent(LockEvent::Lock);
    QVERIFY(tracker.isLocked_dbg());
    tracker.useTimerViaLockEvent(LockEvent::Unlock);
    QVERIFY(!tracker.isLocked_dbg());
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

void TimerTest::test_timer_set_duration_type()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // Add a duration manually
    QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-10), now));
    QCOMPARE(tracker.sessionState_dbg().durations.size(), (size_t)1);
    QCOMPARE(tracker.sessionState_dbg().durations[0].type, DurationType::Activity);

    // Change to Pause
    tracker.setDurationType(0, DurationType::Pause);
    QCOMPARE(tracker.sessionState_dbg().durations[0].type, DurationType::Pause);

    // Invalid index
    tracker.setDurationType(99, DurationType::Activity);
    QCOMPARE(tracker.sessionState_dbg().durations[0].type, DurationType::Pause); // Unchanged
}

void TimerTest::test_timer_checkpoints_paused()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    // Enable checkpoints (interval = 1 minute, but we'll trigger manually)
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

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
    QVERIFY(db2.saveDurations({}, TransactionMode::Append));

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

    QVERIFY(tracker.sessionState_dbg().has_unsaved_data);
    QVERIFY(!tracker.sessionState_dbg().durations.empty());
    std::deque<TimeDuration> unsavedCopy = tracker.sessionState_dbg().durations;

    // Retry while DB is still blocked -> must stay unsaved.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.sessionState_dbg().has_unsaved_data);
    QVERIFY(!tracker.sessionState_dbg().durations.empty());

    // Restore DB and retry again -> previously unsaved rows should be appended.
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
    tracker.useTimerViaButton(Button::Stop);
    tracker.useTimerViaButton(Button::Start);

    auto loaded = db2.loadDurations();

    for (const auto& d : unsavedCopy) {
        bool found = false;
        for (const auto& row : loaded) {
            if (row.type == d.type && row.startTime == d.startTime && row.endTime == d.endTime && row.duration == d.duration) {
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
    QVERIFY(db2.saveDurations({}, TransactionMode::Append));

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    // Force first stop save to fail.  Close the connection before making the
    // file read-only so that ensureOpen() re-tries and fails on the permission.
    db.closeConnection_dbg();
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
    tracker.useTimerViaButton(Button::Stop);

    QVERIFY(tracker.sessionState_dbg().has_unsaved_data);
    QVERIFY(!tracker.sessionState_dbg().durations.empty());
    const size_t sizeBeforeRetry = tracker.sessionState_dbg().durations.size();

    // Retry still fails -> must still be unsaved with data intact.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.sessionState_dbg().has_unsaved_data);
    QVERIFY(!tracker.sessionState_dbg().durations.empty());
    QVERIFY(tracker.sessionState_dbg().durations.size() >= sizeBeforeRetry);

    // Cleanup permissions for following tests.
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther));
}

// ============================================================================
// SessionState transition tests
// ============================================================================

void TimerTest::test_session_state_begin_new_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    QDateTime startTime = QDateTime::currentDateTime();
    QVERIFY(tracker.sessionState_dbg().segment_id.isEmpty());
    QVERIFY(!tracker.sessionState_dbg().segment_start_time.isValid());

    // Act
    tracker.sessionState_dbg().beginNewSegment(startTime);

    // Assert
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
    QCOMPARE(tracker.sessionState_dbg().segment_start_time, startTime);
}

void TimerTest::test_session_state_clear_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.sessionState_dbg().beginNewSegment(QDateTime::currentDateTime());
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());

    // Act
    tracker.sessionState_dbg().clearSegment();

    // Assert
    QVERIFY(tracker.sessionState_dbg().segment_id.isEmpty());
    QVERIFY(!tracker.sessionState_dbg().segment_start_time.isValid());
}

void TimerTest::test_session_state_mark_and_clear_unsaved()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    QVERIFY(!tracker.sessionState_dbg().has_unsaved_data);

    // Act: mark unsaved
    tracker.sessionState_dbg().markUnsaved();

    // Assert
    QVERIFY(tracker.sessionState_dbg().has_unsaved_data);

    // Act: clear unsaved
    tracker.sessionState_dbg().clearUnsaved();

    // Assert
    QVERIFY(!tracker.sessionState_dbg().has_unsaved_data);
    QVERIFY(tracker.sessionState_dbg().unsaved_durations.empty());
}

void TimerTest::test_session_state_reset_for_new_session()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-10), now));
    tracker.sessionState_dbg().has_unsaved_data = true;
    tracker.sessionState_dbg().unsaved_durations = tracker.sessionState_dbg().durations;

    // Act
    tracker.sessionState_dbg().resetForNewSession();

    // Assert
    QVERIFY(tracker.sessionState_dbg().durations.empty());
    QVERIFY(!tracker.sessionState_dbg().has_unsaved_data);
    QVERIFY(tracker.sessionState_dbg().unsaved_durations.empty());
}

void TimerTest::test_replaceAll_success_clears_unsaved_cache()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.emplace_back(
        TimeDuration::create(DurationType::Activity, now.addSecs(-60), now).value());
    tracker.sessionState_dbg().unsaved_durations = tracker.sessionState_dbg().durations;
    tracker.sessionState_dbg().has_unsaved_data = true;

    const SessionStoreResult result = tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt));

    QVERIFY(result.ok());
    QVERIFY(!tracker.sessionState_dbg().has_unsaved_data);
    QVERIFY(tracker.sessionState_dbg().unsaved_durations.empty());
}

void TimerTest::test_replaceAll_failure_keeps_unsaved_cache()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.replaceDurationsResult = SessionStoreResult::transient("injected failure");
    TimeTracker tracker(settings, fakeDb);

    QDateTime now = QDateTime::currentDateTime();
    tracker.sessionState_dbg().durations.emplace_back(
        TimeDuration::create(DurationType::Activity, now.addSecs(-60), now).value());
    tracker.sessionState_dbg().unsaved_durations = tracker.sessionState_dbg().durations;
    tracker.sessionState_dbg().has_unsaved_data = true;

    const SessionStoreResult result = tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt));

    QVERIFY(!result.ok());
    QVERIFY(result.category == SessionStoreResult::TransientError);
    QVERIFY(tracker.sessionState_dbg().has_unsaved_data);
    QVERIFY(!tracker.sessionState_dbg().unsaved_durations.empty());
}

void TimerTest::test_session_state_adopt_ongoing_segment()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    QDateTime start = QDateTime::currentDateTime().addSecs(-30);
    QDateTime end = QDateTime::currentDateTime();
    TimeDuration ongoing = TimeDuration::fromLiveSession(DurationType::Activity, start, end);

    // Act
    tracker.sessionState_dbg().adoptOngoingSegment(ongoing);

    // Assert
    QCOMPARE(tracker.sessionState_dbg().segment_id.toString(), ongoing.segment_id.toString());
    QCOMPARE(tracker.sessionState_dbg().segment_start_time, start);
}

void TimerTest::test_session_state_start_to_pause_transition()
{
    // Arrange: Start -> Activity, then pause
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Act: start
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: Activity mode, valid segment
    QVERIFY(tracker.isActive());
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
    QVERIFY(tracker.sessionState_dbg().segment_start_time.isValid());
    SegmentId activitySegId = tracker.sessionState_dbg().segment_id;

    // Act: pause
    tracker.useTimerViaButton(Button::Pause);

    // Assert: Pause mode, new segment id (different from Activity)
    QVERIFY(tracker.isPaused());
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
    QVERIFY(tracker.sessionState_dbg().segment_id != activitySegId);
    QVERIFY(tracker.sessionState_dbg().segment_start_time.isValid());
    QVERIFY(tracker.sessionState_dbg().durations.size() >= 1);
    QCOMPARE(tracker.sessionState_dbg().durations.back().type, DurationType::Activity);
}

void TimerTest::test_session_state_pause_to_activity_transition()
{
    // Arrange: Start -> Pause -> Resume
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);

    SegmentId pauseSegId = tracker.sessionState_dbg().segment_id;
    size_t durationsBeforeResume = tracker.sessionState_dbg().durations.size();

    // Act: resume (Start from Pause)
    tracker.useTimerViaButton(Button::Start);

    // Assert: Activity mode, new segment id, pause duration recorded
    QVERIFY(tracker.isActive());
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
    QVERIFY(tracker.sessionState_dbg().segment_id != pauseSegId);
    QVERIFY(tracker.sessionState_dbg().durations.size() > durationsBeforeResume);
}

void TimerTest::test_session_state_stop_clears_segment()
{
    // Arrange: Start -> Stop
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());

    // Act
    tracker.useTimerViaButton(Button::Stop);

    // Assert: None mode, segment cleared, durations cleared (saved to DB)
    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.sessionState_dbg().segment_id.isEmpty());
    QVERIFY(!tracker.sessionState_dbg().segment_start_time.isValid());
    QVERIFY(!tracker.sessionState_dbg().has_unsaved_data);
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
    qint64 totalActivity = sumDurations(tracker.sessionState_dbg().durations, DurationType::Activity);
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
    TimeTracker tracker(settings, db);

    // Act: start the timer (first session today, empty DB)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: boot time (30s = 30000ms) was added as an Activity segment
    qint64 totalActivity = sumDurations(tracker.sessionState_dbg().durations, DurationType::Activity);
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

    // Use a separate SqliteSessionStore to seed an entry for today
    {
        SqliteSessionStore seeder(settings);
        QDateTime now = QDateTime::currentDateTimeUtc();
        std::deque<TimeDuration> durations;
        durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-3600), now.addSecs(-3540)));
        QVERIFY(seeder.saveDurations(durations, TransactionMode::Append));
    }

    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // Act: start the timer (DB already has entries for today)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: no boot time added. Only the in-progress segment should exist.
    qint64 totalActivity = sumDurations(tracker.sessionState_dbg().durations, DurationType::Activity);
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
        // Set mode to None to prevent destructor from saving a clean stop.
        tracker.forceMode_dbg(TimeTracker::Mode::None);
        tracker.sessionState_dbg().segment_id = SegmentId{};
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

    QDateTime start = QDateTime::currentDateTime().addSecs(-60);
    QDateTime end = QDateTime::currentDateTime();

    // Act
    tracker.addDuration_dbg(DurationType::Activity, start, end);

    // Assert
    QCOMPARE(tracker.sessionState_dbg().durations.size(), static_cast<size_t>(1));
    QCOMPARE(tracker.sessionState_dbg().durations[0].type, DurationType::Activity);
    QCOMPARE(tracker.sessionState_dbg().durations[0].startTime, start);
    QCOMPARE(tracker.sessionState_dbg().durations[0].endTime, end);
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
    QVERIFY(tracker.sessionState_dbg().durations.empty());
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
    QVERIFY(tracker.sessionState_dbg().durations.empty());
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
    QVERIFY(tracker.sessionState_dbg().durations.empty());

    // Invalid end
    tracker.addDuration_dbg(DurationType::Activity, valid, invalid);
    QVERIFY(tracker.sessionState_dbg().durations.empty());
}

void TimerTest::test_isOngoingSegmentCrossMidnight_returns_false_when_stopped()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // mode_ == None by default
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());
}

void TimerTest::test_isOngoingSegmentCrossMidnight_returns_false_for_same_day()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.forceMode_dbg(TimeTracker::Mode::Activity);
    tracker.sessionState_dbg().segment_start_time = QDateTime::currentDateTime().addSecs(-60);

    // Assert
    QVERIFY(!tracker.isOngoingSegmentCrossMidnight());
}

void TimerTest::test_isOngoingSegmentCrossMidnight_returns_true_when_segment_started_yesterday()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.forceMode_dbg(TimeTracker::Mode::Activity);
    tracker.sessionState_dbg().segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));

    // Assert
    QVERIFY(tracker.isOngoingSegmentCrossMidnight());
}

void TimerTest::test_useTimerViaButton_force_stops_on_cross_midnight_ongoing()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.forceMode_dbg(TimeTracker::Mode::Activity);
    tracker.sessionState_dbg().segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.elapsedTimer_dbg().start();

    // Act: try to stop — discardCrossMidnightOngoingAndStop should fire first
    tracker.useTimerViaButton(Button::Stop);

    // Assert: engine is in None, no cross-midnight row in DB
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.sessionState_dbg().segment_start_time.isValid());
    QVERIFY(!tracker.isAutopausePendingResume_dbg());
    // FakeSessionStore captures writes; no duration should have been written
    // with a cross-midnight segment (the in-flight segment is discarded)
    for (const auto& d : fakeDb.storedDurations) {
        QVERIFY(!isCrossMidnight(d.startTime, d.endTime));
    }
}

void TimerTest::test_useTimerViaButton_pause_event_is_dropped_when_cross_midnight()
{
    // Arrange: simulate cross-midnight state, user tries to pause
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.forceMode_dbg(TimeTracker::Mode::Activity);
    tracker.sessionState_dbg().segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.elapsedTimer_dbg().start();

    // Act: user presses Pause — but the guard should intercept and force-stop
    tracker.useTimerViaButton(Button::Pause);

    // Assert: engine is in None (not Pause)
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.sessionState_dbg().segment_start_time.isValid());
}

void TimerTest::test_useTimerViaLockEvent_unlock_does_not_restart_after_cross_midnight_discard()
{
    // Arrange: simulate cross-midnight with autopause_pending_resume_ set
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.forceMode_dbg(TimeTracker::Mode::Pause);
    tracker.sessionState_dbg().segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.setAutopausePendingResume_dbg(true);
    tracker.elapsedTimer_dbg().start();

    // Act: Unlock event arrives — guard should discard and not restart
    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    // Assert: stayed in None, did not auto-restart
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.isAutopausePendingResume_dbg());
}

void TimerTest::test_saveCheckpoint_locked_does_not_write_cross_midnight_row()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.forceMode_dbg(TimeTracker::Mode::Activity);
    tracker.sessionState_dbg().segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));
    tracker.elapsedTimer_dbg().start();

    // Act: checkpoint fires (simulating late checkpoint after sleep)
    QDateTime now = QDateTime::currentDateTime();
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
    TimeTracker tracker(settings, db);
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(0, 30, 0), Qt::LocalTime));
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Act
    tracker.useTimerViaButton(Button::Start);

    // Assert: cross-midnight boot entry was dropped; no completed durations
    // (the ongoing segment is fresh from startTimer)
    QCOMPARE(tracker.sessionState_dbg().durations.size(), static_cast<size_t>(0));
    QVERIFY(tracker.isActive());

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_getOngoingDuration_returns_nullopt_when_cross_midnight()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.forceMode_dbg(TimeTracker::Mode::Activity);
    tracker.sessionState_dbg().segment_start_time =
        QDateTime(QDate::currentDate().addDays(-1), QTime(23, 59, 58, 0));

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

    bool crossMidnight = tracker.isOngoingSegmentCrossMidnight();
    QVERIFY(!crossMidnight); // started now, so not cross-midnight

    qint64 recovered = tracker.getStartupRecoveredSeconds();
    QCOMPARE(recovered, static_cast<qint64>(0)); // fresh db, nothing to recover

    bool showNotification = tracker.shouldShowStartupRecoveryNotification();
    (void)showNotification;

    auto histStats = tracker.getLastHistoryLoadStats();
    (void)histStats;

    bool canMark = tracker.canMarkCleanShutdown();
    QVERIFY(!canMark); // timer is running

    // Mutators
    tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();

    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // setDurationType: index 0 should exist after Activity+Pause cycle
    if (!tracker.sessionState_dbg().durations.empty()) {
        tracker.setDurationType(0, DurationType::Pause);
        tracker.setDurationType(0, DurationType::Activity);
    }

    tracker.useTimerViaButton(Button::Stop);

    // DB write helpers
    fakeDb.callLog.clear();
    tracker.appendDurationsToDB_dbg();
    tracker.updateDurationsInDB_dbg();
    tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt));

    // replaceCurrentDurations
    tracker.replaceCurrentDurations_dbg({}, std::nullopt);

    // getDurationsHistory
    auto history = tracker.getDurationsHistory();
    (void)history;

    // Confirm the engine predicate is still there
    QVERIFY(tracker.canMarkCleanShutdown()); // stopped, no unsaved data

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
    for (const auto& d : tracker.sessionState_dbg().durations)
        if (d.type == DurationType::Activity)
            totalActivity += d.duration;
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
    TimeTracker tracker(settings, fakeDb);

    // Act
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: a 30-second boot-time Activity segment was prepended
    qint64 totalActivity = 0;
    for (const auto& d : tracker.sessionState_dbg().durations)
        if (d.type == DurationType::Activity)
            totalActivity += d.duration;
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
    for (const auto& d : tracker.sessionState_dbg().durations)
        if (d.type == DurationType::Activity)
            totalActivity += d.duration;
    QVERIFY2(totalActivity < 5000,
             "Boot time must not be added when DB reports Unknown for today");

    tracker.useTimerViaButton(Button::Stop);
}

void TimerTest::test_E_boot_time_inmemory_start_date_skips_boot_time()
{
    // Regression guard for #3: in-memory check uses startTime.date() (not endTime.date()).
    // DB says No entries, but an in-memory completed cross-midnight segment with
    // startTime == today (endTime == tomorrow) must block boot time from being added.
    // Discriminating: reverting to endTime.date() would fail (endTime is tomorrow ≠ today).

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
    // tomorrow. This is the discriminating case — if the production check used
    // endTime.date() instead of startTime.date(), endTime (tomorrow) != today,
    // the check would miss, and boot time would be added (test would FAIL).
    QDateTime now = QDateTime::currentDateTime();
    QDateTime startToday = QDateTime(now.date(), QTime(23, 0, 0));
    QDateTime endTomorrow = startToday.addSecs(3600); // crosses midnight
    tracker.sessionState_dbg().durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, startToday, endTomorrow));

    // Act: start the timer (DB says No, but in-memory entry exists for today)
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Assert: boot time was NOT added (in-memory start-date check fired).
    qint64 totalActivity = 0;
    for (const auto& d : tracker.sessionState_dbg().durations)
        if (d.type == DurationType::Activity && d.duration >= 25000)
            totalActivity += d.duration;
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
 * "commitSession" for the stop write, and must NOT contain "saveDurations"
 * or "updateDurationsById" as write paths (those are no longer in the
 * interface; any occurrence would indicate a regression to the old paths).
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

    // Assert: old direct-write methods were NOT called via the interface
    QVERIFY2(!fakeDb.callLog.contains("saveDurations"),
             "saveDurations must not be called; commitSession is the write path");
    QVERIFY2(!fakeDb.callLog.contains("updateDurationsById"),
             "updateDurationsById must not be called; commitSession is the write path");
}

// Issue 3 Layer A: while dialog is open, LongOngoingLock must not call backpauseTimer.
// After endExclusiveEdit(), the deferred event must be replayed.
void TimerTest::test_dialog_open_blocks_backpause()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);
    const size_t dursBefore = tracker.sessionState_dbg().durations.size();

    // Open dialog — suspends mutation
    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());

    // Fire LongOngoingLock while dialog is open
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock);

    // backpause must NOT have been applied
    QCOMPARE(tracker.sessionState_dbg().durations.size(), dursBefore);
    QVERIFY(tracker.isActive());

    // Pending event recorded
    QCOMPARE(tracker.pendingLockEvent_dbg(), LockEvent::LongOngoingLock);

    // Close dialog — replay deferred LongOngoingLock
    tracker.endExclusiveEdit();
    QVERIFY(!tracker.isDialogOpen_dbg());
    QCOMPARE(tracker.pendingLockEvent_dbg(), LockEvent::None);

    // After replay, timer should be in Pause (backpause applied)
    QVERIFY(tracker.isPaused());
}

// Issue 3 Layer A: pending_midnight_stop_ set directly, endExclusiveEdit replays it.
void TimerTest::test_dialog_open_defers_midnight_stop()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Simulate: dialog is open and midnight fired (deferred)
    tracker.beginExclusiveEdit();
    tracker.setPendingMidnightStop_dbg(true);

    // Close dialog — replay deferred midnight stop
    tracker.endExclusiveEdit();

    QVERIFY(!tracker.pendingMidnightStop_dbg());
    QVERIFY(tracker.isStopped());
}

// Issue 3 Layer A: Lock bookkeeping (is_locked_) must still run even when dialog is open,
// but saveCheckpoint_locked must not be called.
void TimerTest::test_dialog_open_allows_lock_bookkeeping()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    tracker.beginExclusiveEdit();
    QVERIFY(!tracker.isLocked_dbg());

    const size_t checksBefore = fakeDb.callLog.count("saveCheckpoint");

    // Fire Lock while dialog is open
    tracker.useTimerViaLockEvent(LockEvent::Lock);

    // is_locked_ must be set (bookkeeping)
    QVERIFY(tracker.isLocked_dbg());

    // saveCheckpoint (i.e. saveCheckpoint_locked path via DB) must not have fired
    QCOMPARE(fakeDb.callLog.count("saveCheckpoint"), checksBefore);

    tracker.endExclusiveEdit();
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
    const SegmentId activitySegId = tracker.sessionState_dbg().segment_id;

    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(10);
    const SegmentId pauseSegId = tracker.sessionState_dbg().segment_id;
    const size_t dursBefore = tracker.sessionState_dbg().durations.size();

    // Act: resume from Pause
    tracker.useTimerViaButton(Button::Start);

    // Assert: a new Pause segment was appended (not merged into the Activity)
    QVERIFY(tracker.sessionState_dbg().durations.size() > dursBefore);
    // The Pause segment just recorded must have its own id — neither empty,
    // nor the same as the preceding Activity's id.
    const TimeDuration& pauseEntry = tracker.sessionState_dbg().durations.back();
    QCOMPARE(pauseEntry.type, DurationType::Pause);
    QVERIFY2(!pauseEntry.segment_id.isEmpty(),
             "Pause segment must have a non-empty segment_id");
    QVERIFY2(pauseEntry.segment_id != activitySegId,
             "Pause segment must not reuse the Activity segment_id (T1 regression)");
    // The new Activity's segment_id must also be fresh
    QVERIFY2(tracker.sessionState_dbg().segment_id != pauseSegId,
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
    for (const auto& d : tracker.sessionState_dbg().durations) {
        QVERIFY(d.type == DurationType::Activity || d.type == DurationType::Pause);
        // Segment ids must be non-empty (T1 invariant)
        QVERIFY2(!d.segment_id.isEmpty(), "Every completed segment must have a non-empty segment_id");
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
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

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
// beginExclusiveEdit / endExclusiveEdit + replaceCurrentDurations checkpoint guard
// ============================================================================

// T2: replaceCurrentDurations must NOT call saveCheckpoint while dialog_open_ is true.
void TimerTest::test_T2_replaceCurrentDurations_skips_checkpoint_while_dialog_open()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    // Build a plausible ongoing segment for replaceCurrentDurations to adopt.
    const QDateTime start = QDateTime::currentDateTime().addSecs(-60);
    const QDateTime end   = QDateTime::currentDateTime();
    TimeDuration ongoing = TimeDuration::fromLiveSession(DurationType::Activity, start, end, SegmentId::fromString("seg-001"));

    tracker.beginExclusiveEdit();
    QVERIFY(tracker.isDialogOpen_dbg());

    fakeDb.callLog.clear();

    // replaceCurrentDurations with a valid ongoing segment — checkpoint must be suppressed.
    tracker.replaceCurrentDurations_dbg({}, ongoing);

    QCOMPARE(fakeDb.callLog.count("saveCheckpoint"), 0);

    tracker.endExclusiveEdit();
}

// C6: after endExclusiveEdit(), a subsequent replaceCurrentDurations resumes normal writes.
void TimerTest::test_C6_replaceCurrentDurations_writes_checkpoint_after_endExclusiveEdit()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(10);

    const QDateTime start = QDateTime::currentDateTime().addSecs(-60);
    const QDateTime end   = QDateTime::currentDateTime();
    TimeDuration ongoing = TimeDuration::fromLiveSession(DurationType::Activity, start, end, SegmentId::fromString("seg-002"));

    tracker.beginExclusiveEdit();
    tracker.endExclusiveEdit();
    QVERIFY(!tracker.isDialogOpen_dbg());

    fakeDb.callLog.clear();

    // After endExclusiveEdit, dialog_open_ is false — checkpoint write must proceed.
    tracker.replaceCurrentDurations_dbg({}, ongoing);

    QCOMPARE(fakeDb.callLog.count("saveCheckpoint"), 1);
}

void TimerTest::test_S12_marker_error_skips_reconciliation()
{
    // When recoverStartupCheckpoints() returns ok=false (e.g. marker read error),
    // TimeTracker must treat recovered seconds as 0 and must NOT show a notification.
    // The store is responsible for skipping reconciliation on error; TimeTracker only
    // acts on the domain facts returned.

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Simulate store-level failure (marker error or other critical failure).
    fakeDb.startupRecoveryResult.ok = false;
    fakeDb.startupRecoveryResult.recovered_seconds = 0;
    fakeDb.startupRecoveryResult.notify_user = false;

    TimeTracker tracker(settings, fakeDb);
    tracker.initializeFromStore();

    // No seconds recovered; notification suppressed.
    QCOMPARE(tracker.getStartupRecoveredSeconds(), (qint64)0);
    QVERIFY(!tracker.shouldShowStartupRecoveryNotification());
    QVERIFY(fakeDb.callLog.contains("recoverStartupCheckpoints"));
}

void TimerTest::test_QF1_explicit_pause_emits_paused_not_modeChanged()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    QSignalSpy pausedSpy(&tracker, &TimeTracker::paused);
    QSignalSpy modeSpy(&tracker, &TimeTracker::modeChanged);

    tracker.useTimerViaButton(Button::Start);
    tracker.useTimerViaButton(Button::Pause);

    QCOMPARE(pausedSpy.count(), 1);
    QCOMPARE(modeSpy.count(), 0);
}

void TimerTest::test_QF1_explicit_resume_emits_started_not_modeChanged()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    tracker.useTimerViaButton(Button::Pause);

    QSignalSpy startedSpy(&tracker, &TimeTracker::started);
    QSignalSpy modeSpy(&tracker, &TimeTracker::modeChanged);

    tracker.useTimerViaButton(Button::Start);

    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(startedSpy.first().first().toBool()); // fromPause == true
    QCOMPARE(modeSpy.count(), 0);
}

void TimerTest::test_QF1_lock_autopause_emits_modeChanged_not_paused()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);

    QSignalSpy pausedSpy(&tracker, &TimeTracker::paused);
    QSignalSpy modeSpy(&tracker, &TimeTracker::modeChanged);

    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock);

    QCOMPARE(pausedSpy.count(), 0);
    QCOMPARE(modeSpy.count(), 1);
    QCOMPARE(modeSpy.first().first().value<TimeTracker::PauseCause>(),
             TimeTracker::PauseCause::LockAutopause);
}

void TimerTest::test_QF1_lock_resume_emits_started_and_modeChanged_once_each()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock);
    QVERIFY(tracker.isPaused());

    QSignalSpy startedSpy(&tracker, &TimeTracker::started);
    QSignalSpy pausedSpy(&tracker, &TimeTracker::paused);
    QSignalSpy modeSpy(&tracker, &TimeTracker::modeChanged);

    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(startedSpy.first().first().toBool()); // fromPause == true
    QCOMPARE(pausedSpy.count(), 0);
    QCOMPARE(modeSpy.count(), 1);
    QCOMPARE(modeSpy.first().first().value<TimeTracker::PauseCause>(),
             TimeTracker::PauseCause::LockResume);
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

    // commitEditedTimeline must return success and switch mode_ to Pause.
    const auto result = tracker.commitEditedTimeline(Timeline({}, editedOngoing));
    QVERIFY(result.ok);
    QVERIFY(tracker.isPaused());

    // getOngoingDuration() derives type from mode_, so it should now be Pause.
    auto postCommit = tracker.getOngoingDuration();
    QVERIFY(postCommit.has_value());
    QCOMPARE(postCommit->type, DurationType::Pause);
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
    QCOMPARE(tracker.sessionState_dbg().has_unsaved_data, false);
    QCOMPARE(tracker.sessionState_dbg().consecutive_retry_failures, 0);
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
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    // Build an edited timeline with completed segments but NO ongoing segment.
    QDateTime now = QDateTime::currentDateTime();
    std::deque<TimeDuration> completed;
    auto seg = TimeDuration::create(DurationType::Activity, now.addSecs(-60), now);
    QVERIFY(seg.has_value());
    completed.push_back(seg.value());
    Timeline editedTimeline(completed, std::nullopt);

    // Set up spies: the clear path must emit only stopped(), never started()/paused().
    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QSignalSpy startedSpy(&tracker, &TimeTracker::started);
    QSignalSpy pausedSpy(&tracker, &TimeTracker::paused);

    // Act: commit a timeline with no ongoing segment while mode_ != None.
    const auto commitResult = tracker.commitEditedTimeline(editedTimeline);

    // Assert: commit succeeded.
    QVERIFY(commitResult.ok);

    // Segment anchor is cleared: no stale segment_id or start time.
    QVERIFY2(tracker.sessionState_dbg().segment_id.isEmpty(),
             "segment_id must be cleared after commit with no ongoing segment");
    QVERIFY2(!tracker.sessionState_dbg().segment_start_time.isValid(),
             "segment_start_time must be cleared after commit with no ongoing segment");

    // Mode is None.
    QVERIFY2(tracker.isStopped(),
             "mode_ must be None after commit with no ongoing segment");

    // Checkpoint timer is not running.
    QVERIFY2(!tracker.isCheckpointTimerActive_dbg(),
             "checkpoint timer must be stopped after commit with no ongoing segment");

    // getOngoingDuration() returns nullopt.
    QVERIFY2(!tracker.getOngoingDuration().has_value(),
             "getOngoingDuration() must return nullopt after anchor is cleared");

    // stopped(EditApplied) was emitted exactly once; no started()/paused().
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::EditApplied);
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(pausedSpy.count(), 0);
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
    QVERIFY(!tracker.sessionState_dbg().segment_id.isEmpty());

    // Build an edited timeline with completed segments but NO ongoing segment.
    QDateTime now = QDateTime::currentDateTime();
    std::deque<TimeDuration> completed;
    auto seg = TimeDuration::create(DurationType::Activity, now.addSecs(-60), now);
    QVERIFY(seg.has_value());
    completed.push_back(seg.value());
    Timeline editedTimeline(completed, std::nullopt);

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QSignalSpy startedSpy(&tracker, &TimeTracker::started);
    QSignalSpy pausedSpy(&tracker, &TimeTracker::paused);

    // Act: commit a timeline with no ongoing segment while mode_ == Pause.
    const auto commitResult = tracker.commitEditedTimeline(editedTimeline);

    // Assert: commit succeeded and the anchor is fully cleared.
    QVERIFY(commitResult.ok);
    QVERIFY2(tracker.sessionState_dbg().segment_id.isEmpty(),
             "segment_id must be cleared after commit with no ongoing segment");
    QVERIFY2(!tracker.sessionState_dbg().segment_start_time.isValid(),
             "segment_start_time must be cleared after commit with no ongoing segment");
    QVERIFY2(tracker.isStopped(),
             "mode_ must be None after commit with no ongoing segment");
    QVERIFY2(!tracker.isCheckpointTimerActive_dbg(),
             "checkpoint timer must be stopped after commit with no ongoing segment");
    QVERIFY2(!tracker.getOngoingDuration().has_value(),
             "getOngoingDuration() must return nullopt after anchor is cleared");

    // stopped(EditApplied) was emitted exactly once; no started()/paused().
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().first().value<TimeTracker::StopReason>(),
             TimeTracker::StopReason::EditApplied);
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(pausedSpy.count(), 0);
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
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(start);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    QVERIFY(tracker.isStopped());
    QVERIFY(tracker.sessionState_dbg().segment_id.isEmpty());

    // Build an edited timeline with an ongoing segment (as if HistoryDialog
    // still held one from before the engine stopped), but mode_ is None now.
    TimeDuration editedOngoing = TimeDuration::fromLiveSession(
        DurationType::Activity, start, start.addSecs(600));

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);
    QSignalSpy startedSpy(&tracker, &TimeTracker::started);
    QSignalSpy pausedSpy(&tracker, &TimeTracker::paused);

    // Act: commit while mode_ == None.
    const auto result = tracker.commitEditedTimeline(Timeline({}, editedOngoing));

    // Assert: commit still reports success; the ongoing segment is ignored.
    QVERIFY(result.ok);
    QVERIFY2(tracker.isStopped(), "engine must remain stopped, not half-started");
    QVERIFY2(tracker.sessionState_dbg().segment_id.isEmpty(),
             "segment_id must stay empty -- refused, not half-adopted");
    QVERIFY2(!tracker.sessionState_dbg().segment_start_time.isValid(),
             "segment_start_time must stay invalid -- refused, not half-adopted");
    QVERIFY2(!tracker.getOngoingDuration().has_value(),
             "getOngoingDuration() must return nullopt: mode_ == None");

    // No mode-transition signal fires: the engine state did not change.
    QCOMPARE(stoppedSpy.count(), 0);
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(pausedSpy.count(), 0);
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
    TimeTracker tracker(settings, db);

    const QDateTime start(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(start);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Act: start the engine (segment anchored at the fake now), then jump ahead.
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    QCOMPARE(tracker.sessionState_dbg().segment_start_time, start);

    const qint64 ninetyMinutesMs = 90ll * 60ll * 1000ll;
    clock.advance(ninetyMinutesMs);

    // Assert: the ongoing segment reflects the advanced fake clock — no sleeping.
    const auto ongoing = tracker.getOngoingDuration();
    QVERIFY(ongoing.has_value());
    QCOMPARE(ongoing->startTime, start);
    QCOMPARE(ongoing->endTime, start.addMSecs(ninetyMinutesMs));
    QCOMPARE(ongoing->duration, ninetyMinutesMs);
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
    TimeTracker tracker(settings, db);

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
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
    const auto& d = tracker.sessionState_dbg().durations;
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
    TimeTracker tracker(settings, db);

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
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
    TimeTracker tracker(settings, db);

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
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

void TimerTest::test_A4_shutdown_called_cleared_on_fresh_session_start()
{
    // Arrange: timer with fake clock; call shutdown() to set shutdown_called_=true.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore db;
    TimeTracker tracker(settings, db);

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Act: start timer, stop it, then call shutdown().
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(tracker.isStopped());

    // Verify the flag is set after shutdown.
    tracker.shutdown();
    QVERIFY(tracker.wasShutDown());

    // Act: start a fresh session (None → Activity), which should emit started(false).
    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());

    // Assert: shutdown_called_ is now cleared, so wasShutDown() returns false.
    QVERIFY(!tracker.wasShutDown());
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
    TimeTracker tracker(settings, fakeDb);

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(60ll * 1000ll); // elapse a minute so the checkpoint isn't zero-length
    tracker.triggerCheckpoint_dbg();
    QVERIFY(fakeDb.callLog.contains("saveCheckpoint"));
    const SegmentId checkpointSegmentId = tracker.sessionState_dbg().segment_id;
    QVERIFY(!checkpointSegmentId.isEmpty());

    // Act: machine "wakes" well past midnight; shutdown() stops the engine.
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime));
    tracker.shutdown();

    // Assert: the orphaned checkpoint row was discarded.
    QVERIFY(fakeDb.callLog.contains("discardCheckpoint"));
    const bool stillPresent = std::any_of(fakeDb.savedCheckpoints.begin(), fakeDb.savedCheckpoints.end(),
        [&](const FakeSessionStore::CheckpointRecord& cp) { return cp.segmentId == checkpointSegmentId; });
    QVERIFY(!stillPresent);
}

// F2: the deferred-midnight-stop replay in endExclusiveEdit() must discard
// the checkpoint too — it goes through the same stopTimer() call.
void TimerTest::test_F2_deferred_midnight_replay_discards_checkpoint()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime preMidnight(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(preMidnight);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(60ll * 1000ll); // elapse a minute so the checkpoint isn't zero-length
    tracker.triggerCheckpoint_dbg();
    const SegmentId checkpointSegmentId = tracker.sessionState_dbg().segment_id;
    QVERIFY(!checkpointSegmentId.isEmpty());

    // Dialog opens before midnight; the scheduled stop fires while it's open
    // (dialog_open_ defers it — see onMidnightTimerFired), leaving
    // pending_midnight_stop_ set for endExclusiveEdit() to replay.
    tracker.beginExclusiveEdit();
    clock.setNow(QDateTime(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime));
    tracker.setPendingMidnightStop_dbg(true);

    // Act: dialog closes, replaying the deferred midnight stop.
    tracker.endExclusiveEdit();

    // Assert: discardCheckpoint fired for the live segment's id; mode stopped.
    QVERIFY(tracker.isStopped());
    QVERIFY(fakeDb.callLog.contains("discardCheckpoint"));
    const bool stillPresent = std::any_of(fakeDb.savedCheckpoints.begin(), fakeDb.savedCheckpoints.end(),
        [&](const FakeSessionStore::CheckpointRecord& cp) { return cp.segmentId == checkpointSegmentId; });
    QVERIFY(!stillPresent);
}

// F2: an on-time stop (same day) must NOT discard the checkpoint — it gets
// finalized by commitSession's upsert instead.
void TimerTest::test_F2_ontime_stop_does_not_discard_checkpoint()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime t0(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    FakeClock clock(t0);
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
    TimeTracker tracker(settings, fakeDb);

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    clock.advance(60ll * 1000ll); // elapse a minute so the segment isn't zero-length

    // Act: fail the persistence write that finalizePauseTransition performs.
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Pause);

    // Assert: the failure is not swallowed - it is retained for retry.
    QVERIFY(tracker.isPaused());
    QVERIFY(tracker.sessionState_dbg().has_unsaved_data);
    QVERIFY(!tracker.sessionState_dbg().unsaved_durations.empty());
}

// F3: backpauseTimer (via a LongOngoingLock) also finalizes through
// finalizePauseTransition and must get the same treatment as an explicit pause.
void TimerTest::test_F3_backpause_failure_marks_unsaved()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
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
    QVERIFY(tracker.sessionState_dbg().has_unsaved_data);
    QVERIFY(!tracker.sessionState_dbg().unsaved_durations.empty());
}

// F3: a later successful resume-from-Pause write must clear the unsaved flag
// that a prior pause failure set.
void TimerTest::test_F3_recovery_after_pause_failure_clears_unsaved()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.sessionState_dbg().has_unsaved_data);

    // Act: DB recovers; resuming from Pause persists successfully.
    clock.advance(60ll * 1000ll);
    fakeDb.commitSessionResult = SessionStoreResult::success();
    tracker.useTimerViaButton(Button::Start);

    // Assert: the retained unsaved data was cleared by the successful write.
    QVERIFY(!tracker.sessionState_dbg().has_unsaved_data);
    QVERIFY(tracker.sessionState_dbg().unsaved_durations.empty());
}
