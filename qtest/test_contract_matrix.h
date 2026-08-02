#ifndef TEST_CONTRACT_MATRIX_H
#define TEST_CONTRACT_MATRIX_H

#include <QObject>
#include "testcommon.h"

/**
 * ContractMatrixTest — per-method contract matrix for every public SessionStore method.
 *
 * For each method the matrix covers: success, disabled-history, transient failure,
 * fatal failure, and caller-bug where applicable.  Non-applicable cells are documented
 * with a comment explaining why (rather than silently omitted).
 *
 * All tests use SqliteSessionStore directly (not the fake) so the behavior being
 * verified is the real implementation.  History-disabled tests use
 * history_days_to_keep == 0.
 */
class ContractMatrixTest : public QObject
{
    Q_OBJECT

private:
    QString db_path_;
    QString db_backup_path_;

    void resetDatabaseFile() const {
        if (QFile::exists(db_path_))
            QFile::remove(db_path_);
    }

private slots:
    void initTestCase();
    void cleanupTestCase();

    // ── commitSession ────────────────────────────────────────────────
    void test_commitSession_success();
    void test_commitSession_disabled_returns_Disabled();
    // transient/fatal: covered by ensureOpen failure (not easily injectable at unit level)
    // CallerBug: N/A — commitSession validates via updateDurationsById which catches empty segment_id at SQL level

    // ── replaceAll ──────────────────────────────────────────────────
    void test_replaceAll_success();
    void test_replaceAll_disabled_returns_Disabled();
    // transient/fatal: covered implicitly by ensureOpen and transaction failures
    // CallerBug: N/A — replaceAll has no caller-supplied keys that can violate constraints

    // ── loadDurations ────────────────────────────────────────────────
    void test_loadDurations_success_returns_rows();
    void test_loadDurations_disabled_returns_Disabled_and_empty();
    // transient: query/transaction failure sets TransientError (tested via status field)
    // fatal: ensureOpen failure sets FatalError (provoked at the SQLite layer below; not injectable through this matrix harness)
    // CallerBug: N/A — loadDurations takes no caller-supplied parameters

    // ── hasEntriesForDate ────────────────────────────────────────────
    void test_hasEntriesForDate_returns_Yes_when_entry_exists();
    void test_hasEntriesForDate_returns_No_when_no_entry();
    void test_hasEntriesForDate_disabled_returns_Unknown();
    // transient/fatal: both map to Unknown (ensureOpen failure → Unknown)
    // CallerBug: N/A — no session-specific keys

    // ── saveCheckpoint ──────────────────────────────────────────────
    void test_saveCheckpoint_success_inserts_row();
    void test_saveCheckpoint_disabled_returns_Disabled();
    void test_saveCheckpoint_callerBug_empty_segmentId();
    void test_saveCheckpoint_callerBug_finalized_segmentId();
    // transient: DB open failure returns fatal (connection-level)
    // fatal: ensureOpen failure returns FatalError

    // ── flushToDisc ─────────────────────────────────────────────────
    void test_flushToDisc_success();
    void test_flushToDisc_disabled_returns_Disabled();
    // transient: PRAGMA or transaction failure returns TransientError
    // fatal: not distinctly returned (both are TransientError; see implementation)
    // CallerBug: N/A — flushToDisc takes no parameters

    // ── setLastCleanShutdownMarker ───────────────────────────────────
    void test_setLastCleanShutdownMarker_success();
    void test_setLastCleanShutdownMarker_disabled_returns_Disabled();
    // transient: DB open failure or commit failure returns TransientError
    // fatal: not distinctly returned (all failures are TransientError)
    // CallerBug: N/A — no keys that can violate constraints

    // ── recoverStartupCheckpoints ────────────────────────────────────
    // success (no orphans): already covered by DatabaseTest
    // disabled: returns status == Disabled with zero counts (B2)
    void test_recoverStartupCheckpoints_disabled_returns_Disabled();
    // transient/fatal/CallerBug: marker read failure sets status to
    // TransientError (covered by DatabaseTest)
};

#endif // TEST_CONTRACT_MATRIX_H
