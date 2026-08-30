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
 * Most tests use SqliteSessionStore directly (not the fake) so the behavior being
 * verified is the real implementation.  History-disabled tests use
 * history_days_to_keep == 0.
 *
 * C4 (R05-003/R05-005/R05-006/R02-003/R07-003) adds: a discardCheckpoint row and a
 * checkSchemaOnStartup row (both previously missing from this matrix despite its
 * "every public method" header), executable CallerBug cells for commitSession/
 * replaceAll's duplicate-segment_id guard, a same-root-cause consistency check for
 * ensureOpen() failure classification across the methods that used to disagree
 * (R07-003), and a dedicated section that runs the identity/no-mutation cells that
 * matter for caller behavior against FakeSessionStore too, so fake/concrete drift
 * (R05-005/R02-003) cannot silently regress.
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
    // CallerBug: duplicate segment_id within the payload (C4/R05-003). Data
    // rows cover history enabled AND disabled -- CallerBug wins over Disabled
    // (precedence rule in sessionstore.h), not just checked first by luck.
    void test_commitSession_callerBug_duplicate_segmentId_data();
    void test_commitSession_callerBug_duplicate_segmentId();
    // transient/fatal: ensureOpen failure now consistently returns TransientError
    // (C4/R07-003, see test_C4_ensureOpen_failure_classified_consistently).

    // ── replaceAll ──────────────────────────────────────────────────
    void test_replaceAll_success();
    void test_replaceAll_disabled_returns_Disabled();
    // CallerBug: duplicate segment_id across the COMBINED history+session
    // payload (C4/R05-003) -- uniqueness is checked across both buckets, not
    // each one separately. Data rows cover history enabled AND disabled --
    // CallerBug wins over Disabled (precedence rule in sessionstore.h).
    void test_replaceAll_callerBug_duplicate_segmentId_across_history_and_session_data();
    void test_replaceAll_callerBug_duplicate_segmentId_across_history_and_session();
    // transient/fatal: covered implicitly by ensureOpen and transaction failures

    // ── loadDurations ────────────────────────────────────────────────
    void test_loadDurations_success_returns_rows();
    void test_loadDurations_disabled_returns_Disabled_and_empty();
    // transient: query/transaction failure, or ensureOpen() failure (C4/R07-003),
    // sets TransientError (see test_C4_ensureOpen_failure_classified_consistently)
    // fatal: not distinctly returned; see C4/R07-003
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
    // CallerBug wins over Disabled (C4/R05-003): data rows cover history
    // enabled AND disabled so the ordering is actually asserted.
    void test_saveCheckpoint_callerBug_empty_segmentId_data();
    void test_saveCheckpoint_callerBug_empty_segmentId();
    void test_saveCheckpoint_callerBug_finalized_segmentId();
    // C4 fix round 2 (MEDIUM-6): an invalid interval must win over Disabled
    // too, matching commitSession/replaceAll -- saveCheckpoint's DayValidity
    // guard was checked AFTER the Disabled guard, so a Disabled-history
    // caller bug was misreported as Disabled instead of CallerBug.
    void test_saveCheckpoint_callerBug_invalid_interval_data();
    void test_saveCheckpoint_callerBug_invalid_interval();
    // transient: ensureOpen() failure (C4/R07-003) or other SQL failure
    // fatal: not distinctly returned; see C4/R07-003

    // ── discardCheckpoint (C4/R05-005/R02-003: was entirely missing) ──
    void test_discardCheckpoint_success_removes_row();
    void test_discardCheckpoint_disabled_returns_Disabled();
    // CallerBug wins over Disabled (C4/R05-003): data rows cover history
    // enabled AND disabled so the ordering is actually asserted.
    void test_discardCheckpoint_callerBug_empty_segmentId_data();
    void test_discardCheckpoint_callerBug_empty_segmentId();
    // transient/fatal: ensureOpen() failure returns TransientError (already
    // covered by the concrete implementation's ensureOpen()-consistency, see below)

    // ── checkSchemaOnStartup (C4/R05-005/R02-003: was entirely missing) ──
    void test_checkSchemaOnStartup_created_for_fresh_db();
    void test_checkSchemaOnStartup_ready_for_existing_valid_db();
    // Outdated/Inaccessible: extensively covered by DatabaseTest
    // (incompatible-schema and unwritable-file cases); not duplicated here.

    // ── flushToDisc ─────────────────────────────────────────────────
    void test_flushToDisc_success();
    void test_flushToDisc_disabled_returns_Disabled();
    // transient: PRAGMA/transaction failure, or ensureOpen() failure (C4/R07-003),
    // returns TransientError (see test_C4_ensureOpen_failure_classified_consistently)
    // fatal: not distinctly returned; see C4/R07-003
    // CallerBug: N/A — flushToDisc takes no parameters

    // ── setLastCleanShutdownMarker ───────────────────────────────────
    void test_setLastCleanShutdownMarker_success();
    void test_setLastCleanShutdownMarker_disabled_returns_Disabled();
    // transient: commit/write failure, or ensureOpen() failure (C4/R07-003),
    // returns TransientError (see test_C4_ensureOpen_failure_classified_consistently)
    // fatal: not distinctly returned; see C4/R07-003
    // CallerBug: N/A — no keys that can violate constraints

    // ── recoverStartupCheckpoints ────────────────────────────────────
    // success (no orphans): already covered by DatabaseTest
    // disabled: returns status == Disabled with zero counts (B2)
    void test_recoverStartupCheckpoints_disabled_returns_Disabled();
    // transient/fatal/CallerBug: marker read failure sets status to
    // TransientError (covered by DatabaseTest, and by this matrix's
    // "recoverStartupCheckpoints_via_marker_read" row below, which is what
    // that row actually exercises). The SEPARATE dedicated ensureOpen()
    // guard just before the recovery transaction (sqlitesessionstore.cpp,
    // phase 2) is NOT exercised by any test: db_mutex_ is held for the whole
    // method and phase 1 already fails first in every scenario this matrix
    // can construct, so that guard is unreachable here. It is classified as
    // TransientError by inspection (C4/R07-003), matching every other
    // ensureOpen() site, not by a passing test.

    // ── C4/R07-003: ensureOpen() failure classification consistency ──
    // The SAME root cause (connection cannot be (re)opened, e.g. a read-only
    // DB file) used to be reported as FatalError by some write boundaries
    // (replaceAll/saveCheckpoint/discardCheckpoint/loadDurations/
    // recoverStartupCheckpoints) and TransientError by others (commitSession/
    // updateDurationsById, flushToDisc, setLastCleanShutdownMarker). All seven now agree:
    // TransientError -- ensureOpen() is explicitly designed to transparently
    // reopen a lost connection, and this exact failure mode (e.g. a
    // temporarily read-only file) is recovered by an ordinary retry once the
    // underlying cause is fixed, with no restart required (see
    // TimerTest::test_timer_retry_append_failure_then_success_preserves_segments,
    // which exercises precisely this scenario end to end through TimeTracker).
    void test_C4_ensureOpen_failure_classified_consistently_data();
    void test_C4_ensureOpen_failure_classified_consistently();

    // ── C4/R05-005/R02-003: fake/concrete parity for the identity and
    // no-mutation contract facts that matter for caller behavior. The fake
    // deliberately does not duplicate SQL/day-validity/overlap policy (see
    // sessionstore.h), so only the identity (empty/duplicate id) and
    // Disabled-is-a-no-op facts are checked here against FakeSessionStore. ──
    void test_C4_fake_commitSession_callerBug_duplicate_segmentId();
    void test_C4_fake_replaceAll_callerBug_duplicate_segmentId();
    void test_C4_fake_replaceAll_disabled_does_not_mutate_storedDurations();
    // C4 fix round 3 (F7): the fake must classify a segment_id that already
    // belongs to a finalized (commitSession'd) row as CallerBug, exactly
    // like SqliteSessionStore::saveCheckpoint
    // (test_saveCheckpoint_callerBug_finalized_segmentId above), instead of
    // aborting the process via Q_ASSERT_X.
    void test_C4_fake_saveCheckpoint_callerBug_finalized_segmentId();
};

#endif // TEST_CONTRACT_MATRIX_H
