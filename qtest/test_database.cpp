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

void DatabaseTest::test_database_write_refused_on_unwritable_file_then_recovers()
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

    // An already-open fd stays writable on Linux regardless of a later
    // chmod (the permission check happens at open(), not at each write()),
    // so close the connection first: this forces ensureOpen()'s next call to
    // genuinely re-open the file, which is where the real OS-level denial
    // below actually bites.
    manager.closeConnection_dbg();
    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser));

    // The write should fail (ensureOpen()'s reopen sees the unwritable file)
    // without mutating anything.
    std::deque<TimeDuration> additionalData;
    additionalData.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-50), now.addSecs(-40)));
    QVERIFY(!manager.commitSession(Timeline(additionalData, std::nullopt)).ok());

    // Restore permissions
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser));

    // Original data should be preserved -- the store transparently reconnects.
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

void DatabaseTest::test_database_load_computes_duration_from_timestamps()
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

    // Duration is computed from the persisted endpoints and must equal the exact 5 s the row was written with.
    auto loaded = manager.loadDurations();
    QCOMPARE(loaded.size(), (size_t)1);
    QCOMPARE(loaded[0].duration(), (qint64)5000);
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
        TestCommon::seedStoredRows(seed, durations, true);
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
// commitSession contract tests
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
 * B1 (R07-002) at the reconcile layer, overlap-probe branch: a real SQL
 * error on the overlap-probe SELECT (distinct from the UPDATE failure
 * above) must also flip ReconcileResult::ok to false and must NOT be
 * reported as a policy drop. Renaming the durations table away makes the
 * probe's real SELECT fail with "no such table" while leaving the UPDATE
 * path unexercised, proving the two failure sites are attributed
 * correctly and neither is silently absorbed by the other.
 */
void DatabaseTest::test_reconcileUnfinalizedCheckpoints_probe_sql_error_is_not_reported_as_dropped()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    QVERIFY(manager.ensureOpen_dbg());
    // Orphan with no overlap -- would finalize cleanly, but the probe SELECT
    // that must run first is blocked.
    const QDateTime startUtc = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), Qt::UTC);
    const QDateTime endUtc   = QDateTime(QDate(2025, 1, 1), QTime(12, 30, 0), Qt::UTC);
    const qint64 orphanId = seedOrphanRow(manager, startUtc, endUtc);
    QVERIFY(orphanId > 0);

    QSqlQuery rename(manager.rawDb_dbg());
    QVERIFY(rename.exec("ALTER TABLE durations RENAME TO durations_b1_probe_test"));

    std::vector<SqliteSessionStore::OrphanCheckpoint_dbg> toFinalize;
    SqliteSessionStore::OrphanCheckpoint_dbg orphan;
    orphan.id = orphanId;
    orphan.startTime = startUtc.toLocalTime();
    orphan.endTime = endUtc.toLocalTime();
    toFinalize.push_back(orphan);

    SqliteSessionStore::ReconcileResult_dbg result = manager.reconcileUnfinalizedCheckpoints_dbg(toFinalize, {});

    QVERIFY(!result.ok);
    QVERIFY(result.finalized.empty());
    QVERIFY(result.policy_dropped.empty());

    QVERIFY(rename.exec("ALTER TABLE durations_b1_probe_test RENAME TO durations"));

    // Raw state: the row is untouched -- rolled back, not finalized or deleted.
    QSqlQuery verify(manager.rawDb_dbg());
    verify.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    verify.bindValue(":id", orphanId);
    QVERIFY(verify.exec());
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 0);

    // The connection recovers: a retry with the table restored finalizes normally.
    SqliteSessionStore::ReconcileResult_dbg retryResult = manager.reconcileUnfinalizedCheckpoints_dbg(toFinalize, {});
    QVERIFY(retryResult.ok);
    QCOMPARE(retryResult.finalized.size(), static_cast<size_t>(1));

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

    // Seed a valid unfinalized orphan (well inside History retention) with no
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

void DatabaseTest::test_schema_status_ready_enforces_unique_segment_id()
{
    // Arrange: a valid existing DB that is missing app_settings must have it
    // created on the Ready path (checkSchemaOnStartup). The UNIQUE(segment_id)
    // constraint (not an explicit idx_segment_id index) enforces segment_id
    // uniqueness. A freshly created DB via ensureSchema should have no
    // idx_segment_id either.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));

    // Arrange: create only the durations table with the full valid schema
    // (columns + UNIQUE(segment_id) constraint), but no app_settings.
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

    // Act: construct the store and check schema on startup.
    SqliteSessionStore manager(settings);
    QCOMPARE(manager.checkSchemaOnStartup(), SchemaStatus::Ready);

    // Assert: app_settings was created.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery verify(manager.rawDb_dbg());
    QVERIFY(verify.exec(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='app_settings'"
    ));
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 1);

    // Assert: idx_segment_id does NOT exist (unique constraint handles it).
    QVERIFY(verify.exec(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_segment_id'"
    ));
    QVERIFY(verify.next());
    QCOMPARE(verify.value(0).toInt(), 0);

    // Assert: inserting a duplicate segment_id fails.
    QSqlQuery insert1(manager.rawDb_dbg());
    insert1.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, 0, '2024-01-01T00:00:00.000', '2024-01-01T01:00:00.000', 0)"
    );
    insert1.bindValue(":segment_id", "test-segment-1");
    QVERIFY(insert1.exec());

    QSqlQuery insert2(manager.rawDb_dbg());
    insert2.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, 0, '2024-01-01T01:00:00.000', '2024-01-01T02:00:00.000', 0)"
    );
    insert2.bindValue(":segment_id", "test-segment-1");
    QVERIFY(!insert2.exec()); // Should fail due to UNIQUE constraint

    manager.closeConnection_dbg();

    // Assert: a freshly created DB (ensureSchema path) has no idx_segment_id.
    resetDatabaseFile();
    SqliteSessionStore freshManager(settings);
    QVERIFY(freshManager.ensureOpen_dbg());
    QSqlQuery freshVerify(freshManager.rawDb_dbg());
    QVERIFY(freshVerify.exec(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_segment_id'"
    ));
    QVERIFY(freshVerify.next());
    QCOMPARE(freshVerify.value(0).toInt(), 0);
    freshManager.closeConnection_dbg();
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

    // Seed a valid unfinalized orphan (well inside History retention).
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
 * C1/C3 step 4: the old >60s-future-slack filter is gone. A backward clock
 * jump between the checkpoint write and the restart can make an orphan's
 * endTime land ahead of recovery's "now"; that alone is no longer a reason
 * to drop it -- only cross-day, overlap, a discard intent, or falling
 * outside History retention are. Seed an orphan whose endTime is 1-2h ahead
 * of recovery's `now` and confirm recoverStartupCheckpoints() finalizes and
 * credits it.
 */
void DatabaseTest::test_recoverStartupCheckpoints_finalizes_future_dated_orphan()
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

    // Assert: the future-dated orphan is finalized and credited, not dropped.
    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 1);
    QCOMPARE(res.policy_dropped_count, 0);
    QCOMPARE(res.discard_dropped_count, 0);
    QCOMPARE(res.recovered_msec, futureStart.msecsTo(futureEnd));

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery checkOrphan(manager.rawDb_dbg());
    checkOrphan.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    checkOrphan.bindValue(":id", orphanId);
    QVERIFY(checkOrphan.exec());
    QVERIFY(checkOrphan.next());
    QCOMPARE(checkOrphan.value(0).toInt(), 1);
    manager.closeConnection_dbg();
}

void DatabaseTest::test_recoverStartupCheckpoints_drops_unfinalized_cross_day_orphan()
{
    // MUST-FIX4 (review packet C3): B2's malformed-row definition is exactly
    // {empty id, invalid type, invalid timestamp, start>=end} -- a cross-day
    // row is well-formed by that definition, so it would otherwise pass
    // straight through loadUnfinalizedCheckpoints() and get finalized by
    // reconcileUnfinalizedCheckpoints() with no day-validity check at all.
    // Seed an otherwise-recoverable (well inside History retention) unfinalized
    // row that genuinely spans local midnight and assert recovery drops it --
    // never finalizes it into history -- exactly like the retention-cutoff
    // policy drop below.
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

    // Recover shortly after the orphan's end -- well within History
    // retention, so cross-day is the ONLY reason this row is dropped.
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
// C1/C3 step 4: retention cutoff inside recovery, and cleanup restricted to
// finalized rows.
// ============================================================================

/**
 * C1/C3 step 4: the retention cutoff is `now.addDays(-history_days_to_keep_)`,
 * using recovery's own captured `now` (retention = 7 days here). An orphan
 * ending exactly at the cutoff is retained (strict "<" only); one ending a
 * millisecond earlier is dropped as a policy drop, not a discard.
 */
void DatabaseTest::test_recoverStartupCheckpoints_retention_cutoff_is_strict()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);
    QVERIFY(manager.ensureOpen_dbg());

    const QDateTime recoveryNow = QDateTime(QDate(2025, 6, 10), QTime(12, 0, 0), Qt::UTC);
    const QDateTime cutoff = recoveryNow.addDays(-7);

    // Ends exactly at the cutoff -- retained.
    const qint64 atCutoffId = seedOrphanRow(manager, cutoff.addSecs(-60), cutoff);
    // Ends 1 ms before the cutoff -- dropped.
    const qint64 beforeCutoffId = seedOrphanRow(manager, cutoff.addMSecs(-61000), cutoff.addMSecs(-1));
    QVERIFY(atCutoffId > 0 && beforeCutoffId > 0);
    manager.closeConnection_dbg();

    StartupRecoveryResult res = manager.recoverStartupCheckpoints(recoveryNow);

    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 1);
    QCOMPARE(res.policy_dropped_count, 1);
    QCOMPARE(res.discard_dropped_count, 0);

    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery checkAt(manager.rawDb_dbg());
    checkAt.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    checkAt.bindValue(":id", atCutoffId);
    QVERIFY(checkAt.exec());
    QVERIFY(checkAt.next());
    QCOMPARE(checkAt.value(0).toInt(), 1);

    QSqlQuery checkBefore(manager.rawDb_dbg());
    checkBefore.prepare("SELECT COUNT(*) FROM durations WHERE id = :id");
    checkBefore.bindValue(":id", beforeCutoffId);
    QVERIFY(checkBefore.exec());
    QVERIFY(checkBefore.next());
    QCOMPARE(checkBefore.value(0).toInt(), 0);
    manager.closeConnection_dbg();
}

/**
 * C1/C3 step 4: performRetentionCleanup() (run from checkSchemaOnStartup())
 * must delete only finalized rows. An expired unfinalized row is left in
 * place for recoverStartupCheckpoints() to classify and drop with its own
 * clock.
 */
void DatabaseTest::test_performRetentionCleanup_is_finalized_only()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const QDateTime now = QDateTime(QDate(2025, 6, 10), QTime(12, 0, 0), Qt::UTC);
    manager.setNowProvider_dbg([now]() { return now; });

    QVERIFY(manager.ensureOpen_dbg());
    const QDateTime expiredStart = now.addDays(-10);
    const QDateTime expiredEnd   = expiredStart.addSecs(30);

    // Expired finalized row -- cleanup deletes it.
    std::deque<TimeDuration> seeded;
    seeded.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, expiredStart.toLocalTime(), expiredEnd.toLocalTime()));
    QVERIFY(manager.commitSession(Timeline(seeded, std::nullopt)).ok());

    // Expired unfinalized row -- cleanup must leave it alone.
    const qint64 orphanId = seedOrphanRow(manager, expiredStart, expiredEnd);
    QVERIFY(orphanId > 0);
    manager.closeConnection_dbg();

    // A second store instance, as checkSchemaOnStartup() only runs cleanup
    // on the fresh-DB path when db_was_fresh_ is false (i.e. re-opening an
    // already-created DB), matching the on-disk startup sequence.
    SqliteSessionStore manager2(settings);
    manager2.setNowProvider_dbg([now]() { return now; });
    QCOMPARE(manager2.checkSchemaOnStartup(), SchemaStatus::Ready);

    QVERIFY(manager2.ensureOpen_dbg());
    QSqlQuery countFinalized(manager2.rawDb_dbg());
    QVERIFY(countFinalized.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 1"));
    QVERIFY(countFinalized.next());
    QCOMPARE(countFinalized.value(0).toInt(), 0);

    QSqlQuery countOrphan(manager2.rawDb_dbg());
    countOrphan.prepare("SELECT COUNT(*) FROM durations WHERE id = :id AND is_finalized = 0");
    countOrphan.bindValue(":id", orphanId);
    QVERIFY(countOrphan.exec());
    QVERIFY(countOrphan.next());
    QCOMPARE(countOrphan.value(0).toInt(), 1);
    manager2.closeConnection_dbg();

    // recoverStartupCheckpoints() then drops the orphan itself as expired.
    StartupRecoveryResult res = manager2.recoverStartupCheckpoints(now);
    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 0);
    QCOMPARE(res.policy_dropped_count, 1);
}

/**
 * C1/C3 step 4: a cleanup failure (blocked DELETE on a finalized row) must
 * not admit an expired orphan into history. checkSchemaOnStartup() still
 * reports Ready (cleanup failure is logged and non-fatal), and the
 * subsequent recovery pass still drops the expired orphan.
 */
void DatabaseTest::test_performRetentionCleanup_failure_does_not_admit_expired_orphan()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const QDateTime now = QDateTime(QDate(2025, 6, 10), QTime(12, 0, 0), Qt::UTC);
    manager.setNowProvider_dbg([now]() { return now; });

    QVERIFY(manager.ensureOpen_dbg());
    const QDateTime expiredStart = now.addDays(-10);
    const QDateTime expiredEnd   = expiredStart.addSecs(30);

    std::deque<TimeDuration> seeded;
    seeded.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, expiredStart.toLocalTime(), expiredEnd.toLocalTime()));
    QVERIFY(manager.commitSession(Timeline(seeded, std::nullopt)).ok());

    const qint64 orphanId = seedOrphanRow(manager, expiredStart, expiredEnd);
    QVERIFY(orphanId > 0);

    QSqlQuery trig(manager.rawDb_dbg());
    QVERIFY(trig.exec(
        "CREATE TRIGGER block_cleanup_delete_c3_test BEFORE DELETE ON durations "
        "WHEN OLD.is_finalized = 1 "
        "BEGIN SELECT RAISE(ABORT, 'C3 test: blocked cleanup delete'); END"
    ));
    manager.closeConnection_dbg();

    // A second store instance (see the sibling test above for why): its
    // checkSchemaOnStartup() re-opens the already-created DB, so cleanup
    // actually runs and its DELETE hits the trigger.
    SqliteSessionStore manager2(settings);
    manager2.setNowProvider_dbg([now]() { return now; });

    // Cleanup's DELETE fails, but startup is still Ready -- cleanup failure
    // does not block startup.
    QCOMPARE(manager2.checkSchemaOnStartup(), SchemaStatus::Ready);

    QVERIFY(manager2.ensureOpen_dbg());
    QSqlQuery trig2(manager2.rawDb_dbg());
    QVERIFY(trig2.exec("DROP TRIGGER block_cleanup_delete_c3_test"));
    manager2.closeConnection_dbg();

    // The expired orphan is dropped by recovery, not admitted.
    StartupRecoveryResult res = manager2.recoverStartupCheckpoints(now);
    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 0);
    QCOMPARE(res.policy_dropped_count, 1);

    QVERIFY(manager2.ensureOpen_dbg());
    QSqlQuery checkOrphan(manager2.rawDb_dbg());
    checkOrphan.prepare("SELECT COUNT(*) FROM durations WHERE id = :id");
    checkOrphan.bindValue(":id", orphanId);
    QVERIFY(checkOrphan.exec());
    QVERIFY(checkOrphan.next());
    QCOMPARE(checkOrphan.value(0).toInt(), 0);
    manager2.closeConnection_dbg();
}

/**
 * C1/C3 step 4: a backward clock move between cleanup and recovery must not
 * let cleanup delete an orphan on an earlier, stricter clock than recovery
 * itself uses. Cleanup runs at `T`; the orphan's end is older than T's
 * cutoff but still inside `(T - 2h)`'s cutoff. Since cleanup never touches
 * unfinalized rows at all now, this also proves recovery (at T - 2h) is the
 * only clock that decides the orphan's fate.
 */
void DatabaseTest::test_backward_clock_across_cutoff_between_cleanup_and_recovery()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore manager(settings);

    const QDateTime cleanupNow = QDateTime(QDate(2025, 6, 10), QTime(12, 0, 0), Qt::UTC);
    const QDateTime recoveryNow = cleanupNow.addSecs(-2 * 60 * 60); // backward 2h step

    manager.setNowProvider_dbg([cleanupNow]() { return cleanupNow; });

    QVERIFY(manager.ensureOpen_dbg());
    // Older than cleanupNow's 7-day cutoff, but inside recoveryNow's cutoff
    // (recoveryNow is 2h earlier, so its cutoff is 2h earlier too).
    const QDateTime orphanEnd = cleanupNow.addDays(-7).addSecs(-3600); // 1h past cleanup's cutoff
    const QDateTime orphanStart = orphanEnd.addSecs(-30);
    const qint64 orphanId = seedOrphanRow(manager, orphanStart, orphanEnd);
    QVERIFY(orphanId > 0);
    manager.closeConnection_dbg();

    // A second store instance (see the sibling tests above for why):
    // checkSchemaOnStartup() re-opens the already-created DB and runs
    // cleanup at cleanupNow; recovery below then runs at the older,
    // backward-stepped recoveryNow.
    SqliteSessionStore manager2(settings);
    manager2.setNowProvider_dbg([cleanupNow]() { return cleanupNow; });
    QCOMPARE(manager2.checkSchemaOnStartup(), SchemaStatus::Ready);

    StartupRecoveryResult res = manager2.recoverStartupCheckpoints(recoveryNow);

    QVERIFY(res.ok());
    QCOMPARE(res.finalized_count, 1);
    QCOMPARE(res.policy_dropped_count, 0);

    QVERIFY(manager2.ensureOpen_dbg());
    QSqlQuery checkOrphan(manager2.rawDb_dbg());
    checkOrphan.prepare("SELECT is_finalized FROM durations WHERE id = :id");
    checkOrphan.bindValue(":id", orphanId);
    QVERIFY(checkOrphan.exec());
    QVERIFY(checkOrphan.next());
    QCOMPARE(checkOrphan.value(0).toInt(), 1);
    manager2.closeConnection_dbg();
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

    // Act: recover shortly after both orphans' end times, well within
    // History retention.
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
void DatabaseTest::test_C2_commitSession_fails_on_commit_failure_with_empty_error_text()
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

    // Act: the update's COMMIT genuinely fails with an EMPTY error text.
    manager.setForceCommitCloseBefore_dbg(true);
    std::deque<TimeDuration> addition;
    addition.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-80), now.addSecs(-40)));
    const SessionStoreResult result = manager.commitSession(Timeline(addition, std::nullopt));

    // Assert: a commit failure is a commit failure, even with no message to
    // report -- callers must never read "no error text" as "success".
    QVERIFY(!result.ok());
    QCOMPARE(result.message, QStringLiteral("Failed to commit update transaction: "));

    // Assert: raw state is still the ORIGINAL two rows -- the update never
    // durably committed.
    QVERIFY(manager.ensureOpen_dbg());
    QSqlQuery count(manager.rawDb_dbg());
    QVERIFY(count.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(count.next());
    QCOMPARE(count.value(0).toInt(), 2);

    // Assert: the connection recovers for a subsequent write.
    QVERIFY(manager.commitSession(Timeline(addition, std::nullopt)).ok());
    QSqlQuery count2(manager.rawDb_dbg());
    QVERIFY(count2.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(count2.next());
    QCOMPARE(count2.value(0).toInt(), 3);

    manager.closeConnection_dbg();
}

/**
 * D13e (routed-in from D11 part 1 review): updateDurationsById() is
 * commitSession()'s commit site -- the dominant production write path --
 * but unlike saveCheckpoint() it called db.commit()/
 * db.rollback() directly instead of going through commitOrRollback(). That
 * meant a failed COMMIT there had no closeConnection() recovery when the
 * rollback also failed (C2/R07-001), and force_commit_failure_dbg_ (which
 * only fires on the NEXT commitOrRollback() call) could never reach this
 * path at all -- so this exact scenario had zero coverage. Same C2 standard
 * as the saveCheckpoint tests above, exercised via
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
 * replaced by the rollback failure. Uses commitSession(), which returns a
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
void DatabaseTest::test_C2_commitSession_preserves_commit_error_when_rollback_also_fails()
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
    std::deque<TimeDuration> addition;
    addition.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-80), now.addSecs(-40)));
    const SessionStoreResult result = manager.commitSession(Timeline(addition, std::nullopt));

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
    QVERIFY(manager.commitSession(Timeline(addition, std::nullopt)).ok());
    QSqlQuery count2(manager.rawDb_dbg());
    QVERIFY(count2.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(count2.next());
    QCOMPARE(count2.value(0).toInt(), 3);

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
// (MUST-FIX1, review packet C3: HistoryEditSession never carries such a row
// into its scoped save diff; see historyeditsession.cpp's buildScopedSave()
// and qtest/test_historyeditsession.cpp's
// test_WP4_cross_midnight_absent_from_scopedSave_unrelatedEditSucceeds).
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

// ── WP1b (H2): loadHistoryRows acceptance tests ──

// Acceptance: rows seeded out of order come back sorted by start_utc, then id.
void DatabaseTest::test_H2_loadHistoryRows_unsorted_seeding_returns_sorted_by_start_then_id()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(dir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate today = QDate::currentDate();
    const SegmentId id1 = SegmentId::mint();
    const SegmentId id2 = SegmentId::mint();
    const SegmentId id3 = SegmentId::mint();

    // Seed three rows out of order (by start time) via raw SQL to avoid timing issues
    QSqlQuery q(db.rawDb_dbg());
    const QString id1Str = id1.toString();
    const QString id2Str = id2.toString();
    const QString id3Str = id3.toString();
    const QString t0 = QDateTime(today, QTime(8, 0, 0), Qt::UTC).toString(Qt::ISODateWithMs);
    const QString t1 = QDateTime(today, QTime(8, 30, 0), Qt::UTC).toString(Qt::ISODateWithMs);
    const QString t2 = QDateTime(today, QTime(8, 15, 0), Qt::UTC).toString(Qt::ISODateWithMs);
    const QString t3 = QDateTime(today, QTime(14, 0, 0), Qt::UTC).toString(Qt::ISODateWithMs);
    const QString t4 = QDateTime(today, QTime(14, 30, 0), Qt::UTC).toString(Qt::ISODateWithMs);

    // Insert via raw SQL to avoid session normalization
    QVERIFY(q.exec(QString(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES ('%1', %2, '%3', '%4', 1)")
        .arg(id3Str).arg(static_cast<int>(DurationType::Activity)).arg(t3).arg(t4)));
    QVERIFY(q.exec(QString(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES ('%1', %2, '%3', '%4', 1)")
        .arg(id1Str).arg(static_cast<int>(DurationType::Activity)).arg(t0).arg(t1)));
    QVERIFY(q.exec(QString(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES ('%1', %2, '%3', '%4', 1)")
        .arg(id2Str).arg(static_cast<int>(DurationType::Pause)).arg(t0).arg(t2)));

    const HistoryRowsResult result = db.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QCOMPARE(result.rows.size(), static_cast<size_t>(3));

    // Verify sorted by start_utc, then id
    QCOMPARE(result.rows[0].startUtc, t0);
    QCOMPARE(result.rows[0].segmentId, id1Str);
    QCOMPARE(result.rows[1].startUtc, t0);
    QCOMPARE(result.rows[1].segmentId, id2Str);
    QCOMPARE(result.rows[2].startUtc, t3);
    QCOMPARE(result.rows[2].segmentId, id3Str);
}

// Acceptance: raw isFinalized is exactly "1" for finalized rows.
void DatabaseTest::test_H2_loadHistoryRows_finalized_row_raw_isFinalized_is_1()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    QVERIFY(db.ensureOpen_dbg());

    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();
    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(10, 0, 0), Qt::UTC),
        QDateTime(today, QTime(10, 30, 0), Qt::UTC), id));

    QVERIFY(db.commitSession(Timeline(rows, std::nullopt)).ok());

    const HistoryRowsResult result = db.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QCOMPARE(result.rows.size(), static_cast<size_t>(1));
    QCOMPARE(result.rows[0].isFinalized, QString("1"));
}

// Acceptance: raw isFinalized is "0" for checkpoint rows.
void DatabaseTest::test_H2_loadHistoryRows_checkpoint_raw_isFinalized_is_0()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    QVERIFY(db.ensureOpen_dbg());

    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();
    const QDateTime t0(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(10, 30, 0), Qt::UTC);

    QVERIFY(db.saveCheckpoint(DurationType::Activity, t0, t1, id).ok());

    const HistoryRowsResult result = db.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QCOMPARE(result.rows.size(), static_cast<size_t>(1));
    QCOMPARE(result.rows[0].isFinalized, QString("0"));
}

// Acceptance: parsed has exact id, type, with local-time QDateTimes.
void DatabaseTest::test_H2_loadHistoryRows_parsed_has_exact_id_type_and_local_times()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    QVERIFY(db.ensureOpen_dbg());

    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();
    const QDateTime localStart(today, QTime(10, 0, 0), Qt::LocalTime);
    const QDateTime localEnd(today, QTime(10, 30, 0), Qt::LocalTime);

    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, localStart, localEnd, id));

    QVERIFY(db.commitSession(Timeline(rows, std::nullopt)).ok());

    const HistoryRowsResult result = db.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QCOMPARE(result.rows.size(), static_cast<size_t>(1));

    QVERIFY(result.rows[0].parsed.has_value());
    const TimeDuration& parsed = result.rows[0].parsed.value();
    QCOMPARE(parsed.segment_id, id);
    QCOMPARE(parsed.type, DurationType::Activity);
    // Local time should match
    QCOMPARE(parsed.startTime.toLocalTime(), localStart);
    QCOMPARE(parsed.endTime.toLocalTime(), localEnd);
}

// Acceptance: a row written by saveCheckpoint appears with "0", after commitSession it appears with "1".
void DatabaseTest::test_H2_loadHistoryRows_saveCheckpoint_commitSession_round_trip()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    QVERIFY(db.ensureOpen_dbg());

    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();
    const QDateTime t0(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(10, 30, 0), Qt::UTC);

    // First: save a checkpoint
    QVERIFY(db.saveCheckpoint(DurationType::Activity, t0, t1, id).ok());

    HistoryRowsResult result = db.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QCOMPARE(result.rows.size(), static_cast<size_t>(1));
    QCOMPARE(result.rows[0].isFinalized, QString("0"));

    // Second: commit that same id
    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t0, t1, id));
    QVERIFY(db.commitSession(Timeline(rows, std::nullopt)).ok());

    result = db.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QCOMPARE(result.rows.size(), static_cast<size_t>(1));
    QCOMPARE(result.rows[0].isFinalized, QString("1"));
}

// ── H5/WP2: saveHistoryDays acceptance suite ──────────────────────────────

namespace {

// Builds a HistoryDayChange whose dayStartUtc/dayEndUtc are the canonical
// half-open UTC span of `date`, exactly as a real caller (TimeTracker/
// HistoryEditSession) would compute them.
HistoryDayChange wp2DayChange(const QDate& date, std::vector<SegmentId> deleted,
                               std::deque<TimeDuration> upserts)
{
    HistoryDayChange c;
    c.date = date;
    c.dayStartUtc = DayValidity::localDayStart(date).toUTC();
    c.dayEndUtc = DayValidity::localDayEnd(date).toUTC();
    c.deletedIds = std::move(deleted);
    c.upserts = std::move(upserts);
    return c;
}

// Raw fingerprint of the whole durations table, order-independent, for
// byte-identical before/after comparisons. Assumes db can be opened.
QStringList wp2RawSnapshot(SqliteSessionStore& db)
{
    db.ensureOpen_dbg();
    QSqlQuery q(db.rawDb_dbg());
    q.exec("SELECT segment_id, type, start_utc, end_utc, is_finalized FROM durations");
    QStringList out;
    while (q.next()) {
        out << q.value(0).toString() + "|" + q.value(1).toString() + "|" +
               q.value(2).toString() + "|" + q.value(3).toString() + "|" + q.value(4).toString();
    }
    out.sort();
    return out;
}

// Raw fingerprint of exactly one row (empty string if absent), for isolating
// a single id's before/after state.
QString wp2RowFingerprint(SqliteSessionStore& db, const QString& id)
{
    db.ensureOpen_dbg();
    QSqlQuery q(db.rawDb_dbg());
    q.prepare("SELECT segment_id, type, start_utc, end_utc, is_finalized FROM durations WHERE segment_id = :id");
    q.bindValue(":id", id);
    q.exec();
    if (!q.next())
        return QString();
    return q.value(0).toString() + "|" + q.value(1).toString() + "|" +
           q.value(2).toString() + "|" + q.value(3).toString() + "|" + q.value(4).toString();
}

bool wp2InsertRaw(SqliteSessionStore& db, const QString& id, DurationType type,
                   const QDateTime& startUtc, const QDateTime& endUtc, bool finalized)
{
    db.ensureOpen_dbg();
    QSqlQuery q(db.rawDb_dbg());
    q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
              "VALUES (:id, :type, :s, :e, :f)");
    q.bindValue(":id", id);
    q.bindValue(":type", static_cast<int>(type));
    q.bindValue(":s", startUtc.toUTC().toString(Qt::ISODateWithMs));
    q.bindValue(":e", endUtc.toUTC().toString(Qt::ISODateWithMs));
    q.bindValue(":f", finalized ? 1 : 0);
    return q.exec();
}

} // namespace

// ---- Success cases ----

// Only the selected rows change. Other dates, and an unfinalized "live" row
// on another date, stay byte-identical.
void DatabaseTest::test_H5_WP2_saveHistoryDays_success_only_selected_rows_change()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const QDate dateB = dateA.addDays(-1);
    const SegmentId idA = SegmentId::mint();
    const SegmentId idB = SegmentId::mint();
    const SegmentId idLive = SegmentId::mint();

    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA));
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateB, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(dateB, QTime(9, 30, 0), Qt::LocalTime), idB));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());
    QVERIFY(db.saveCheckpoint(DurationType::Pause,
        QDateTime(dateB, QTime(11, 0, 0), Qt::LocalTime),
        QDateTime(dateB, QTime(11, 15, 0), Qt::LocalTime), idLive).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    QCOMPARE(snapshot.status, SessionStoreResult::Success);
    const QString bBefore = wp2RowFingerprint(db, idB.toString());
    const QString liveBefore = wp2RowFingerprint(db, idLive.toString());

    // Delta counts, not absolute zero: data_dir_ is shared across this whole
    // test class, so earlier tests may already have left *.backup/
    // *.durations.txt files in it.
    QDir testDir(data_dir_.path());
    const int backupsBefore = testDir.entryList(QStringList() << "*.backup", QDir::Files).size();
    const int txtBefore = testDir.entryList(QStringList() << "*.durations.txt", QDir::Files).size();

    TimeDuration updatedA = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime), idA);
    std::deque<TimeDuration> upserts{updatedA};
    const HistoryDayChange day = wp2DayChange(dateA, {}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Success);

    QCOMPARE(wp2RowFingerprint(db, idB.toString()), bBefore);
    QCOMPARE(wp2RowFingerprint(db, idLive.toString()), liveBefore);
    QCOMPARE(wp2RowFingerprint(db, idA.toString()),
             QString("%1|%2|%3|%4|1").arg(idA.toString())
                 .arg(static_cast<int>(DurationType::Activity))
                 .arg(updatedA.startTime.toUTC().toString(Qt::ISODateWithMs))
                 .arg(updatedA.endTime.toUTC().toString(Qt::ISODateWithMs)));

    QCOMPARE(testDir.entryList(QStringList() << "*.backup", QDir::Files).size(), backupsBefore);
    QCOMPARE(testDir.entryList(QStringList() << "*.durations.txt", QDir::Files).size(), txtBefore);
}

// A split does an UPDATE of the original id plus an INSERT of a new id.
void DatabaseTest::test_H5_WP2_saveHistoryDays_success_split_update_plus_insert()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idA = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    const SegmentId idNew = SegmentId::mint();
    TimeDuration updatedA = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA);
    TimeDuration newHalf = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime), idNew);
    std::deque<TimeDuration> upserts{updatedA, newHalf};
    const HistoryDayChange day = wp2DayChange(dateA, {}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Success);

    QVERIFY(db.ensureOpen_dbg());
    QSqlQuery countQ(db.rawDb_dbg());
    QVERIFY(countQ.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(countQ.next());
    QCOMPARE(countQ.value(0).toInt(), 2);

    QCOMPARE(wp2RowFingerprint(db, idA.toString()),
             QString("%1|%2|%3|%4|1").arg(idA.toString())
                 .arg(static_cast<int>(DurationType::Activity))
                 .arg(updatedA.startTime.toUTC().toString(Qt::ISODateWithMs))
                 .arg(updatedA.endTime.toUTC().toString(Qt::ISODateWithMs)));
    QCOMPARE(wp2RowFingerprint(db, idNew.toString()),
             QString("%1|%2|%3|%4|1").arg(idNew.toString())
                 .arg(static_cast<int>(DurationType::Activity))
                 .arg(newHalf.startTime.toUTC().toString(Qt::ISODateWithMs))
                 .arg(newHalf.endTime.toUTC().toString(Qt::ISODateWithMs)));
}

// A merge deletes the loser id via deletedIds and the survivor is updated
// via upserts.
void DatabaseTest::test_H5_WP2_saveHistoryDays_success_merge_via_delete_and_update()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idSurvivor = SegmentId::mint();
    const SegmentId idLoser = SegmentId::mint();
    // Seeded via raw SQL, not commitSession(): two adjacent same-type rows
    // would be silently normalized (merged) by commitSession()'s own
    // pipeline before ever reaching the database as two separate rows, which
    // would defeat this test's premise of two independently editable rows.
    QVERIFY(wp2InsertRaw(db, idSurvivor.toString(), DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime).toUTC(), true));
    QVERIFY(wp2InsertRaw(db, idLoser.toString(), DurationType::Activity,
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime).toUTC(), true));
    db.closeConnection_dbg();

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    TimeDuration merged = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime), idSurvivor);
    std::deque<TimeDuration> upserts{merged};
    const HistoryDayChange day = wp2DayChange(dateA, {idLoser}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Success);

    QCOMPARE(wp2RowFingerprint(db, idLoser.toString()), QString());
    QCOMPARE(wp2RowFingerprint(db, idSurvivor.toString()),
             QString("%1|%2|%3|%4|1").arg(idSurvivor.toString())
                 .arg(static_cast<int>(DurationType::Activity))
                 .arg(merged.startTime.toUTC().toString(Qt::ISODateWithMs))
                 .arg(merged.endTime.toUTC().toString(Qt::ISODateWithMs)));

    QVERIFY(db.ensureOpen_dbg());
    QSqlQuery countQ(db.rawDb_dbg());
    QVERIFY(countQ.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(countQ.next());
    QCOMPARE(countQ.value(0).toInt(), 1);
}

// An empty replacement succeeds: every editable row on the date is deleted,
// upserts is empty. Rows on other dates are untouched.
void DatabaseTest::test_H5_WP2_saveHistoryDays_success_empty_replacement_deletes_all_editable_rows()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const QDate dateB = dateA.addDays(-1);
    const SegmentId id1 = SegmentId::mint();
    const SegmentId id2 = SegmentId::mint();
    const SegmentId idOther = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(9, 30, 0), Qt::LocalTime), id1));
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 15, 0), Qt::LocalTime), id2));
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateB, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(dateB, QTime(9, 30, 0), Qt::LocalTime), idOther));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QString otherBefore = wp2RowFingerprint(db, idOther.toString());

    const HistoryDayChange day = wp2DayChange(dateA, {id1, id2}, {});
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Success);

    QCOMPARE(wp2RowFingerprint(db, id1.toString()), QString());
    QCOMPARE(wp2RowFingerprint(db, id2.toString()), QString());
    QCOMPARE(wp2RowFingerprint(db, idOther.toString()), otherBefore);
}

// An untouched touching-or-overlapping pair of different types (a legitimate
// pre-existing Activity/Pause overlap) on the same date keeps both ids and
// every byte, while a third row on that date is edited.
void DatabaseTest::test_H5_WP2_saveHistoryDays_success_untouched_cross_type_overlap_preserved()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idActivity = SegmentId::mint();
    const SegmentId idPause = SegmentId::mint();
    const SegmentId idEdited = SegmentId::mint();

    // Seeded via raw SQL: a legitimate pre-existing Activity/Pause overlap is
    // legacy data that no ordinary write path can create today, but it can
    // already exist on disk.
    QVERIFY(wp2InsertRaw(db, idActivity.toString(), DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime).toUTC(), true));
    QVERIFY(wp2InsertRaw(db, idPause.toString(), DurationType::Pause,
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateA, QTime(10, 45, 0), Qt::LocalTime).toUTC(), true));
    QVERIFY(wp2InsertRaw(db, idEdited.toString(), DurationType::Activity,
        QDateTime(dateA, QTime(12, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateA, QTime(12, 30, 0), Qt::LocalTime).toUTC(), true));
    db.closeConnection_dbg();

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QString activityBefore = wp2RowFingerprint(db, idActivity.toString());
    const QString pauseBefore = wp2RowFingerprint(db, idPause.toString());

    TimeDuration updatedEdited = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(12, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(13, 0, 0), Qt::LocalTime), idEdited);
    std::deque<TimeDuration> upserts{updatedEdited};
    const HistoryDayChange day = wp2DayChange(dateA, {}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Success);

    QCOMPARE(wp2RowFingerprint(db, idActivity.toString()), activityBefore);
    QCOMPARE(wp2RowFingerprint(db, idPause.toString()), pauseBefore);
    QCOMPARE(wp2RowFingerprint(db, idEdited.toString()),
             QString("%1|%2|%3|%4|1").arg(idEdited.toString())
                 .arg(static_cast<int>(DurationType::Activity))
                 .arg(updatedEdited.startTime.toUTC().toString(Qt::ISODateWithMs))
                 .arg(updatedEdited.endTime.toUTC().toString(Qt::ISODateWithMs)));
}

// A malformed finalized row on the date is left completely untouched by an
// edit to a different row on the same date.
void DatabaseTest::test_H5_WP2_saveHistoryDays_success_malformed_row_on_date_untouched_by_edit()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idGood = SegmentId::mint();
    const SegmentId idMalformed = SegmentId::mint();

    QVERIFY(wp2InsertRaw(db, idGood.toString(), DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime).toUTC(), true));

    // Malformed: invalid type (99), otherwise a well-formed interval on dateA.
    QVERIFY(db.ensureOpen_dbg());
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:id, 99, :s, :e, 1)");
        q.bindValue(":id", idMalformed.toString());
        q.bindValue(":s", QDateTime(dateA, QTime(14, 0, 0), Qt::LocalTime).toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":e", QDateTime(dateA, QTime(14, 30, 0), Qt::LocalTime).toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
    }
    db.closeConnection_dbg();

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QString malformedBefore = wp2RowFingerprint(db, idMalformed.toString());

    TimeDuration updatedGood = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime), idGood);
    std::deque<TimeDuration> upserts{updatedGood};
    const HistoryDayChange day = wp2DayChange(dateA, {}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Success);

    QCOMPARE(wp2RowFingerprint(db, idMalformed.toString()), malformedBefore);
}

// M4: retained = re-read minus deleted/updated ids. Changing a row's type IN
// PLACE (same id, same interval) upserts a new-type row that occupies the
// exact same interval as its OWN pre-change row. If the touched id were not
// excluded from "retained", this row's own prior state would be seen as an
// untouched row of the other type at the same interval and wrongly rejected
// as a new cross-type overlap (U3).
void DatabaseTest::test_H5_WP2_saveHistoryDays_success_type_changed_in_place_same_interval()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idA = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    TimeDuration retyped = TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA);
    std::deque<TimeDuration> upserts{retyped};
    const HistoryDayChange day = wp2DayChange(dateA, {}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Success);

    QCOMPARE(wp2RowFingerprint(db, idA.toString()),
             QString("%1|%2|%3|%4|1").arg(idA.toString())
                 .arg(static_cast<int>(DurationType::Pause))
                 .arg(retyped.startTime.toUTC().toString(Qt::ISODateWithMs))
                 .arg(retyped.endTime.toUTC().toString(Qt::ISODateWithMs)));
}

// M4: retained = re-read minus deleted/updated ids. Deleting a row of one
// type and inserting a NEW id of the other type over the exact same interval
// must succeed -- if the deleted id were not excluded from "retained", the
// still-present (pre-delete) row would be seen as an untouched row of the
// other type at the same interval and wrongly rejected as a new cross-type
// overlap (U3).
void DatabaseTest::test_H5_WP2_saveHistoryDays_success_delete_one_type_insert_other_type_same_interval()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idOld = SegmentId::mint();
    const SegmentId idNew = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idOld));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    TimeDuration replacement = TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idNew);
    std::deque<TimeDuration> upserts{replacement};
    const HistoryDayChange day = wp2DayChange(dateA, {idOld}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Success);

    QCOMPARE(wp2RowFingerprint(db, idOld.toString()), QString());
    QCOMPARE(wp2RowFingerprint(db, idNew.toString()),
             QString("%1|%2|%3|%4|1").arg(idNew.toString())
                 .arg(static_cast<int>(DurationType::Pause))
                 .arg(replacement.startTime.toUTC().toString(Qt::ISODateWithMs))
                 .arg(replacement.endTime.toUTC().toString(Qt::ISODateWithMs)));
}

// ---- Conflict cases: nothing changed ----

// A selected-date row changed by raw SQL after the snapshot was taken.
void DatabaseTest::test_H5_WP2_saveHistoryDays_conflict_row_changed_after_snapshot()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idA = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    QVERIFY(db.ensureOpen_dbg());
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("UPDATE durations SET end_utc = :e WHERE segment_id = :id");
        q.bindValue(":e", QDateTime(dateA, QTime(10, 45, 0), Qt::LocalTime).toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":id", idA.toString());
        QVERIFY(q.exec());
    }
    const QStringList beforeAct = wp2RawSnapshot(db);

    const HistoryDayChange day = wp2DayChange(dateA, {}, {});
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Conflict);

    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// A selected-date row deleted (raw SQL) after the snapshot.
void DatabaseTest::test_H5_WP2_saveHistoryDays_conflict_row_deleted_after_snapshot()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idA = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    QVERIFY(db.ensureOpen_dbg());
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("DELETE FROM durations WHERE segment_id = :id");
        q.bindValue(":id", idA.toString());
        QVERIFY(q.exec());
    }
    const QStringList beforeAct = wp2RawSnapshot(db);

    const HistoryDayChange day = wp2DayChange(dateA, {}, {});
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Conflict);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// A row inserted (raw SQL) on the date after the snapshot.
void DatabaseTest::test_H5_WP2_saveHistoryDays_conflict_row_inserted_after_snapshot()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idA = SegmentId::mint();
    const SegmentId idB = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    QVERIFY(wp2InsertRaw(db, idB.toString(), DurationType::Pause,
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateA, QTime(11, 30, 0), Qt::LocalTime).toUTC(), true));

    const QStringList beforeAct = wp2RawSnapshot(db);

    const HistoryDayChange day = wp2DayChange(dateA, {}, {});
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Conflict);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// A multi-boundary (cross-midnight) row overlapping the date, changed (raw
// SQL) after the snapshot.
void DatabaseTest::test_H5_WP2_saveHistoryDays_conflict_cross_midnight_row_changed_after_snapshot()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idX = SegmentId::mint();
    QVERIFY(wp2InsertRaw(db, idX.toString(), DurationType::Activity,
        QDateTime(dateA, QTime(23, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateA.addDays(1), QTime(1, 0, 0), Qt::LocalTime).toUTC(), true));

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    QVERIFY(db.ensureOpen_dbg());
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("UPDATE durations SET end_utc = :e WHERE segment_id = :id");
        q.bindValue(":e", QDateTime(dateA.addDays(1), QTime(2, 0, 0), Qt::LocalTime).toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":id", idX.toString());
        QVERIFY(q.exec());
    }
    const QStringList beforeAct = wp2RawSnapshot(db);

    const HistoryDayChange day = wp2DayChange(dateA, {}, {});
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Conflict);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// M2: a row starting on D-1 23:00 and ending on D 01:00, INSERTED (raw SQL)
// AFTER the snapshot was taken, while saving day D. A start-date-only
// predicate (start_utc >= dayStart AND start_utc < dayEnd) would exclude this
// row from D's re-read entirely, since its start_utc falls on D-1 -- only the
// real predicate's end_utc > dayStart term catches it.
void DatabaseTest::test_H5_WP2_saveHistoryDays_conflict_crossMidnightIntoDay_row_inserted_after_snapshot()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateD = QDate::currentDate();
    const QDate dateDMinus1 = dateD.addDays(-1);
    const SegmentId idA = SegmentId::mint();
    const SegmentId idX = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateD, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateD, QTime(10, 30, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    QVERIFY(wp2InsertRaw(db, idX.toString(), DurationType::Activity,
        QDateTime(dateDMinus1, QTime(23, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateD, QTime(1, 0, 0), Qt::LocalTime).toUTC(), true));
    const QStringList beforeAct = wp2RawSnapshot(db);

    const HistoryDayChange day = wp2DayChange(dateD, {}, {});
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Conflict);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// M2: same shape, but the D-1 23:00 -> D 01:00 row already existed at
// snapshot time and is CHANGED (raw SQL) afterwards, while saving day D.
void DatabaseTest::test_H5_WP2_saveHistoryDays_conflict_crossMidnightIntoDay_row_changed_after_snapshot()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateD = QDate::currentDate();
    const QDate dateDMinus1 = dateD.addDays(-1);
    const SegmentId idX = SegmentId::mint();
    QVERIFY(wp2InsertRaw(db, idX.toString(), DurationType::Activity,
        QDateTime(dateDMinus1, QTime(23, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateD, QTime(1, 0, 0), Qt::LocalTime).toUTC(), true));

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    QVERIFY(db.ensureOpen_dbg());
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("UPDATE durations SET end_utc = :e WHERE segment_id = :id");
        q.bindValue(":e", QDateTime(dateD, QTime(2, 0, 0), Qt::LocalTime).toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":id", idX.toString());
        QVERIFY(q.exec());
    }
    const QStringList beforeAct = wp2RawSnapshot(db);

    const HistoryDayChange day = wp2DayChange(dateD, {}, {});
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Conflict);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// An unfinalized row (checkpoint) that appeared (raw SQL) on the date after
// the snapshot.
void DatabaseTest::test_H5_WP2_saveHistoryDays_conflict_unfinalized_row_appeared_after_snapshot()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idA = SegmentId::mint();
    const SegmentId idCheckpoint = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    QVERIFY(wp2InsertRaw(db, idCheckpoint.toString(), DurationType::Pause,
        QDateTime(dateA, QTime(12, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateA, QTime(12, 15, 0), Qt::LocalTime).toUTC(), false));

    const QStringList beforeAct = wp2RawSnapshot(db);

    const HistoryDayChange day = wp2DayChange(dateA, {}, {});
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Conflict);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// ---- CallerBug cases: nothing changed ----

// A new upsert row that cross-type-overlaps a retained (untouched) row on
// the same date.
void DatabaseTest::test_H5_WP2_saveHistoryDays_callerBug_new_upsert_crosstype_overlaps_retained()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idRetained = SegmentId::mint();
    const SegmentId idEdited = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idRetained));
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(12, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(12, 30, 0), Qt::LocalTime), idEdited));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QStringList beforeAct = wp2RawSnapshot(db);

    // Move idEdited to overlap idRetained's Pause interval: a NEW cross-type
    // overlap with an untouched row (U3).
    TimeDuration moved = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 15, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 45, 0), Qt::LocalTime), idEdited);
    std::deque<TimeDuration> upserts{moved};
    const HistoryDayChange day = wp2DayChange(dateA, {}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// A non-editable snapshot id used as a delete target: a legacy cross-midnight
// row's id.
void DatabaseTest::test_H5_WP2_saveHistoryDays_callerBug_delete_targets_cross_midnight_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idX = SegmentId::mint();
    QVERIFY(wp2InsertRaw(db, idX.toString(), DurationType::Activity,
        QDateTime(dateA, QTime(23, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateA.addDays(1), QTime(1, 0, 0), Qt::LocalTime).toUTC(), true));

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QStringList beforeAct = wp2RawSnapshot(db);

    const HistoryDayChange day = wp2DayChange(dateA, {idX}, {});
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// A non-editable snapshot id used as a delete target: an unfinalized row's id.
void DatabaseTest::test_H5_WP2_saveHistoryDays_callerBug_delete_targets_unfinalized_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idCheckpoint = SegmentId::mint();
    QVERIFY(db.saveCheckpoint(DurationType::Activity,
        QDateTime(dateA, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(9, 30, 0), Qt::LocalTime), idCheckpoint).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QStringList beforeAct = wp2RawSnapshot(db);

    const HistoryDayChange day = wp2DayChange(dateA, {idCheckpoint}, {});
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// M3: a non-editable snapshot id reused on the UPSERT side (not deletedIds):
// a legacy cross-midnight row's id, upserted with a NEW interval that lies
// entirely inside the day's span. rowEditableInSpan() rejects the OLD
// snapshot row (its real end lies past the day's end), so this is CallerBug
// even though the new interval itself is perfectly valid for the day.
void DatabaseTest::test_H5_WP2_saveHistoryDays_callerBug_upsert_reuses_cross_midnight_snapshot_id()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idX = SegmentId::mint();
    QVERIFY(wp2InsertRaw(db, idX.toString(), DurationType::Activity,
        QDateTime(dateA, QTime(23, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateA.addDays(1), QTime(1, 0, 0), Qt::LocalTime).toUTC(), true));

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QStringList beforeAct = wp2RawSnapshot(db);

    TimeDuration reused = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idX);
    std::deque<TimeDuration> upserts{reused};
    const HistoryDayChange day = wp2DayChange(dateA, {}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// M3: a non-editable snapshot id reused on the UPSERT side: an unfinalized
// (checkpoint) row's id, upserted with a new interval inside the day's span.
void DatabaseTest::test_H5_WP2_saveHistoryDays_callerBug_upsert_reuses_unfinalized_snapshot_id()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idCheckpoint = SegmentId::mint();
    QVERIFY(db.saveCheckpoint(DurationType::Activity,
        QDateTime(dateA, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(9, 30, 0), Qt::LocalTime), idCheckpoint).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QStringList beforeAct = wp2RawSnapshot(db);

    TimeDuration reused = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idCheckpoint);
    std::deque<TimeDuration> upserts{reused};
    const HistoryDayChange day = wp2DayChange(dateA, {}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// An upsert whose interval lies outside its day's span (a well-formed
// same-day interval, but declared under a DIFFERENT day's HistoryDayChange).
void DatabaseTest::test_H5_WP2_saveHistoryDays_callerBug_upsert_outside_day_span()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const QDate dateB = dateA.addDays(1);
    const SegmentId idNew = SegmentId::mint();

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QStringList beforeAct = wp2RawSnapshot(db);

    TimeDuration outOfSpan = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateB, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(dateB, QTime(9, 30, 0), Qt::LocalTime), idNew);
    std::deque<TimeDuration> upserts{outOfSpan};
    const HistoryDayChange day = wp2DayChange(dateA, {}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// Duplicate ids: the same id appears twice in upserts across two days.
void DatabaseTest::test_H5_WP2_saveHistoryDays_callerBug_duplicate_id_across_days()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const QDate dateB = dateA.addDays(-1);
    const SegmentId sharedId = SegmentId::mint();

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QStringList beforeAct = wp2RawSnapshot(db);

    TimeDuration rowA = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(9, 30, 0), Qt::LocalTime), sharedId);
    TimeDuration rowB = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateB, QTime(9, 0, 0), Qt::LocalTime),
        QDateTime(dateB, QTime(9, 30, 0), Qt::LocalTime), sharedId);
    std::deque<TimeDuration> upsertsA{rowA};
    std::deque<TimeDuration> upsertsB{rowB};
    const HistoryDayChange dayA = wp2DayChange(dateA, {}, upsertsA);
    const HistoryDayChange dayB = wp2DayChange(dateB, {}, upsertsB);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {dayA, dayB});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// Empty `days`.
void DatabaseTest::test_H5_WP2_saveHistoryDays_callerBug_empty_days()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const SessionStoreResult r = db.saveHistoryDays({}, {});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
}

// ---- Failure and recovery ----

// setForceCommitFailure_dbg(true): returns TransientError, nothing changed,
// and a retry with a freshly re-read snapshot succeeds.
void DatabaseTest::test_H5_WP2_saveHistoryDays_forceCommitFailure_then_retry_succeeds()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idA = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    HistoryRowsResult snapshot = db.loadHistoryRows();
    const QStringList beforeAct = wp2RawSnapshot(db);

    TimeDuration updated = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime), idA);
    std::deque<TimeDuration> upserts{updated};

    // Delta counts, not absolute zero: data_dir_ is shared across this whole
    // test class, so earlier tests may already have left *.backup/
    // *.durations.txt files in it.
    QDir testDir(data_dir_.path());
    const int backupsBefore = testDir.entryList(QStringList() << "*.backup", QDir::Files).size();
    const int txtBefore = testDir.entryList(QStringList() << "*.durations.txt", QDir::Files).size();

    db.setForceCommitFailure_dbg(true);
    SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {wp2DayChange(dateA, {}, upserts)});
    QCOMPARE(r.category, SessionStoreResult::TransientError);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);

    // No backup/export files created on failure either.
    QCOMPARE(testDir.entryList(QStringList() << "*.backup", QDir::Files).size(), backupsBefore);
    QCOMPARE(testDir.entryList(QStringList() << "*.durations.txt", QDir::Files).size(), txtBefore);

    // Retry with a freshly re-read snapshot succeeds.
    snapshot = db.loadHistoryRows();
    r = db.saveHistoryDays(snapshot.rows, {wp2DayChange(dateA, {}, upserts)});
    QCOMPARE(r.category, SessionStoreResult::Success);

    QCOMPARE(wp2RowFingerprint(db, idA.toString()),
             QString("%1|%2|%3|%4|1").arg(idA.toString())
                 .arg(static_cast<int>(DurationType::Activity))
                 .arg(updated.startTime.toUTC().toString(Qt::ISODateWithMs))
                 .arg(updated.endTime.toUTC().toString(Qt::ISODateWithMs)));
}

// Commit failure plus setForceRollbackFailure_dbg(true): the connection is
// closed, then transparently recovers for a subsequent write.
void DatabaseTest::test_H5_WP2_saveHistoryDays_commitFailure_and_rollbackFailure_closes_connection()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idA = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QStringList beforeAct = wp2RawSnapshot(db);

    TimeDuration updated = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime), idA);
    std::deque<TimeDuration> upserts{updated};

    db.setForceCommitFailure_dbg(true);
    db.setForceRollbackFailure_dbg(true);
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {wp2DayChange(dateA, {}, upserts)});
    QCOMPARE(r.category, SessionStoreResult::TransientError);

    QCOMPARE(wp2RawSnapshot(db), beforeAct);

    // The connection recovers transparently for a subsequent write.
    const HistoryRowsResult snapshot2 = db.loadHistoryRows();
    TimeDuration updated2 = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(11, 30, 0), Qt::LocalTime), idA);
    std::deque<TimeDuration> upserts2{updated2};
    QVERIFY(db.saveHistoryDays(snapshot2.rows, {wp2DayChange(dateA, {}, upserts2)}).ok());
}

// A separate closeConnection_dbg() call before invoking saveHistoryDays: the
// store transparently reconnects and the save succeeds.
void DatabaseTest::test_H5_WP2_saveHistoryDays_reconnects_after_explicit_closeConnection()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idA = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    db.closeConnection_dbg();

    TimeDuration updated = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime), idA);
    std::deque<TimeDuration> upserts{updated};

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {wp2DayChange(dateA, {}, upserts)});
    QCOMPARE(r.category, SessionStoreResult::Success);
}

// A UNIQUE clash on INSERT that happens AFTER an UPDATE already executed in
// the same call rolls back the whole transaction, including the earlier
// UPDATE.
void DatabaseTest::test_H5_WP2_saveHistoryDays_uniqueClash_on_insert_after_earlier_update_rolls_back_both()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateD1 = QDate::currentDate();
    const QDate dateD2 = dateD1.addDays(2);
    const SegmentId idRA = SegmentId::mint();
    const SegmentId idX = SegmentId::mint();

    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateD1, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateD1, QTime(10, 30, 0), Qt::LocalTime), idRA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    // Snapshot BEFORE idX exists anywhere in the DB.
    const HistoryRowsResult snapshot = db.loadHistoryRows();
    const QString raBefore = wp2RowFingerprint(db, idRA.toString());

    // An unrelated row lands on D2 with segment_id idX, AFTER the snapshot.
    QVERIFY(wp2InsertRaw(db, idX.toString(), DurationType::Activity,
        QDateTime(dateD2, QTime(9, 0, 0), Qt::LocalTime).toUTC(),
        QDateTime(dateD2, QTime(9, 30, 0), Qt::LocalTime).toUTC(), true));
    const QString xBefore = wp2RowFingerprint(db, idX.toString());
    const QStringList beforeAct = wp2RawSnapshot(db);

    // D1's change: UPDATE idRA, then INSERT a "new" row using idX -- idX is
    // not present in the OLD snapshot (so it is treated as an INSERT), but
    // it already exists in the live DB (on D2), so the INSERT hits its
    // UNIQUE(segment_id) constraint.
    TimeDuration updatedRA = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateD1, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateD1, QTime(11, 0, 0), Qt::LocalTime), idRA);
    TimeDuration clashing = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateD1, QTime(12, 0, 0), Qt::LocalTime),
        QDateTime(dateD1, QTime(12, 30, 0), Qt::LocalTime), idX);
    std::deque<TimeDuration> upserts{updatedRA, clashing};
    const HistoryDayChange day = wp2DayChange(dateD1, {}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::TransientError);

    // Whole transaction rolled back: RA's earlier UPDATE undone too.
    QCOMPARE(wp2RowFingerprint(db, idRA.toString()), raBefore);
    QCOMPARE(wp2RowFingerprint(db, idX.toString()), xBefore);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);
}

// M1: a failed db.rollback() on an EARLY-EXIT path (the stale-Conflict check,
// which never reaches commitOrRollback()) must still close the connection --
// otherwise the connection is left inside an open transaction forever.
// setForceRollbackFailure_dbg(true) alone (no commit failure armed) is
// consumed by saveHistoryDays()'s own rollbackOrClose() helper the first time
// it rolls back, which is exactly the stale-check path here.
void DatabaseTest::test_H5_WP2_saveHistoryDays_rollbackFailure_on_staleConflict_closes_connection()
{
    resetDatabaseFile();
    QTemporaryDir tempDir; QVERIFY(tempDir.isValid());
    SqliteSessionStore db(Settings(createSettingsFile(tempDir.path(), 30)));
    db.checkSchemaOnStartup();

    const QDate dateA = QDate::currentDate();
    const SegmentId idA = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();

    // Make the snapshot stale via a raw-SQL change after it was taken.
    QVERIFY(db.ensureOpen_dbg());
    {
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("UPDATE durations SET end_utc = :e WHERE segment_id = :id");
        q.bindValue(":e", QDateTime(dateA, QTime(10, 45, 0), Qt::LocalTime).toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":id", idA.toString());
        QVERIFY(q.exec());
    }
    const QStringList beforeAct = wp2RawSnapshot(db);

    db.setForceRollbackFailure_dbg(true);
    const HistoryDayChange day = wp2DayChange(dateA, {}, {});
    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Conflict);
    QCOMPARE(wp2RawSnapshot(db), beforeAct);

    // The connection recovers transparently for a subsequent write -- proof
    // it was closed rather than left inside a broken open transaction.
    const HistoryRowsResult snapshot2 = db.loadHistoryRows();
    TimeDuration updated = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime), idA);
    std::deque<TimeDuration> upserts{updated};
    QVERIFY(db.saveHistoryDays(snapshot2.rows, {wp2DayChange(dateA, {}, upserts)}).ok());
}

// WP9 (C11): the automatic backup/export mechanism (createBackup(),
// pruneOldBackups(), the .backup/.durations.txt sidecars) is removed
// entirely. Old sidecar files a previous build left behind are user files
// now: checkSchemaOnStartup() must not prune them, and a successful
// saveHistoryDays() must not touch them either -- no backup, no export, on
// any path.
void DatabaseTest::test_H5_WP9_old_sidecars_untouched_by_startup_and_saveHistoryDays()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);

    const QDate dateA = QDate::currentDate();
    const SegmentId idA = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(10, 30, 0), Qt::LocalTime), idA));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    // Seed 7 of each suffix (14 files total), named the way createBackup()
    // used to name them (base + ".<timestamp>.backup" / "...durations.txt"),
    // in the DB's own directory. 7 per suffix is deliberate: the removed
    // pruneOldBackups(keepCount = 5) only deleted files once a single
    // suffix's matches exceeded 5, so seeding at least 6 of each is what
    // makes this test actually catch a regression that restores it.
    const QDir dbDir(QFileInfo(db_path_).absoluteDir());
    QStringList sidecarPaths;
    for (int i = 0; i < 14; ++i) {
        const QString suffix = (i < 7) ? ".backup" : ".durations.txt";
        const QString path = QString("%1.2020-01-%2T00-00-00-%3%4")
            .arg(db_path_).arg(QString::number((i % 7) + 1).rightJustified(2, '0'))
            .arg(QString::number(i).rightJustified(3, '0')).arg(suffix);
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QString("old sidecar %1").arg(i).toUtf8());
        f.close();
        sidecarPaths << path;
    }

    QVector<qint64> sizesBefore;
    QVector<QDateTime> mtimesBefore;
    for (const QString& p : sidecarPaths) {
        sizesBefore << QFileInfo(p).size();
        mtimesBefore << QFileInfo(p).lastModified();
    }
    const int fileCountBefore = dbDir.entryList(QDir::Files).size();

    // Act 1: checkSchemaOnStartup() -- used to prune old backups; must not
    // touch these files at all any more. (This store was just constructed on
    // a fresh file, so the first call reports Created, not Ready -- both
    // mean the DB is usable.)
    QCOMPARE(db.checkSchemaOnStartup(), SchemaStatus::Created);

    // Act 2: a successful saveHistoryDays() -- must not create a backup or
    // export either.
    const HistoryRowsResult snapshot = db.loadHistoryRows();
    TimeDuration updated = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(dateA, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(dateA, QTime(11, 0, 0), Qt::LocalTime), idA);
    std::deque<TimeDuration> upserts{updated};
    QVERIFY(db.saveHistoryDays(snapshot.rows, {wp2DayChange(dateA, {}, upserts)}).ok());

    // Assert: all 14 sidecars survive, byte-for-byte, mtime-for-mtime.
    for (int i = 0; i < sidecarPaths.size(); ++i) {
        QVERIFY(QFile::exists(sidecarPaths[i]));
        QCOMPARE(QFileInfo(sidecarPaths[i]).size(), sizesBefore[i]);
        QCOMPARE(QFileInfo(sidecarPaths[i]).lastModified(), mtimesBefore[i]);
    }
    // Assert: no new sidecar (or any other new file) appeared.
    QCOMPARE(dbDir.entryList(QDir::Files).size(), fileCountBefore);

    // Act 3: checkSchemaOnStartup() again, from a new store on the same
    // file -- this time it reports Ready (schema already exists), covering
    // the other call site that used to run pruning. Must still not touch
    // the sidecars.
    SqliteSessionStore db2(settings);
    QCOMPARE(db2.checkSchemaOnStartup(), SchemaStatus::Ready);
    for (int i = 0; i < sidecarPaths.size(); ++i) {
        QVERIFY(QFile::exists(sidecarPaths[i]));
        QCOMPARE(QFileInfo(sidecarPaths[i]).size(), sizesBefore[i]);
        QCOMPARE(QFileInfo(sidecarPaths[i]).lastModified(), mtimesBefore[i]);
    }
    QCOMPARE(dbDir.entryList(QDir::Files).size(), fileCountBefore);
}
