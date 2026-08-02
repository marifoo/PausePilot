/**
 * ContractMatrixTest — per-method contract matrix for every public SessionStore method.
 *
 * Matrix columns: success | disabled-history | transient | fatal | caller-bug.
 * Rows: one per public SessionStore method.
 *
 * Non-applicable cells are documented with comments in the .h file rather than
 * silently omitted.  Every test uses the real SqliteSessionStore, not the fake.
 */

#include "test_contract_matrix.h"
#include "../sqlitesessionstore.h"
#include "../timeline.h"
#include "../types.h"
#include "testcommon.h"
#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>

using TestCommon::createSettingsFile;

void ContractMatrixTest::initTestCase()
{
    db_path_ = QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite");
    if (QFile::exists(db_path_)) {
        db_backup_path_ = db_path_ + ".bak_matrix_test";
        QFile::remove(db_backup_path_);
        QVERIFY(QFile::rename(db_path_, db_backup_path_));
    }
}

void ContractMatrixTest::cleanupTestCase()
{
    if (!db_path_.isEmpty())
        QFile::remove(db_path_);
    if (!db_backup_path_.isEmpty()) {
        QFile::remove(db_path_);
        QVERIFY(QFile::rename(db_backup_path_, db_path_));
    }
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
std::deque<TimeDuration> oneRow() {
    const QDate today = QDate::currentDate();
    const QDateTime t0(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime t1(today, QTime(10, 30, 0), Qt::UTC);
    std::deque<TimeDuration> rows;
    rows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, t0, t1));
    return rows;
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
    // CallerBug: N/A — commitSession validates inputs at the SQL constraint level
    // transient/fatal: cannot be injected without closing the connection externally
}

// ── replaceAll ───────────────────────────────────────────────────────────────

// Success: returns Success, pre-existing rows are cleared, new rows written.
void ContractMatrixTest::test_replaceAll_success()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    // Seed one row, then replaceAll with different rows.
    QVERIFY(db.commitSession(Timeline(oneRow(), std::nullopt)).ok());
    QCOMPARE(db.loadDurations().size(), static_cast<size_t>(1));

    std::deque<TimeDuration> newRows;
    const QDate today = QDate::currentDate();
    newRows.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, QDateTime(today, QTime(11,0,0), Qt::UTC),
                                                    QDateTime(today, QTime(11,30,0), Qt::UTC)));
    newRows.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, QDateTime(today, QTime(12,0,0), Qt::UTC),
                                                    QDateTime(today, QTime(12,30,0), Qt::UTC)));

    const SessionStoreResult r = db.replaceAll(Timeline(newRows, std::nullopt), Timeline({}, std::nullopt));
    QCOMPARE(r.category, SessionStoreResult::Success);
    QVERIFY(r.ok());
    QCOMPARE(db.loadDurations().size(), static_cast<size_t>(2));
    // CallerBug: N/A — replaceAll uses no caller-supplied unique keys
    // transient/fatal: tested implicitly by ensureOpen/transaction failure paths
}

// Disabled: returns Disabled, nothing is written.
void ContractMatrixTest::test_replaceAll_disabled_returns_Disabled()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const SessionStoreResult r = db.replaceAll(Timeline(oneRow(), std::nullopt), Timeline({}, std::nullopt));
    QCOMPARE(r.category, SessionStoreResult::Disabled);
    QVERIFY(!r.ok());
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
    // transient/fatal: ensureOpen failure sets FatalError; transaction failure sets TransientError
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
    QCOMPARE(r.skipped, 0);
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
    // transient/fatal: both also map to Unknown (ensureOpen failure path)
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
        QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc().addSecs(60),
        SegmentId::mint());
    QCOMPARE(r.category, SessionStoreResult::Disabled);
    QVERIFY(!r.ok());
}

// CallerBug: empty segment_id → CallerBug, checked before the disabled guard.
void ContractMatrixTest::test_saveCheckpoint_callerBug_empty_segmentId()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    // Even with history enabled, empty segment_id is a CallerBug.
    SqliteSessionStore db(Settings(enabledSettings(dir)));
    db.checkSchemaOnStartup();

    const SessionStoreResult r = db.saveCheckpoint(DurationType::Activity,
        QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc().addSecs(60),
        SegmentId{});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    // transient: DB open failure returns FatalError
    // fatal: connection-level failure returns FatalError
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
    // transient: PRAGMA or transaction failure → TransientError
    // fatal: not distinctly distinguished from transient; both return TransientError
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

    const SessionStoreResult r = db.setLastCleanShutdownMarker(QDateTime::currentDateTime());
    QCOMPARE(r.category, SessionStoreResult::Success);
    QVERIFY(r.ok());
    // CallerBug: N/A — no caller-supplied unique keys that can violate constraints
    // transient: DB open or commit failure → TransientError
    // fatal: not distinctly returned from transient
}

// Disabled: returns Disabled.
void ContractMatrixTest::test_setLastCleanShutdownMarker_disabled_returns_Disabled()
{
    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(disabledSettings(dir)));

    const SessionStoreResult r = db.setLastCleanShutdownMarker(QDateTime::currentDateTime());
    QCOMPARE(r.category, SessionStoreResult::Disabled);
    QVERIFY(!r.ok());
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
    QVERIFY(!r.notify_user);
    // transient/fatal: marker read failure sets status to TransientError (tested in DatabaseTest)
    // CallerBug: N/A — recoverStartupCheckpoints takes only a timestamp (cannot be wrong)
}
