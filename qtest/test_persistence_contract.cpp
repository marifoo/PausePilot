#include "test_persistence_contract.h"
#include "../sqlitesessionstore.h"
#include "../timeline.h"
#include "../types.h"
#include "testcommon.h"
#include <QtTest>

using TestCommon::createSettingsFile;

void PersistenceContractTest::initTestCase()
{
    QVERIFY(data_dir_.isValid());
    TestCommon::setDataDirOverride(data_dir_.path());
    db_path_ = QDir(data_dir_.path()).filePath("PausePilot.sqlite");
}

void PersistenceContractTest::cleanupTestCase()
{
    TestCommon::clearDataDirOverride();
}

// ---------------------------------------------------------------------------
// 1. commitSession normalizes adjacent same-type segments before writing.
//    Two adjacent Activity rows with the same type must be merged into one.
// ---------------------------------------------------------------------------
void PersistenceContractTest::test_commitSession_normalizes_adjacent_same_type()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);

    const QDate today = QDate::currentDate();
    const QDateTime t0(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(10, 30, 0), Qt::UTC);
    const QDateTime t2(today, QTime(11, 0, 0), Qt::UTC);

    // Arrange: two adjacent Activity segments (identical type, gap <= merge threshold)
    std::deque<TimeDuration> durations;
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t0, t1));
    durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t1, t2));  // adjacent, same type
    Timeline session(durations, std::nullopt);

    // Act
    QVERIFY(db.commitSession(session).ok());

    // Assert: exactly one merged row in the DB covering t0–t2
    auto loaded = db.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded[0].type, DurationType::Activity);
    QCOMPARE(loaded[0].startTime.toUTC(), t0);
    QCOMPARE(loaded[0].endTime.toUTC(), t2);
}

// ---------------------------------------------------------------------------
// 2. commitSession upserts (updates) a segment already in the DB.
//    A second commitSession call with the same segment_id updates the row
//    rather than inserting a duplicate.
// ---------------------------------------------------------------------------
void PersistenceContractTest::test_commitSession_upserts_existing_segment_id()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);

    const QDate today = QDate::currentDate();
    const QDateTime t0(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(10, 30, 0), Qt::UTC);
    const QDateTime t2(today, QTime(11, 0, 0), Qt::UTC);

    // Arrange: first commit — insert one Activity row
    const SegmentId stableId = SegmentId::mint();
    std::deque<TimeDuration> initial;
    initial.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t0, t1, stableId));
    QVERIFY(db.commitSession(Timeline(initial, std::nullopt)).ok());

    // Act: second commit with the same segment_id but an extended end_utc
    std::deque<TimeDuration> updated;
    updated.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t0, t2, stableId));
    QVERIFY(db.commitSession(Timeline(updated, std::nullopt)).ok());

    // Assert: still exactly one row; end_utc updated to t2
    auto loaded = db.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded[0].segment_id, stableId);
    QCOMPARE(loaded[0].endTime.toUTC(), t2);
}

// ---------------------------------------------------------------------------
// 3. commitSession removes a segment_id from the DB when normalization merges
//    it away.  Seeding two rows with distinct segment_ids and then committing
//    them as adjacent same-type causes the second id to be deleted.
// ---------------------------------------------------------------------------
void PersistenceContractTest::test_commitSession_removes_merged_away_segment_id()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);

    const QDate today = QDate::currentDate();
    const QDateTime t0(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(10, 30, 0), Qt::UTC);
    const QDateTime t2(today, QTime(11, 0, 0), Qt::UTC);

    // Arrange: seed two rows with distinct segment_ids
    const SegmentId firstId  = SegmentId::mint();
    const SegmentId secondId = SegmentId::mint();

    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t0, t1, firstId));
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, t1, t2, secondId));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());
    QCOMPARE(db.loadDurations().size(), static_cast<size_t>(2));

    // Act: commit both as Activity (same type, adjacent) — normalization merges them;
    // secondId is merged away and must be deleted from the DB.
    std::deque<TimeDuration> toMerge;
    toMerge.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t0, t1, firstId));
    toMerge.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t1, t2, secondId));  // same type now → will be merged
    QVERIFY(db.commitSession(Timeline(toMerge, std::nullopt)).ok());

    // Assert: one merged row remains; secondId is gone
    auto loaded = db.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded[0].segment_id, firstId);
    QCOMPARE(loaded[0].startTime.toUTC(), t0);
    QCOMPARE(loaded[0].endTime.toUTC(), t2);
}

