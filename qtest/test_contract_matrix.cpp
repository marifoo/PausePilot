/**
 * ContractMatrixTest — per-method contract matrix for every public SessionStore method.
 *
 * Matrix columns: success | disabled-history | transient | fatal | caller-bug.
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

using TestCommon::createSettingsFile;

// C4 (R02-003/"done when"): this file has one section below per
// SessionStore::Operation. If SessionStore grows a 12th virtual, add its
// Operation enumerator AND a matching section here, then update this count --
// otherwise this fails to compile as a reminder instead of silently shipping
// an unverified operation.
static_assert(static_cast<int>(Operation::Count) == 10,
    "SessionStore::Operation changed size -- add a matrix section to "
    "test_contract_matrix.cpp for the new/removed operation, then update "
    "this static_assert's count.");

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
    // transient/fatal: cannot be injected without closing the connection externally
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
    // CallerBug: covered by C3's dayvalidity.h payload guard (see test_database.cpp)
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

void ContractMatrixTest::test_replaceAll_callerBug_duplicate_segmentId_across_history_and_session_data()
{
    QTest::addColumn<bool>("historyEnabled");
    QTest::newRow("history-enabled")  << true;
    QTest::newRow("history-disabled") << false;
}

// CallerBug: a history row and a session row share a segment_id (C4/R05-003).
// Uniqueness is checked across the COMBINED payload, not each bucket alone.
// Rejected before any mutation -- the pre-existing row is left untouched. Run
// with history both enabled and disabled: CallerBug must win over Disabled
// (precedence rule in sessionstore.h), not just happen to be checked first
// when history is on.
void ContractMatrixTest::test_replaceAll_callerBug_duplicate_segmentId_across_history_and_session()
{
    QFETCH(bool, historyEnabled);

    resetDatabaseFile();
    QTemporaryDir dir; QVERIFY(dir.isValid());
    SqliteSessionStore db(Settings(historyEnabled ? enabledSettings(dir) : disabledSettings(dir)));
    db.checkSchemaOnStartup();

    if (historyEnabled) {
        QVERIFY(db.commitSession(Timeline(oneRow(), std::nullopt)).ok());
        QCOMPARE(db.loadDurations().size(), static_cast<size_t>(1));
    }

    const QDate today(2025, 6, 15);
    const SegmentId sharedId = SegmentId::mint();
    std::deque<TimeDuration> history;
    history.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(8, 0, 0), Qt::UTC), QDateTime(today, QTime(8, 30, 0), Qt::UTC), sharedId));
    std::deque<TimeDuration> session;
    session.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(today, QTime(9, 0, 0), Qt::UTC), QDateTime(today, QTime(9, 30, 0), Qt::UTC), sharedId));

    const SessionStoreResult r = db.replaceAll(Timeline(history, std::nullopt), Timeline(session, std::nullopt));
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    if (historyEnabled) {
        // The pre-existing row (from the earlier commitSession) is untouched.
        QCOMPARE(db.loadDurations().size(), static_cast<size_t>(1));
    }
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
    // transient/fatal: ensureOpen() or transaction failure sets TransientError (C4/R07-003)
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
        QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc().addSecs(60),
        SegmentId{});
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    // transient/fatal: ensureOpen() failure returns TransientError (C4/R07-003)
}

// C4 fix round 2 (MEDIUM-6): CallerBug wins over Disabled for an invalid
// interval too, matching commitSession/replaceAll's precedence -- run with
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
        QDateTime::currentDateTimeUtc(), QDateTime::currentDateTimeUtc().addSecs(-60),
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
    // return TransientError; fatal: not distinctly returned (C4/R07-003)
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
    // TransientError; fatal: not distinctly returned (C4/R07-003)
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
    QVERIFY(!r.notify_user);
    // transient/fatal: marker read failure sets status to TransientError (tested in DatabaseTest)
    // CallerBug: N/A — recoverStartupCheckpoints takes only a timestamp (cannot be wrong)
}

// ── C4/R07-003: ensureOpen() failure classification consistency ──────────────
//
// Mirrors the read-only-at-construction pattern DatabaseTest already uses
// (see test_retention_cleanup_retries_after_failure): making the DB file
// read-only BEFORE construction leaves the connection closed, so ensureOpen()
// fails deterministically and repeatedly for as long as the file stays
// read-only -- the same root cause replaceAll/saveCheckpoint/discardCheckpoint/
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
    QTest::newRow("replaceAll")                 << QString("replaceAll");
    QTest::newRow("loadDurations")              << QString("loadDurations");
    QTest::newRow("saveCheckpoint")             << QString("saveCheckpoint");
    QTest::newRow("discardCheckpoint")          << QString("discardCheckpoint");
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
    } else if (method == "replaceAll") {
        category = db.replaceAll(Timeline(oneRow(), std::nullopt), Timeline({}, std::nullopt)).category;
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

void ContractMatrixTest::test_C4_fake_replaceAll_callerBug_duplicate_segmentId()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    const SegmentId sharedId = SegmentId::mint();
    std::deque<TimeDuration> history;
    history.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(8, 0, 0), Qt::UTC), QDateTime(today, QTime(8, 30, 0), Qt::UTC), sharedId));
    std::deque<TimeDuration> session;
    session.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(today, QTime(9, 0, 0), Qt::UTC), QDateTime(today, QTime(9, 30, 0), Qt::UTC), sharedId));

    const SessionStoreResult r = fakeDb.replaceAll(Timeline(history, std::nullopt), Timeline(session, std::nullopt));
    QCOMPARE(r.category, SessionStoreResult::CallerBug);
    QVERIFY(fakeDb.storedDurations.empty());
}

// Disabled must be a true no-op in the fake, exactly like the interface and
// the concrete store: pre-existing in-memory state must survive untouched
// (R05-005/R02-003 -- the fake used to clear/replace it on Disabled).
void ContractMatrixTest::test_C4_fake_replaceAll_disabled_does_not_mutate_storedDurations()
{
    FakeSessionStore fakeDb;
    const QDate today = QDate::currentDate();
    TimeDuration preExisting = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(today, QTime(7, 0, 0), Qt::UTC), QDateTime(today, QTime(7, 30, 0), Qt::UTC));
    fakeDb.storedDurations.push_back(preExisting);

    fakeDb.replaceDurationsResult = SessionStoreResult::disabled();
    const SessionStoreResult r = fakeDb.replaceAll(Timeline(oneRow(), std::nullopt), Timeline({}, std::nullopt));

    QCOMPARE(r.category, SessionStoreResult::Disabled);
    QCOMPARE(fakeDb.storedDurations.size(), static_cast<size_t>(1));
    QCOMPARE(fakeDb.storedDurations[0].segment_id, preExisting.segment_id);
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
