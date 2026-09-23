#include "test_database.h"
#include "fakesessionstore.h"
#include <QtTest>
#include <algorithm>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using TestCommon::createSettingsFile;
using TestCommon::fixedNow;
using TestCommon::fixedNowUtc;
using TestCommon::mk;

void DatabaseTest::initTestCase()
{
    QVERIFY(data_dir_.isValid());
    TestCommon::setDataDirOverride(data_dir_.path());
    db_path_ = QDir(data_dir_.path()).filePath("uTimer.sqlite");
}

void DatabaseTest::cleanupTestCase()
{
    TestCommon::clearDataDirOverride();
}

void DatabaseTest::test_database_replaceAll_persists_durations_when_retention_enabled()
{
    // Arrange: history_days_to_keep>0 => ensureOpen/ensureSchema create DB
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 2));
    SqliteSessionStore manager(settings);
    QVERIFY(manager.ensureOpen_dbg()); // no-op but should succeed

    std::deque<TimeDuration> durations;
    QDateTime now = fixedNowUtc();
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addDays(-3), now.addDays(-3).addSecs(60)));
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addDays(-1), now.addDays(-1).addSecs(60)));
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now, now.addSecs(60)));

    // Act
    auto result = manager.replaceAll(Timeline(durations, std::nullopt), Timeline());

    // Assert
    QVERIFY(result.ok());
    // performRetentionCleanup() only runs from checkSchemaOnStartup()
    // (sqlitesessionstore.cpp), which this test never calls -- ensureOpen()
    // and the constructor do not invoke it. So no pruning happens here at
    // all, and this >= 2 does not exercise retention; that exact boundary is
    // what test_retention_cleanup_runs_once_across_multiple_opens and
    // test_D2_retentionCutoff_uses_injected_clock_exact_boundary pin.
    // Kept as-is: it is the original assertion, restated verbatim.
    auto loaded = manager.loadDurations();
    QVERIFY(static_cast<int>(loaded.size()) >= 2);
}

void DatabaseTest::test_database_disabled_history_ignores_writes_and_returns_no_durations()
{
    // Arrange: history_days_to_keep=0 disables db
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings disabled(createSettingsFile(tempDir.path(), 0));
    SqliteSessionStore managerDisabled(disabled);
    std::deque<TimeDuration> durations;
    QDateTime now = fixedNowUtc();
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now, now.addSecs(60)));

    // Act
    auto result = managerDisabled.commitSession(Timeline(durations, std::nullopt));

    // Assert
    QVERIFY(result.okOrDisabled());
    auto loadedDisabled = managerDisabled.loadDurations();
    QCOMPARE(static_cast<int>(loadedDisabled.size()), 0);
}

void DatabaseTest::test_explicitStartTimes_constructorComputesDuration()
{
    // Description: TimeDuration constructor computes duration from start/end

    // Arrange & Act
    QDateTime start = QDateTime::fromMSecsSinceEpoch(1000, Qt::UTC);
    QDateTime end = QDateTime::fromMSecsSinceEpoch(5000, Qt::UTC);
    TimeDuration d = TimeDuration::fromPersistedRow(DurationType::Activity, start, end);

    // Assert
    QCOMPARE(d.duration(), (qint64)4000);
    QCOMPARE(d.startTime.toMSecsSinceEpoch(), (qint64)1000);
    QCOMPARE(d.endTime.toMSecsSinceEpoch(), (qint64)5000);
}

void DatabaseTest::test_explicitStartTimes_startTimePreservedAfterClean()
{
    // Description: cleanDurations preserves startTime field after merges

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Activity, 1000, 2000)); // touches exactly -> will merge

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().startTime.toMSecsSinceEpoch(), (qint64)0);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)2000);
    QCOMPARE(d.front().duration(), (qint64)2000);
}

void DatabaseTest::test_explicitStartTimes_mergeUpdatesAllFields()
{
    // Description: Merge branch updates startTime, endTime, and duration consistently

    // Arrange - entry that extends before prev
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 1000, 2000)); // [1000, 2000]
    d.push_back(mk(DurationType::Activity, 500, 1500));  // starts before, ends inside

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().startTime.toMSecsSinceEpoch(), (qint64)500);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)2000);
    QCOMPARE(d.front().duration(), (qint64)1500);
}

void DatabaseTest::test_splitPreservesStartTime()
{
    // Description: When splitting a duration, the first segment should keep original startTime

    // Arrange
    QDateTime start = QDateTime::fromMSecsSinceEpoch(1000, Qt::UTC);
    QDateTime split = QDateTime::fromMSecsSinceEpoch(3000, Qt::UTC);
    QDateTime end = QDateTime::fromMSecsSinceEpoch(5000, Qt::UTC);

    // Act - simulate a split
    TimeDuration first = TimeDuration::fromPersistedRow(DurationType::Activity, start, split);
    TimeDuration second = TimeDuration::fromPersistedRow(DurationType::Pause, split, end);

    // Assert
    QCOMPARE(first.startTime.toMSecsSinceEpoch(), (qint64)1000);
    QCOMPARE(first.endTime.toMSecsSinceEpoch(), (qint64)3000);
    QCOMPARE(first.duration(), (qint64)2000);
    QCOMPARE(second.startTime.toMSecsSinceEpoch(), (qint64)3000);
    QCOMPARE(second.endTime.toMSecsSinceEpoch(), (qint64)5000);
    QCOMPARE(second.duration(), (qint64)2000);
}

void DatabaseTest::test_zeroDurationNotAdded()
{
    // Description: Zero-duration segments should not affect the deque

    // Arrange
    QDateTime now = QDateTime::fromMSecsSinceEpoch(1000, Qt::UTC);
    TimeDuration d = TimeDuration::fromPersistedRow(DurationType::Activity, now, now);

    // Assert
    QCOMPARE(d.duration(), (qint64)0);
}

void DatabaseTest::test_negativeDurationHandled()
{
    // Description: If end is before start, duration should be negative (validation catches this)

    // Arrange
    QDateTime start = QDateTime::fromMSecsSinceEpoch(5000, Qt::UTC);
    QDateTime end = QDateTime::fromMSecsSinceEpoch(3000, Qt::UTC);
    TimeDuration d = TimeDuration::fromPersistedRow(DurationType::Activity, start, end);

    // Assert - duration is negative, validation layer should reject
    QCOMPARE(d.duration(), (qint64)-2000);
}

void DatabaseTest::test_exactMatching_upsertReplacesById()
{
    // Description: Upsert replaces by exact start_time/type, leaving one row

    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(1'000'000, Qt::UTC);
    QDateTime end1 = start.addSecs(10);
    QDateTime end2 = start.addSecs(20);

    std::deque<TimeDuration> durations;
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start, end1));
    QVERIFY(manager.commitSession(Timeline(durations, std::nullopt)).ok());
    const SegmentId stableSegmentId = durations.front().segment_id;

    durations.clear();
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start, end2, stableSegmentId));
    QVERIFY(manager.commitSession(Timeline(durations, std::nullopt)).ok());

    const QString connName = "exact_match_query";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare(
            "SELECT COUNT(*), end_utc FROM durations "
            "WHERE segment_id = :segment_id"
        );
        query.bindValue(":segment_id", durations.front().segment_id.toString());
        QVERIFY(query.exec());
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        QCOMPARE(query.value(1).toString(), end2.toUTC().toString(Qt::ISODateWithMs));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void DatabaseTest::test_checkpointPreservesStartTimeOnUpdateBySegmentId()
{
    // Description: Checkpoint updates do not overwrite the original start time

    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(2'000'000, Qt::UTC);
    QDateTime end1 = start.addSecs(5);
    QDateTime end2 = start.addSecs(15);

    const SegmentId checkpointSegmentId = SegmentId::mint();
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, end1, checkpointSegmentId).ok());

    // "Drifted" only in the sense of differing from the original start passed
    // above; must stay < end2 (a positive interval) so C3's payload guard
    // does not itself reject this call -- the point under test is that the
    // UPDATE path ignores this parameter entirely, not that it is invalid.
    QDateTime driftedStart = start.addSecs(10);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, driftedStart, end2, checkpointSegmentId).ok());

    const QString connName = "checkpoint_query";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare("SELECT start_utc, end_utc FROM durations WHERE segment_id = :segment_id");
        query.bindValue(":segment_id", checkpointSegmentId.toString());
        QVERIFY(query.exec());
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), start.toUTC().toString(Qt::ISODateWithMs));
        QCOMPARE(query.value(1).toString(), end2.toUTC().toString(Qt::ISODateWithMs));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void DatabaseTest::test_clockDriftResilience_durationStoredFromElapsed()
{
    // Description: After dropping the stored duration column, duration is computed
    // from start_utc/end_utc at load time. Verify the checkpoint row is written
    // and loadable with the correct computed duration.

    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(3'000'000, Qt::UTC);
    QDateTime end = start.addSecs(3600);
    const qint64 wallClockMs = start.msecsTo(end); // 3600000ms

    const SegmentId checkpointSegmentId = SegmentId::mint();
    // The elapsed parameter is ignored for storage; only start/end UTC matter.
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, end, checkpointSegmentId).ok());

    const QString connName = "drift_query";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare("SELECT start_utc, end_utc FROM durations WHERE segment_id = :segment_id");
        query.bindValue(":segment_id", checkpointSegmentId.toString());
        QVERIFY(query.exec());
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), start.toUTC().toString(Qt::ISODateWithMs));
        QCOMPARE(query.value(1).toString(), end.toUTC().toString(Qt::ISODateWithMs));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    // Duration is computed from timestamps at load.
    auto orphans = manager.loadUnfinalizedCheckpoints_dbg(); 
    QCOMPARE(static_cast<int>(orphans.size()), 1);
    QCOMPARE(orphans[0].duration, wallClockMs);
}

void DatabaseTest::test_loadDurations_skipsNegativeDurationRows()
{
    // Description: rows with start_utc > end_utc (reversed timestamps) are skipped at load.
    // (Negative stored duration was the old concept; now duration is always computed.)

    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    SqliteSessionStore manager(settings);

    QDateTime start = QDateTime::fromMSecsSinceEpoch(4'000'000, Qt::UTC);
    QDateTime end = start.addSecs(10);
    std::deque<TimeDuration> durations;
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));
    QVERIFY(manager.commitSession(Timeline(durations, std::nullopt)).ok());

    // Insert a row with start > end (reversed) — should be skipped at load.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    query.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, :type, :start_utc, :end_utc, 1)"
    );
    query.bindValue(":segment_id", SegmentId::mint().toString());
    query.bindValue(":type", static_cast<int>(DurationType::Activity));
    query.bindValue(":start_utc", end.toUTC().toString(Qt::ISODateWithMs));   // reversed
    query.bindValue(":end_utc", start.toUTC().toString(Qt::ISODateWithMs));   // reversed
    QVERIFY(query.exec());
    manager.closeConnection_dbg();

    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1); // reversed row skipped
}

void DatabaseTest::test_database_transaction_rollback_on_replace_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Create initial data
    QDateTime now = fixedNow();
    std::deque<TimeDuration> originalData;
    originalData.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-200), now.addSecs(-190)));
    originalData.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-180), now.addSecs(-170)));
    QVERIFY(manager.commitSession(Timeline(originalData, std::nullopt)).ok());
    
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2);
    
    // Make database read-only before entering replaceAll (R07-004): this
    // fails the first write inside the transaction (the Replace-mode DELETE),
    // not a boundary between DELETE and INSERT -- nothing changes permissions
    // mid-transaction here.
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser));
    
    // Try Replace mode - should fail and rollback
    std::deque<TimeDuration> replacementData;
    replacementData.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-50), now.addSecs(-40)));
    QVERIFY(!manager.replaceAll(Timeline(replacementData, std::nullopt), Timeline()).ok());
    
    // Restore permissions
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser));
    
    // Original data should be preserved (rollback after DELETE)
    loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2);
}

void DatabaseTest::test_database_checkpoint_single_row_per_segment()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime start = fixedNow();
    QDateTime end = start.addSecs(10);
    const SegmentId segmentId = SegmentId::mint();

    // First checkpoint - creates new row
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, end, segmentId).ok());

    // Second checkpoint with same ID - should UPDATE existing row
    QDateTime newEnd = start.addSecs(20);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, newEnd, segmentId).ok());

    // Verify checkpoint row remains unfinalized and updated in place.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    query.prepare("SELECT COUNT(*), end_utc, is_finalized FROM durations WHERE segment_id = :segment_id");
    query.bindValue(":segment_id", segmentId.toString());
    QVERIFY(query.exec());
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    QCOMPARE(query.value(1).toString(), newEnd.toUTC().toString(Qt::ISODateWithMs));
    QCOMPARE(query.value(2).toInt(), 0);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_database_checkpoint_missing_segment_reinserts_same_segment_id()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime start = fixedNow();
    QDateTime end = start.addSecs(10);
    const SegmentId segmentId = SegmentId::mint();

    // Create checkpoint
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, end, segmentId).ok());

    // Manually delete the checkpoint row (simulating retention cleanup)
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    query.prepare("DELETE FROM durations WHERE segment_id = :segment_id");
    query.bindValue(":segment_id", segmentId.toString());
    QVERIFY(query.exec());
    manager.closeConnection_dbg();

    // Try to update checkpoint - should detect missing row and INSERT new one
    QDateTime newEnd = start.addSecs(20);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, newEnd, segmentId).ok());

    // Verify new checkpoint row exists as unfinalized
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verifyQuery(manager.rawDb_dbg());
    verifyQuery.prepare("SELECT COUNT(*), end_utc, is_finalized FROM durations WHERE segment_id = :segment_id");
    verifyQuery.bindValue(":segment_id", segmentId.toString());
    QVERIFY(verifyQuery.exec());
    QVERIFY(verifyQuery.next());
    QCOMPARE(verifyQuery.value(0).toInt(), 1);
    QCOMPARE(verifyQuery.value(1).toString(), newEnd.toUTC().toString(Qt::ISODateWithMs));
    QCOMPARE(verifyQuery.value(2).toInt(), 0);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_database_checkpoint_preserves_start_time()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime originalStart = fixedNow();
    QDateTime firstEnd = originalStart.addSecs(10);
    const SegmentId checkpointSegmentId = SegmentId::mint();

    // First checkpoint
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, originalStart, firstEnd, checkpointSegmentId).ok());

    // Update checkpoint with new end time (simulate ongoing timer)
    QDateTime secondEnd = originalStart.addSecs(30);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, originalStart, secondEnd, checkpointSegmentId).ok());

    // Verify start_utc/end_utc are correct and the row is unfinalized.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    query.prepare("SELECT start_utc, end_utc, is_finalized FROM durations WHERE segment_id = :segment_id");
    query.bindValue(":segment_id", checkpointSegmentId.toString());
    QVERIFY(query.exec());
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), originalStart.toUTC().toString(Qt::ISODateWithMs));
    QCOMPARE(query.value(1).toString(), secondEnd.toUTC().toString(Qt::ISODateWithMs));
    QCOMPARE(query.value(2).toInt(), 0);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_database_update_by_id_insert_mode()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNow();
    std::deque<TimeDuration> durations;
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-90)));
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-80), now.addSecs(-70)));
    
    // First upsert - should INSERT both
    QVERIFY(manager.commitSession(Timeline(durations, std::nullopt)).ok());
    
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2);
}

void DatabaseTest::test_database_update_by_id_updates_existing_row_on_start_drift()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNow();
    QDateTime start1 = now.addSecs(-100);
    QDateTime end1 = now.addSecs(-90);
    
    // Insert initial duration
    std::deque<TimeDuration> initial;
    initial.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start1, end1));
    QVERIFY(manager.commitSession(Timeline(initial, std::nullopt)).ok());
    
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].duration(), (qint64)10000);
    
    // Update same segment with drifted start time and longer end.
    std::deque<TimeDuration> updated;
    QDateTime driftedStart = start1.addSecs(5);
    QDateTime end2 = now.addSecs(-80); // Extended duration
    updated.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, driftedStart, end2, initial.front().segment_id));
    QVERIFY(manager.commitSession(Timeline(updated, std::nullopt)).ok());
    
    // Should update existing row by segment_id (no duplicate).
    loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].duration(), driftedStart.msecsTo(end2));
    QCOMPARE(loaded[0].startTime, driftedStart);
}

void DatabaseTest::test_database_update_by_id_different_segments_same_start_both_exist()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNow();
    QDateTime start = now.addSecs(-100);
    
    // Insert Activity at same start time
    std::deque<TimeDuration> activity;
    activity.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start, start.addSecs(10)));
    QVERIFY(manager.commitSession(Timeline(activity, std::nullopt)).ok());
    
    // Insert Pause at SAME start time (different type)
    std::deque<TimeDuration> pause;
    pause.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, start, start.addSecs(5)));
    QVERIFY(manager.commitSession(Timeline(pause, std::nullopt)).ok());
    
    // Both should exist because segment_id differs.
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2);
}

void DatabaseTest::test_database_update_by_id_empty_deque()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    std::deque<TimeDuration> empty;
    QVERIFY(manager.commitSession(Timeline(empty, std::nullopt)).ok()); // Should succeed as no-op
}

void DatabaseTest::test_database_load_negative_duration()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Directly insert a row with valid timestamps.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime end = now.addSecs(5);
    query.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                 "VALUES (:segment_id, 0, :start_utc, :end_utc, 1)");
    query.bindValue(":segment_id", SegmentId::mint().toString());
    query.bindValue(":start_utc", now.toString(Qt::ISODateWithMs));
    query.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
    QVERIFY(query.exec());
    manager.closeConnection_dbg();

    // Duration is computed from timestamps at load — always non-negative.
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QVERIFY(loaded[0].duration() >= 0);
}

void DatabaseTest::test_database_load_start_after_end()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Manually insert start > end (reversed)
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime start = now;
    const QDateTime end = now.addSecs(-10); // End BEFORE start

    query.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                 "VALUES (:segment_id, 0, :start_utc, :end_utc, 1)");
    query.bindValue(":segment_id", SegmentId::mint().toString());
    query.bindValue(":start_utc", start.toString(Qt::ISODateWithMs));
    query.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
    QVERIFY(query.exec());
    manager.closeConnection_dbg();

    // Load should SKIP invalid entry
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)0);
}

void DatabaseTest::test_database_load_invalid_enum_type()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Manually insert invalid type (valid are 0=Activity, 1=Pause)
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime end = now.addSecs(10);

    query.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                 "VALUES (:segment_id, 99, :start_utc, :end_utc, 1)");
    query.bindValue(":segment_id", SegmentId::mint().toString());
    query.bindValue(":start_utc", now.toString(Qt::ISODateWithMs));
    query.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
    QVERIFY(query.exec());
    manager.closeConnection_dbg();

    // Load should skip invalid type
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)0);
}

void DatabaseTest::test_database_load_duration_mismatch_tolerance()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const QDateTime start = QDateTime::currentDateTimeUtc();
    const QDateTime end = start.addMSecs(1000);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    query.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                 "VALUES (:segment_id, 0, :start_utc, :end_utc, 1)");
    query.bindValue(":segment_id", SegmentId::mint().toString());
    query.bindValue(":start_utc", start.toString(Qt::ISODateWithMs));
    query.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
    QVERIFY(query.exec());
    manager.closeConnection_dbg();

    // Duration is computed from timestamps.
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].duration(), (qint64)1000);
}

void DatabaseTest::test_database_timezone_roundtrip()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Create duration in local time
    QDateTime localStart = fixedNow();
    QDateTime localEnd = localStart.addSecs(60);
    
    std::deque<TimeDuration> durations;
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, localStart, localEnd));
    
    // Save and reload
    QVERIFY(manager.commitSession(Timeline(durations, std::nullopt)).ok());
    auto loaded = manager.loadDurations();
    
    QCOMPARE(loaded.size(), (size_t)1);
    // Times should be preserved (stored as UTC, loaded as local)
    QCOMPARE(loaded[0].startTime.toString(Qt::ISODate), localStart.toString(Qt::ISODate));
    QCOMPARE(loaded[0].endTime.toString(Qt::ISODate), localEnd.toString(Qt::ISODate));
}

void DatabaseTest::test_database_load_invalid_type_increments_skipped_and_omits_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    const QDateTime start = QDateTime::currentDateTimeUtc();
    const QDateTime end = start.addSecs(10);
    query.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                 "VALUES (:segment_id, 99, :start_utc, :end_utc, 1)");
    query.bindValue(":segment_id", SegmentId::mint().toString());
    query.bindValue(":start_utc", start.toString(Qt::ISODateWithMs));
    query.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
    QVERIFY(query.exec());
    const qint64 rowId = query.lastInsertId().toLongLong();
    manager.closeConnection_dbg();

    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(0));

    // C3 (R13-001/R14-001, extends B2's recovery-time half to ordinary load):
    // a malformed row must be deleted, not merely omitted from the returned
    // result -- otherwise it survives to be silently purged later by an
    // unrelated edited History save.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery check(manager.rawDb_dbg());
    check.prepare("SELECT COUNT(*) FROM durations WHERE id = :id");
    check.bindValue(":id", rowId);
    QVERIFY(check.exec());
    QVERIFY(check.next());
    QCOMPARE(check.value(0).toInt(), 0);
    manager.closeConnection_dbg();
}

/**
 * C3 (R13-001/R14-001): loadDurations() must delete every genuinely malformed
 * finalized row (invalid timestamp, invalid type, empty id, non-positive
 * interval) in one transaction and return only the valid row -- mirroring
 * B2's recoverStartupCheckpoints() malformed-row deletion, extended here to
 * ordinary (already-finalized) load rather than only unfinalized checkpoints.
 */
void DatabaseTest::test_C3_loadDurations_deletes_all_malformed_row_shapes()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    // A genuinely valid finalized row that must survive and load normally.
    const QDateTime validStart = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime validEnd   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);
    QSqlQuery validInsert(manager.rawDb_dbg());
    validInsert.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                         "VALUES (:seg, 0, :s, :e, 1)");
    validInsert.bindValue(":seg", SegmentId::mint().toString());
    validInsert.bindValue(":s", validStart.toString(Qt::ISODateWithMs));
    validInsert.bindValue(":e", validEnd.toString(Qt::ISODateWithMs));
    QVERIFY(validInsert.exec());

    QSqlQuery pragmaOn(manager.rawDb_dbg());
    QVERIFY(pragmaOn.exec("PRAGMA ignore_check_constraints = ON"));

    // Malformed: empty segment_id.
    QSqlQuery emptyId(manager.rawDb_dbg());
    emptyId.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                     "VALUES ('', 0, :s, :e, 1)");
    emptyId.bindValue(":s", QDateTime(QDate(2025, 6, 1), QTime(11, 0, 0), Qt::UTC).toString(Qt::ISODateWithMs));
    emptyId.bindValue(":e", QDateTime(QDate(2025, 6, 1), QTime(11, 5, 0), Qt::UTC).toString(Qt::ISODateWithMs));
    QVERIFY(emptyId.exec());
    const qint64 emptyIdRow = emptyId.lastInsertId().toLongLong();

    // Malformed: invalid type value.
    QSqlQuery badType(manager.rawDb_dbg());
    badType.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                     "VALUES (:seg, 99, :s, :e, 1)");
    badType.bindValue(":seg", SegmentId::mint().toString());
    badType.bindValue(":s", QDateTime(QDate(2025, 6, 1), QTime(12, 0, 0), Qt::UTC).toString(Qt::ISODateWithMs));
    badType.bindValue(":e", QDateTime(QDate(2025, 6, 1), QTime(12, 5, 0), Qt::UTC).toString(Qt::ISODateWithMs));
    QVERIFY(badType.exec());
    const qint64 badTypeRow = badType.lastInsertId().toLongLong();

    // Malformed: invalid (unparseable) timestamp.
    QSqlQuery badTs(manager.rawDb_dbg());
    badTs.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, 'not-a-date', 'also-not-a-date', 1)");
    badTs.bindValue(":seg", SegmentId::mint().toString());
    QVERIFY(badTs.exec());
    const qint64 badTsRow = badTs.lastInsertId().toLongLong();

    // Malformed: non-positive interval (start >= end).
    QSqlQuery badInterval(manager.rawDb_dbg());
    badInterval.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                         "VALUES (:seg, 0, :s, :e, 1)");
    badInterval.bindValue(":seg", SegmentId::mint().toString());
    const QString sameTs = QDateTime(QDate(2025, 6, 1), QTime(13, 0, 0), Qt::UTC).toString(Qt::ISODateWithMs);
    badInterval.bindValue(":s", sameTs);
    badInterval.bindValue(":e", sameTs);
    QVERIFY(badInterval.exec());
    const qint64 badIntervalRow = badInterval.lastInsertId().toLongLong();

    manager.closeConnection_dbg();

    const LoadResult loaded = manager.loadDurations();

    // Assert: only the valid row is returned.
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded[0].startTime, validStart.toLocalTime());

    // Assert: all four malformed rows are physically gone (deleted
    // transactionally, logged once -- no UI notification, matching B2).
    QVERIFY(manager.ensureOpen_dbg());
    for (qint64 id : {emptyIdRow, badTypeRow, badTsRow, badIntervalRow}) {
        QSqlQuery check(manager.rawDb_dbg());
        check.prepare("SELECT COUNT(*) FROM durations WHERE id = :id");
        check.bindValue(":id", id);
        QVERIFY(check.exec());
        QVERIFY(check.next());
        QCOMPARE(check.value(0).toInt(), 0);
    }
    // The valid row is untouched.
    QSqlQuery countAll(manager.rawDb_dbg());
    QVERIFY(countAll.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(countAll.next());
    QCOMPARE(countAll.value(0).toInt(), 1);
    manager.closeConnection_dbg();
}

/**
 * Finding 4 (C3 review, Medium): loadDurations()'s malformed-row deletion
 * says "will retry on next load" but had no test proving commit-or-rollback
 * on a genuine commit failure -- the C2 standard for every other write
 * boundary (see test_C2_*_rolls_back_on_genuine_commit_*). Reuses C2's
 * existing setForceCommitFailure_dbg() injection seam (loadDurations' delete
 * now routes through commitOrRollback(), same as every other write path).
 */
void DatabaseTest::test_C3_loadDurations_rolls_back_malformed_delete_on_commit_failure_and_retries()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    // A valid row that must survive and load normally regardless of the
    // malformed row's fate.
    const QDateTime validStart = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime validEnd   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);
    QSqlQuery validInsert(manager.rawDb_dbg());
    validInsert.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                         "VALUES (:seg, 0, :s, :e, 1)");
    validInsert.bindValue(":seg", SegmentId::mint().toString());
    validInsert.bindValue(":s", validStart.toString(Qt::ISODateWithMs));
    validInsert.bindValue(":e", validEnd.toString(Qt::ISODateWithMs));
    QVERIFY(validInsert.exec());

    // Malformed: non-positive interval (start == end).
    QSqlQuery badInterval(manager.rawDb_dbg());
    badInterval.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                         "VALUES (:seg, 0, :s, :e, 1)");
    badInterval.bindValue(":seg", SegmentId::mint().toString());
    const QString sameTs = QDateTime(QDate(2025, 6, 1), QTime(13, 0, 0), Qt::UTC).toString(Qt::ISODateWithMs);
    badInterval.bindValue(":s", sameTs);
    badInterval.bindValue(":e", sameTs);
    QVERIFY(badInterval.exec());
    const qint64 badIntervalRow = badInterval.lastInsertId().toLongLong();
    manager.closeConnection_dbg();

    // Act: inject a COMMIT failure for the malformed-row delete transaction.
    manager.setForceCommitFailure_dbg(true);
    const LoadResult firstLoad = manager.loadDurations();

    // Assert: the returned result is unaffected (built from valid rows before
    // the delete-commit ever runs), but the malformed row is still PHYSICALLY
    // present -- the delete was rolled back, not left half-applied.
    QCOMPARE(firstLoad.size(), static_cast<size_t>(1));
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery stillThere(manager.rawDb_dbg());
    stillThere.prepare("SELECT COUNT(*) FROM durations WHERE id = :id");
    stillThere.bindValue(":id", badIntervalRow);
    QVERIFY(stillThere.exec());
    QVERIFY(stillThere.next());
    QCOMPARE(stillThere.value(0).toInt(), 1);

    // Assert: the connection recovers -- a second (non-injected) load
    // retries the delete and succeeds this time.
    const LoadResult secondLoad = manager.loadDurations();
    QCOMPARE(secondLoad.size(), static_cast<size_t>(1));
    QSqlQuery goneNow(manager.rawDb_dbg());
    goneNow.prepare("SELECT COUNT(*) FROM durations WHERE id = :id");
    goneNow.bindValue(":id", badIntervalRow);
    QVERIFY(goneNow.exec());
    QVERIFY(goneNow.next());
    QCOMPARE(goneNow.value(0).toInt(), 0);

    manager.closeConnection_dbg();
}

void DatabaseTest::test_database_load_200ms_mismatch_increments_repaired_and_uses_computed()
{
    // Duration is now always computed from timestamps; no stored duration to mismatch.
    // Verify that a valid row loads with the correct computed duration.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    const QDateTime start = QDateTime::currentDateTimeUtc();
    const QDateTime end = start.addMSecs(1000);
    query.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                 "VALUES (:segment_id, 0, :start_utc, :end_utc, 1)");
    query.bindValue(":segment_id", SegmentId::mint().toString());
    query.bindValue(":start_utc", start.toString(Qt::ISODateWithMs));
    query.bindValue(":end_utc", end.toString(Qt::ISODateWithMs));
    QVERIFY(query.exec());
    manager.closeConnection_dbg();

    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded[0].duration(), static_cast<qint64>(1000));
}

void DatabaseTest::test_database_millisecond_precision()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Create timestamps with millisecond precision
    QDateTime start = fixedNow();
    qint64 startMsec = start.toMSecsSinceEpoch();
    startMsec = (startMsec / 1000) * 1000 + 123; // Set milliseconds to 123
    start = QDateTime::fromMSecsSinceEpoch(startMsec);
    
    QDateTime end = start.addMSecs(4567); // Add 4567ms
    
    std::deque<TimeDuration> durations;
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));
    
    QVERIFY(manager.commitSession(Timeline(durations, std::nullopt)).ok());
    auto loaded = manager.loadDurations();
    
    QCOMPARE(loaded.size(), (size_t)1);
    // Milliseconds should be preserved
    QCOMPARE(loaded[0].startTime.time().msec(), 123);
    QCOMPARE(loaded[0].duration(), (qint64)4567);
}

void DatabaseTest::test_database_schema_validation_fresh_database()
{
    resetDatabaseFile();
    
    // No database file exists
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);
    
    // Fresh DB: should return Created (file did not exist, schema was freshly built)
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Created);
}

void DatabaseTest::test_database_schema_creates_idx_finalized_start()
{
    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery query(manager.rawDb_dbg());
    QVERIFY(query.exec(
        "SELECT name FROM sqlite_master "
        "WHERE type = 'index' AND name = 'idx_start_utc'"
    ));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QString("idx_start_utc"));
}

void DatabaseTest::test_database_backup_file_creation()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Snapshot existing artifacts so stale files from prior runs don't mask failures
    QDir testDir(data_dir_.path());
    const int backupsBefore = testDir.entryList(QStringList() << "*.backup", QDir::Files).size();
    const int txtBefore     = testDir.entryList(QStringList() << "*.durations.txt", QDir::Files).size();

    // replaceAll() is the only public write path that creates a backup
    // (D11: commitSession() never backs up -- see
    // test_W_backup_created_before_replaceAll_not_commitSession).
    QDateTime now = fixedNow();
    std::deque<TimeDuration> durations;
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-90)));

    QVERIFY(manager.replaceAll(Timeline(durations, std::nullopt), Timeline()).ok());

    // Assert this run produced new backup artifacts (delta from snapshot)
    QVERIFY(testDir.entryList(QStringList() << "*.backup", QDir::Files).size() > backupsBefore);
    QVERIFY(testDir.entryList(QStringList() << "*.durations.txt", QDir::Files).size() > txtBefore);
}

void DatabaseTest::test_database_backup_preserves_contents()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Create data
    QDateTime now = fixedNow();
    std::deque<TimeDuration> original;
    original.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-90)));
    original.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-80), now.addSecs(-70)));
    
    QVERIFY(manager.commitSession(Timeline(original, std::nullopt)).ok());

    // Trigger backup via replaceAll — creates a backup of the 2-row DB, then writes 1 row
    std::deque<TimeDuration> replacement;
    replacement.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-50), now.addSecs(-40)));
    QVERIFY(manager.replaceAll(Timeline(replacement, std::nullopt), Timeline()).ok());

    // Identify the exact backup replaceAll() wrote instead of scanning the
    // data directory (mtime-ordered scans tie at millisecond granularity).
    QString backupPath = manager.lastBackupPath_dbg();
    QVERIFY2(!backupPath.isEmpty(), "No new backup file was created by this test");

    // Restore from backup and verify it has original data
    QFile::remove(db_path_);
    QVERIFY(QFile::copy(backupPath, db_path_));

    // Open a fresh connection so we read the restored file, not the old cached connection.
    SqliteSessionStore restored(settings);
    auto loaded = restored.loadDurations();
    QCOMPARE(loaded.size(), (size_t)2); // Original had 2 entries
}

void DatabaseTest::test_database_backup_filenames_collision_free()
{
    // Regression: two back-to-back saves must produce two DISTINCT .backup files
    // even if both calls happen within the same millisecond.  The old second-
    // resolution names would collide and QFile::copy() would silently skip the
    // second backup, leaving only 1 distinct file.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDir testDir(data_dir_.path());
    const int backupsBefore = testDir.entryList(QStringList() << "*.backup", QDir::Files).size();

    QDateTime now = fixedNow();
    std::deque<TimeDuration> durations;
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-90)));

    // Two writes back-to-back via replaceAll (the backup-creating public
    // path) -- no qWait, may land in the same millisecond.
    QVERIFY(manager.replaceAll(Timeline(durations, std::nullopt), Timeline()).ok());
    QVERIFY(manager.replaceAll(Timeline(durations, std::nullopt), Timeline()).ok());

    const int backupsAfter = testDir.entryList(QStringList() << "*.backup", QDir::Files).size();
    QCOMPARE(backupsAfter - backupsBefore, 2); // both saves must produce distinct files
}

// ============================================================================
// hasEntriesForDate tri-state result tests (T18)
// ============================================================================

void DatabaseTest::test_hasEntriesForDate_returns_unknown_when_history_disabled()
{
    // Arrange: history_days_to_keep = 0 disables the database entirely.
    // hasEntriesForDate must return Unknown (not No) so the caller
    // never assumes "no entries exist" when it simply can't check.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 0));
    SqliteSessionStore manager(settings);

    // Act
    EntriesForDateResult result = manager.hasEntriesForDate(QDate::currentDate());

    // Assert
    QCOMPARE(result, EntriesForDateResult::Unknown);
}

void DatabaseTest::test_hasEntriesForDate_returns_no_on_empty_database()
{
    // Arrange: history enabled, fresh empty database.
    // hasEntriesForDate must return No (not Unknown) to confirm that
    // zero entries exist, allowing the caller to add boot time.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Act
    EntriesForDateResult result = manager.hasEntriesForDate(QDate::currentDate());

    // Assert
    QCOMPARE(result, EntriesForDateResult::No);
}

void DatabaseTest::test_hasEntriesForDate_returns_yes_when_entries_exist()
{
    // Arrange: history enabled, database has finalized entries for today.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNowUtc();
    std::deque<TimeDuration> durations;
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-60), now));
    QVERIFY(manager.commitSession(Timeline(durations, std::nullopt)).ok());

    // Act
    EntriesForDateResult result = manager.hasEntriesForDate(QDate::currentDate());

    // Assert
    QCOMPARE(result, EntriesForDateResult::Yes);
}

void DatabaseTest::test_hasEntriesForDate_utc_positive_offset()
{
    // Simulate Asia/Tokyo (+09:00).
    // A row stored as UTC end_date='2024-12-31' end_time='15:30:00.000'
    // equals local 2025-01-01 00:30:00 +09:00, so it belongs to local date
    // 2025-01-01, not 2024-12-31.

    // RAII guard: restores TZ even if a QVERIFY/QCOMPARE assertion fires return early.
    struct TzGuard {
        QByteArray orig;
        TzGuard() : orig(qgetenv("TZ")) {}
        ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
    } tzGuard;
    qputenv("TZ", "Asia/Tokyo");
    tzset();

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    // Use a large retention window so old test-specific dates are never pruned.
    Settings settings(createSettingsFile(tempDir.path(), 30000));

    // Bootstrap schema.
    {
        SqliteSessionStore bootstrap(settings);
        QVERIFY(bootstrap.ensureOpen_dbg());
    }

    // Seed the row with the UTC values directly.
    const QString connName = "seed_tz_pos";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, '2024-12-31T15:00:00.000Z', '2024-12-31T15:30:00.000Z', 1)"
        );
        q.bindValue(":sid", SegmentId::mint().toString());
        QVERIFY(q.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);

    // The row's UTC start is 2024-12-31T15:00Z = local 2025-01-01 00:00 in Tokyo.
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 1, 1)), EntriesForDateResult::Yes);
    QCOMPARE(manager.hasEntriesForDate(QDate(2024, 12, 31)), EntriesForDateResult::No);
}

void DatabaseTest::test_hasEntriesForDate_utc_negative_offset()
{
    // Simulate Pacific/Honolulu (-10:00).
    // A row stored as UTC end_date='2025-01-02' end_time='08:00:00.000'
    // equals local 2025-01-01 22:00:00 -10:00, so it belongs to local date
    // 2025-01-01, not 2025-01-02.

    struct TzGuard {
        QByteArray orig;
        TzGuard() : orig(qgetenv("TZ")) {}
        ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
    } tzGuard;
    qputenv("TZ", "Pacific/Honolulu");
    tzset();

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    // Use a large retention window so old test-specific dates are never pruned.
    Settings settings(createSettingsFile(tempDir.path(), 30000));

    {
        SqliteSessionStore bootstrap(settings);
        QVERIFY(bootstrap.ensureOpen_dbg());
    }

    const QString connName = "seed_tz_neg";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, '2025-01-02T07:00:00.000Z', '2025-01-02T08:00:00.000Z', 1)"
        );
        q.bindValue(":sid", SegmentId::mint().toString());
        QVERIFY(q.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);

    // The row's UTC start is 2025-01-02T07:00Z = local 2025-01-01 21:00 in Honolulu.
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 1, 1)), EntriesForDateResult::Yes);
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 1, 2)), EntriesForDateResult::No);
}

void DatabaseTest::test_hasEntriesForDate_utc_regression()
{
    // UTC zone, mid-day entry: end_date='2025-06-15' end_time='12:00:00.000'.
    // With TZ=UTC local date == UTC date, so behaviour should be unchanged.

    struct TzGuard {
        QByteArray orig;
        TzGuard() : orig(qgetenv("TZ")) {}
        ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
    } tzGuard;
    qputenv("TZ", "UTC");
    tzset();

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    // Use a large retention window so old test-specific dates are never pruned.
    Settings settings(createSettingsFile(tempDir.path(), 30000));

    {
        SqliteSessionStore bootstrap(settings);
        QVERIFY(bootstrap.ensureOpen_dbg());
    }

    const QString connName = "seed_tz_utc";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, '2025-06-15T11:00:00.000Z', '2025-06-15T12:00:00.000Z', 1)"
        );
        q.bindValue(":sid", SegmentId::mint().toString());
        QVERIFY(q.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);

    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 6, 15)), EntriesForDateResult::Yes);
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 6, 14)), EntriesForDateResult::No);
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 6, 16)), EntriesForDateResult::No);
}

void DatabaseTest::test_hasEntriesForDate_overlap_detects_legacy_cross_midnight_row()
{
    // D1 (R08-004): a legacy row spanning local midnight -- unsupported by any
    // uTimer write path, but reachable via manual database modification --
    // must still suppress boot credit for the day it spills into. A row
    // starting 2025-06-14 23:30 local and ending 2025-06-15 00:30 local
    // overlaps 2025-06-15 even though it did not *start* there; boot credit
    // must not be added for 2025-06-15 in that case (double-counting the
    // 00:00-00:30 portion already covered by the legacy row).
    struct TzGuard {
        QByteArray orig;
        TzGuard() : orig(qgetenv("TZ")) {}
        ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
    } tzGuard;
    qputenv("TZ", "UTC");
    tzset();

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));

    {
        SqliteSessionStore bootstrap(settings);
        QVERIFY(bootstrap.ensureOpen_dbg());
    }

    const QString connName = "seed_legacy_cross_midnight";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, '2025-06-14T23:30:00.000Z', '2025-06-15T00:30:00.000Z', 1)"
        );
        q.bindValue(":sid", SegmentId::mint().toString());
        QVERIFY(q.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);

    // The row overlaps both calendar dates it touches.
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 6, 14)), EntriesForDateResult::Yes);
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 6, 15)), EntriesForDateResult::Yes);
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 6, 13)), EntriesForDateResult::No);
    QCOMPARE(manager.hasEntriesForDate(QDate(2025, 6, 16)), EntriesForDateResult::No);
}

void DatabaseTest::test_hasEntriesForDate_exact_midnight_endpoint()
{
    // D1 (R08-004, MEDIUM-2): pins both comparison operators in
    // hasEntriesForDate's predicate (start_utc < :utcEndTs AND end_utc >
    // :utcStartTs) as strict. Reuses the same interval as
    // TestCommon::dayOverlapRows()'s "exact_midnight_endpoint_same_day" /
    // "exact_midnight_endpoint_no_spill_to_next_day" rows -- a row ending
    // exactly at local midnight belongs to its start date only. Flipping
    // either operator to non-strict would make this test fail while every
    // other hasEntriesForDate test (which deliberately sits off both
    // boundaries) keeps passing.
    struct TzGuard {
        QByteArray orig;
        TzGuard() : orig(qgetenv("TZ")) {}
        ~TzGuard() { orig.isEmpty() ? qunsetenv("TZ") : qputenv("TZ", orig); tzset(); }
    } tzGuard;
    qputenv("TZ", "UTC");
    tzset();

    const auto rows = TestCommon::dayOverlapRows();
    auto findRow = [&rows](const char* name) -> const TestCommon::DayOverlapRow& {
        auto it = std::find_if(rows.begin(), rows.end(),
            [name](const TestCommon::DayOverlapRow& r) { return qstrcmp(r.name, name) == 0; });
        Q_ASSERT(it != rows.end());
        return *it;
    };
    const TestCommon::DayOverlapRow& sameDay = findRow("exact_midnight_endpoint_same_day");
    const TestCommon::DayOverlapRow& noSpill = findRow("exact_midnight_endpoint_no_spill_to_next_day");

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));

    {
        SqliteSessionStore bootstrap(settings);
        QVERIFY(bootstrap.ensureOpen_dbg());
    }

    const QDateTime start(QDate(sameDay.startY, sameDay.startMo, sameDay.startD),
                           QTime(sameDay.startH, sameDay.startMi, sameDay.startS), Qt::UTC);
    const QDateTime end(QDate(sameDay.endY, sameDay.endMo, sameDay.endD),
                         QTime(sameDay.endH, sameDay.endMi, sameDay.endS), Qt::UTC);

    const QString connName = "seed_exact_midnight_endpoint";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, :s, :e, 1)"
        );
        q.bindValue(":sid", SegmentId::mint().toString());
        q.bindValue(":s", start.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":e", end.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);

    const QDate sameDayQuery(sameDay.queryY, sameDay.queryMo, sameDay.queryD);
    const QDate noSpillQuery(noSpill.queryY, noSpill.queryMo, noSpill.queryD);
    QCOMPARE(manager.hasEntriesForDate(sameDayQuery),
              sameDay.expectedOverlap ? EntriesForDateResult::Yes : EntriesForDateResult::No);
    QCOMPARE(manager.hasEntriesForDate(noSpillQuery),
              noSpill.expectedOverlap ? EntriesForDateResult::Yes : EntriesForDateResult::No);

    // D1 (R08-004 round 2, MEDIUM-1): the row above pins end_utc > (strict).
    // Pin start_utc < (strict) too, in a fresh database so the earlier row
    // (which already legitimately overlaps 2025-06-15) cannot mask the
    // mutation: a row starting exactly at 2025-06-16's local midnight must
    // NOT count as an entry for 2025-06-15. Flipping start_utc < to <= would
    // make this assertion fail while every other case in this test (and the
    // rest of the suite) keeps passing.
    resetDatabaseFile();
    {
        SqliteSessionStore bootstrap(settings);
        QVERIFY(bootstrap.ensureOpen_dbg());
    }
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, '2025-06-16T00:00:00.000Z', '2025-06-16T02:00:00.000Z', 1)"
        );
        q.bindValue(":sid", SegmentId::mint().toString());
        QVERIFY(q.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore boundaryManager(settings);
    QCOMPARE(boundaryManager.hasEntriesForDate(QDate(2025, 6, 15)), EntriesForDateResult::No);
}

// ============================================================================
// Retention cleanup once-per-session tests (T22)
// ============================================================================

void DatabaseTest::test_retention_cleanup_runs_once_across_multiple_opens()
{
    // Retention cleanup now runs in checkSchemaOnStartup(), not in the
    // constructor.  The test verifies: (1) after construction but before
    // checkSchemaOnStartup() the old entry is still present, (2) after
    // checkSchemaOnStartup() the old entry is removed, (3) additional
    // ensureOpen() calls are no-ops and do not re-trigger cleanup.

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 2)); // keep 2 days

    {
        SqliteSessionStore bootstrap(settings);
        QVERIFY(bootstrap.ensureOpen_dbg());
    }

    QDateTime now = QDateTime::currentDateTimeUtc();
    const QString connName = "seed_retention_test";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);

        const QDateTime oldStart = now.addDays(-10);
        const QDateTime oldEnd = oldStart.addSecs(60);
        query.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, :start_utc, :end_utc, 1)"
        );
        query.bindValue(":sid", SegmentId::mint().toString());
        query.bindValue(":start_utc", oldStart.toUTC().toString(Qt::ISODateWithMs));
        query.bindValue(":end_utc", oldEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(query.exec());

        const QDateTime recentStart = now.addSecs(-60);
        const QDateTime recentEnd = now;
        query.bindValue(":sid", SegmentId::mint().toString());
        query.bindValue(":start_utc", recentStart.toUTC().toString(Qt::ISODateWithMs));
        query.bindValue(":end_utc", recentEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(query.exec());

        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    // Construction does not run cleanup — both entries are still present.
    SqliteSessionStore manager(settings);
    QCOMPARE(static_cast<int>(manager.loadDurations().size()), 2);

    // checkSchemaOnStartup() runs performRetentionCleanup() — old entry removed.
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);
    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);
}

void DatabaseTest::test_retention_cleanup_retries_after_failure()
{
    // Cleanup runs in checkSchemaOnStartup(), not in the constructor.  When the
    // DB is read-only at construction time, the constructor detects the unwritable
    // file (via QFileInfo::isWritable()) and leaves the connection closed.
    // checkSchemaOnStartup() fails while the file is read-only.  Once write
    // permissions are restored, checkSchemaOnStartup() succeeds and runs cleanup.

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 2));

    {
        SqliteSessionStore seed(settings);
        QDateTime now = fixedNowUtc();
        std::deque<TimeDuration> durations;
        durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addDays(-10), now.addDays(-10).addSecs(60)));
        durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-60), now));
        QVERIFY(seed.replaceAll(Timeline(durations, std::nullopt), Timeline()).ok());
    }

    // Make the DB read-only.
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser));

    // Constructor detects read-only file and leaves connection closed.
    SqliteSessionStore manager(settings);
    QVERIFY(!manager.rawDb_dbg().isOpen());

    // checkSchemaOnStartup() fails while DB is read-only (ensureOpen() returns false).
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Inaccessible);

    // Restore write permissions.
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser));

    // checkSchemaOnStartup() now succeeds and runs cleanup.
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);

    // Old entry removed; recent entry survives.
    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);
}

/**
 * T25: Verifies that connection names are unique even when SqliteSessionStores
 * are repeatedly created and destroyed in a loop.
 *
 * The old implementation used reinterpret_cast<quintptr>(this) for connection
 * names, which collides when the allocator reuses the same address after
 * destruction. The atomic counter guarantees uniqueness regardless of address
 * reuse.
 */
void DatabaseTest::test_connection_names_unique_across_100_instances()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 0)); // history disabled — avoids DB I/O

    QSet<QString> connectionNames;

    // Act: create and destroy 100 SqliteSessionStores, collecting connection names.
    // With reinterpret_cast<quintptr>(this), the allocator frequently reuses the
    // same address, causing name collisions. With an atomic counter, every name
    // is guaranteed unique.
    for (int i = 0; i < 100; ++i) {
        auto* mgr = new SqliteSessionStore(settings);
        QString name = mgr->dbConnectionName_dbg();
        connectionNames.insert(name);
        delete mgr;
    }

    // Assert: all 100 names were distinct.
    QCOMPARE(connectionNames.size(), 100);
}

// ============================================================================
// Long-lived connection tests
// ============================================================================

/**
 * The database connection must be open immediately after construction
 * (history_days_to_keep_ > 0), without any caller needing to call ensureOpen().
 */
void DatabaseTest::test_connection_open_after_construction()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.rawDb_dbg().isOpen());
}

/**
 * PRAGMA synchronous=NORMAL must be set on the open connection.
 * Verified by querying PRAGMA synchronous after construction — the value
 * should be 1 (NORMAL).  This also confirms the PRAGMA was applied during
 * the single connection-open, not skipped.
 */
void DatabaseTest::test_pragma_synchronous_normal_set_after_construction()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.rawDb_dbg().isOpen());
    QSqlQuery q(manager.rawDb_dbg());
    QVERIFY(q.exec("PRAGMA synchronous"));
    QVERIFY(q.next());
    // 1 = NORMAL, 2 = FULL (SQLite default is FULL = 2)
    QCOMPARE(q.value(0).toInt(), 1);
}

// ============================================================================
// commitSession and replaceAll contract tests
// ============================================================================

/**
 * T: commitSession is upsert-by-segment-id.
 * Save a Timeline; mutate one segment's type; commitSession again;
 * assert one row in the DB, not two.
 */
void DatabaseTest::test_T_commitSession_upserts_by_segment_id()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNow();
    SegmentId segId = SegmentId::mint();
    std::deque<TimeDuration> segs;
    segs.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-60), now, segId));

    // First save
    QVERIFY(manager.commitSession(Timeline(segs, std::nullopt)).ok());

    // Mutate the type and save again with the same segment_id
    segs.front().type = DurationType::Pause;
    QVERIFY(manager.commitSession(Timeline(segs, std::nullopt)).ok());

    // Should have exactly one row
    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);
    QCOMPARE(loaded.durations.front().type, DurationType::Pause);
    QCOMPARE(loaded.durations.front().segment_id.toString(), segId.toString());
}

/**
 * U: Orphan cleanup is internal to commitSession.
 * Save a Timeline with two mergeable adjacent segments; commitSession
 * a normalized smaller Timeline (one segment); assert the merged-away
 * row is gone without the caller ever passing orphan IDs.
 */
void DatabaseTest::test_U_commitSession_orphan_cleanup_is_internal()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNow();
    SegmentId segA = SegmentId::mint();
    SegmentId segB = SegmentId::mint();

    // Two adjacent Activity segments — will merge inside commitSession's normalized()
    std::deque<TimeDuration> twoSegs;
    twoSegs.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50), segA));
    twoSegs.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-50), now.addSecs(-1), segB));

    // Seed them via scoped raw SQL (bypasses commitSession) so they're both in DB
    QVERIFY(manager.ensureOpen_dbg());
    for (const auto& seg : twoSegs) {
        QSqlQuery q(manager.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, :type, :s, :e, 1)");
        q.bindValue(":seg", seg.segment_id.toString());
        q.bindValue(":type", static_cast<int>(seg.type));
        q.bindValue(":s", seg.startTime.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":e", seg.endTime.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }
    manager.closeConnection_dbg();
    QCOMPARE(static_cast<int>(manager.loadDurations().size()), 2);

    // Now commitSession with the same two segments — commitSession normalizes
    // internally, merges them into one, and deletes the orphan (segB)
    QVERIFY(manager.commitSession(Timeline(twoSegs, std::nullopt)).ok());

    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);

    // Only segA (the keeper after merge) should remain
    bool segAFound = false;
    bool segBFound = false;
    for (const auto& d : loaded.durations) {
        if (d.segment_id == segA) segAFound = true;
        if (d.segment_id == segB) segBFound = true;
    }
    QVERIFY(segAFound || !segBFound); // exactly one segment, segB is gone
    QVERIFY(!segBFound);
}

/**
 * V: replaceAll wipes and rewrites.
 * Pre-populate with X rows, call replaceAll with Y rows, assert exactly Y remain.
 */
void DatabaseTest::test_V_replaceAll_wipes_and_rewrites()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNow();

    // Seed 3 rows
    std::deque<TimeDuration> initial;
    initial.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-300), now.addSecs(-200)));
    initial.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-200), now.addSecs(-100)));
    initial.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));
    QVERIFY(manager.commitSession(Timeline(initial, std::nullopt)).ok());
    QCOMPARE(static_cast<int>(manager.loadDurations().size()), 3);

    // replaceAll with 2 different rows
    std::deque<TimeDuration> replacement;
    replacement.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-80), now.addSecs(-40)));
    replacement.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-40), now.addSecs(-10)));

    QVERIFY(manager.replaceAll(Timeline(replacement, std::nullopt),
                                Timeline({}, std::nullopt)).ok());

    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 2);
}

/**
 * W: Backup is created before replaceAll but NOT before commitSession.
 * commitSession should NOT create a .backup file; replaceAll should.
 */
void DatabaseTest::test_W_backup_created_before_replaceAll_not_commitSession()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDir appDir(data_dir_.path());
    QStringList before = appDir.entryList(QStringList() << "*.backup", QDir::Files);

    QDateTime now = fixedNow();
    std::deque<TimeDuration> segs;
    segs.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-60), now));

    // Seed via commitSession, which itself creates no backup (that is the
    // fact under test below), so the .sqlite file exists for later steps.
    QVERIFY(manager.commitSession(Timeline(segs, std::nullopt)).ok());

    QStringList afterSeed = appDir.entryList(QStringList() << "*.backup", QDir::Files);
    int countAfterSeed = afterSeed.size();

    // commitSession should NOT create new backup files
    QVERIFY(manager.commitSession(Timeline(segs, std::nullopt)).ok());
    QStringList afterCommit = appDir.entryList(QStringList() << "*.backup", QDir::Files);
    QCOMPARE(afterCommit.size(), countAfterSeed); // no new backup

    // Wait >1 s so the timestamp in the backup filename differs from the seed backup.
    // createBackup uses second-level timestamps; QFile::copy silently fails if the
    // destination already exists.
    QTest::qWait(1100);

    // replaceAll SHOULD create a new backup file
    QVERIFY(manager.replaceAll(Timeline(segs, std::nullopt),
                                Timeline({}, std::nullopt)).ok());
    QStringList afterReplace = appDir.entryList(QStringList() << "*.backup", QDir::Files);
    QVERIFY(afterReplace.size() > countAfterSeed); // new backup created

    (void)before; // silence unused variable warning
}

// ============================================================================
// flushToDisc durable heartbeat tests (Issue 2)
// ============================================================================

void DatabaseTest::test_flushToDisc_writes_heartbeat_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Ensure the DB is created before flushing
    QVERIFY(manager.ensureOpen_dbg());

    manager.flushToDisc();

    // Open DB directly and check for the heartbeat row
    const QString connName = "flush_heartbeat_check";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery query(db);
        query.prepare("SELECT value FROM app_settings WHERE key = 'last_flush_utc'");
        QVERIFY(query.exec());
        QVERIFY(query.next());
        const QString value = query.value(0).toString();
        QVERIFY(!value.isEmpty());
        QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
        QVERIFY(parsed.isValid());
        QCOMPARE(parsed.timeSpec(), Qt::UTC);
        QVERIFY(parsed.secsTo(QDateTime::currentDateTimeUtc()) < 5);
        QVERIFY(!query.next()); // exactly one row
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void DatabaseTest::test_flushToDisc_idempotent()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    manager.flushToDisc();

    // Capture timestamp after first flush
    QString firstTs;
    const QString connName = "flush_idempotent_check";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare("SELECT value FROM app_settings WHERE key = 'last_flush_utc'");
        QVERIFY(q.exec());
        QVERIFY(q.next());
        firstTs = q.value(0).toString();
        QVERIFY(!q.next()); // exactly one row
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    manager.flushToDisc(); // second call must not fail

    const QString connName2 = "flush_idempotent_check2";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName2);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        q.prepare("SELECT value FROM app_settings WHERE key = 'last_flush_utc'");
        QVERIFY(q.exec());
        QVERIFY(q.next());
        const QString secondTs = q.value(0).toString();
        QVERIFY(!q.next()); // still exactly one row
        QDateTime parsed = QDateTime::fromString(secondTs, Qt::ISODateWithMs);
        QVERIFY(parsed.isValid());
        QCOMPARE(parsed.timeSpec(), Qt::UTC);
        // Second flush timestamp must be >= first flush timestamp
        QVERIFY(secondTs >= firstTs);
        db.close();
    }
    QSqlDatabase::removeDatabase(connName2);
}

/**
 * flushToDisc temporarily promotes PRAGMA synchronous to FULL to guarantee the
 * durable write, then must restore it to NORMAL afterward -- otherwise every
 * write for the rest of the session pays the FULL fsync cost.
 */
void DatabaseTest::test_flushToDisc_restores_synchronous_normal()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    QVERIFY(manager.flushToDisc().ok());

    QSqlQuery q(manager.rawDb_dbg());
    QVERIFY(q.exec("PRAGMA synchronous"));
    QVERIFY(q.next());
    // 1 = NORMAL, 2 = FULL -- must be back to NORMAL after the flush.
    QCOMPARE(q.value(0).toInt(), 1);
}

// D2/LOW-H: flushToDisc()'s 'last_flush_utc' timestamp must be UTC-
// normalized regardless of the injected now-provider's own offset --
// performRetentionCleanup() already does this via toUtcIso(); flushToDisc()
// must too, not store whatever timeSpec the provider itself returns.
void DatabaseTest::test_D2_flushToDisc_normalizes_injected_offset_time_to_utc()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);
    QVERIFY(manager.ensureOpen_dbg());

    // A fixed +05:00 offset, independent of the test host's real timezone,
    // so a regression to storing the raw (non-UTC) reading is guaranteed to
    // differ from the correct UTC value by exactly 5 hours.
    const QDateTime offsetNow(QDate(2025, 6, 15), QTime(12, 0, 0), Qt::OffsetFromUTC, 5 * 3600);
    manager.setNowProvider_dbg([offsetNow]() { return offsetNow; });

    QVERIFY(manager.flushToDisc().ok());

    QSqlQuery q(manager.rawDb_dbg());
    QVERIFY(q.exec("SELECT value FROM app_settings WHERE key = 'last_flush_utc'"));
    QVERIFY(q.next());
    const QDateTime parsed = QDateTime::fromString(q.value(0).toString(), Qt::ISODateWithMs);
    QVERIFY(parsed.isValid());
    QCOMPARE(parsed.timeSpec(), Qt::UTC);
    QCOMPARE(parsed, offsetNow.toUTC());
}

/**
 * Atomic overlap-guarded finalisation.
 *
 * Helper: seed an orphan (is_finalized = 0) row directly via SQL with the given
 * UTC start/end and return its rowid.  Mirrors the columns that
 * loadUnfinalizedCheckpoints() reads.
 */
static qint64 seedOrphanRow(SqliteSessionStore& manager,
                            const QDateTime& startUtc,
                            const QDateTime& endUtc,
                            DurationType type = DurationType::Activity)
{
    QSqlQuery q(manager.rawDb_dbg());
    q.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)"
    );
    q.bindValue(":segment_id", SegmentId::mint().toString());
    q.bindValue(":type", static_cast<int>(type));
    q.bindValue(":start_utc", startUtc.toString(Qt::ISODateWithMs));
    q.bindValue(":end_utc", endUtc.toString(Qt::ISODateWithMs));
    if (!q.exec()) {
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

void DatabaseTest::test_finalizeIfNoOverlap_succeeds_when_no_overlap()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    // Existing finalised row from 10:00 to 11:00.
    const QDateTime existingStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime existingEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 0, 0), Qt::UTC);
    std::deque<TimeDuration> seeded;
    seeded.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, existingStartUtc.toLocalTime(), existingEndUtc.toLocalTime()));
    manager.closeConnection_dbg();
    QVERIFY(manager.commitSession(Timeline(seeded, std::nullopt)).ok());

    // Orphan from 12:00 to 12:30 — no overlap with the finalised row above.
    const QDateTime orphanStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime orphanEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);

    QVERIFY(manager.ensureOpen_dbg());
    const qint64 orphanRowId = seedOrphanRow(manager, orphanStartUtc, orphanEndUtc);
    QVERIFY(orphanRowId > 0);
    manager.closeConnection_dbg();

    const auto result = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(result.outcome, SqliteSessionStore::FinalizeOutcome_dbg::Finalized);
    QVERIFY(result.errorMessage.isEmpty());

    // Verify the row is now finalised.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verify(manager.rawDb_dbg());
    verify.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    verify.bindValue(":id", orphanRowId);
    QVERIFY(verify.exec());
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 1);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_finalizeIfNoOverlap_rejects_when_overlapping_finalized_row_exists()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Existing finalised row from 10:00 to 11:00.
    const QDateTime existingStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime existingEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 0, 0), Qt::UTC);
    std::deque<TimeDuration> seeded;
    seeded.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, existingStartUtc.toLocalTime(), existingEndUtc.toLocalTime()));
    QVERIFY(manager.commitSession(Timeline(seeded, std::nullopt)).ok());

    // Orphan from 10:30 to 11:30 — overlaps the finalised row.
    const QDateTime orphanStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 30, 0), Qt::UTC);
    const QDateTime orphanEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 30, 0), Qt::UTC);

    QVERIFY(manager.ensureOpen_dbg());
    const qint64 orphanRowId = seedOrphanRow(manager, orphanStartUtc, orphanEndUtc);
    QVERIFY(orphanRowId > 0);
    manager.closeConnection_dbg();

    // The overlap probe must reject the finalise attempt with a typed
    // OverlapRejected outcome -- a policy decision, not an error.
    const auto result = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(result.outcome, SqliteSessionStore::FinalizeOutcome_dbg::OverlapRejected);
    QVERIFY(result.errorMessage.isEmpty());
}

void DatabaseTest::test_finalizeIfNoOverlap_leaves_row_unchanged_on_overlap()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Existing finalised row from 10:00 to 11:00.
    const QDateTime existingStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime existingEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 0, 0), Qt::UTC);
    std::deque<TimeDuration> seeded;
    seeded.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, existingStartUtc.toLocalTime(), existingEndUtc.toLocalTime()));
    QVERIFY(manager.commitSession(Timeline(seeded, std::nullopt)).ok());

    // Orphan whose interval straddles the existing row.
    const QDateTime orphanStartUtc = QDateTime(QDate(2025, 1, 1), QTime(9, 30, 0), Qt::UTC);
    const QDateTime orphanEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(10, 30, 0), Qt::UTC);

    QVERIFY(manager.ensureOpen_dbg());
    const qint64 orphanRowId = seedOrphanRow(manager, orphanStartUtc, orphanEndUtc);
    QVERIFY(orphanRowId > 0);
    manager.closeConnection_dbg();

    const auto result = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(result.outcome, SqliteSessionStore::FinalizeOutcome_dbg::OverlapRejected);

    // After rejection, the row must still be is_finalized = 0 (transaction atomicity).
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verify(manager.rawDb_dbg());
    verify.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    verify.bindValue(":id", orphanRowId);
    QVERIFY(verify.exec());
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 0);
    manager.closeConnection_dbg();
}

/**
 * B1 (R07-002): a row that no longer matches "id = :id AND is_finalized = 0"
 * (for example it vanished between orphan load and reconcile) is a distinct
 * Missing outcome -- a policy no-op, not an overlap and not a database error.
 */
void DatabaseTest::test_finalizeIfNoOverlap_reports_missing_when_row_absent()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    const QDateTime orphanStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime orphanEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    const qint64 orphanRowId = seedOrphanRow(manager, orphanStartUtc, orphanEndUtc);
    QVERIFY(orphanRowId > 0);

    // The row disappears before finalize runs (e.g. already discarded).
    QSqlQuery del(manager.rawDb_dbg());
    del.prepare("DELETE FROM durations WHERE id = :id");
    del.bindValue(":id", orphanRowId);
    QVERIFY(del.exec());

    const auto result = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(result.outcome, SqliteSessionStore::FinalizeOutcome_dbg::Missing);
    QVERIFY(result.errorMessage.isEmpty());

    // Connection remains usable after the rollback: a fresh orphan finalizes normally.
    const qint64 secondRowId = seedOrphanRow(manager, orphanStartUtc, orphanEndUtc);
    QVERIFY(secondRowId > 0);
    const auto retryResult = manager.finalizeIfNoOverlap_dbg(secondRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(retryResult.outcome, SqliteSessionStore::FinalizeOutcome_dbg::Finalized);
    manager.closeConnection_dbg();
}

/**
 * B1 (R07-002): a BEGIN failure must map to Error, not to a false value that
 * could also mean overlap. The failure is real SQLite behaviour: SQLite does
 * not support nested transactions, so starting finalizeIfNoOverlap() while
 * the connection is already inside a transaction makes its own db.transaction()
 * fail with "cannot start a transaction within a transaction".
 */
void DatabaseTest::test_finalizeIfNoOverlap_returns_error_on_begin_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    const QDateTime orphanStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime orphanEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    const qint64 orphanRowId = seedOrphanRow(manager, orphanStartUtc, orphanEndUtc);
    QVERIFY(orphanRowId > 0);

    // Force a real BEGIN failure by leaving the connection already inside an
    // outer transaction before finalizeIfNoOverlap() tries to start its own.
    QVERIFY(manager.rawDb_dbg().transaction());

    const auto result = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(result.outcome, SqliteSessionStore::FinalizeOutcome_dbg::Error);
    QVERIFY(!result.errorMessage.isEmpty());

    // Clean up the outer transaction the test started.
    QVERIFY(manager.rawDb_dbg().rollback());

    // The row is untouched and the connection remains usable afterward.
    QSqlQuery verify(manager.rawDb_dbg());
    verify.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    verify.bindValue(":id", orphanRowId);
    QVERIFY(verify.exec());
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 0);

    const auto retryResult = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(retryResult.outcome, SqliteSessionStore::FinalizeOutcome_dbg::Finalized);
    manager.closeConnection_dbg();
}

/**
 * B1 (R07-002): an overlap-probe SELECT failure must also map to Error, not
 * be conflated with a genuine overlap rejection. Renaming the durations table
 * away makes the probe's real SELECT fail with "no such table".
 */
void DatabaseTest::test_finalizeIfNoOverlap_returns_error_on_probe_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    const QDateTime orphanStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime orphanEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    const qint64 orphanRowId = seedOrphanRow(manager, orphanStartUtc, orphanEndUtc);
    QVERIFY(orphanRowId > 0);

    QSqlQuery rename(manager.rawDb_dbg());
    QVERIFY(rename.exec("ALTER TABLE durations RENAME TO durations_b1_probe_test"));

    const auto result = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(result.outcome, SqliteSessionStore::FinalizeOutcome_dbg::Error);
    QVERIFY(!result.errorMessage.isEmpty());

    QVERIFY(rename.exec("ALTER TABLE durations_b1_probe_test RENAME TO durations"));

    QSqlQuery verify(manager.rawDb_dbg());
    verify.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    verify.bindValue(":id", orphanRowId);
    QVERIFY(verify.exec());
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 0);

    const auto retryResult = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(retryResult.outcome, SqliteSessionStore::FinalizeOutcome_dbg::Finalized);
    manager.closeConnection_dbg();
}

/**
 * B1 (R07-002): an UPDATE failure must map to Error. A BEFORE UPDATE trigger
 * that raises a real SQLite error blocks only the UPDATE statement, leaving
 * the earlier overlap-probe SELECT unaffected -- proving the failure is
 * attributed to the correct stage.
 */
void DatabaseTest::test_finalizeIfNoOverlap_returns_error_on_update_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    const QDateTime orphanStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime orphanEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    const qint64 orphanRowId = seedOrphanRow(manager, orphanStartUtc, orphanEndUtc);
    QVERIFY(orphanRowId > 0);

    QSqlQuery trig(manager.rawDb_dbg());
    QVERIFY(trig.exec(
        "CREATE TRIGGER block_finalize_update_b1_test BEFORE UPDATE ON durations "
        "BEGIN SELECT RAISE(ABORT, 'B1 test: blocked update'); END"
    ));

    const auto result = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(result.outcome, SqliteSessionStore::FinalizeOutcome_dbg::Error);
    QVERIFY(result.errorMessage.contains("blocked update"));

    QVERIFY(trig.exec("DROP TRIGGER block_finalize_update_b1_test"));

    QSqlQuery verify(manager.rawDb_dbg());
    verify.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    verify.bindValue(":id", orphanRowId);
    QVERIFY(verify.exec());
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 0);

    const auto retryResult = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(retryResult.outcome, SqliteSessionStore::FinalizeOutcome_dbg::Finalized);
    manager.closeConnection_dbg();
}

/**
 * B1 (R07-002): a COMMIT failure must map to Error and roll back, not be
 * conflated with a successful drop. Reproduces the finding's own two-connection
 * diagnostic: a second connection holds a SHARED read lock so the store's
 * UPDATE (RESERVED) still succeeds but its COMMIT (needs EXCLUSIVE) is
 * refused with "database is locked".
 */
void DatabaseTest::test_finalizeIfNoOverlap_returns_error_on_commit_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    const QDateTime orphanStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime orphanEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    const qint64 orphanRowId = seedOrphanRow(manager, orphanStartUtc, orphanEndUtc);
    QVERIFY(orphanRowId > 0);

    const QString connName = "finalize_commit_lock_b1_test";
    {
        QSqlDatabase lockConn = QSqlDatabase::addDatabase("QSQLITE", connName);
        lockConn.setDatabaseName(db_path_);
        QVERIFY(lockConn.open());
        QVERIFY(lockConn.transaction());
        QSqlQuery holdRead(lockConn);
        QVERIFY(holdRead.exec("SELECT COUNT(*) FROM durations"));
        QVERIFY(holdRead.next());

        const auto result = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
        QCOMPARE(result.outcome, SqliteSessionStore::FinalizeOutcome_dbg::Error);
        QVERIFY(!result.errorMessage.isEmpty());

        lockConn.rollback();
        lockConn.close();
    }
    QSqlDatabase::removeDatabase(connName);

    QSqlQuery verify(manager.rawDb_dbg());
    verify.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    verify.bindValue(":id", orphanRowId);
    QVERIFY(verify.exec());
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 0);

    const auto retryResult = manager.finalizeIfNoOverlap_dbg(orphanRowId, orphanStartUtc, orphanEndUtc);
    QCOMPARE(retryResult.outcome, SqliteSessionStore::FinalizeOutcome_dbg::Finalized);
    manager.closeConnection_dbg();
}

/**
 * B1 (R07-002) at the reconcile layer: a real SQL error on one orphan must
 * flip ReconcileResult::ok to false and must NOT be added to `dropped`
 * (which would silently misreport a database failure as a policy drop).
 * A second, genuinely overlapping orphan in the same batch still lands in
 * `dropped`, proving Error and OverlapRejected remain distinguishable.
 */
/**
 * B2: reconcileUnfinalizedCheckpoints() now assumes an already-open outer
 * transaction and stops immediately (ok=false) on the FIRST failure, instead
 * of continuing to mutate later rows -- a real SQL failure on one orphan must
 * roll back the ENTIRE disposition, so a later orphan in the same batch (B,
 * which would otherwise be a clean OverlapRejected deletion) must NOT be
 * touched either. The reconcileUnfinalizedCheckpoints_dbg() wrapper mirrors
 * production by wrapping this call in its own transaction and rolling back
 * on ok=false, so both rows survive completely untouched.
 */
void DatabaseTest::test_reconcileUnfinalizedCheckpoints_sql_error_is_not_reported_as_dropped()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Existing finalised row 10:00 - 11:00 so one orphan genuinely overlaps.
    const QDateTime existingStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime existingEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 0, 0), Qt::UTC);
    std::deque<TimeDuration> seeded;
    seeded.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, existingStartUtc.toLocalTime(), existingEndUtc.toLocalTime()));
    QVERIFY(manager.commitSession(Timeline(seeded, std::nullopt)).ok());

    QVERIFY(manager.ensureOpen_dbg());
    // Orphan A: 12:00 - 12:30, no overlap -- would finalize, but UPDATE is blocked.
    // Orphan B: 10:30 - 11:30, overlaps existing -- would be OverlapRejected,
    // but A's failure must stop reconciliation before B is even reached.
    const QDateTime aStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime aEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    const QDateTime bStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 30, 0), Qt::UTC);
    const QDateTime bEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 30, 0), Qt::UTC);
    const qint64 aId = seedOrphanRow(manager, aStartUtc, aEndUtc);
    const qint64 bId = seedOrphanRow(manager, bStartUtc, bEndUtc);
    QVERIFY(aId > 0 && bId > 0);

    QSqlQuery trig(manager.rawDb_dbg());
    QVERIFY(trig.exec(
        "CREATE TRIGGER block_reconcile_update_b1_test BEFORE UPDATE ON durations "
        "BEGIN SELECT RAISE(ABORT, 'B1 test: blocked update'); END"
    ));

    std::vector<SqliteSessionStore::OrphanCheckpoint_dbg> toFinalize;
    SqliteSessionStore::OrphanCheckpoint_dbg a; a.id = aId; a.startTime = aStartUtc.toLocalTime(); a.endTime = aEndUtc.toLocalTime();
    SqliteSessionStore::OrphanCheckpoint_dbg b; b.id = bId; b.startTime = bStartUtc.toLocalTime(); b.endTime = bEndUtc.toLocalTime();
    toFinalize.push_back(a);
    toFinalize.push_back(b);

    SqliteSessionStore::ReconcileResult_dbg result = manager.reconcileUnfinalizedCheckpoints_dbg(toFinalize, {});

    QVERIFY(!result.ok);
    QVERIFY(result.finalized.empty());
    // A's UPDATE error must not be misreported as a dropped/policy row, and
    // the whole disposition rolled back before B's overlap deletion applied.
    QVERIFY(result.policy_dropped.empty());

    QVERIFY(trig.exec("DROP TRIGGER block_reconcile_update_b1_test"));

    // Raw state: both A and B untouched -- the outer transaction rolled back
    // in full, so B was never even reached, let alone deleted.
    QSqlQuery verify(manager.rawDb_dbg());
    verify.prepare("SELECT id, is_finalized FROM durations WHERE id IN (:a, :b) ORDER BY id");
    verify.bindValue(":a", aId);
    verify.bindValue(":b", bId);
    QVERIFY(verify.exec());
    int seen = 0;
    while (verify.next()) {
        QCOMPARE(verify.value(1).toInt(), 0);
        seen++;
    }
    QCOMPARE(seen, 2);
    manager.closeConnection_dbg();
}

/**
 * B1 (R07-002) regression at the public recovery API: a real SQL error
 * finalizing one orphan must make recoverStartupCheckpoints() report failure
 * and must preserve the clean-shutdown marker -- the exact bug the finding
 * describes (a failed finalize was previously reported as a successful drop
 * with ok==true, consuming the marker).
 */
void DatabaseTest::test_recoverStartupCheckpoints_marker_intact_on_finalize_sql_error()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    // Seed a valid unfinalized orphan (duration > 1 s, not stale) with no
    // overlap, so it is eligible for finalization.
    const QDateTime startUtc = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime endUtc   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);
    const qint64 orphanId = seedOrphanRow(manager, startUtc, endUtc);
    QVERIFY(orphanId > 0);

    QSqlQuery q(manager.rawDb_dbg());
    QVERIFY(q.exec(
        "INSERT OR REPLACE INTO app_settings (key, value) "
        "VALUES ('last_clean_shutdown', '2025-06-01T10:05:00.000Z')"
    ));

    // Block the finalizing UPDATE with a real trigger-raised SQLite error.
    QVERIFY(q.exec(
        "CREATE TRIGGER block_recovery_update_b1_test BEFORE UPDATE ON durations "
        "BEGIN SELECT RAISE(ABORT, 'B1 test: blocked update'); END"
    ));

    const QDateTime now = QDateTime(QDate(2025, 6, 1), QTime(10, 10, 0), Qt::UTC);
    StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    // Assert: recovery reports failure, not a false success with a bogus drop.
    QVERIFY(!res.ok());
    QCOMPARE(res.status, SessionStoreResult::TransientError);

    QVERIFY(q.exec("DROP TRIGGER block_recovery_update_b1_test"));

    // Assert: marker is still present (not consumed by the failed recovery).
    QSqlQuery checkMarker(manager.rawDb_dbg());
    QVERIFY(checkMarker.exec(
        "SELECT COUNT(*) FROM app_settings WHERE key = 'last_clean_shutdown'"
    ));
    QVERIFY(checkMarker.next());
    QCOMPARE(checkMarker.value(0).toInt(), 1);

    // Assert: the orphan row is still unfinalized (rolled back, not resurrected).
    QSqlQuery checkOrphan(manager.rawDb_dbg());
    checkOrphan.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    checkOrphan.bindValue(":id", orphanId);
    QVERIFY(checkOrphan.exec());
    QVERIFY(checkOrphan.next());
    QCOMPARE(checkOrphan.value(0).toInt(), 0);

    manager.closeConnection_dbg();
}

void DatabaseTest::test_reconcileUnfinalizedCheckpoints_reports_finalized_and_dropped()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Existing finalised row 10:00 - 11:00 to force one orphan to be rejected.
    const QDateTime existingStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime existingEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 0, 0), Qt::UTC);
    std::deque<TimeDuration> seeded;
    seeded.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, existingStartUtc.toLocalTime(), existingEndUtc.toLocalTime()));
    QVERIFY(manager.commitSession(Timeline(seeded, std::nullopt)).ok());

    // Orphan A: 12:00 - 12:30 — no overlap → should be finalised.
    // Orphan B: 10:30 - 11:30 — overlaps existing → should be reported as dropped.
    QVERIFY(manager.ensureOpen_dbg());
    const QDateTime aStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime aEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    const QDateTime bStartUtc = QDateTime(QDate(2025, 1, 1), QTime(10, 30, 0), Qt::UTC);
    const QDateTime bEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(11, 30, 0), Qt::UTC);
    const qint64 aId = seedOrphanRow(manager, aStartUtc, aEndUtc);
    const qint64 bId = seedOrphanRow(manager, bStartUtc, bEndUtc);
    QVERIFY(aId > 0 && bId > 0);
    manager.closeConnection_dbg();

    std::vector<SqliteSessionStore::OrphanCheckpoint_dbg> toFinalize;
    SqliteSessionStore::OrphanCheckpoint_dbg a; a.id = aId; a.startTime = aStartUtc.toLocalTime(); a.endTime = aEndUtc.toLocalTime();
    SqliteSessionStore::OrphanCheckpoint_dbg b; b.id = bId; b.startTime = bStartUtc.toLocalTime(); b.endTime = bEndUtc.toLocalTime();
    toFinalize.push_back(a);
    toFinalize.push_back(b);

    SqliteSessionStore::ReconcileResult_dbg result = manager.reconcileUnfinalizedCheckpoints_dbg(toFinalize, {});
    QVERIFY(result.ok);
    QCOMPARE(result.finalized.size(), (size_t)1);
    QCOMPARE(result.finalized[0], aId);
    // B2 (R08-002): an overlap-rejected orphan is now physically deleted
    // (not left is_finalized=0 to recur every launch), and its deletion IS a
    // policy-drop count, unlike before.
    QCOMPARE(result.policy_dropped.size(), (size_t)1);
    QCOMPARE(result.policy_dropped[0], bId);

    // Verify DB state: A is finalised, B is gone entirely.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verifyA(manager.rawDb_dbg());
    verifyA.prepare("SELECT is_finalized FROM durations WHERE id = :a");
    verifyA.bindValue(":a", aId);
    QVERIFY(verifyA.exec());
    QVERIFY(verifyA.next());
    QCOMPARE(verifyA.value(0).toInt(), 1);

    QSqlQuery verifyB(manager.rawDb_dbg());
    verifyB.prepare("SELECT id FROM durations WHERE id = :b");
    verifyB.bindValue(":b", bId);
    QVERIFY(verifyB.exec());
    QVERIFY(!verifyB.next());
    manager.closeConnection_dbg();
}

void DatabaseTest::test_reconcileUnfinalizedCheckpoints_outright_drops_are_deleted()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Two orphans, no overlapping finalised rows in the DB.
    QVERIFY(manager.ensureOpen_dbg());
    const QDateTime aStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime aEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    const QDateTime bStartUtc = QDateTime(QDate(2025, 1, 1), QTime(13, 0, 0), Qt::UTC);
    const QDateTime bEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(13, 30, 0), Qt::UTC);
    const qint64 aId = seedOrphanRow(manager, aStartUtc, aEndUtc);
    const qint64 bId = seedOrphanRow(manager, bStartUtc, bEndUtc);
    QVERIFY(aId > 0 && bId > 0);
    manager.closeConnection_dbg();

    // Reconcile with B in the outright-drop list and A in toFinalize.
    std::vector<SqliteSessionStore::OrphanCheckpoint_dbg> toFinalize;
    SqliteSessionStore::OrphanCheckpoint_dbg a; a.id = aId; a.startTime = aStartUtc.toLocalTime(); a.endTime = aEndUtc.toLocalTime();
    toFinalize.push_back(a);

    SqliteSessionStore::ReconcileResult_dbg result = manager.reconcileUnfinalizedCheckpoints_dbg(toFinalize, {bId});
    QVERIFY(result.ok);
    QCOMPARE(result.finalized.size(), (size_t)1);
    QCOMPARE(result.finalized[0], aId);
    // B2: policy_dropped now reports every row deleted for a policy reason,
    // including outright (caller-supplied) drops like B here.
    QCOMPARE(result.policy_dropped.size(), (size_t)1);
    QCOMPARE(result.policy_dropped[0], bId);

    // Verify DB state: A finalised, B deleted entirely.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verify(manager.rawDb_dbg());
    verify.prepare("SELECT id FROM durations WHERE id = :b");
    verify.bindValue(":b", bId);
    QVERIFY(verify.exec());
    QVERIFY(!verify.next()); // B is gone

    QSqlQuery verifyA(manager.rawDb_dbg());
    verifyA.prepare("SELECT is_finalized FROM durations WHERE id = :a");
    verifyA.bindValue(":a", aId);
    QVERIFY(verifyA.exec());
    QVERIFY(verifyA.next());
    QCOMPARE(verifyA.value(0).toInt(), 1);
    manager.closeConnection_dbg();
}

// ============================================================================
// SchemaStatus: validation of existing DB shape
// ============================================================================

void DatabaseTest::test_schema_status_ready_on_existing_valid_db()
{
    // A DB that was created and used should report Ready on subsequent calls.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    {
        // Create the DB file via construction.
        SqliteSessionStore seed(settings);
        QVERIFY(seed.ensureOpen_dbg());
    }

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);
}

void DatabaseTest::test_schema_status_outdated_legacy_columns()
{
    // A DB with old start_date/end_date columns (pre-UTC schema) must return Outdated.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    // Build a legacy DB directly with old column names.
    const QString connName = "legacy_schema_test";
    {
        QSqlDatabase legacyDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        legacyDb.setDatabaseName(db_path_);
        QVERIFY(legacyDb.open());
        QSqlQuery q(legacyDb);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL,"
            "type INTEGER NOT NULL,"
            "start_date TEXT NOT NULL,"   // old column name
            "end_date TEXT NOT NULL,"     // old column name
            "is_finalized INTEGER NOT NULL DEFAULT 0"
            ")"
        ));
        legacyDb.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Outdated);
}

void DatabaseTest::test_schema_status_outdated_missing_column()
{
    // A DB missing the is_finalized column must return Outdated.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString connName = "missing_col_test";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL UNIQUE,"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL"
            // is_finalized intentionally omitted
            ")"
        ));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Outdated);
}

void DatabaseTest::test_schema_status_outdated_missing_unique_constraint()
{
    // A DB with correct columns but without UNIQUE(segment_id) must return Outdated.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString connName = "no_unique_test";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        // All columns correct, but no UNIQUE on segment_id.
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL,"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0"
            ")"
        ));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Outdated);
}

void DatabaseTest::test_schema_status_ready_recreates_missing_idx_start_utc()
{
    // If idx_start_utc is missing, checkSchemaOnStartup recreates it and returns Ready.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    // Create a valid DB, then drop the partial index.
    {
        SqliteSessionStore seed(settings);
        QVERIFY(seed.ensureOpen_dbg());
        QVERIFY(seed.ensureOpen_dbg());
        QSqlQuery q(seed.rawDb_dbg());
        QVERIFY(q.exec("DROP INDEX IF EXISTS idx_start_utc"));
        seed.closeConnection_dbg();
    }

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);

    // The index should now exist again.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verify(manager.rawDb_dbg());
    QVERIFY(verify.exec(
        "SELECT name FROM sqlite_master WHERE type='index' AND name='idx_start_utc'"
    ));
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toString(), QString("idx_start_utc"));
    manager.closeConnection_dbg();
}

void DatabaseTest::test_schema_status_ready_creates_missing_app_settings_and_idx_segment_id()
{
    // A valid existing DB that is missing app_settings and idx_segment_id must
    // have them created on the Ready path (checkSchemaOnStartup ~:376-392).
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    // Seed: create only the durations table with the full valid schema
    // (columns + UNIQUE(segment_id) constraint), but no app_settings and no idx_segment_id.
    const QString seedConn = "ready_additive_ddl_seed";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", seedConn);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL CHECK(length(segment_id) > 0),"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(segment_id)"
            ")"
        ));
        db.close();
    }
    QSqlDatabase::removeDatabase(seedConn);

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);

    // Verify app_settings and idx_segment_id were created.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verify(manager.rawDb_dbg());
    QVERIFY(verify.exec(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='app_settings'"
    ));
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 1);

    QVERIFY(verify.exec(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_segment_id'"
    ));
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 1);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_schema_status_inaccessible_readonly_file()
{
    // A DB file that exists but cannot be opened (read-only) returns Inaccessible.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    // Create and close the DB first.
    {
        SqliteSessionStore seed(settings);
        QVERIFY(seed.ensureOpen_dbg());
    }

    // Make the file read-only.
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser));

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Inaccessible);

    // Restore permissions.
    QVERIFY(dbFile.setPermissions(
        QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser));
}

// ============================================================================
// TimeTracker::initializeFromStore isolation: no side effects before recovery is called
// ============================================================================

void DatabaseTest::test_constructor_only_does_not_consume_marker()
{
    // The TimeTracker constructor must not perform startup recovery.
    // Only initializeFromStore() does that (which calls recoverStartupCheckpoints).
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    FakeSessionStore fakeDb;

    // Construct TimeTracker — must NOT call recoverStartupCheckpoints.
    TimeTracker tracker(settings, fakeDb);
    QVERIFY(!fakeDb.callLog.contains("recoverStartupCheckpoints"));

    // After initializeFromStore(), recovery should have run.
    tracker.initializeFromStore();
    QVERIFY(fakeDb.callLog.contains("recoverStartupCheckpoints"));
}

void DatabaseTest::test_outdated_schema_does_not_mutate_db()
{
    // When checkSchemaOnStartup() returns Outdated, the caller (main.cpp) must
    // not call initializeFromStore(). This test verifies the contract from the
    // store side: an Outdated DB with a pending orphan and a clean-shutdown
    // marker — neither should be touched.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    // Seed a DB with legacy columns (no start_utc/end_utc → Outdated).
    const QString connName = "outdated_isolation_test";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL,"
            "type INTEGER NOT NULL,"
            "start_date TEXT NOT NULL,"
            "end_date TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0"
            ")"
        ));
        // Insert an unfinalized row.
        QVERIFY(q.exec(
            "INSERT INTO durations (segment_id, type, start_date, end_date, is_finalized) "
            "VALUES ('seg-1', 0, '2025-01-01', '2025-01-01', 0)"
        ));
        QVERIFY(q.exec(
            "CREATE TABLE app_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)"
        ));
        QVERIFY(q.exec(
            "INSERT INTO app_settings (key, value) VALUES ('last_clean_shutdown', '2025-01-01T00:00:00.000Z')"
        ));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);
    SchemaStatus status = manager.checkSchemaOnStartup();
    QCOMPARE(status, SchemaStatus::Outdated);

    // Simulate correct main.cpp behavior: do NOT call initializeFromStore() on Outdated.
    // Verify the unfinalized row and marker are still present.
    const QString connName2 = "outdated_verify";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName2);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1); // unfinalized row untouched

        QVERIFY(q.exec("SELECT COUNT(*) FROM app_settings WHERE key = 'last_clean_shutdown'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1); // marker untouched
        db.close();
    }
    QSqlDatabase::removeDatabase(connName2);
}

void DatabaseTest::test_saveCheckpoint_does_not_demote_finalized_row()
{
    // Seed a finalized row, then call saveCheckpoint with its segment_id.
    // The row must remain finalized and saveCheckpoint must return CallerBug.

    resetDatabaseFile();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    SqliteSessionStore manager(settings);

    // Seed a finalized row directly via SQL.
    const SegmentId finalizedId = SegmentId::mint();
    const QDateTime start = QDateTime::fromMSecsSinceEpoch(10'000'000, Qt::UTC);
    const QDateTime end   = start.addSecs(60);
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery insertQuery(manager.rawDb_dbg());
    insertQuery.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, 0, :start_utc, :end_utc, 1)"
    );
    insertQuery.bindValue(":segment_id", finalizedId.toString());
    insertQuery.bindValue(":start_utc", start.toUTC().toString(Qt::ISODateWithMs));
    insertQuery.bindValue(":end_utc",   end.toUTC().toString(Qt::ISODateWithMs));
    QVERIFY(insertQuery.exec());
    manager.closeConnection_dbg();

    // Attempt to save a checkpoint using the same segment_id.
    const QDateTime newEnd = end.addSecs(30);
    SessionStoreResult result = manager.saveCheckpoint(DurationType::Activity, start, newEnd, finalizedId);

    // Must be CallerBug.
    QCOMPARE(result.category, SessionStoreResult::CallerBug);

    // The row must still be finalized and its end_utc must be unchanged.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verifyQuery(manager.rawDb_dbg());
    verifyQuery.prepare(
        "SELECT is_finalized, end_utc FROM durations WHERE segment_id = :segment_id"
    );
    verifyQuery.bindValue(":segment_id", finalizedId.toString());
    QVERIFY(verifyQuery.exec());
    QVERIFY(verifyQuery.next());
    QCOMPARE(verifyQuery.value(0).toInt(), 1);
    QCOMPARE(verifyQuery.value(1).toString(), end.toUTC().toString(Qt::ISODateWithMs));
    manager.closeConnection_dbg();
}

// ============================================================================
// Item C: validate schema before any additive DDL
// ============================================================================

/**
 * An incompatible existing DB (legacy columns, no app_settings, no indexes)
 * must cause checkSchemaOnStartup() to return Outdated without creating
 * app_settings, idx_segment_id, or idx_start_utc.
 */
void DatabaseTest::test_outdated_schema_no_additive_ddl_on_incompatible_db()
{
    // Arrange: seed a legacy durations table (wrong columns → Outdated).
    // Do NOT create app_settings or any indexes.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString seedConn = "item_c_seed";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", seedConn);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        // Legacy schema: wrong column names, no UNIQUE, no app_settings, no indexes.
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL,"
            "type INTEGER NOT NULL,"
            "start_date TEXT NOT NULL,"
            "end_date TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0"
            ")"
        ));
        db.close();
    }
    QSqlDatabase::removeDatabase(seedConn);

    // Act: construct the store and call checkSchemaOnStartup().
    SqliteSessionStore manager(settings);
    SchemaStatus status = manager.checkSchemaOnStartup();

    // Assert: schema is Outdated.
    QCOMPARE(status, SchemaStatus::Outdated);

    // Assert: no additive objects were created by the constructor or
    // checkSchemaOnStartup() before the Outdated verdict was returned.
    const QString verifyConn = "item_c_verify";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", verifyConn);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);

        // app_settings must NOT exist.
        QVERIFY(q.exec(
            "SELECT COUNT(*) FROM sqlite_master "
            "WHERE type='table' AND name='app_settings'"
        ));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);

        // idx_segment_id must NOT exist.
        QVERIFY(q.exec(
            "SELECT COUNT(*) FROM sqlite_master "
            "WHERE type='index' AND name='idx_segment_id'"
        ));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);

        // idx_start_utc must NOT exist.
        QVERIFY(q.exec(
            "SELECT COUNT(*) FROM sqlite_master "
            "WHERE type='index' AND name='idx_start_utc'"
        ));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);

        db.close();
    }
    QSqlDatabase::removeDatabase(verifyConn);
}

// ============================================================================
// Item D: marker must survive a reconciliation failure
// ============================================================================

/**
 * When recoverStartupCheckpoints() returns ok==false due to a reconciliation
 * failure, the last_clean_shutdown marker must still be present in app_settings
 * (no rows were mutated).
 *
 * Failure injection: setForceReconcileFailure_dbg(true) makes
 * reconcileUnfinalizedCheckpoints() return ok=false immediately, simulating a
 * DB error during the reconcile step — before any rows are touched.
 */
void DatabaseTest::test_recoverStartupCheckpoints_marker_intact_on_reconcile_failure()
{
    // Arrange: fresh DB with one unfinalized orphan and a clean-shutdown marker.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    // Seed a valid unfinalized orphan (duration > 1 s, not stale).
    const QDateTime startUtc = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime endUtc   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);
    const qint64 orphanId = seedOrphanRow(manager, startUtc, endUtc);
    QVERIFY(orphanId > 0);

    // Seed the clean-shutdown marker directly.
    QSqlQuery q(manager.rawDb_dbg());
    QVERIFY(q.exec(
        "INSERT OR REPLACE INTO app_settings (key, value) "
        "VALUES ('last_clean_shutdown', '2025-06-01T10:05:00.000Z')"
    ));

    manager.closeConnection_dbg();

    // Act: force reconciliation to fail, then recover.
    manager.setForceReconcileFailure_dbg(true);
    const QDateTime now = QDateTime(QDate(2025, 6, 1), QTime(10, 10, 0), Qt::UTC);
    StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    // Assert: recovery reports failure.
    QVERIFY(!res.ok());

    // Assert: marker is still present (no rows were mutated).
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery checkMarker(manager.rawDb_dbg());
    QVERIFY(checkMarker.exec(
        "SELECT COUNT(*) FROM app_settings WHERE key = 'last_clean_shutdown'"
    ));
    QVERIFY(checkMarker.next());
    QCOMPARE(checkMarker.value(0).toInt(), 1);

    // Also verify the orphan row is still unfinalized.
    QSqlQuery checkOrphan(manager.rawDb_dbg());
    checkOrphan.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    checkOrphan.bindValue(":id", orphanId);
    QVERIFY(checkOrphan.exec());
    QVERIFY(checkOrphan.next());
    QCOMPARE(checkOrphan.value(0).toInt(), 0);

    manager.closeConnection_dbg();
}

/**
 * When loadUnfinalizedCheckpoints() fails (durations table dropped), the
 * clean-shutdown marker must be preserved and recoverStartupCheckpoints()
 * must return ok==false.
 */
void DatabaseTest::test_recoverStartupCheckpoints_marker_intact_on_load_failure()
{
    // Arrange: fresh DB with a clean-shutdown marker; then drop the durations table
    // so the SELECT in loadUnfinalizedCheckpoints() fails.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    // Seed the clean-shutdown marker.
    QSqlQuery q(manager.rawDb_dbg());
    QVERIFY(q.exec(
        "INSERT OR REPLACE INTO app_settings (key, value) "
        "VALUES ('last_clean_shutdown', '2025-06-01T10:05:00.000Z')"
    ));

    // Drop the durations table so loadUnfinalizedCheckpoints() SELECT fails.
    QVERIFY(q.exec("DROP TABLE durations"));

    manager.closeConnection_dbg();

    // Act: recover should fail because loadUnfinalizedCheckpoints returns nullopt.
    const QDateTime now = QDateTime(QDate(2025, 6, 1), QTime(10, 10, 0), Qt::UTC);
    StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    // Assert: recovery reports failure.
    QVERIFY(!res.ok());

    // Assert: marker is still present (not consumed).
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery checkMarker(manager.rawDb_dbg());
    QVERIFY(checkMarker.exec(
        "SELECT COUNT(*) FROM app_settings WHERE key = 'last_clean_shutdown'"
    ));
    QVERIFY(checkMarker.next());
    QCOMPARE(checkMarker.value(0).toInt(), 1);

    manager.closeConnection_dbg();
}

/**
 * A11: the crash-recovery notification must compare the clean-shutdown marker
 * against the NEWEST finalized orphan's end, not the oldest.  Two orphans where
 * the marker covers only the older one must still notify the user (the newer
 * orphan represents genuinely unsaved work past the last clean shutdown).
 */
void DatabaseTest::test_recoverStartupCheckpoints_notifies_when_marker_covers_only_older_orphan()
{
    // Arrange: fresh DB with two valid unfinalized orphans and a marker that
    // covers only the older orphan's end time.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    // Older orphan ends at 10:30; newer orphan ends at 11:30.
    const QDateTime oldStartUtc = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime oldEndUtc   = QDateTime(QDate(2025, 6, 1), QTime(10, 30, 0), Qt::UTC);
    const QDateTime newStartUtc = QDateTime(QDate(2025, 6, 1), QTime(11, 0, 0), Qt::UTC);
    const QDateTime newEndUtc   = QDateTime(QDate(2025, 6, 1), QTime(11, 30, 0), Qt::UTC);
    const qint64 oldId = seedOrphanRow(manager, oldStartUtc, oldEndUtc);
    const qint64 newId = seedOrphanRow(manager, newStartUtc, newEndUtc);
    QVERIFY(oldId > 0 && newId > 0);

    // Marker covers only the older orphan's end (10:30), not the newer (11:30).
    QSqlQuery q(manager.rawDb_dbg());
    QVERIFY(q.exec(
        "INSERT OR REPLACE INTO app_settings (key, value) "
        "VALUES ('last_clean_shutdown', '2025-06-01T10:30:00.000Z')"
    ));
    manager.closeConnection_dbg();

    // Act: recover just after the newer orphan (both are recent, > 1 s → finalize).
    const QDateTime now = QDateTime(QDate(2025, 6, 1), QTime(11, 40, 0), Qt::UTC);
    StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    // Assert: recovery succeeded, both orphans finalized, and the user is notified
    // because the newest finalized orphan ends after the marker.
    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 2);
    QVERIFY2(res.notify_user,
             "must notify: newest finalized orphan ends after the clean-shutdown marker");
}

/**
 * A14: a backward clock jump between the checkpoint write and the restart
 * makes `endTime.msecsTo(now)` negative, defeating the >24h stale check and
 * letting a future-dated orphan pass through to be finalized. Seed an orphan
 * whose endTime is well past `now` and confirm recoverStartupCheckpoints()
 * drops it instead of resurrecting it.
 */
void DatabaseTest::test_recoverStartupCheckpoints_drops_future_dated_orphan()
{
    // Arrange: fresh DB with one orphan whose endTime is 2 hours after `now`
    // (simulating a clock that jumped backward after the checkpoint was written).
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    const QDateTime now         = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime futureStart = now.addSecs(60 * 60);       // now + 1h
    const QDateTime futureEnd   = now.addSecs(2 * 60 * 60);   // now + 2h
    const qint64 orphanId = seedOrphanRow(manager, futureStart, futureEnd);
    QVERIFY(orphanId > 0);
    manager.closeConnection_dbg();

    // Act: recover at `now`, which is before the orphan's endTime.
    StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    // Assert: recovery succeeded, but the future-dated orphan was dropped, not finalized.
    // Outright drops are deleted from the table entirely (see
    // reconcileUnfinalizedCheckpoints), so no row survives with is_finalized == 0.
    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 0);
    QCOMPARE(res.policy_dropped_count, 1);
    QCOMPARE(res.discard_dropped_count, 0);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery checkOrphan(manager.rawDb_dbg());
    checkOrphan.prepare("SELECT COUNT(*) FROM durations WHERE id = :id");
    checkOrphan.bindValue(":id", orphanId);
    QVERIFY(checkOrphan.exec());
    QVERIFY(checkOrphan.next());
    QCOMPARE(checkOrphan.value(0).toInt(), 0);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_recoverStartupCheckpoints_drops_unfinalized_cross_day_orphan()
{
    // MUST-FIX4 (review packet C3): B2's malformed-row definition is exactly
    // {empty id, invalid type, invalid timestamp, start>=end} -- a cross-day
    // row is well-formed by that definition, so it would otherwise pass
    // straight through loadUnfinalizedCheckpoints() and get finalized by
    // reconcileUnfinalizedCheckpoints() with no day-validity check at all.
    // Seed an otherwise-recoverable (not too-short/stale/future-dated)
    // unfinalized row that genuinely spans local midnight and assert
    // recovery drops it -- never finalizes it into history -- exactly like
    // the existing too-short/stale/future-dated policy drops above.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDateTime localStart(yesterday, QTime(23, 50, 0), Qt::LocalTime);
    const QDateTime localEnd(QDate::currentDate(), QTime(0, 10, 0), Qt::LocalTime);
    const qint64 orphanId = seedOrphanRow(manager, localStart.toUTC(), localEnd.toUTC());
    QVERIFY(orphanId > 0);
    manager.closeConnection_dbg();

    // Recover shortly after the orphan's end -- well within the stale/future
    // thresholds, so cross-day is the ONLY reason this row is dropped.
    const QDateTime now(QDate::currentDate(), QTime(0, 20, 0), Qt::LocalTime);
    StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 0);
    QCOMPARE(res.policy_dropped_count, 1);
    QCOMPARE(res.discard_dropped_count, 0);
    QCOMPARE(res.recovered_msec, static_cast<qint64>(0));

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery checkOrphan(manager.rawDb_dbg());
    checkOrphan.prepare("SELECT COUNT(*) FROM durations WHERE id = :id");
    checkOrphan.bindValue(":id", orphanId);
    QVERIFY(checkOrphan.exec());
    QVERIFY(checkOrphan.next());
    QCOMPARE(checkOrphan.value(0).toInt(), 0);
    manager.closeConnection_dbg();
}

// ============================================================================
// B2 (R08-001..003,005, R05-004): atomic recovery disposition, exact
// millisecond aggregation, malformed-row cleanup, and idempotency.
// ============================================================================

/**
 * B2 (R08-003): malformed unfinalized rows (invalid timestamp, invalid type,
 * empty id, non-positive interval) are explicit delete work, not a silent
 * omission -- they are deleted transactionally as part of the SAME recovery
 * disposition as a genuine valid orphan, but their count is logged only and
 * never appears in the public finalized/policy_dropped counts (only a real
 * recovery POLICY decision does).
 */
void DatabaseTest::test_recoverStartupCheckpoints_deletes_malformed_rows_without_public_count()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    // A genuinely valid orphan that should finalize normally.
    const QDateTime validStart = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime validEnd   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);
    const qint64 validId = seedOrphanRow(manager, validStart, validEnd);
    QVERIFY(validId > 0);

    QSqlQuery pragmaOn(manager.rawDb_dbg());
    QVERIFY(pragmaOn.exec("PRAGMA ignore_check_constraints = ON"));

    // Malformed: empty segment_id.
    QSqlQuery emptyId(manager.rawDb_dbg());
    emptyId.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                     "VALUES ('', 0, :s, :e, 0)");
    emptyId.bindValue(":s", QDateTime(QDate(2025, 6, 1), QTime(11, 0, 0), Qt::UTC).toString(Qt::ISODateWithMs));
    emptyId.bindValue(":e", QDateTime(QDate(2025, 6, 1), QTime(11, 5, 0), Qt::UTC).toString(Qt::ISODateWithMs));
    QVERIFY(emptyId.exec());
    const qint64 emptyIdRow = emptyId.lastInsertId().toLongLong();

    // Malformed: invalid type value.
    QSqlQuery badType(manager.rawDb_dbg());
    badType.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                     "VALUES (:seg, 99, :s, :e, 0)");
    badType.bindValue(":seg", SegmentId::mint().toString());
    badType.bindValue(":s", QDateTime(QDate(2025, 6, 1), QTime(12, 0, 0), Qt::UTC).toString(Qt::ISODateWithMs));
    badType.bindValue(":e", QDateTime(QDate(2025, 6, 1), QTime(12, 5, 0), Qt::UTC).toString(Qt::ISODateWithMs));
    QVERIFY(badType.exec());
    const qint64 badTypeRow = badType.lastInsertId().toLongLong();

    // Malformed: invalid (unparseable) timestamp.
    QSqlQuery badTs(manager.rawDb_dbg());
    badTs.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, 'not-a-date', 'also-not-a-date', 0)");
    badTs.bindValue(":seg", SegmentId::mint().toString());
    QVERIFY(badTs.exec());
    const qint64 badTsRow = badTs.lastInsertId().toLongLong();

    // Malformed: non-positive interval (start >= end).
    QSqlQuery badInterval(manager.rawDb_dbg());
    badInterval.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                         "VALUES (:seg, 0, :s, :e, 0)");
    badInterval.bindValue(":seg", SegmentId::mint().toString());
    const QString sameTs = QDateTime(QDate(2025, 6, 1), QTime(13, 0, 0), Qt::UTC).toString(Qt::ISODateWithMs);
    badInterval.bindValue(":s", sameTs);
    badInterval.bindValue(":e", sameTs);
    QVERIFY(badInterval.exec());
    const qint64 badIntervalRow = badInterval.lastInsertId().toLongLong();

    manager.closeConnection_dbg();

    const QDateTime now = QDateTime(QDate(2025, 6, 1), QTime(14, 0, 0), Qt::UTC);
    const StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    // Assert: recovery committed; the valid orphan finalized normally, and
    // NONE of the four malformed rows appear in either public count -- they
    // are store-internal cleanup, not a recovery policy decision.
    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 1);
    QCOMPARE(res.policy_dropped_count, 0);
    QCOMPARE(res.recovered_msec, validStart.msecsTo(validEnd));

    // Assert: all four malformed rows are physically gone (deleted
    // transactionally, logged only -- no UI notification is possible since
    // no public field even carries this count).
    QVERIFY(manager.ensureOpen_dbg());
    for (qint64 id : {emptyIdRow, badTypeRow, badTsRow, badIntervalRow}) {
        QSqlQuery check(manager.rawDb_dbg());
        check.prepare("SELECT COUNT(*) FROM durations WHERE id = :id");
        check.bindValue(":id", id);
        QVERIFY(check.exec());
        QVERIFY(check.next());
        QCOMPARE(check.value(0).toInt(), 0);
    }
    // The valid orphan is finalized, not deleted.
    QSqlQuery checkValid(manager.rawDb_dbg());
    checkValid.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    checkValid.bindValue(":id", validId);
    QVERIFY(checkValid.exec());
    QVERIFY(checkValid.next());
    QCOMPARE(checkValid.value(0).toInt(), 1);
    manager.closeConnection_dbg();
}

/**
 * B2: several sub-second/fractional-second orphans prove recovered_msec is
 * accumulated EXACTLY from the underlying millisecond endpoints, with no
 * per-row truncation or final rounding anywhere in the path (R08-005). Naive
 * per-row integer-second truncation would report 1+1+2 = 4000 ms here;
 * the true sum is 1400+1700+2900 = 6000 ms.
 */
void DatabaseTest::test_recoverStartupCheckpoints_aggregates_exact_milliseconds_no_truncation()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    const QDateTime base = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const qint64 aId = seedOrphanRow(manager, base, base.addMSecs(1400));
    const qint64 bId = seedOrphanRow(manager, base.addSecs(10), base.addSecs(10).addMSecs(1700));
    const qint64 cId = seedOrphanRow(manager, base.addSecs(20), base.addSecs(20).addMSecs(2900));
    QVERIFY(aId > 0 && bId > 0 && cId > 0);
    manager.closeConnection_dbg();

    const QDateTime now = base.addSecs(60);
    const StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 3);
    QCOMPARE(res.recovered_msec, static_cast<qint64>(1400 + 1700 + 2900));
}

/**
 * B2 (B1 follow-up): the Missing outcome (a row that vanished between load
 * and finalize) is impossible in the single-process recoverStartupCheckpoints
 * flow -- db_mutex_ has been held continuously since the load -- so it is
 * escalated to an internal error rather than a silent policy no-op, and (per
 * B2's single-transaction invariant) the whole batch is rolled back rather
 * than proceeding to the next candidate.
 */
void DatabaseTest::test_reconcileUnfinalizedCheckpoints_missing_row_is_internal_error()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    const QDateTime aStartUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime aEndUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    const qint64 aId = seedOrphanRow(manager, aStartUtc, aEndUtc);
    QVERIFY(aId > 0);

    // The row vanishes before reconcile runs (e.g. deleted by something else
    // in-process between load and reconcile -- cannot legitimately happen via
    // the real recoverStartupCheckpoints() call path, but is directly
    // reachable here as a white-box regression for the escalation itself).
    QSqlQuery del(manager.rawDb_dbg());
    del.prepare("DELETE FROM durations WHERE id = :id");
    del.bindValue(":id", aId);
    QVERIFY(del.exec());

    std::vector<SqliteSessionStore::OrphanCheckpoint_dbg> toFinalize;
    SqliteSessionStore::OrphanCheckpoint_dbg a; a.id = aId; a.startTime = aStartUtc.toLocalTime(); a.endTime = aEndUtc.toLocalTime();
    toFinalize.push_back(a);

    const SqliteSessionStore::ReconcileResult_dbg result = manager.reconcileUnfinalizedCheckpoints_dbg(toFinalize, {});

    QVERIFY(!result.ok);
    QVERIFY(result.finalized.empty());
    QVERIFY(result.policy_dropped.empty());
    manager.closeConnection_dbg();
}

/**
 * B2 (R08-001): a marker-consumption failure (the DELETE statement itself
 * fails) inside the recovery transaction must roll back the WHOLE
 * disposition, leaving the marker present -- not just "checked and ignored"
 * as before.
 */
void DatabaseTest::test_recoverStartupCheckpoints_marker_delete_failure_preserves_marker()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery q(manager.rawDb_dbg());
    QVERIFY(q.exec(
        "INSERT OR REPLACE INTO app_settings (key, value) "
        "VALUES ('last_clean_shutdown', '2025-06-01T10:05:00.000Z')"
    ));

    // Block the marker DELETE with a real trigger-raised SQLite error.
    QVERIFY(q.exec(
        "CREATE TRIGGER block_marker_delete_b2_test BEFORE DELETE ON app_settings "
        "WHEN old.key = 'last_clean_shutdown' "
        "BEGIN SELECT RAISE(ABORT, 'B2 test: blocked marker delete'); END"
    ));
    manager.closeConnection_dbg();

    const QDateTime now = QDateTime(QDate(2025, 6, 1), QTime(10, 10, 0), Qt::UTC);
    const StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    QVERIFY(!res.ok());
    QCOMPARE(res.status, SessionStoreResult::TransientError);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery drop(manager.rawDb_dbg());
    QVERIFY(drop.exec("DROP TRIGGER block_marker_delete_b2_test"));

    QSqlQuery checkMarker(manager.rawDb_dbg());
    QVERIFY(checkMarker.exec("SELECT COUNT(*) FROM app_settings WHERE key = 'last_clean_shutdown'"));
    QVERIFY(checkMarker.next());
    QCOMPARE(checkMarker.value(0).toInt(), 1);
    manager.closeConnection_dbg();
}

/**
 * B2: a discard-intent resolution failure inside the recovery transaction
 * must also roll back the whole disposition -- an unmatched tombstone (named
 * no live row) is left exactly as it was, not silently dropped.
 */
void DatabaseTest::test_recoverStartupCheckpoints_discard_intent_clear_failure_preserves_everything()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Created);

    // An unmatched discard intent: names no unfinalized row, so it is
    // "resolved" (deleted) unless the delete itself fails.
    const SegmentId segId = SegmentId::mint();
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery insertIntent(manager.rawDb_dbg());
    insertIntent.prepare("INSERT INTO discard_intents (segment_id) VALUES (:id)");
    insertIntent.bindValue(":id", segId.toString());
    QVERIFY(insertIntent.exec());

    QSqlQuery q(manager.rawDb_dbg());
    QVERIFY(q.exec(
        "CREATE TRIGGER block_intent_delete_b2_test BEFORE DELETE ON discard_intents "
        "BEGIN SELECT RAISE(ABORT, 'B2 test: blocked intent delete'); END"
    ));
    manager.closeConnection_dbg();

    const QDateTime now = QDateTime(QDate(2025, 6, 1), QTime(11, 0, 0), Qt::UTC);
    const StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    QVERIFY(!res.ok());

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery drop(manager.rawDb_dbg());
    QVERIFY(drop.exec("DROP TRIGGER block_intent_delete_b2_test"));

    QSqlQuery checkIntent(manager.rawDb_dbg());
    checkIntent.prepare("SELECT COUNT(*) FROM discard_intents WHERE segment_id = :id");
    checkIntent.bindValue(":id", segId.toString());
    QVERIFY(checkIntent.exec());
    QVERIFY(checkIntent.next());
    QCOMPARE(checkIntent.value(0).toInt(), 1);
    manager.closeConnection_dbg();
}

/**
 * B2: a second startup over already-resolved rows must be a true no-op --
 * zero NEW finalized/policy-dropped counts and zero new recovered_msec.
 * Idempotency is what makes retrying startup after a Retry/Quit choice safe.
 */
void DatabaseTest::test_recoverStartupCheckpoints_second_startup_is_idempotent()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    const QDateTime startUtc = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime endUtc   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);
    QVERIFY(seedOrphanRow(manager, startUtc, endUtc) > 0);
    manager.closeConnection_dbg();

    const QDateTime now = QDateTime(QDate(2025, 6, 1), QTime(10, 10, 0), Qt::UTC);
    const StartupRecoveryResult first = manager.recoverStartupCheckpoints(now);
    QVERIFY(first.ok());
    QCOMPARE(first.finalized_count, 1);
    QCOMPARE(first.recovered_msec, startUtc.msecsTo(endUtc));

    const StartupRecoveryResult second = manager.recoverStartupCheckpoints(now);
    QVERIFY(second.ok());
    QCOMPARE(second.finalized_count, 0);
    QCOMPARE(second.policy_dropped_count, 0);
    QCOMPARE(second.recovered_msec, static_cast<qint64>(0));
    QVERIFY(!second.notify_user);
}

/**
 * A12: an empty segment_id row must be treated as a corrupt row at load —
 * skipped (skipped++) rather than passed to SegmentId::fromString(), which
 * asserts non-empty in debug builds.  The same guard protects orphan loading.
 */
void DatabaseTest::test_loadDurations_skips_empty_segment_id_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30000));
    SqliteSessionStore manager(settings);

    // A valid finalized row so we can confirm valid rows still load.
    const QDateTime start = QDateTime::fromMSecsSinceEpoch(4'000'000, Qt::UTC);
    const QDateTime end   = start.addSecs(10);
    std::deque<TimeDuration> durations;
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));
    QVERIFY(manager.commitSession(Timeline(durations, std::nullopt)).ok());

    // Seed an empty segment_id FINALIZED row directly.  The
    // CHECK(length(segment_id) > 0) constraint models a healthy DB, so bypass it
    // (as an old/hand-edited DB would) to exercise the corrupt-row guard at load.
    // Only one empty-id row can exist at a time (UNIQUE(segment_id)).
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery pragmaOn(manager.rawDb_dbg());
    QVERIFY(pragmaOn.exec("PRAGMA ignore_check_constraints = ON"));

    QSqlQuery finalizedEmpty(manager.rawDb_dbg());
    finalizedEmpty.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES ('', :type, :start_utc, :end_utc, 1)"
    );
    finalizedEmpty.bindValue(":type", static_cast<int>(DurationType::Activity));
    finalizedEmpty.bindValue(":start_utc", start.addSecs(100).toString(Qt::ISODateWithMs));
    finalizedEmpty.bindValue(":end_utc", start.addSecs(110).toString(Qt::ISODateWithMs));
    QVERIFY(finalizedEmpty.exec());
    manager.closeConnection_dbg();

    // loadDurations() must skip the empty-id finalized row (no assert abort).
    auto loaded = manager.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1); // only the valid row survives

    // Replace the empty-id row with an UNFINALIZED one to exercise the same guard
    // in loadUnfinalizedCheckpoints() (UNIQUE forbids a second empty-id row).
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery del(manager.rawDb_dbg());
    QVERIFY(del.exec("DELETE FROM durations WHERE segment_id = ''"));
    QSqlQuery pragmaOn2(manager.rawDb_dbg());
    QVERIFY(pragmaOn2.exec("PRAGMA ignore_check_constraints = ON"));
    QSqlQuery unfinalizedEmpty(manager.rawDb_dbg());
    unfinalizedEmpty.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES ('', :type, :start_utc, :end_utc, 0)"
    );
    unfinalizedEmpty.bindValue(":type", static_cast<int>(DurationType::Activity));
    unfinalizedEmpty.bindValue(":start_utc", start.addSecs(200).toString(Qt::ISODateWithMs));
    unfinalizedEmpty.bindValue(":end_utc", start.addSecs(210).toString(Qt::ISODateWithMs));
    QVERIFY(unfinalizedEmpty.exec());
    manager.closeConnection_dbg();

    // loadUnfinalizedCheckpoints() must skip the empty-id orphan row (no assert).
    auto orphans = manager.loadUnfinalizedCheckpoints_dbg();
    QCOMPARE(orphans.size(), static_cast<size_t>(0));
}

// ============================================================================
// F6: ensureSettingsTable() must validate the shape of a pre-existing
// app_settings table, not just its presence (CREATE ... IF NOT EXISTS no-ops
// on a wrong-shaped table).
// ============================================================================

void DatabaseTest::test_ensureSettingsTable_rebuilds_malformed_table()
{
    // Seed a valid durations table (so checkSchemaOnStartup reaches the
    // additive-DDL path) plus a malformed app_settings table.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString seedConn = "f6_malformed_seed";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", seedConn);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL CHECK(length(segment_id) > 0),"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(segment_id)"
            ")"
        ));
        QVERIFY(q.exec("CREATE TABLE app_settings (wrong TEXT)"));
        db.close();
    }
    QSqlDatabase::removeDatabase(seedConn);

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);

    // The malformed table must have been rebuilt so the marker can be written.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVERIFY(manager.setLastCleanShutdownMarker_dbg(now).ok());

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery colQuery(manager.rawDb_dbg());
    QVERIFY(colQuery.exec("PRAGMA table_info(app_settings)"));
    QSet<QString> foundColumns;
    while (colQuery.next()) {
        foundColumns.insert(colQuery.value(1).toString());
    }
    QCOMPARE(foundColumns, (QSet<QString>{"key", "value"}));
    manager.closeConnection_dbg();
}

void DatabaseTest::test_ensureSettingsTable_rebuilds_constraint_malformed_table()
{
    // Right column names, but no PRIMARY KEY on key / no NOT NULL on value.
    // "CREATE ... IF NOT EXISTS" no-ops, and the old validation only checked
    // column names, so this shape passed through unrebuilt - meaning
    // INSERT OR REPLACE loses its REPLACE semantics (no unique constraint on
    // key) and marker writes accumulate duplicate rows instead of replacing.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString seedConn = "f6_constraint_malformed_seed";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", seedConn);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL CHECK(length(segment_id) > 0),"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(segment_id)"
            ")"
        ));
        QVERIFY(q.exec("CREATE TABLE app_settings (key TEXT, value TEXT)"));
        db.close();
    }
    QSqlDatabase::removeDatabase(seedConn);

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);

    // Write the marker twice with different timestamps. With REPLACE
    // semantics restored, exactly one row should remain for the key.
    const QDateTime first = QDateTime::currentDateTimeUtc().addSecs(-60);
    const QDateTime second = QDateTime::currentDateTimeUtc();
    QVERIFY(manager.setLastCleanShutdownMarker_dbg(first).ok());
    QVERIFY(manager.setLastCleanShutdownMarker_dbg(second).ok());

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery countQuery(manager.rawDb_dbg());
    countQuery.prepare("SELECT COUNT(*) FROM app_settings WHERE key = 'last_clean_shutdown'");
    QVERIFY(countQuery.exec());
    QVERIFY(countQuery.next());
    QCOMPARE(countQuery.value(0).toInt(), 1);

    QSqlQuery colQuery(manager.rawDb_dbg());
    QVERIFY(colQuery.exec("PRAGMA table_info(app_settings)"));
    bool keyIsPk = false;
    bool valueNotNull = false;
    while (colQuery.next()) {
        const QString name = colQuery.value(1).toString();
        if (name == "key") {
            keyIsPk = colQuery.value(5).toInt() == 1;
        } else if (name == "value") {
            valueNotNull = colQuery.value(3).toInt() == 1;
        }
    }
    QVERIFY(keyIsPk);
    QVERIFY(valueNotNull);

    manager.closeConnection_dbg();
}

void DatabaseTest::test_ensureSettingsTable_leaves_healthy_table_untouched()
{
    // A well-formed app_settings table must not be rebuilt, and its data
    // must survive a second checkSchemaOnStartup() call.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    QDateTime now;
    {
        SqliteSessionStore seed(settings);
        QCOMPARE(seed.checkSchemaOnStartup(), SchemaStatus::Created);
        now = QDateTime::currentDateTimeUtc();
        QVERIFY(seed.setLastCleanShutdownMarker_dbg(now).ok());
    }

    // Simulate the next app start: a fresh instance over the same DB file,
    // hitting the existing-DB validation path (not the fresh-DB shortcut).
    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);

    // The marker written before this startup check must still be there.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery q(manager.rawDb_dbg());
    q.prepare("SELECT value FROM app_settings WHERE key = 'last_clean_shutdown'");
    QVERIFY(q.exec());
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), now.toUTC().toString(Qt::ISODateWithMs));
    manager.closeConnection_dbg();
}

// ============================================================================
// A4 (R05-001): durable checkpoint-discard intent.
// ============================================================================

static int countDiscardIntents(SqliteSessionStore& manager, const SegmentId& id)
{
    QSqlQuery q(manager.rawDb_dbg());
    q.prepare("SELECT COUNT(*) FROM discard_intents WHERE segment_id = :id");
    q.bindValue(":id", id.toString());
    if (!q.exec() || !q.next()) return -1;
    return q.value(0).toInt();
}

static int countDurationsForSegment(SqliteSessionStore& manager, const SegmentId& id)
{
    QSqlQuery q(manager.rawDb_dbg());
    q.prepare("SELECT COUNT(*) FROM durations WHERE segment_id = :id");
    q.bindValue(":id", id.toString());
    if (!q.exec() || !q.next()) return -1;
    return q.value(0).toInt();
}

void DatabaseTest::test_A4_discardCheckpoint_success_leaves_no_residual_intent()
{
    // Arrange: an unfinalized checkpoint row.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const SegmentId segId = SegmentId::mint();
    const QDateTime start = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime end   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, end, segId).ok());

    // Act
    const SessionStoreResult result = manager.discardCheckpoint(segId);

    // Assert: success, row gone, no residual tombstone.
    QVERIFY(result.ok());
    QVERIFY(manager.ensureOpen_dbg());
    QCOMPARE(countDurationsForSegment(manager, segId), 0);
    QCOMPARE(countDiscardIntents(manager, segId), 0);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_A4_discardCheckpoint_row_delete_failure_leaves_durable_intent()
{
    // Arrange: an unfinalized checkpoint row, and the row-delete step forced
    // to fail (simulating a transient failure AFTER the tombstone commits).
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const SegmentId segId = SegmentId::mint();
    const QDateTime start = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime end   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, end, segId).ok());

    manager.setForceDiscardRowDeleteFailure_dbg(true);

    // Act
    const SessionStoreResult result = manager.discardCheckpoint(segId);

    // Assert: TransientError, but the intent is durably recorded and the
    // (still unfinalized) row is untouched -- nothing was lost.
    QCOMPARE(result.category, SessionStoreResult::TransientError);
    QVERIFY(manager.ensureOpen_dbg());
    QCOMPARE(countDiscardIntents(manager, segId), 1);
    QCOMPARE(countDurationsForSegment(manager, segId), 1);
    QSqlQuery checkFinalized(manager.rawDb_dbg());
    checkFinalized.prepare("SELECT is_finalized FROM durations WHERE segment_id = :id");
    checkFinalized.bindValue(":id", segId.toString());
    QVERIFY(checkFinalized.exec());
    QVERIFY(checkFinalized.next());
    QCOMPARE(checkFinalized.value(0).toInt(), 0);

    manager.setForceDiscardRowDeleteFailure_dbg(false);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_A4_discardCheckpoint_repeated_application_is_idempotent()
{
    // Arrange: discard once (row and tombstone both gone).
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const SegmentId segId = SegmentId::mint();
    const QDateTime start = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime end   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, end, segId).ok());
    QVERIFY(manager.discardCheckpoint(segId).ok());

    // Act: discard the SAME segment id again -- nothing left to do.
    const SessionStoreResult second = manager.discardCheckpoint(segId);

    // Assert: still a clean success, no error, state unchanged.
    QVERIFY(second.ok());
    QVERIFY(manager.ensureOpen_dbg());
    QCOMPARE(countDurationsForSegment(manager, segId), 0);
    QCOMPARE(countDiscardIntents(manager, segId), 0);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_A4_discardCheckpoint_disabled_history_no_mutation()
{
    // Arrange: history storage disabled -- no DB file is even created.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 0));
    SqliteSessionStore manager(settings);

    // Act
    const SessionStoreResult result = manager.discardCheckpoint(SegmentId::mint());

    // Assert: a deliberate no-op, and no DB file was ever created.
    QCOMPARE(result.category, SessionStoreResult::Disabled);
    QVERIFY(!QFile::exists(db_path_));
}

/**
 * The plan's required real-SQLite close-and-reopen proof: a discard intent
 * that survived a row-delete failure (durable tombstone, row still present)
 * must be honored by the NEXT startup recovery after a genuine close and
 * reopen of the database file -- the row is deleted, never finalized/
 * credited, and the tombstone is resolved.
 */
void DatabaseTest::test_A4_recovery_after_reopen_deletes_row_matching_durable_intent()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const SegmentId segId = SegmentId::mint();
    const QDateTime start = QDateTime(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::UTC);
    const QDateTime end   = QDateTime(QDate(2025, 6, 1), QTime(23, 55, 0), Qt::UTC);

    {
        // Session 1: a checkpoint exists, the engine deliberately rejects it
        // (cross-midnight), but the physical row delete fails transiently --
        // exactly the R05-001 trigger. The process then ends (closes) before
        // a later retry can resolve it.
        SqliteSessionStore seed(settings);
        QCOMPARE(seed.checkSchemaOnStartup(), SchemaStatus::Created);
        QVERIFY(seed.saveCheckpoint(DurationType::Activity, start, end, segId).ok());

        seed.setForceDiscardRowDeleteFailure_dbg(true);
        const SessionStoreResult discardResult = seed.discardCheckpoint(segId);
        QCOMPARE(discardResult.category, SessionStoreResult::TransientError);

        // Sanity: tombstone durable, row still present and unfinalized.
        QVERIFY(seed.ensureOpen_dbg());
        QCOMPARE(countDiscardIntents(seed, segId), 1);
        QCOMPARE(countDurationsForSegment(seed, segId), 1);
        seed.closeConnection_dbg();
        // seed is destructed here -- a real close of the database file.
    }

    // Session 2: a fresh instance over the same file (a real reopen).
    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);

    const QDateTime now = QDateTime(QDate(2025, 6, 2), QTime(0, 5, 0), Qt::UTC);
    const StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    // Assert: recovery succeeded, the rejected row was neither finalized nor
    // credited, and it no longer exists.
    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 0);
    QCOMPARE(res.recovered_msec, qint64(0));
    QVERIFY(!res.notify_user);
    QVERIFY(manager.ensureOpen_dbg());
    QCOMPARE(countDurationsForSegment(manager, segId), 0);
    QCOMPARE(countDiscardIntents(manager, segId), 0);
    manager.closeConnection_dbg();
}

/**
 * M4: a single recovery that policy-drops one intent-matched row and one
 * cross-day row must report discard_dropped_count == 1 (the intent-matched
 * subset only) while policy_dropped_count == 2 (both). Fixed calendar dates
 * are used throughout -- not QDate::currentDate() -- so the fixture cannot
 * flip behaviour depending on when the suite happens to run near midnight.
 */
void DatabaseTest::test_M4_recovery_splits_discard_forced_from_policy_drops()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());

    // Row A: an ordinary same-day orphan shape that will be force-dropped by
    // a durable discard intent keyed on its own segment_id.
    const QDateTime startA = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime endA   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);
    const qint64 orphanIdA = seedOrphanRow(manager, startA, endA);
    QVERIFY(orphanIdA > 0);

    QSqlQuery segQuery(manager.rawDb_dbg());
    segQuery.prepare("SELECT segment_id FROM durations WHERE id = :id");
    segQuery.bindValue(":id", orphanIdA);
    QVERIFY(segQuery.exec());
    QVERIFY(segQuery.next());
    const QString segIdA = segQuery.value(0).toString();

    QSqlQuery insertIntent(manager.rawDb_dbg());
    insertIntent.prepare("INSERT INTO discard_intents (segment_id) VALUES (:id)");
    insertIntent.bindValue(":id", segIdA);
    QVERIFY(insertIntent.exec());

    // Row B: a cross-day orphan (no discard intent) -- policy-dropped for
    // spanning local midnight, not because it was ever discarded.
    const QDate yesterday = QDate(2025, 5, 31);
    const QDate today     = QDate(2025, 6, 1);
    const QDateTime localStartB(yesterday, QTime(23, 50, 0), Qt::LocalTime);
    const QDateTime localEndB(today, QTime(0, 10, 0), Qt::LocalTime);
    const qint64 orphanIdB = seedOrphanRow(manager, localStartB.toUTC(), localEndB.toUTC());
    QVERIFY(orphanIdB > 0);

    manager.closeConnection_dbg();

    // Act: recover shortly after both orphans' end times, well within the
    // stale/future thresholds.
    const QDateTime now(today, QTime(0, 20, 0), Qt::LocalTime);
    const StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    // Assert
    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 0);
    QCOMPARE(res.policy_dropped_count, 2);
    QCOMPARE(res.discard_dropped_count, 1);
    QVERIFY(res.discard_dropped_count <= res.policy_dropped_count);
    QCOMPARE(res.recovered_msec, qint64(0));
    QVERIFY(!res.notify_user);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery checkA(manager.rawDb_dbg());
    checkA.prepare("SELECT COUNT(*) FROM durations WHERE id = :id");
    checkA.bindValue(":id", orphanIdA);
    QVERIFY(checkA.exec());
    QVERIFY(checkA.next());
    QCOMPARE(checkA.value(0).toInt(), 0);

    QSqlQuery checkB(manager.rawDb_dbg());
    checkB.prepare("SELECT COUNT(*) FROM durations WHERE id = :id");
    checkB.bindValue(":id", orphanIdB);
    QVERIFY(checkB.exec());
    QVERIFY(checkB.next());
    QCOMPARE(checkB.value(0).toInt(), 0);

    QSqlQuery checkIntent(manager.rawDb_dbg());
    checkIntent.prepare("SELECT COUNT(*) FROM discard_intents WHERE segment_id = :id");
    checkIntent.bindValue(":id", segIdA);
    QVERIFY(checkIntent.exec());
    QVERIFY(checkIntent.next());
    QCOMPARE(checkIntent.value(0).toInt(), 0);
    manager.closeConnection_dbg();
}

/**
 * A discard intent whose segment_id happens to name a FINALIZED row (a
 * reused/corrupt identity, per the plan's explicit guard) must never delete
 * real history -- only the tombstone itself may be resolved.
 */
void DatabaseTest::test_A4_recovery_never_deletes_finalized_row_matching_intent()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Created);

    // Seed a FINALIZED row under a known segment id.
    // Anchored at local noon (Qt::LocalTime), not a fixed UTC hour: a
    // 9:00-10:00 UTC row straddles local midnight under a large enough
    // offset (e.g. Marquesas, UTC-9:30, puts it at 23:30-00:30 local),
    // which trips Timeline's assertSameDayInvariant() Q_ASSERT when
    // Timeline(finalized, ...) is constructed just below.
    const SegmentId segId = SegmentId::mint();
    const QDateTime start = QDateTime(QDate(2025, 6, 1), QTime(12, 0, 0), Qt::LocalTime);
    const QDateTime end   = QDateTime(QDate(2025, 6, 1), QTime(13, 0, 0), Qt::LocalTime);
    std::deque<TimeDuration> finalized;
    finalized.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start, end, segId));
    QVERIFY(manager.commitSession(Timeline(finalized, std::nullopt)).ok());

    // Manually plant a discard intent naming the SAME segment id, simulating
    // a corrupt/reused identity (the only way this state can arise, since the
    // production discardCheckpoint() path never targets a finalized row).
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery insertIntent(manager.rawDb_dbg());
    insertIntent.prepare("INSERT INTO discard_intents (segment_id) VALUES (:id)");
    insertIntent.bindValue(":id", segId.toString());
    QVERIFY(insertIntent.exec());
    manager.closeConnection_dbg();

    // Act: recovery runs (no unfinalized rows exist at all).
    const QDateTime now = QDateTime(QDate(2025, 6, 1), QTime(14, 0, 0), Qt::LocalTime);
    const StartupRecoveryResult res = manager.recoverStartupCheckpoints(now);

    // Assert: recovery succeeded, the finalized row survives untouched, and
    // the tombstone was resolved (not left as permanently unresolvable work).
    QVERIFY(res.ok());
    QVERIFY(manager.ensureOpen_dbg());
    QCOMPARE(countDurationsForSegment(manager, segId), 1);
    QSqlQuery checkFinalized(manager.rawDb_dbg());
    checkFinalized.prepare("SELECT is_finalized FROM durations WHERE segment_id = :id");
    checkFinalized.bindValue(":id", segId.toString());
    QVERIFY(checkFinalized.exec());
    QVERIFY(checkFinalized.next());
    QCOMPARE(checkFinalized.value(0).toInt(), 1);
    QCOMPARE(countDiscardIntents(manager, segId), 0);
    manager.closeConnection_dbg();
}

// ============================================================================
// C2 (R07-001): a failed COMMIT must roll back so the long-lived connection
// is never left inside that transaction, and later autocommit operations
// (a fresh write, a checkpoint discard) behave normally afterward.
// setForceCommitFailure_dbg() makes the NEXT commit inside the store treat
// COMMIT as having failed and roll back, without needing an environmental
// fault (e.g. a locked file) to trigger a real one.
// ============================================================================

void DatabaseTest::test_C2_replaceAll_rolls_back_on_commit_failure()
{
    // Arrange: two committed rows.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNow();
    std::deque<TimeDuration> original;
    original.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-300), now.addSecs(-200)));
    original.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-200), now.addSecs(-100)));
    QVERIFY(manager.commitSession(Timeline(original, std::nullopt)).ok());

    // Act: inject a COMMIT failure for the replace.
    manager.setForceCommitFailure_dbg(true);
    std::deque<TimeDuration> replacement;
    replacement.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-80), now.addSecs(-40)));
    QVERIFY(!manager.replaceAll(Timeline(replacement, std::nullopt), Timeline({}, std::nullopt)).ok());

    // Assert: raw state is still the ORIGINAL two rows -- the DELETE+INSERT
    // was rolled back, not left with the durations table emptied.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery countAfterFailure(manager.rawDb_dbg());
    QVERIFY(countAfterFailure.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(countAfterFailure.next());
    QCOMPARE(countAfterFailure.value(0).toInt(), 2);

    // Assert: the connection recovers -- a second (non-injected) replace commits normally.
    QVERIFY(manager.replaceAll(Timeline(replacement, std::nullopt), Timeline({}, std::nullopt)).ok());
    QSqlQuery countAfterSecondWrite(manager.rawDb_dbg());
    QVERIFY(countAfterSecondWrite.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(countAfterSecondWrite.next());
    QCOMPARE(countAfterSecondWrite.value(0).toInt(), 1);

    // Assert: checkpoint discard also still works.
    const SegmentId segId = SegmentId::mint();
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, now, now.addSecs(10), segId).ok());
    QVERIFY(manager.discardCheckpoint(segId).ok());

    manager.closeConnection_dbg();
}

void DatabaseTest::test_C2_saveCheckpoint_insert_rolls_back_on_commit_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const SegmentId segId = SegmentId::mint();
    const QDateTime start = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime end   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);

    // Act: inject a COMMIT failure on the first (INSERT branch) checkpoint
    // write for this segment_id.
    manager.setForceCommitFailure_dbg(true);
    QCOMPARE(manager.saveCheckpoint(DurationType::Activity, start, end, segId).category,
             SessionStoreResult::TransientError);

    // Assert: raw state has no row for this segment -- the INSERT was rolled back.
    QVERIFY(manager.ensureOpen_dbg());
    QCOMPARE(countDurationsForSegment(manager, segId), 0);

    // Assert: the connection recovers -- a second (non-injected) checkpoint write commits.
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, end, segId).ok());
    QCOMPARE(countDurationsForSegment(manager, segId), 1);

    // Assert: checkpoint discard also still works.
    QVERIFY(manager.discardCheckpoint(segId).ok());
    QCOMPARE(countDurationsForSegment(manager, segId), 0);

    manager.closeConnection_dbg();
}

void DatabaseTest::test_C2_saveCheckpoint_update_rolls_back_on_commit_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const SegmentId segId = SegmentId::mint();
    const QDateTime start     = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime firstEnd  = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);
    const QDateTime secondEnd = QDateTime(QDate(2025, 6, 1), QTime(10, 10, 0), Qt::UTC);

    // Arrange: an existing unfinalized checkpoint row (INSERT branch, no injected fault).
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, firstEnd, segId).ok());

    // Act: inject a COMMIT failure on the UPDATE branch (same segment_id, later end).
    manager.setForceCommitFailure_dbg(true);
    QCOMPARE(manager.saveCheckpoint(DurationType::Activity, start, secondEnd, segId).category,
             SessionStoreResult::TransientError);

    // Assert: raw state still shows the ORIGINAL end, exactly one row, and
    // the untouched columns (type/start_utc/is_finalized) unchanged -- so a
    // rollback that only partially undoes the UPDATE (or verify.next()
    // matching some other row) cannot mask a change.
    QVERIFY(manager.ensureOpen_dbg());
    QCOMPARE(countDurationsForSegment(manager, segId), 1);
    QSqlQuery verify(manager.rawDb_dbg());
    verify.prepare("SELECT type, start_utc, end_utc, is_finalized FROM durations WHERE segment_id = :id");
    verify.bindValue(":id", segId.toString());
    QVERIFY(verify.exec());
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), static_cast<int>(DurationType::Activity));
    QCOMPARE(verify.value(1).toString(), start.toUTC().toString(Qt::ISODateWithMs));
    QCOMPARE(verify.value(2).toString(), firstEnd.toUTC().toString(Qt::ISODateWithMs));
    QCOMPARE(verify.value(3).toInt(), 0);

    // Assert: the connection recovers -- a second (non-injected) update commits.
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, secondEnd, segId).ok());
    QSqlQuery verify2(manager.rawDb_dbg());
    verify2.prepare("SELECT end_utc FROM durations WHERE segment_id = :id");
    verify2.bindValue(":id", segId.toString());
    QVERIFY(verify2.exec());
    QVERIFY(verify2.next());
    QCOMPARE(verify2.value(0).toString(), secondEnd.toUTC().toString(Qt::ISODateWithMs));

    // Assert: checkpoint discard also still works.
    QVERIFY(manager.discardCheckpoint(segId).ok());
    QCOMPARE(countDurationsForSegment(manager, segId), 0);

    manager.closeConnection_dbg();
}

/**
 * C2 round-2 fix (R07-001): db.commit() can return false WITHOUT ever
 * calling setLastError() -- confirmed empirically for Qt 5.15.18 via a
 * "commit after close" repro (QSqlDatabase::commit() and
 * QSQLiteDriver::commitTransaction() both bail on an unopened driver before
 * touching the error state). The old contract inferred success from
 * db.lastError().text().isEmpty(), so a commit failure with an empty error
 * text would have been misreported as success. setForceCommitCloseBefore_dbg()
 * reproduces that exact mechanism -- closing the real connection immediately
 * before commitOrRollback()'s real db.commit() call -- so this exercises the
 * genuine driver behavior, not a fabricated message.
 */
void DatabaseTest::test_C2_replaceAll_fails_on_commit_failure_with_empty_error_text()
{
    // Arrange: two committed baseline rows.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNow();
    std::deque<TimeDuration> original;
    original.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-300), now.addSecs(-200)));
    original.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-200), now.addSecs(-100)));
    QVERIFY(manager.commitSession(Timeline(original, std::nullopt)).ok());

    // Act: the replace's COMMIT genuinely fails with an EMPTY error text.
    manager.setForceCommitCloseBefore_dbg(true);
    std::deque<TimeDuration> replacement;
    replacement.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-80), now.addSecs(-40)));
    const SessionStoreResult result =
        manager.replaceAll(Timeline(replacement, std::nullopt), Timeline({}, std::nullopt));

    // Assert: a commit failure is a commit failure, even with no message to
    // report -- callers must never read "no error text" as "success".
    QVERIFY(!result.ok());
    QCOMPARE(result.message, QStringLiteral("Failed to commit replace transaction: "));

    // Assert: raw state is still the ORIGINAL two rows -- the replace never
    // durably committed.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery count(manager.rawDb_dbg());
    QVERIFY(count.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(count.next());
    QCOMPARE(count.value(0).toInt(), 2);

    // Assert: the connection recovers for a subsequent write.
    QVERIFY(manager.replaceAll(Timeline(replacement, std::nullopt), Timeline({}, std::nullopt)).ok());
    QSqlQuery count2(manager.rawDb_dbg());
    QVERIFY(count2.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(count2.next());
    QCOMPARE(count2.value(0).toInt(), 1);

    manager.closeConnection_dbg();
}

/**
 * D13e (routed-in from D11 part 1 review): updateDurationsById() is
 * commitSession()'s commit site -- the dominant production write path --
 * but unlike replaceAll()/saveCheckpoint() it called db.commit()/
 * db.rollback() directly instead of going through commitOrRollback(). That
 * meant a failed COMMIT there had no closeConnection() recovery when the
 * rollback also failed (C2/R07-001), and force_commit_failure_dbg_ (which
 * only fires on the NEXT commitOrRollback() call) could never reach this
 * path at all -- so this exact scenario had zero coverage. Same C2 standard
 * as the replaceAll/saveCheckpoint tests above, exercised via
 * commitSession() since that is updateDurationsById()'s only caller.
 */
void DatabaseTest::test_C2_updateDurationsById_rolls_back_on_commit_failure()
{
    // Arrange: one already-committed baseline row.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNow();
    std::deque<TimeDuration> original;
    original.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-300), now.addSecs(-200)));
    QVERIFY(manager.commitSession(Timeline(original, std::nullopt)).ok());

    // Act: inject a COMMIT failure for a second commitSession() call.
    manager.setForceCommitFailure_dbg(true);
    std::deque<TimeDuration> addition;
    addition.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-100), now.addSecs(-50)));
    const SessionStoreResult result = manager.commitSession(Timeline(addition, std::nullopt));

    // Assert: the failure is reported as TransientError, not Success.
    QCOMPARE(result.category, SessionStoreResult::TransientError);

    // Assert: raw state is still just the ORIGINAL row -- the INSERT for
    // the second row was rolled back, not left partially committed.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery count(manager.rawDb_dbg());
    QVERIFY(count.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(count.next());
    QCOMPARE(count.value(0).toInt(), 1);

    // Assert: the connection recovers -- a second (non-injected) commit succeeds.
    QVERIFY(manager.commitSession(Timeline(addition, std::nullopt)).ok());
    QSqlQuery count2(manager.rawDb_dbg());
    QVERIFY(count2.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(count2.next());
    QCOMPARE(count2.value(0).toInt(), 2);

    manager.closeConnection_dbg();
}

/**
 * Rollback-failure diagnostic (R07-001's proposed direction): if the
 * rollback that follows a failed COMMIT ALSO fails, the primary commit
 * error must still be what the caller sees -- not silently overwritten or
 * replaced by the rollback failure. Uses replaceAll(), which returns a
 * SessionStoreResult carrying a message, so the message can be asserted
 * directly.
 *
 * setForceRollbackFailure_dbg() makes the injected rollback fail WITHOUT
 * calling db.rollback() or closing the connection itself -- unlike an
 * earlier version of this seam, it leaves the real transaction genuinely
 * open on a still-open connection. That means the "connection recovers"
 * assertion below is an honest test of commitOrRollback()'s own
 * close-on-failed-rollback cleanup (F3/R07-001), not an artifact of the
 * injection already having closed the connection.
 */
void DatabaseTest::test_C2_replaceAll_preserves_commit_error_when_rollback_also_fails()
{
    // Arrange: two committed baseline rows.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QDateTime now = fixedNow();
    std::deque<TimeDuration> original;
    original.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-300), now.addSecs(-200)));
    original.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-200), now.addSecs(-100)));
    QVERIFY(manager.commitSession(Timeline(original, std::nullopt)).ok());

    // Act: inject a COMMIT failure AND make the follow-up rollback also fail.
    manager.setForceCommitFailure_dbg(true);
    manager.setForceRollbackFailure_dbg(true);
    std::deque<TimeDuration> replacement;
    replacement.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-80), now.addSecs(-40)));
    const SessionStoreResult result =
        manager.replaceAll(Timeline(replacement, std::nullopt), Timeline({}, std::nullopt));

    // Assert: the primary commit error is what the caller sees -- not
    // silently overwritten by the rollback failure.
    QVERIFY(!result.ok());
    QVERIFY(result.message.contains("injected commit failure (test)"));

    // Assert: raw state is still the ORIGINAL two rows.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery count(manager.rawDb_dbg());
    QVERIFY(count.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(count.next());
    QCOMPARE(count.value(0).toInt(), 2);

    // Assert: the connection still recovers for a subsequent write.
    QVERIFY(manager.replaceAll(Timeline(replacement, std::nullopt), Timeline({}, std::nullopt)).ok());
    QSqlQuery count2(manager.rawDb_dbg());
    QVERIFY(count2.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(count2.next());
    QCOMPARE(count2.value(0).toInt(), 1);

    manager.closeConnection_dbg();
}

/**
 * F1 stronger option (R07-001's "Commands and Results" diagnostic):
 * reproduces a GENUINE COMMIT failure -- not the force_commit_failure_dbg_
 * seam -- via a second connection to the same file holding an open read
 * transaction. This app uses SQLite's default rollback-journal mode (see
 * ensureSchema's doc comment), where a writer's COMMIT must upgrade to an
 * EXCLUSIVE lock; that upgrade is blocked while another connection holds a
 * SHARED lock via an open read transaction. Qt's QSQLITE driver DOES install
 * a busy handler by default (QSQLITE_BUSY_TIMEOUT defaults to 5000 ms), so
 * on an unmodified connection COMMIT would busy-wait for up to 5 s before
 * failing with "database is locked" -- measured at ~5019 ms on Qt 5.15.18.
 * The wait is still bounded and the outcome still deterministic: this test
 * is single-threaded, so the reader's SHARED lock can never be released
 * while the writer's busy-wait is running, and the handler always exhausts
 * and returns SQLITE_BUSY rather than eventually succeeding. To avoid
 * paying that ~5 s on every run, the writer connection below is reopened
 * with QSQLITE_BUSY_TIMEOUT=0 via the existing ensureOpen_dbg()/
 * closeConnection_dbg()/rawDb_dbg() test seams -- this is test-only setup on
 * this one connection; it does not touch how production opens its
 * connection. With the timeout at 0, COMMIT fails immediately (measured at
 * 0 ms) for the same "database is locked" reason. SQLite tracks
 * same-process multi-connection locking correctly (its unix VFS works
 * around the POSIX advisory-lock per-process limitation), so two
 * connections opened by this same test process genuinely contend.
 *
 * Done for one representative call site (saveCheckpoint's INSERT branch)
 * exercising the real db.lastError() capture in commitOrRollback(). Not
 * repeated for the other three C2 sites: they would each need their own
 * second live connection for no additional coverage of commitOrRollback()
 * itself (already shared, already exercised here for real), so the cheaper
 * force_commit_failure_dbg_ seam above is kept for those.
 */
void DatabaseTest::test_C2_saveCheckpoint_insert_rolls_back_on_genuine_commit_lock_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);
    // Test-only: reopen with a 0 ms busy timeout so the deterministic
    // "database is locked" outcome below is reached immediately instead of
    // after Qt's real 5 s default busy-wait (see comment above). Only this
    // connection is affected; production's own open path is untouched.
    manager.closeConnection_dbg();
    manager.rawDb_dbg().setConnectOptions("QSQLITE_BUSY_TIMEOUT=0");
    QVERIFY(manager.ensureOpen_dbg());

    const SegmentId segId = SegmentId::mint();
    const QDateTime start = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime end   = QDateTime(QDate(2025, 6, 1), QTime(10, 5, 0), Qt::UTC);

    {
        // A second connection to the SAME file holds an open read
        // transaction, acquiring a SHARED lock that blocks the writer's
        // COMMIT from upgrading to EXCLUSIVE.
        QSqlDatabase reader = QSqlDatabase::addDatabase("QSQLITE", "C2_lock_reader");
        reader.setDatabaseName(db_path_);
        QVERIFY(reader.open());
        QVERIFY(reader.transaction());
        QSqlQuery lockQuery(reader);
        QVERIFY(lockQuery.exec("SELECT COUNT(*) FROM durations"));
        QVERIFY(lockQuery.next());

        // Act: the INSERT branch's COMMIT genuinely fails with "database is locked".
        const SessionStoreResult result = manager.saveCheckpoint(DurationType::Activity, start, end, segId);
        QCOMPARE(result.category, SessionStoreResult::TransientError);
        QVERIFY(result.message.contains("locked", Qt::CaseInsensitive));

        reader.rollback();
        reader.close();
    }
    QSqlDatabase::removeDatabase("C2_lock_reader");

    // Assert: raw state has no row for this segment -- the INSERT was rolled back.
    QVERIFY(manager.ensureOpen_dbg());
    QCOMPARE(countDurationsForSegment(manager, segId), 0);

    // Assert: the connection recovers now that the lock is released.
    QVERIFY(manager.saveCheckpoint(DurationType::Activity, start, end, segId).ok());
    QCOMPARE(countDurationsForSegment(manager, segId), 1);

    manager.closeConnection_dbg();
}

// ============================================================================
// C3 (R13-001/R14-001): the day-validity/overlap payload guard (dayvalidity.h)
// rejects an invalid payload as a CallerBug before any mutation at every
// uTimer write boundary, uniformly -- including a legacy cross-midnight row
// (MUST-FIX1, review packet C3: HistoryEditSession drops any such row from
// the save payload before it ever reaches replaceAll(); see
// historyeditsession.cpp's buildSavePayload() and
// qtest/test_historyeditsession.cpp's test_cross_midnight_dropped_from_payload).
//
// SHOULD-FIX #6 (review packet C3): these cross-day fixtures are built with
// Qt::LocalTime, not Qt::UTC. isCrossMidnight() (types.h) is timezone-aware
// -- it converts to local time via .toLocalTime() before comparing dates;
// tagging 23:50->00:10 as UTC would make it cross the UTC date while
// landing on the SAME local calendar day on this machine (Europe/Berlin,
// UTC+2 in June), which would not exercise the guard for the reason the
// test name claims.
// ============================================================================

void DatabaseTest::test_C3_commitSession_rejects_cross_day_payload()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // A segment spanning local midnight, built via fromPersistedRows() (as a
    // caller bypassing TimeDuration::create()/Timeline's normal same-day
    // debug assertion would) -- commitSession() must catch it as a final
    // guard rather than writing it.
    const SegmentId segId = SegmentId::mint();
    const QDateTime start(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    const QDateTime end(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime);
    std::deque<TimeDuration> crossDay;
    crossDay.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start, end, segId));

    const SessionStoreResult result = manager.commitSession(Timeline::fromPersistedRows(crossDay));

    QCOMPARE(result.category, SessionStoreResult::CallerBug);
    QCOMPARE(static_cast<int>(manager.loadDurations().size()), 0);
}

void DatabaseTest::test_C3_saveCheckpoint_rejects_cross_day_interval()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const SegmentId segId = SegmentId::mint();
    const QDateTime start(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    const QDateTime end(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime);

    const SessionStoreResult result = manager.saveCheckpoint(DurationType::Activity, start, end, segId);

    QCOMPARE(result.category, SessionStoreResult::CallerBug);
    QVERIFY(manager.ensureOpen_dbg());
    QCOMPARE(countDurationsForSegment(manager, segId), 0);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_C3_replaceAll_rejects_cross_type_overlap()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Seed one pre-existing row that must survive untouched if replaceAll()
    // correctly rejects the whole payload before any mutation.
    const QDateTime seedStart = QDateTime(QDate(2025, 6, 1), QTime(8, 0, 0), Qt::UTC);
    const QDateTime seedEnd   = QDateTime(QDate(2025, 6, 1), QTime(8, 30, 0), Qt::UTC);
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, seedStart, seedEnd));
    QVERIFY(manager.commitSession(Timeline(seed, std::nullopt)).ok());

    // Activity 10:00-10:30 overlapping Pause 10:15-10:45 -- same day, no
    // reversed/empty/duplicate id, but an Activity/Pause overlap.
    const QDateTime aStart = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::UTC);
    const QDateTime aEnd   = QDateTime(QDate(2025, 6, 1), QTime(10, 30, 0), Qt::UTC);
    const QDateTime pStart = QDateTime(QDate(2025, 6, 1), QTime(10, 15, 0), Qt::UTC);
    const QDateTime pEnd   = QDateTime(QDate(2025, 6, 1), QTime(10, 45, 0), Qt::UTC);
    std::deque<TimeDuration> overlapping;
    overlapping.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, aStart, aEnd));
    overlapping.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, pStart, pEnd));

    const SessionStoreResult result = manager.replaceAll(Timeline(overlapping, std::nullopt),
                                                          Timeline({}, std::nullopt));

    QCOMPARE(result.category, SessionStoreResult::CallerBug);
    // Nothing mutated: the pre-existing seeded row is still exactly there.
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded[0].startTime, seedStart.toLocalTime());
}

void DatabaseTest::test_C3_replaceAll_rejects_cross_day_payload()
{
    // MUST-FIX1 (user decision, review packet C3): replaceAll() now rejects a
    // cross-day row the same uniform way as commitSession()/saveCheckpoint(),
    // with no allowCrossDay exception. HistoryEditSession is responsible for
    // never sending one this far (it drops a legacy cross-midnight row from
    // the save payload instead -- see buildSavePayload()); this test pins
    // replaceAll()'s own guard as the final backstop for any other caller.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    // Seed one pre-existing row that must survive untouched if replaceAll()
    // correctly rejects the whole payload before any mutation.
    const QDateTime seedStart = QDateTime(QDate(2025, 6, 1), QTime(8, 0, 0), Qt::LocalTime);
    const QDateTime seedEnd   = QDateTime(QDate(2025, 6, 1), QTime(8, 30, 0), Qt::LocalTime);
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, seedStart, seedEnd));
    QVERIFY(manager.commitSession(Timeline(seed, std::nullopt)).ok());

    const QDateTime cmStart(QDate(2025, 6, 1), QTime(23, 50, 0), Qt::LocalTime);
    const QDateTime cmEnd(QDate(2025, 6, 2), QTime(0, 10, 0), Qt::LocalTime);
    std::deque<TimeDuration> history;
    history.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, cmStart, cmEnd));

    const SessionStoreResult result = manager.replaceAll(Timeline::fromPersistedRows(history),
                                                          Timeline({}, std::nullopt));

    QCOMPARE(result.category, SessionStoreResult::CallerBug);
    // Nothing mutated: the pre-existing seeded row is still exactly there.
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded[0].startTime, seedStart.toLocalTime());
}

void DatabaseTest::test_C3_saveCheckpoint_rejects_nonpositive_interval()
{
    // MUST-FIX3 (review packet C3): pin DayValidity::Violation::NonPositive at
    // the saveCheckpoint() write boundary. Mutation testing showed disabling
    // this branch caused zero test failures once
    // test_checkpointPreservesStartTimeOnUpdateBySegmentId's drifted-start
    // offset was reduced from 3600s to 10s (necessarily, since a 3600s drift
    // reversed that test's own interval and this guard now correctly rejects
    // it -- see that test's comment).
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const SegmentId segId = SegmentId::mint();
    const QDateTime start(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::LocalTime);
    const QDateTime end = start; // end <= start: non-positive interval.

    const SessionStoreResult result = manager.saveCheckpoint(DurationType::Activity, start, end, segId);

    QCOMPARE(result.category, SessionStoreResult::CallerBug);
    QVERIFY(manager.ensureOpen_dbg());
    QCOMPARE(countDurationsForSegment(manager, segId), 0);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_C3_commitSession_rejects_duplicate_id()
{
    // MUST-FIX3 (review packet C3): pin DayValidity::Violation::DuplicateId at
    // the commitSession() write boundary -- mutation testing showed disabling
    // this branch caused zero test failures.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const SegmentId sharedId = SegmentId::mint();
    const QDateTime aStart = QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    const QDateTime aEnd   = QDateTime(QDate(2025, 6, 1), QTime(9, 30, 0), Qt::LocalTime);
    const QDateTime bStart = QDateTime(QDate(2025, 6, 1), QTime(10, 0, 0), Qt::LocalTime);
    const QDateTime bEnd   = QDateTime(QDate(2025, 6, 1), QTime(10, 30, 0), Qt::LocalTime);
    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, aStart, aEnd, sharedId));
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, bStart, bEnd, sharedId));

    const SessionStoreResult result = manager.commitSession(Timeline::fromPersistedRows(rows));

    QCOMPARE(result.category, SessionStoreResult::CallerBug);
    QCOMPARE(static_cast<int>(manager.loadDurations().size()), 0);
}

void DatabaseTest::test_C3_commitSession_rejects_invalid_timestamp()
{
    // MUST-FIX3 (review packet C3): pin DayValidity::Violation::InvalidTimestamp
    // at the commitSession() write boundary -- cheaply reachable by passing a
    // default-constructed (invalid) QDateTime. Both start and end are
    // invalid here, so QDateTime::msecsTo() between them is 0 and the
    // NonPositive check ("<= 0") would ALSO reject this row if
    // InvalidTimestamp were disabled -- assert the specific reported reason
    // (not just CallerBug) so this test actually pins the InvalidTimestamp
    // branch rather than being silently covered by NonPositive.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const SegmentId segId = SegmentId::mint();
    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, QDateTime(), QDateTime(), segId));

    const SessionStoreResult result = manager.commitSession(Timeline::fromPersistedRows(rows));

    QCOMPARE(result.category, SessionStoreResult::CallerBug);
    QVERIFY2(result.message.contains("invalid timestamp"), qPrintable(result.message));
    QCOMPARE(static_cast<int>(manager.loadDurations().size()), 0);
}

// D2/MEDIUM-7: retention cutoff is computed from an injectable now-provider
// seam (SqliteSessionStore::now_provider_), not a bare QDateTime::
// currentDateTimeUtc() read -- proves the exact boundary deterministically,
// independent of the real day the suite happens to run.
void DatabaseTest::test_D2_retentionCutoff_uses_injected_clock_exact_boundary()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7)); // keep 7 days

    SqliteSessionStore store(settings);
    QVERIFY(store.ensureOpen_dbg());
    const QDateTime fixedNow(QDate(2025, 6, 15), QTime(12, 0, 0), Qt::UTC);
    store.setNowProvider_dbg([fixedNow]() { return fixedNow; });

    // performRetentionCleanup() prunes on end_utc < threshold, so the
    // boundary is drawn on each row's END time.
    const QDateTime threshold = fixedNow.addDays(-7);
    const QDateTime keptEnd = threshold.addSecs(1);     // newer than cutoff -> kept
    const QDateTime prunedEnd = threshold.addSecs(-1);  // older than cutoff -> pruned

    auto seed = [&](const QDateTime& end) {
        QSqlQuery q(store.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:seg, 0, :s, :e, 1)");
        q.bindValue(":seg", SegmentId::mint().toString());
        q.bindValue(":s", end.addSecs(-60).toString(Qt::ISODateWithMs));
        q.bindValue(":e", end.toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    };
    seed(keptEnd);
    seed(prunedEnd);

    // Act: checkSchemaOnStartup() runs retention cleanup against the
    // injected clock, not real time.
    const SchemaStatus status = store.checkSchemaOnStartup();
    QVERIFY(status == SchemaStatus::Ready || status == SchemaStatus::Created);

    // Assert: exactly the outside-cutoff row was pruned.
    auto loaded = store.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);
    QCOMPARE(loaded[0].endTime.toUTC(), keptEnd);
}

// ============================================================================
// D8 (R06-001..004): schema semantic validation, write-capability probing,
// atomic app_settings rebuild, and centralized reconnect PRAGMA policy.
// ============================================================================

void DatabaseTest::test_D8_schema_outdated_same_name_missing_autoincrement()
{
    // Same column NAMES as canonical, but "id" lacks AUTOINCREMENT: row ids
    // can be reused after DELETE, breaking the stable-per-row identity that
    // recovery's UPDATE/DELETE WHERE id=:id relies on.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString connName = "d8_no_autoincrement";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY,"
            "segment_id TEXT NOT NULL CHECK(length(segment_id) > 0),"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(segment_id)"
            ")"
        ));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Outdated);
}

void DatabaseTest::test_D8_schema_outdated_same_name_missing_check_constraint()
{
    // Same column names/types, PK/AUTOINCREMENT, and the same trailing
    // UNIQUE(segment_id) table constraint as canonical -- exactly one
    // difference from the canonical DDL: no CHECK(length(segment_id) > 0),
    // so an empty segment_id could be inserted directly (which
    // SegmentId::fromString asserts against). Isolating to one difference
    // (review round 2, finding 7i) proves the CHECK itself is what the text
    // comparison catches, not incidentally the UNIQUE constraint's shape.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString connName = "d8_no_check";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL,"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(segment_id)"
            ")"
        ));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Outdated);
}

void DatabaseTest::test_D8_schema_outdated_partial_unique_segment_id_index()
{
    // Same columns/types/PK/CHECK as canonical, but segment_id uniqueness is
    // enforced only among finalized rows via a partial index instead of a
    // table-wide UNIQUE constraint -- two unfinalized checkpoints could
    // collide on segment_id.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString connName = "d8_partial_unique";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL CHECK(length(segment_id) > 0),"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0"
            ")"
        ));
        QVERIFY(q.exec(
            "CREATE UNIQUE INDEX idx_segment_id_partial ON durations(segment_id) WHERE is_finalized = 1"
        ));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Outdated);
}

void DatabaseTest::test_D8_schema_ready_repairs_mismatched_idx_start_utc_predicate()
{
    // A same-named idx_start_utc that is NOT partial: "CREATE INDEX IF NOT
    // EXISTS" silently no-ops on a same-named index regardless of its
    // definition, so a wrong existing definition must be detected and
    // rebuilt rather than left as a full (non-partial) index.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString connName = "d8_wrong_idx_predicate";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL CHECK(length(segment_id) > 0),"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(segment_id)"
            ")"
        ));
        QVERIFY(q.exec("CREATE INDEX idx_start_utc ON durations(start_utc)")); // no WHERE clause
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verify(manager.rawDb_dbg());
    QVERIFY(verify.exec("SELECT sql FROM sqlite_master WHERE type='index' AND name='idx_start_utc'"));
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toString(),
             QString("CREATE INDEX idx_start_utc ON durations(start_utc) WHERE is_finalized = 1"));
    manager.closeConnection_dbg();
}

void DatabaseTest::test_D8_schema_inaccessible_when_directory_not_writable()
{
#ifndef Q_OS_UNIX
    // On Windows, QFile::setPermissions() on a *directory* does not prevent
    // file creation inside it -- there is no POSIX-style directory write bit
    // to clear -- so this probe cannot be reproduced this way here. R06-002's
    // native Windows ACL counterpart is still missing (see R06's open
    // questions); this is a real portability gap, not exercised by this test.
    QSKIP("directory-unwritable simulation via setPermissions() is Unix-specific; R06-002's native Windows ACL case is not covered here");
#else
    // Arrange
    if (geteuid() == 0)
        QSKIP("root ignores directory permission bits, so this probe cannot be reproduced under test as root");

    // R06-002: a readable, file-permission-writable DB file is not proof
    // that its containing directory can host SQLite's rollback journal.
    // Uses AppPaths' UTIMER_DATA_DIR override (already relied on by the
    // apptest suite) to point a real store at an isolated temp directory
    // instead of the shared applicationDirPath() database.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    struct EnvGuard {
        QByteArray orig;
        bool hadOrig;
        ~EnvGuard() {
            if (hadOrig) qputenv("UTIMER_DATA_DIR", orig);
            else qunsetenv("UTIMER_DATA_DIR");
        }
    } envGuard{ qgetenv("UTIMER_DATA_DIR"), qEnvironmentVariableIsSet("UTIMER_DATA_DIR") };
    qputenv("UTIMER_DATA_DIR", tempDir.path().toUtf8());

    Settings settings(createSettingsFile(tempDir.path(), 7));

    // Arrange: create a valid DB while the directory is still writable.
    {
        SqliteSessionStore seed(settings);
        QVERIFY(seed.ensureOpen_dbg());
    }

    // Make the directory (not the file) unwritable. Restored by the guard's
    // destructor even on an early QVERIFY failure below, so QTemporaryDir
    // can still clean itself up.
    struct DirPermGuard {
        QString path;
        QFileDevice::Permissions original;
        ~DirPermGuard() { QFile::setPermissions(path, original); }
    } dirGuard{ tempDir.path(), QFileInfo(tempDir.path()).permissions() };
    QVERIFY(QFile::setPermissions(tempDir.path(),
        QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    // Act
    SqliteSessionStore manager(settings);

    // Assert
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Inaccessible);
#endif
}

void DatabaseTest::test_D8_settingsRebuild_dropFailure_preservesTableAndMarker()
{
    // R06-003: if the DROP step of the app_settings rebuild fails, the old
    // (malformed) table and its marker row must survive untouched, and
    // startup must not report Ready.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString seedConn = "d8_drop_fail_seed";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", seedConn);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL CHECK(length(segment_id) > 0),"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(segment_id)"
            ")"
        ));
        // Malformed app_settings (no PK/NOT NULL) already carrying a marker.
        QVERIFY(q.exec("CREATE TABLE app_settings (key TEXT, value TEXT)"));
        QSqlQuery ins(db);
        ins.prepare("INSERT INTO app_settings (key, value) VALUES ('last_clean_shutdown', :v)");
        ins.bindValue(":v", "2025-01-01T00:00:00.000Z");
        QVERIFY(ins.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(seedConn);

    SqliteSessionStore manager(settings);
    manager.setForceSettingsDropFailure_dbg(true);

    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Inaccessible);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verify(manager.rawDb_dbg());
    QVERIFY(verify.exec("SELECT value FROM app_settings WHERE key = 'last_clean_shutdown'"));
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toString(), QString("2025-01-01T00:00:00.000Z"));

    QSqlQuery colQuery(manager.rawDb_dbg());
    QVERIFY(colQuery.exec("PRAGMA table_info(app_settings)"));
    bool keyIsPk = false;
    while (colQuery.next()) {
        if (colQuery.value(1).toString() == "key") {
            keyIsPk = colQuery.value(5).toInt() == 1;
        }
    }
    QVERIFY(!keyIsPk); // still the malformed shape -- rebuild did not commit
    manager.closeConnection_dbg();

    // A subsequent attempt without the injected failure recovers.
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);
}

void DatabaseTest::test_D8_settingsRebuild_createFailure_preservesTableAndMarker()
{
    // R06-003: if the CREATE step fails (DROP already succeeded inside the
    // same transaction), the rollback must restore the old table and marker
    // -- not leave the database with no app_settings table at all.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString seedConn = "d8_create_fail_seed";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", seedConn);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL CHECK(length(segment_id) > 0),"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(segment_id)"
            ")"
        ));
        QVERIFY(q.exec("CREATE TABLE app_settings (key TEXT, value TEXT)"));
        QSqlQuery ins(db);
        ins.prepare("INSERT INTO app_settings (key, value) VALUES ('last_clean_shutdown', :v)");
        ins.bindValue(":v", "2025-01-01T00:00:00.000Z");
        QVERIFY(ins.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(seedConn);

    SqliteSessionStore manager(settings);
    manager.setForceSettingsCreateFailure_dbg(true);

    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Inaccessible);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verify(manager.rawDb_dbg());
    QVERIFY(verify.exec("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='app_settings'"));
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 1); // table still exists (DROP was rolled back)

    QSqlQuery markerQuery(manager.rawDb_dbg());
    QVERIFY(markerQuery.exec("SELECT value FROM app_settings WHERE key = 'last_clean_shutdown'"));
    QVERIFY(markerQuery.next());
    QCOMPARE(markerQuery.value(0).toString(), QString("2025-01-01T00:00:00.000Z"));
    manager.closeConnection_dbg();

    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);
}

void DatabaseTest::test_D8_settingsRebuild_commitFailure_preservesTableAndMarker()
{
    // R06-003: if COMMIT fails after a successful DROP+CREATE, the rollback
    // must restore the OLD table and marker, not leave the freshly-created
    // (uncommitted) empty table in place.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    const QString seedConn = "d8_commit_fail_seed";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", seedConn);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(
            "CREATE TABLE durations ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "segment_id TEXT NOT NULL CHECK(length(segment_id) > 0),"
            "type INTEGER NOT NULL,"
            "start_utc TEXT NOT NULL,"
            "end_utc TEXT NOT NULL,"
            "is_finalized INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(segment_id)"
            ")"
        ));
        QVERIFY(q.exec("CREATE TABLE app_settings (key TEXT, value TEXT)"));
        QSqlQuery ins(db);
        ins.prepare("INSERT INTO app_settings (key, value) VALUES ('last_clean_shutdown', :v)");
        ins.bindValue(":v", "2025-01-01T00:00:00.000Z");
        QVERIFY(ins.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(seedConn);

    SqliteSessionStore manager(settings);
    // Use the rebuild-scoped seam (review round 2, finding 7ii), not the
    // shared setForceCommitFailure_dbg(): that one is consumed by whichever
    // commitOrRollback() call is reached FIRST on the path, which happens to
    // be ensureSettingsTable()'s today but would silently retarget if an
    // earlier commit were ever added to checkSchemaOnStartup()'s path.
    manager.setForceSettingsCommitFailure_dbg(true);

    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Inaccessible);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery markerQuery(manager.rawDb_dbg());
    QVERIFY(markerQuery.exec("SELECT value FROM app_settings WHERE key = 'last_clean_shutdown'"));
    QVERIFY(markerQuery.next());
    QCOMPARE(markerQuery.value(0).toString(), QString("2025-01-01T00:00:00.000Z"));

    QSqlQuery colQuery(manager.rawDb_dbg());
    QVERIFY(colQuery.exec("PRAGMA table_info(app_settings)"));
    bool keyIsPk = false;
    while (colQuery.next()) {
        if (colQuery.value(1).toString() == "key") {
            keyIsPk = colQuery.value(5).toInt() == 1;
        }
    }
    QVERIFY(!keyIsPk); // old malformed shape -- the rebuild's commit never landed
    manager.closeConnection_dbg();

    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);
}

void DatabaseTest::test_D8_ensureOpen_reconnect_restores_synchronous_normal()
{
    // R06-004: a transparent reconnect through ensureOpen() must re-apply
    // synchronous=NORMAL -- a brand-new native connection otherwise reverts
    // to SQLite's default (FULL) and stays there silently.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);
    QVERIFY(manager.ensureOpen_dbg());

    manager.closeConnection_dbg();
    QVERIFY(manager.ensureOpen_dbg());

    QSqlQuery q(manager.rawDb_dbg());
    QVERIFY(q.exec("PRAGMA synchronous"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1); // 1 == NORMAL
}

void DatabaseTest::test_D8_createBackup_reopen_restores_synchronous_normal()
{
    // R06-004: createBackup()'s close-copy-reopen cycle must also leave the
    // connection at synchronous=NORMAL via the same centralized helper.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);
    QVERIFY(manager.ensureOpen_dbg());

    // replaceAll() triggers createBackup()'s close-copy-reopen cycle.
    QVERIFY(manager.replaceAll(Timeline(), Timeline()).ok());

    QSqlQuery q(manager.rawDb_dbg());
    QVERIFY(q.exec("PRAGMA synchronous"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);
}
