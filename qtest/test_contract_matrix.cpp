/**
 * ContractMatrixTest — per-method contract matrix for every public SessionStore method.
 *
 * Matrix columns: success | disabled-history | transient | caller-bug | conflict.
 * Rows: one per public SessionStore method.
 *
 * Non-applicable cells are documented with comments in the .h file rather than
 * silently omitted.  Most tests use the real SqliteSessionStore; the C4
 * section near the end additionally runs the identity/no-mutation cells
 * against FakeSessionStore to catch fake/concrete drift (R05-005/R02-003).
 */

#include "test_contract_matrix.h"
#include "../sqlitesessionstore.h"
#include "../timeline.h"
#include "../types.h"
#include "fakesessionstore.h"
#include "testcommon.h"
#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

using TestCommon::createSettingsFile;
using TestCommon::fixedNowUtc;

// C4 (R02-003/"done when"): this file has one section below per
// SessionStore::Operation. If SessionStore grows an 11th virtual, add its
// Operation enumerator AND a matching section here, then update this count --
// otherwise this fails to compile as a reminder instead of silently shipping
// an unverified operation.
static_assert(static_cast<int>(Operation::Count) == 11,
    "SessionStore::Operation changed size -- add a matrix section to "
    "test_contract_matrix.cpp for the new/removed operation, then update "
    "this static_assert's count.");

// WP10 (C2): FatalError is gone. The vocabulary is exactly these five
// categories -- a change here (an addition or removal) is a reminder to
// re-audit every per-method matrix section above, not just this count.
static_assert(SessionStoreResult::Success == 0 &&
              SessionStoreResult::Disabled == 1 &&
              SessionStoreResult::TransientError == 2 &&
              SessionStoreResult::CallerBug == 3 &&
              SessionStoreResult::Conflict == 4,
    "SessionStoreResult::Category changed -- update the matrix and this "
    "assertion; the vocabulary must stay exactly {Success, Disabled, "
    "TransientError, CallerBug, Conflict}.");

void ContractMatrixTest::initTestCase()
{
    QVERIFY(data_dir_.isValid());
    TestCommon::setDataDirOverride(data_dir_.path());
    db_path_ = QDir(data_dir_.path()).filePath("uTimer.sqlite");
}

void ContractMatrixTest::cleanupTestCase()
{
    TestCommon::clearDataDirOverride();
}

// ── Helpers ─────────────────────────────────────────────────────────────────

namespace {

// Returns a settings file with history enabled (30 days).
QString enabledSettings(const QTemporaryDir& dir) {
    return createSettingsFile(dir.path(), 30);
}

// Returns a settings file with history DISABLED (0 days).
QString disabledSettings(const QTemporaryDir& dir) {
    return createSettingsFile(dir.path(), 0);
}

// One finalized row for today.
//
// Qt::LocalTime, not Qt::UTC: pairing the local QDate::currentDate() with a
// Qt::UTC time spec names a different local day than the date field says at
// any nonzero offset (e.g. Pacific/Kiritimati, UTC+14, turns "today 10:00
// UTC" into local midnight the next day) -- the same local/UTC field-mixing
// fixed in test_historyeditsession.cpp's helpers (commit 4c54044). That
// mismatch makes hasEntriesForDate(QDate::currentDate()) return No for a row
// this function just committed for "today". Qt::LocalTime leaves nothing to
// convert.
std::deque<TimeDuration> oneRow() {
    const QDate today = QDate::currentDate();
    const QDateTime t0(today, QTime(10, 0, 0), Qt::LocalTime);
    const QDateTime t1(today, QTime(10, 30, 0), Qt::LocalTime);
    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t0, t1));
    return rows;
}

// H5/WP2: builds a HistoryDayChange whose dayStartUtc/dayEndUtc are the
// canonical half-open UTC span of `date`, matching what a real caller
// (TimeTracker/HistoryEditSession) would compute.
HistoryDayChange dayChangeFor(const QDate& date, std::vector<SegmentId> deleted,
                               std::deque<TimeDuration> upserts) {
    HistoryDayChange c;
    c.date = date;
    c.dayStartUtc = DayValidity::localDayStart(date).toUTC();
    c.dayEndUtc = DayValidity::localDayEnd(date).toUTC();
    c.deletedIds = std::move(deleted);
    c.upserts = std::move(upserts);
    return c;
}

} // namespace

// ── commitSession ────────────────────────────────────────────────────────────

// Success: returns Success, row appears in loadDurations().
void ContractMatrixTest::test_commitSession_success()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    const SessionStoreResult r = db.commitSession(Timeline(oneRow(), std::nullopt));
    QCOMPARE(r.category, SessionStoreResult::Success);
    QVERIFY(r.ok());
    QVERIFY(!db.loadDurations().empty());
}

// Disabled: history_days_to_keep == 0 → returns Disabled, no data written.
void ContractMatrixTest::test_commitSession_disabled_returns_Disabled()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const SessionStoreResult r = db.commitSession(Timeline(oneRow(), std::nullopt));
    QCOMPARE(r.category, SessionStoreResult::Disabled);
    QVERIFY(!r.ok());
    // transient: cannot be injected without closing the connection externally
}

void ContractMatrixTest::test_commitSession_callerBug_duplicate_segmentId_data()
{
    QTest::addColumn<bool>("historyEnabled");
    QTest::newRow("history-enabled")  << true;
    QTest::newRow("history-disabled") << false;
}

// CallerBug: two rows in the payload share a segment_id (C4/R05-003).
// Rejected before any mutation -- nothing is written. Run with history both
// enabled and disabled: CallerBug must win over Disabled (precedence rule in
// sessionstore.h), not just happen to be checked first when history is on.
void ContractMatrixTest::test_commitSession_callerBug_duplicate_segmentId()
{
    QFETCH(bool, historyEnabled);

    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(historyEnabled ? enabledSettings(dir) : disabledSettings(dir)));
    db.checkSchemaOnStartup();

    const QDate today(2025, 6, 15);
    const SegmentId sharedId = SegmentId::mint();
    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(9, 0, 0), Qt::UTC), QDateTime(today, QTime(9, 30, 0), Qt::UTC), sharedId));
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(today, QTime(10, 0, 0), Qt::UTC), QDateTime(today, QTime(10, 30, 0), Qt::UTC), sharedId));

    const SessionStoreResult r = db.commitSession(Timeline(rows, std::nullopt));
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QVERIFY(db.loadDurations().empty());
}

// ── loadDurations ────────────────────────────────────────────────────────────

// Success: status == Success, rows returned.
void ContractMatrixTest::test_loadDurations_success_returns_rows()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();
    QVERIFY(db.commitSession(Timeline(oneRow(), std::nullopt)).ok());

    const LoadResult r = db.loadDurations();
    QCOMPARE(r.status, SessionStoreResult::Success);
    QVERIFY(!r.empty());
    // CallerBug: N/A — loadDurations takes no parameters
    // transient: ensureOpen() or transaction failure sets TransientError (C4/R07-003)
}

// Disabled: status == Disabled, durations is empty.
void ContractMatrixTest::test_loadDurations_disabled_returns_Disabled_and_empty()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const LoadResult r = db.loadDurations();
    QCOMPARE(r.status, SessionStoreResult::Disabled);
    QVERIFY(r.empty());
}

// ── loadHistoryRows (H5/WP1) ───────────────────────────────────────────────

// Success: every row (finalized, unfinalized, multi-boundary, malformed) is
// returned raw; `parsed` is set only for well-formed rows; nothing is deleted.
void ContractMatrixTest::test_loadHistoryRows_success_returns_all_rows_raw()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();
    QVERIFY(db.commitSession(Timeline(oneRow(), std::nullopt)).ok());

    // Seed an unfinalized (checkpoint) row and a malformed row (empty id)
    // directly via raw SQL -- loadHistoryRows() must surface both untouched.
    QVERIFY(db.ensureOpen_dbg());
    {
        QSqlQuery insert(db.rawDb_dbg());
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)");
        insert.bindValue(":segment_id", SegmentId::mint().toString());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        insert.bindValue(":start_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        insert.bindValue(":end_utc", QDateTime::currentDateTimeUtc().addSecs(60).toString(Qt::ISODateWithMs));
        QVERIFY(insert.exec());
    }
    // Malformed rows, one of each kind the contract requires (reusing
    // DatabaseTest::test_C3_loadDurations_deletes_all_malformed_row_shapes'
    // four shapes): empty id, bad type, bad timestamp, zero interval.
    // CHECK(length(segment_id) > 0) models a healthy DB; bypass it here (as
    // an old/hand-edited DB would) to exercise the malformed-row path.
    QSqlQuery pragmaOn(db.rawDb_dbg());
    QVERIFY(pragmaOn.exec("PRAGMA ignore_check_constraints = ON"));
    {
        QSqlQuery insert(db.rawDb_dbg());
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES ('', :type, :start_utc, :end_utc, 1)");
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        insert.bindValue(":start_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        insert.bindValue(":end_utc", QDateTime::currentDateTimeUtc().addSecs(60).toString(Qt::ISODateWithMs));
        QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));
    }
    {
        QSqlQuery insert(db.rawDb_dbg());
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, 99, :start_utc, :end_utc, 1)");
        insert.bindValue(":segment_id", SegmentId::mint().toString());
        insert.bindValue(":start_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        insert.bindValue(":end_utc", QDateTime::currentDateTimeUtc().addSecs(60).toString(Qt::ISODateWithMs));
        QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));
    }
    {
        QSqlQuery insert(db.rawDb_dbg());
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, 'not-a-date', 'also-not-a-date', 1)");
        insert.bindValue(":segment_id", SegmentId::mint().toString());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));
    }
    const QString zeroIntervalTs = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    {
        QSqlQuery insert(db.rawDb_dbg());
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, :ts, :ts, 1)");
        insert.bindValue(":segment_id", SegmentId::mint().toString());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        insert.bindValue(":ts", zeroIntervalTs);
        QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));
    }
    // Multi-boundary row: a legacy row spanning more than one local date
    // (out of contract for a WRITE, but loadable -- see C3). loadHistoryRows()
    // must return it (and parse it) exactly like any other well-formed row.
    {
        QSqlQuery insert(db.rawDb_dbg());
        insert.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, :start_utc, :end_utc, 1)");
        insert.bindValue(":segment_id", SegmentId::mint().toString());
        insert.bindValue(":type", static_cast<int>(DurationType::Activity));
        const QDateTime multiStart = QDateTime::currentDateTimeUtc().addDays(-2);
        insert.bindValue(":start_utc", multiStart.toString(Qt::ISODateWithMs));
        insert.bindValue(":end_utc", multiStart.addDays(2).toString(Qt::ISODateWithMs));
        QVERIFY(insert.exec());
    }
    db.closeConnection_dbg();

    QSqlQuery countBefore(db.rawDb_dbg());
    QVERIFY(db.ensureOpen_dbg());
    QVERIFY(countBefore.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(countBefore.next());
    const int rowCountBefore = countBefore.value(0).toInt();
    db.closeConnection_dbg();

    const HistoryRowsResult r = db.loadHistoryRows();
    QCOMPARE(r.status, SessionStoreResult::Success);
    QCOMPARE(static_cast<int>(r.rows.size()), rowCountBefore);

    // Each of the four malformed kinds must come back raw (untouched text)
    // with `parsed` unset; well-formed rows (committed + unfinalized
    // checkpoint + multi-boundary) must come back parsed.
    int parsedCount = 0, malformedCount = 0, unfinalizedCount = 0;
    bool sawEmptyId = false, sawBadType = false, sawBadTimestamp = false, sawZeroInterval = false;
    for (const auto& row : r.rows) {
        if (row.parsed.has_value()) parsedCount++; else malformedCount++;
        if (!row.finalized()) unfinalizedCount++;

        if (row.segmentId.isEmpty()) {
            QVERIFY(!row.parsed.has_value());
            sawEmptyId = true;
        } else if (row.type == "99") {
            QVERIFY(!row.parsed.has_value());
            sawBadType = true;
        } else if (row.startUtc == "not-a-date") {
            QVERIFY(!row.parsed.has_value());
            QCOMPARE(row.endUtc, QString("also-not-a-date"));
            sawBadTimestamp = true;
        } else if (row.startUtc == zeroIntervalTs && row.endUtc == zeroIntervalTs) {
            QVERIFY(!row.parsed.has_value());
            sawZeroInterval = true;
        }
    }
    QVERIFY(sawEmptyId);
    QVERIFY(sawBadType);
    QVERIFY(sawBadTimestamp);
    QVERIFY(sawZeroInterval);
    QCOMPARE(malformedCount, 4);
    QCOMPARE(unfinalizedCount, 1);
    QCOMPARE(parsedCount, 3); // committed row + unfinalized checkpoint row + multi-boundary row

    // Nothing was deleted or repaired.
    QVERIFY(db.ensureOpen_dbg());
    QSqlQuery countAfter(db.rawDb_dbg());
    QVERIFY(countAfter.exec("SELECT COUNT(*) FROM durations"));
    QVERIFY(countAfter.next());
    QCOMPARE(countAfter.value(0).toInt(), rowCountBefore);
    db.closeConnection_dbg();
}

// Disabled: status == Disabled, rows is empty.
void ContractMatrixTest::test_loadHistoryRows_disabled_returns_Disabled_and_empty()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const HistoryRowsResult r = db.loadHistoryRows();
    QCOMPARE(r.status, SessionStoreResult::Disabled);
    QVERIFY(r.rows.empty());
}

// Transient: an unopenable (read-only, pre-closed) connection reports
// TransientError, matching loadDurations()'s ensureOpen() classification
// (C4/R07-003).
void ContractMatrixTest::test_loadHistoryRows_transient_on_closed_connection()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString settingsPath = enabledSettings(dir);
    {
        SqliteSessionStore seed{Settings(settingsPath)};
        QVERIFY(seed.checkSchemaOnStartup() == SchemaStatus::Created);
    }

    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser));

    SqliteSessionStore db{Settings(settingsPath)};
    QVERIFY(!db.rawDb_dbg().isOpen());

    const HistoryRowsResult r = db.loadHistoryRows();

    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser));
    QCOMPARE(r.status, SessionStoreResult::TransientError);
    QVERIFY(r.rows.empty());
}

// Fake parity: loadHistoryRows() logs the call, mirrors the same
// storedDurations/loadDurationsResult projection as loadDurations() (all
// marked finalized), then appends historyUnfinalizedRows as unfinalized.
void ContractMatrixTest::test_C4_fake_loadHistoryRows_projects_storedDurations_and_unfinalized()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    TimeDuration committed = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(8, 0, 0), Qt::UTC), QDateTime(today, QTime(8, 30, 0), Qt::UTC));
    fakeDb.storedDurations.push_back(committed);
    TimeDuration unfinalized = TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(today, QTime(9, 0, 0), Qt::UTC), QDateTime(today, QTime(9, 30, 0), Qt::UTC));
    fakeDb.historyUnfinalizedRows.push_back(unfinalized);

    const HistoryRowsResult r = fakeDb.loadHistoryRows();

    QVERIFY(fakeDb.callLog.contains("loadHistoryRows"));
    QCOMPARE(r.status, SessionStoreResult::Success);
    QCOMPARE(static_cast<int>(r.rows.size()), 2);

    int finalizedCount = 0, unfinalizedCount = 0;
    for (const auto& row : r.rows) {
        QVERIFY(row.parsed.has_value());
        if (row.finalized()) finalizedCount++; else unfinalizedCount++;
    }
    QCOMPARE(finalizedCount, 1);
    QCOMPARE(unfinalizedCount, 1);
}

// ── saveHistoryDays (H5/WP2) ─────────────────────────────────────────────────

// Success: a simple one-row update against the real store.
void ContractMatrixTest::test_saveHistoryDays_success_updates_row()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();
    std::deque<TimeDuration> seed;
    seed.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(today, QTime(10, 30, 0), Qt::LocalTime), id));
    QVERIFY(db.commitSession(Timeline(seed, std::nullopt)).ok());

    const HistoryRowsResult snapshot = db.loadHistoryRows();
    QCOMPARE(snapshot.status, SessionStoreResult::Success);

    TimeDuration updated = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(10, 0, 0), Qt::LocalTime),
        QDateTime(today, QTime(11, 0, 0), Qt::LocalTime), id);
    std::deque<TimeDuration> upserts{updated};
    const HistoryDayChange day = dayChangeFor(today, {}, upserts);

    const SessionStoreResult r = db.saveHistoryDays(snapshot.rows, {day});
    QCOMPARE(r.category, SessionStoreResult::Success);

    const HistoryRowsResult after = db.loadHistoryRows();
    QCOMPARE(after.rows.size(), static_cast<size_t>(1));
    QVERIFY(after.rows[0].parsed.has_value());
    QCOMPARE(after.rows[0].parsed->endTime, updated.endTime);
}

// Disabled: history_days_to_keep == 0, no mutation. A structurally valid
// (non-empty, well-formed span, no ids) days payload avoids tripping the
// CallerBug guard so the Disabled path is actually reached.
void ContractMatrixTest::test_saveHistoryDays_disabled_returns_Disabled()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const QDate today = QDate::currentDate();
    const HistoryDayChange day = dayChangeFor(today, {}, {});

    const SessionStoreResult r = db.saveHistoryDays({}, {day});
    QCOMPARE(r.category, SessionStoreResult::Disabled);
}

// CallerBug: empty `days`.
void ContractMatrixTest::test_saveHistoryDays_callerBug_empty_days()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    const SessionStoreResult r = db.saveHistoryDays({}, {});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
}

// CallerBug precedence over Disabled (C4/R05-003 precedence rule extended to
// saveHistoryDays): empty `days` on a Disabled store is still CallerBug.
void ContractMatrixTest::test_saveHistoryDays_callerBug_precedence_over_disabled()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const SessionStoreResult r = db.saveHistoryDays({}, {});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
}

// Fake parity: logs the call and records the exact arguments.
void ContractMatrixTest::test_C4_fake_saveHistoryDays_logs_and_records_args()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();
    std::vector<StoredHistoryRow> snapshot;
    StoredHistoryRow row;
    row.segmentId = id.toString();
    row.type = "0";
    row.startUtc = QDateTime(today, QTime(9, 0, 0), Qt::UTC).toString(Qt::ISODateWithMs);
    row.endUtc = QDateTime(today, QTime(9, 30, 0), Qt::UTC).toString(Qt::ISODateWithMs);
    row.isFinalized = "1";
    snapshot.push_back(row);

    std::deque<TimeDuration> upserts;
    upserts.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(9, 0, 0), Qt::UTC), QDateTime(today, QTime(9, 45, 0), Qt::UTC), id));
    const std::vector<HistoryDayChange> days{dayChangeFor(today, {}, upserts)};

    const SessionStoreResult r = fakeDb.saveHistoryDays(snapshot, days);

    QVERIFY(r.ok());
    QVERIFY(fakeDb.callLog.contains("saveHistoryDays"));
    QCOMPARE(fakeDb.lastHistorySaveSnapshot.size(), snapshot.size());
    QCOMPARE(fakeDb.lastHistorySaveSnapshot[0].segmentId, snapshot[0].segmentId);
    QCOMPARE(fakeDb.lastHistorySaveDays.size(), days.size());
    QCOMPARE(fakeDb.lastHistorySaveDays[0].date, days[0].date);
    QCOMPARE(fakeDb.lastHistorySaveDays[0].upserts.size(), days[0].upserts.size());
}

// Fake parity: on Success, applies deletes and upserts to storedDurations.
void ContractMatrixTest::test_C4_fake_saveHistoryDays_applies_deletes_and_upserts_on_success()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId keepId = SegmentId::mint();
    const SegmentId deleteId = SegmentId::mint();
    const SegmentId newId = SegmentId::mint();

    TimeDuration keep = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(8, 0, 0), Qt::UTC), QDateTime(today, QTime(8, 30, 0), Qt::UTC), keepId);
    TimeDuration toDelete = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(9, 0, 0), Qt::UTC), QDateTime(today, QTime(9, 30, 0), Qt::UTC), deleteId);
    fakeDb.storedDurations.push_back(keep);
    fakeDb.storedDurations.push_back(toDelete);

    std::deque<TimeDuration> upserts;
    upserts.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(today, QTime(10, 0, 0), Qt::UTC), QDateTime(today, QTime(10, 30, 0), Qt::UTC), newId));
    const HistoryDayChange day = dayChangeFor(today, {deleteId}, upserts);

    const SessionStoreResult r = fakeDb.saveHistoryDays({}, {day});
    QVERIFY(r.ok());
    QCOMPARE(fakeDb.storedDurations.size(), static_cast<size_t>(2));
    bool sawKeep = false, sawNew = false, sawDeleted = false;
    for (const auto& d : fakeDb.storedDurations) {
        if (d.segment_id == keepId) sawKeep = true;
        if (d.segment_id == newId) sawNew = true;
        if (d.segment_id == deleteId) sawDeleted = true;
    }
    QVERIFY(sawKeep);
    QVERIFY(sawNew);
    QVERIFY(!sawDeleted);
}

// Fake parity: a duplicate segment_id across the payload is CallerBug, exactly
// like the identity fact enforced elsewhere in the fake.
void ContractMatrixTest::test_C4_fake_saveHistoryDays_callerBug_duplicate_id()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId sharedId = SegmentId::mint();

    std::deque<TimeDuration> upserts;
    upserts.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(8, 0, 0), Qt::UTC), QDateTime(today, QTime(8, 30, 0), Qt::UTC), sharedId));
    const HistoryDayChange day = dayChangeFor(today, {sharedId}, upserts);

    const SessionStoreResult r = fakeDb.saveHistoryDays({}, {day});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QVERIFY(fakeDb.storedDurations.empty());
}

// Fake parity: a non-conflicting payload that WOULD violate the real store's
// span/editability policy (an id not present in the snapshot at all, deleted
// as if it were an existing editable row) is NOT rejected by the fake -- it
// is a spy, not a policy engine (sessionstore.h/fakesessionstore.cpp).
void ContractMatrixTest::test_C4_fake_saveHistoryDays_does_not_enforce_span_editability()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId unknownId = SegmentId::mint();

    const HistoryDayChange day = dayChangeFor(today, {unknownId}, {});

    const SessionStoreResult r = fakeDb.saveHistoryDays({}, {day});
    QVERIFY(r.ok());
}

// ── hasEntriesForDate ────────────────────────────────────────────────────────

// Success / Yes: a finalized entry exists for today.
void ContractMatrixTest::test_hasEntriesForDate_returns_Yes_when_entry_exists()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();
    QVERIFY(db.commitSession(Timeline(oneRow(), std::nullopt)).ok());

    const EntriesForDateResult r = db.hasEntriesForDate(QDate::currentDate());
    QCOMPARE(r, EntriesForDateResult::Yes);
}

// Success / No: no entry for today.
void ContractMatrixTest::test_hasEntriesForDate_returns_No_when_no_entry()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    const EntriesForDateResult r = db.hasEntriesForDate(QDate::currentDate());
    QCOMPARE(r, EntriesForDateResult::No);
    // CallerBug: N/A — no session-owned unique keys
}

// Disabled: ensureOpen returns false → Unknown (history off = "answer unknowable").
// Note: Unknown subsumes both the disabled case and DB open failures —
// callers must never treat Unknown as "no entries exist."
void ContractMatrixTest::test_hasEntriesForDate_disabled_returns_Unknown()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const EntriesForDateResult r = db.hasEntriesForDate(QDate::currentDate());
    QCOMPARE(r, EntriesForDateResult::Unknown);
    // transient: both also map to Unknown (ensureOpen failure path)
}

// ── saveCheckpoint ───────────────────────────────────────────────────────────

// Success: inserts a new unfinalized row.
void ContractMatrixTest::test_saveCheckpoint_success_inserts_row()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    const QDate today = QDate::currentDate();
    const QDateTime t0(today, QTime(9, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(9, 30, 0), Qt::UTC);
    const SegmentId id = SegmentId::mint();

    const SessionStoreResult r = db.saveCheckpoint(DurationType::Activity, t0, t1, id);
    QCOMPARE(r.category, SessionStoreResult::Success);
    QVERIFY(r.ok());

    // Verify the unfinalized row exists via raw SQL.
    QVERIFY(db.ensureOpen_dbg());
    QSqlQuery q(db.rawDb_dbg());
    q.prepare("SELECT COUNT(*) FROM durations WHERE is_finalized = 0 AND segment_id = :sid");
    q.bindValue(":sid", id.toString());
    QVERIFY(q.exec());
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);
    db.closeConnection_dbg();
}

// Disabled: returns Disabled, no row written.
void ContractMatrixTest::test_saveCheckpoint_disabled_returns_Disabled()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const SessionStoreResult r = db.saveCheckpoint(DurationType::Activity,
        fixedNowUtc(), fixedNowUtc().addSecs(60),
        SegmentId::mint());
    QCOMPARE(r.category, SessionStoreResult::Disabled);
    QVERIFY(!r.ok());
}

// CallerBug: empty segment_id → CallerBug, checked before the disabled guard.
void ContractMatrixTest::test_saveCheckpoint_callerBug_empty_segmentId_data()
{
    QTest::addColumn<bool>("historyEnabled");
    QTest::newRow("history-enabled")  << true;
    QTest::newRow("history-disabled") << false;
}

// CallerBug wins over Disabled (C4/R05-003 precedence rule in sessionstore.h):
// run with history both enabled and disabled so the ordering is actually
// asserted, not just commented.
void ContractMatrixTest::test_saveCheckpoint_callerBug_empty_segmentId()
{
    QFETCH(bool, historyEnabled);

    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(historyEnabled ? enabledSettings(dir) : disabledSettings(dir)));
    db.checkSchemaOnStartup();

    const SessionStoreResult r = db.saveCheckpoint(DurationType::Activity,
        fixedNowUtc(), fixedNowUtc().addSecs(60),
        SegmentId{});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    // transient: ensureOpen() failure returns TransientError (C4/R07-003)
}

// C4 fix round 2 (MEDIUM-6): CallerBug wins over Disabled for an invalid
// interval too, matching commitSession's precedence -- run with
// history both enabled and disabled so the ordering is actually asserted.
void ContractMatrixTest::test_saveCheckpoint_callerBug_invalid_interval_data()
{
    QTest::addColumn<bool>("historyEnabled");
    QTest::newRow("history-enabled")  << true;
    QTest::newRow("history-disabled") << false;
}

void ContractMatrixTest::test_saveCheckpoint_callerBug_invalid_interval()
{
    QFETCH(bool, historyEnabled);

    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(historyEnabled ? enabledSettings(dir) : disabledSettings(dir)));
    db.checkSchemaOnStartup();

    // Reversed interval (end before start) -- a NonPositive violation.
    const SessionStoreResult r = db.saveCheckpoint(DurationType::Activity,
        fixedNowUtc(), fixedNowUtc().addSecs(-60),
        SegmentId::mint());
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
}

// CallerBug: segment_id already belongs to a finalized row → CallerBug.
void ContractMatrixTest::test_saveCheckpoint_callerBug_finalized_segmentId()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    const QDate today = QDate::currentDate();
    const QDateTime t0(today, QTime(8, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(8, 30, 0), Qt::UTC);
    const SegmentId id = SegmentId::mint();

    // Finalize the row via commitSession.
    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t0, t1, id));
    QVERIFY(db.commitSession(Timeline(rows, std::nullopt)).ok());

    // Now try to checkpoint with the same segment_id → CallerBug.
    const SessionStoreResult r = db.saveCheckpoint(DurationType::Activity,
        t0, t1.addSecs(60), id);
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
}

// ── discardCheckpoint (C4/R05-005/R02-003: was entirely missing from this matrix) ──

// Success: an existing unfinalized checkpoint row is removed.
void ContractMatrixTest::test_discardCheckpoint_success_removes_row()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    const QDate today = QDate::currentDate();
    const QDateTime t0(today, QTime(9, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(9, 30, 0), Qt::UTC);
    const SegmentId id = SegmentId::mint();
    QVERIFY(db.saveCheckpoint(DurationType::Activity, t0, t1, id).ok());

    const SessionStoreResult r = db.discardCheckpoint(id);
    QCOMPARE(r.category, SessionStoreResult::Success);

    QVERIFY(db.ensureOpen_dbg());
    QSqlQuery q(db.rawDb_dbg());
    q.prepare("SELECT COUNT(*) FROM durations WHERE segment_id = :sid");
    q.bindValue(":sid", id.toString());
    QVERIFY(q.exec());
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 0);
    db.closeConnection_dbg();
}

// Disabled: returns Disabled, no-op.
void ContractMatrixTest::test_discardCheckpoint_disabled_returns_Disabled()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const SessionStoreResult r = db.discardCheckpoint(SegmentId::mint());
    QCOMPARE(r.category, SessionStoreResult::Disabled);
    QVERIFY(!r.ok());
}

// CallerBug: empty segment_id, checked before the disabled guard.
void ContractMatrixTest::test_discardCheckpoint_callerBug_empty_segmentId_data()
{
    QTest::addColumn<bool>("historyEnabled");
    QTest::newRow("history-enabled")  << true;
    QTest::newRow("history-disabled") << false;
}

// CallerBug wins over Disabled (C4/R05-003 precedence rule in sessionstore.h):
// run with history both enabled and disabled so the ordering is actually
// asserted, not just commented.
void ContractMatrixTest::test_discardCheckpoint_callerBug_empty_segmentId()
{
    QFETCH(bool, historyEnabled);

    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(historyEnabled ? enabledSettings(dir) : disabledSettings(dir)));
    db.checkSchemaOnStartup();

    const SessionStoreResult r = db.discardCheckpoint(SegmentId{});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
}

// ── checkSchemaOnStartup (C4/R05-005/R02-003: was entirely missing from this matrix) ──

// Created: no DB file exists yet.
void ContractMatrixTest::test_checkSchemaOnStartup_created_for_fresh_db()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));

    QCOMPARE(db.checkSchemaOnStartup(), SchemaStatus::Created);
}

// Ready: a compatible DB file already exists.
void ContractMatrixTest::test_checkSchemaOnStartup_ready_for_existing_valid_db()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString settingsPath = enabledSettings(dir);
    {
        SqliteSessionStore seed{Settings(settingsPath)};
        QCOMPARE(seed.checkSchemaOnStartup(), SchemaStatus::Created);
    }

    SqliteSessionStore db{Settings(settingsPath)};
    QCOMPARE(db.checkSchemaOnStartup(), SchemaStatus::Ready);
}

// ── flushToDisc ──────────────────────────────────────────────────────────────

// Success: returns Success.
void ContractMatrixTest::test_flushToDisc_success()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    const SessionStoreResult r = db.flushToDisc();
    QCOMPARE(r.category, SessionStoreResult::Success);
    QVERIFY(r.ok());
    // CallerBug: N/A — flushToDisc takes no parameters
    // transient: PRAGMA or transaction failure, or ensureOpen() failure, both
    // return TransientError (C4/R07-003)
}

// Disabled: returns Disabled.
void ContractMatrixTest::test_flushToDisc_disabled_returns_Disabled()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const SessionStoreResult r = db.flushToDisc();
    QCOMPARE(r.category, SessionStoreResult::Disabled);
    QVERIFY(!r.ok());
}

// ── setLastCleanShutdownMarker ────────────────────────────────────────────────

// Success: returns Success.
void ContractMatrixTest::test_setLastCleanShutdownMarker_success()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    const SessionStoreResult r = db.setLastCleanShutdownMarker_dbg(QDateTime::currentDateTime());
    QCOMPARE(r.category, SessionStoreResult::Success);
    QVERIFY(r.ok());
    // CallerBug: N/A — no caller-supplied unique keys that can violate constraints
    // transient: commit/write failure, or ensureOpen() failure, both return
    // TransientError (C4/R07-003)
}

// Disabled: returns Disabled.
void ContractMatrixTest::test_setLastCleanShutdownMarker_disabled_returns_Disabled()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const SessionStoreResult r = db.setLastCleanShutdownMarker_dbg(QDateTime::currentDateTime());
    QCOMPARE(r.category, SessionStoreResult::Disabled);
    QVERIFY(!r.ok());
}

// ── markCleanShutdown (D6/R05-002/R15-004) ────────────────────────────────────

// Success: writes the marker (via the store's own now_provider_ seam, not a
// caller-supplied timestamp) then completes the FULL durability barrier.
// M1: markerWritten must be true -- the marker step itself succeeded.
void ContractMatrixTest::test_markCleanShutdown_success()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    const MarkCleanShutdownResult r = db.markCleanShutdown();
    QCOMPARE(r.result.category, SessionStoreResult::Success);
    QVERIFY(r.ok());
    QVERIFY(r.markerWritten);

    QVERIFY(db.ensureOpen_dbg());
    QSqlQuery q(db.rawDb_dbg());
    q.prepare("SELECT value FROM app_settings WHERE key = 'last_clean_shutdown'");
    QVERIFY(q.exec());
    QVERIFY(q.next());
    QVERIFY(!q.value(0).toString().isEmpty());
}

// Disabled: history off, so the marker write itself returns Disabled and the
// barrier is never attempted. M1: markerWritten must be false -- Disabled
// means the marker step did not itself commit (no-op), same as a failure
// for the purpose of "was the barrier reached".
void ContractMatrixTest::test_markCleanShutdown_disabled_returns_Disabled()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const MarkCleanShutdownResult r = db.markCleanShutdown();
    QCOMPARE(r.result.category, SessionStoreResult::Disabled);
    QVERIFY(!r.ok());
    QVERIFY(!r.markerWritten);
}

// ── recoverStartupCheckpoints ────────────────────────────────────────────────

// Disabled: returns status == Disabled with zero counts and no notification
// (B2: recoverStartupCheckpoints now uses the same SessionStoreResult::Category
// vocabulary as every other method, in the same struct as its domain data).
void ContractMatrixTest::test_recoverStartupCheckpoints_disabled_returns_Disabled()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const StartupRecoveryResult r = db.recoverStartupCheckpoints(QDateTime::currentDateTime());
    QCOMPARE(r.status, SessionStoreResult::Disabled);
    QCOMPARE(r.recovered_msec, static_cast<qint64>(0));
    QCOMPARE(r.finalized_count, 0);
    QCOMPARE(r.policy_dropped_count, 0);
    QCOMPARE(r.discard_dropped_count, 0);
    QVERIFY(!r.notify_user);
    // transient: marker read failure sets status to TransientError (tested in DatabaseTest)
    // CallerBug: N/A — recoverStartupCheckpoints takes only a timestamp (cannot be wrong)
}

// ── C4/R07-003: ensureOpen() failure classification consistency ──────────────
//
// Mirrors the read-only-at-construction pattern DatabaseTest already uses
// (see test_retention_cleanup_retries_after_failure): making the DB file
// read-only BEFORE construction leaves the connection closed, so ensureOpen()
// fails deterministically and repeatedly for as long as the file stays
// read-only -- the same root cause saveCheckpoint/discardCheckpoint/
// loadDurations/recoverStartupCheckpoints used to report FatalError while
// commitSession/updateDurationsById, flushToDisc, and setLastCleanShutdownMarker
// reported TransientError. All are now TransientError: ensureOpen() is explicitly
// documented to transparently reopen a lost connection, and this exact
// scenario (a temporarily read-only file) is proven end to end to recover via
// an ordinary retry, with no restart, by
// TimerTest::test_timer_retry_append_failure_then_success_preserves_segments
// -- which is incompatible with C4's own FatalError latch (a permanent,
// restart-only unavailable state).
void ContractMatrixTest::test_C4_ensureOpen_failure_classified_consistently_data()
{
    QTest::addColumn<QString>("method");
    QTest::newRow("commitSession")               << QString("commitSession");
    QTest::newRow("flushToDisc")                << QString("flushToDisc");
    QTest::newRow("setLastCleanShutdownMarker") << QString("setLastCleanShutdownMarker");
    QTest::newRow("loadDurations")              << QString("loadDurations");
    QTest::newRow("saveCheckpoint")             << QString("saveCheckpoint");
    QTest::newRow("discardCheckpoint")          << QString("discardCheckpoint");
    QTest::newRow("saveHistoryDays")            << QString("saveHistoryDays");
    // Named for what this row actually exercises (C4 finding 3): a closed
    // connection is caught by readLastCleanShutdownMarker()'s own
    // ensureOpen() call, not by the dedicated phase-2 guard just before the
    // recovery transaction -- that guard is unreachable here because
    // db_mutex_ is held for the whole method and phase 1 (which this row
    // exercises) already failed before phase 2 is ever reached.
    QTest::newRow("recoverStartupCheckpoints_via_marker_read")
        << QString("recoverStartupCheckpoints_via_marker_read");
}

void ContractMatrixTest::test_C4_ensureOpen_failure_classified_consistently()
{
    QFETCH(QString, method);

    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString settingsPath = enabledSettings(dir);
    {
        // Seed the DB file so it exists before the read-only store is built.
        SqliteSessionStore seed{Settings(settingsPath)};
        QVERIFY(seed.checkSchemaOnStartup() == SchemaStatus::Created);
    }

    QFile dbFile(db_path_);
    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::ReadUser));

    // Constructor detects the read-only file and leaves the connection closed.
    SqliteSessionStore db{Settings(settingsPath)};
    QVERIFY(!db.rawDb_dbg().isOpen());

    SessionStoreResult::Category category;
    if (method == "commitSession") {
        category = db.commitSession(Timeline(oneRow(), std::nullopt)).category;
    } else if (method == "flushToDisc") {
        category = db.flushToDisc().category;
    } else if (method == "setLastCleanShutdownMarker") {
        category = db.setLastCleanShutdownMarker_dbg(QDateTime::currentDateTime()).category;
    } else if (method == "loadDurations") {
        category = db.loadDurations().status;
    } else if (method == "saveCheckpoint") {
        // Fixed same-local-day interval (C4/R14-002): currentDateTimeUtc()
        // passes through DayValidity's local-day check before reaching
        // ensureOpen() -- run in the last minute before local midnight, it
        // would return CallerBug and fail for an unrelated reason.
        const QDateTime t0(QDate(2025, 6, 15), QTime(9, 0, 0), Qt::UTC);
        category = db.saveCheckpoint(DurationType::Activity, t0, t0.addSecs(60), SegmentId::mint()).category;
    } else if (method == "discardCheckpoint") {
        category = db.discardCheckpoint(SegmentId::mint()).category;
    } else if (method == "saveHistoryDays") {
        const QDate today = QDate::currentDate();
        const HistoryDayChange day = dayChangeFor(today, {}, {});
        category = db.saveHistoryDays({}, {day}).category;
    } else {
        // recoverStartupCheckpoints: a closed connection is detected earliest
        // by readLastCleanShutdownMarker()'s own ensureOpen() call, before the
        // dedicated ensureOpen() guard just before the recovery transaction
        // (C4/R07-003) is ever reached -- exercising the same root cause and
        // the same required TransientError outcome end to end.
        category = db.recoverStartupCheckpoints(QDateTime::currentDateTime()).status;
    }

    QVERIFY(dbFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser));
    QCOMPARE(category, SessionStoreResult::TransientError);
}

// ── C4/R05-005/R02-003: fake/concrete parity for identity + no-mutation facts ──

// The fake must reject a duplicate segment_id exactly like the concrete
// store, not silently record it as a successful spy call.
void ContractMatrixTest::test_C4_fake_commitSession_callerBug_duplicate_segmentId()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId sharedId = SegmentId::mint();
    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(9, 0, 0), Qt::UTC), QDateTime(today, QTime(9, 30, 0), Qt::UTC), sharedId));
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(today, QTime(10, 0, 0), Qt::UTC), QDateTime(today, QTime(10, 30, 0), Qt::UTC), sharedId));

    const SessionStoreResult r = fakeDb.commitSession(Timeline(rows, std::nullopt));
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QVERIFY(fakeDb.storedDurations.empty());
}

// The fake must classify a segment_id already finalized via commitSession as
// CallerBug when it is submitted to saveCheckpoint, exactly like
// SqliteSessionStore (test_saveCheckpoint_callerBug_finalized_segmentId
// above) -- not abort the process.
void ContractMatrixTest::test_C4_fake_saveCheckpoint_callerBug_finalized_segmentId()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const QDateTime t0(today, QTime(8, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(8, 30, 0), Qt::UTC);
    const SegmentId id = SegmentId::mint();

    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t0, t1, id));
    QVERIFY(fakeDb.commitSession(Timeline(rows, std::nullopt)).ok());

    const SessionStoreResult r = fakeDb.saveCheckpoint(DurationType::Activity,
        t0, t1.addSecs(60), id);
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QVERIFY(fakeDb.savedCheckpoints.empty());
}

// ── WP1b (H2): fake loadHistoryRows acceptance tests ──

// Acceptance: a checkpoint shows up as unfinalized
void ContractMatrixTest::test_H2_fake_checkpoint_projects_as_unfinalized()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();
    const QDateTime t0(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(10, 30, 0), Qt::UTC);

    QVERIFY(fakeDb.saveCheckpoint(DurationType::Activity, t0, t1, id).ok());

    const HistoryRowsResult result = fakeDb.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QCOMPARE(result.rows.size(), static_cast<size_t>(1));
    QCOMPARE(result.rows[0].isFinalized, QString("0"));
}

// Acceptance: after commit it appears once, finalized
void ContractMatrixTest::test_H2_fake_checkpoint_promoted_to_finalized_on_commit()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();
    const QDateTime t0(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(10, 30, 0), Qt::UTC);

    QVERIFY(fakeDb.saveCheckpoint(DurationType::Activity, t0, t1, id).ok());

    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t0, t1, id));
    QVERIFY(fakeDb.commitSession(Timeline(rows, std::nullopt)).ok());

    const HistoryRowsResult result = fakeDb.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QCOMPARE(result.rows.size(), static_cast<size_t>(1));
    QCOMPARE(result.rows[0].isFinalized, QString("1"));
}

// Acceptance: after successful discard it is gone
void ContractMatrixTest::test_H2_fake_checkpoint_removed_by_successful_discard()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();
    const QDateTime t0(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(10, 30, 0), Qt::UTC);

    QVERIFY(fakeDb.saveCheckpoint(DurationType::Activity, t0, t1, id).ok());

    QVERIFY(fakeDb.discardCheckpoint(id).ok());

    const HistoryRowsResult result = fakeDb.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QVERIFY(result.rows.empty());
}

// Acceptance: after failing discard it is still there
void ContractMatrixTest::test_H2_fake_checkpoint_retained_on_failing_discard()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();
    const QDateTime t0(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(10, 30, 0), Qt::UTC);

    QVERIFY(fakeDb.saveCheckpoint(DurationType::Activity, t0, t1, id).ok());

    fakeDb.discardCheckpointResult = SessionStoreResult::disabled();
    const SessionStoreResult discardResult = fakeDb.discardCheckpoint(id);
    QCOMPARE(discardResult.category, SessionStoreResult::Disabled);

    // Checkpoint should still be there
    const HistoryRowsResult result = fakeDb.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QCOMPARE(result.rows.size(), static_cast<size_t>(1));
    QCOMPARE(result.rows[0].isFinalized, QString("0"));
}

// Acceptance: preset unfinalized row removed by successful discard
void ContractMatrixTest::test_H2_fake_preset_unfinalized_row_removed_by_discard()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();

    // Add a preset unfinalized row
    fakeDb.historyUnfinalizedRows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(9, 0, 0), Qt::UTC), QDateTime(today, QTime(9, 30, 0), Qt::UTC), id));

    QVERIFY(fakeDb.discardCheckpoint(id).ok());

    const HistoryRowsResult result = fakeDb.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QVERIFY(result.rows.empty());
}

// Acceptance: duplicate unfinalized preset id dropped
void ContractMatrixTest::test_H2_fake_duplicate_unfinalized_preset_id_dropped()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId id = SegmentId::mint();

    // Add two preset unfinalized rows with the same id
    fakeDb.historyUnfinalizedRows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(9, 0, 0), Qt::UTC), QDateTime(today, QTime(9, 30, 0), Qt::UTC), id));
    fakeDb.historyUnfinalizedRows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(9, 0, 0), Qt::UTC), QDateTime(today, QTime(10, 0, 0), Qt::UTC), id));

    const HistoryRowsResult result = fakeDb.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    // Should only return one row (the first one, due to dedup by id)
    QCOMPARE(result.rows.size(), static_cast<size_t>(1));
}

// Acceptance: output is sorted and parsed times are local
void ContractMatrixTest::test_H2_fake_loadHistoryRows_output_sorted_and_parsed_local()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId id1 = SegmentId::mint();
    const SegmentId id2 = SegmentId::mint();

    // Add checkpoint rows out of order
    fakeDb.savedCheckpoints.push_back({DurationType::Activity,
        QDateTime(today, QTime(14, 0, 0), Qt::UTC),
        QDateTime(today, QTime(14, 30, 0), Qt::UTC),
        id1});
    fakeDb.savedCheckpoints.push_back({DurationType::Pause,
        QDateTime(today, QTime(10, 0, 0), Qt::UTC),
        QDateTime(today, QTime(10, 30, 0), Qt::UTC),
        id2});

    const HistoryRowsResult result = fakeDb.loadHistoryRows();
    QCOMPARE(result.status, SessionStoreResult::Success);
    QCOMPARE(result.rows.size(), static_cast<size_t>(2));

    // Verify sorted by startUtc
    QVERIFY(result.rows[0].startUtc < result.rows[1].startUtc);

    // Verify parsed times are in local time (not UTC)
    QVERIFY(result.rows[0].parsed.has_value());
    QVERIFY(result.rows[1].parsed.has_value());
    QCOMPARE(result.rows[0].parsed->startTime.timeSpec(), Qt::LocalTime);
    QCOMPARE(result.rows[1].parsed->startTime.timeSpec(), Qt::LocalTime);
}

