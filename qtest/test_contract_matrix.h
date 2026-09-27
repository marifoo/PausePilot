#ifndef TEST_CONTRACT_MATRIX_H
#define TEST_CONTRACT_MATRIX_H

#include <QObject>
#include "testcommon.h"

/**
 * ContractMatrixTest — per-method contract matrix for every public SessionStore method.
 *
 * For each method the matrix covers: success, disabled-history, transient failure,
 * caller-bug, and conflict where applicable.  Non-applicable cells are documented
 * with a comment explaining why (rather than silently omitted).
 *
 * Most tests use SqliteSessionStore directly (not the fake) so the behavior being
 * verified is the real implementation.  History-disabled tests use
 * history_days_to_keep == 0.
 *
 * C4 (R05-003/R05-005/R05-006/R02-003/R07-003) adds: a discardCheckpoint row and a
 * checkSchemaOnStartup row (both previously missing from this matrix despite its
 * "every public method" header), executable CallerBug cells for commitSession's
 * duplicate-segment_id guard, a same-root-cause consistency check for
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
    QTemporaryDir data_dir_;

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

    // ── loadDurations ────────────────────────────────────────────────
    void test_loadDurations_success_returns_rows();
    void test_loadDurations_disabled_returns_Disabled_and_empty();
    // transient: query/transaction failure, or ensureOpen() failure (C4/R07-003),
    // sets TransientError (see test_C4_ensureOpen_failure_classified_consistently)
    // fatal: not distinctly returned; see C4/R07-003
    // CallerBug: N/A — loadDurations takes no caller-supplied parameters

    // ── loadHistoryRows (H5/WP1) ─────────────────────────────────────
    void test_loadHistoryRows_success_returns_all_rows_raw();
    void test_loadHistoryRows_disabled_returns_Disabled_and_empty();
    void test_loadHistoryRows_transient_on_closed_connection();
    // CallerBug/fatal: N/A -- no caller-supplied parameters, no write
    void test_C4_fake_loadHistoryRows_projects_storedDurations_and_unfinalized();

    // ── saveHistoryDays (H5/WP2) ──────────────────────────────────────
    void test_saveHistoryDays_success_updates_row();
    void test_saveHistoryDays_disabled_returns_Disabled();
    void test_saveHistoryDays_callerBug_empty_days();
    void test_saveHistoryDays_callerBug_precedence_over_disabled();
    // transient: ensureOpen() failure covered by the "saveHistoryDays" row
    // added to test_C4_ensureOpen_failure_classified_consistently_data() below.
    void test_C4_fake_saveHistoryDays_logs_and_records_args();
    void test_C4_fake_saveHistoryDays_applies_deletes_and_upserts_on_success();
    void test_C4_fake_saveHistoryDays_callerBug_duplicate_id();
    void test_C4_fake_saveHistoryDays_does_not_enforce_span_editability();

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
    // too, matching commitSession -- saveCheckpoint's DayValidity
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

    // ── markCleanShutdown (D6/R05-002/R15-004) ────────────────────────
    // The single durable shutdown operation: marker write, then -- only if
    // that succeeded -- the FULL durability barrier.
    void test_markCleanShutdown_success();
    void test_markCleanShutdown_disabled_returns_Disabled();
    // transient: covered by ShutdownCoordinatorTest (injected marker/flush
    // failure via FakeSessionStore) and DatabaseTest's real-store marker
    // reopen test; not duplicated here.
    // fatal: not distinctly returned; see C4/R07-003
    // CallerBug: N/A — markCleanShutdown takes no caller-supplied parameters

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
    // (saveCheckpoint/discardCheckpoint/loadDurations/
    // recoverStartupCheckpoints) and TransientError by others (commitSession/
    // updateDurationsById, flushToDisc, setLastCleanShutdownMarker). All six now agree:
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
    // C4 fix round 3 (F7): the fake must classify a segment_id that already
    // belongs to a finalized (commitSession'd) row as CallerBug, exactly
    // like SqliteSessionStore::saveCheckpoint
    // (test_saveCheckpoint_callerBug_finalized_segmentId above), instead of
    // aborting the process via Q_ASSERT_X.
    void test_C4_fake_saveCheckpoint_callerBug_finalized_segmentId();

    // WP1b (H2): fake loadHistoryRows acceptance tests
    void test_H2_fake_checkpoint_projects_as_unfinalized();
    void test_H2_fake_checkpoint_promoted_to_finalized_on_commit();
    void test_H2_fake_checkpoint_removed_by_successful_discard();
    void test_H2_fake_checkpoint_retained_on_failing_discard();
    void test_H2_fake_preset_unfinalized_row_removed_by_discard();
    void test_H2_fake_duplicate_unfinalized_preset_id_dropped();
    void test_H2_fake_loadHistoryRows_output_sorted_and_parsed_local();
};

#endif // TEST_CONTRACT_MATRIX_H
