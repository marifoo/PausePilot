#ifndef TEST_INTEGRATION_H
#define TEST_INTEGRATION_H

#include <QObject>
#include "testcommon.h"

class IntegrationTest : public QObject
{
    Q_OBJECT

private:
    QString db_path_;
    QTemporaryDir data_dir_;
    
    void resetDatabaseFile() const
    {
        if (QFile::exists(db_path_)) {
            QFile::remove(db_path_);
        }
    }

private slots:
    void initTestCase();
    void cleanupTestCase();
    
    void test_integration_checkpoint_recovery_on_restart();
    void test_integration_orphan_reconciliation_is_idempotent();
    void test_integration_orphan_reconciliation_drops_stale_and_too_short();
    void test_integration_orphan_reconciliation_marker_present_is_silent();
    void test_integration_orphan_reconciliation_marker_absent_shows_notification();
    void test_integration_memory_db_consistency();
    void test_integration_retention_cleanup_preserves_current();
    void test_integration_duplicate_prevention();
    void test_integration_empty_database_operations();
    void test_integration_backpause_db_update();
    // C12b: LockStateWatcher's captured lock instant, wired through its real
    // signal into TimeTracker::useTimerViaLockEvent, is the timestamp that
    // ends the Activity segment -- not a now-relative fallback.
    void test_C12b_watcher_propagates_lockTime_to_backpause_anchor();

    // Shutdown sequence end-to-end: stop → flush → clean-shutdown marker.
    void test_F_shutdown_sequence_stop_flush_marker();

    // DayBoundaryWatcher regression: cross-midnight behavior
    void test_5_0a_normal_same_day_no_discard();
    void test_5_0a_cross_midnight_ongoing_discarded_completed_preserved();
    void test_5_0a_discard_is_idempotent();

    // DayBoundaryWatcher: scheduled vs watchdog stop signals
    void test_Y_engine_scheduled_stop_emits_MidnightScheduled();
    void test_Z_engine_watchdog_emits_MidnightWatchdog();
    void test_AA_no_duplicate_stop_signal();
    void test_AB_scheduleMidnightStop_is_gone();

    // recoverStartupCheckpoints: overlap rejection and count reporting
    void test_recoverStartupCheckpoints_overlap_rejected_not_counted();
    void test_recoverStartupCheckpoints_reports_finalized_and_dropped_counts();
    void test_timer_startup_recovery_uses_store_recovered_msec();

    // Step 19 (C9 timezone): parameterized timezone tests for hasEntriesForDate.
    void test_hasEntriesForDate_timezone_data();
    void test_hasEntriesForDate_timezone();
    void test_bootCredit_suppressed_by_legacy_cross_midnight_row_on_dst_day();
    void test_midnight_boundary_timezone_data();
    void test_midnight_boundary_timezone();

    // Item A: toggle ongoing type then crash — refreshed end_utc survives recovery.
    void test_A_toggle_ongoing_to_pause_crash_recovery_uses_refreshed_end_utc();

    // A3 (R11-001): Exit Anyway (drain attempted and failed, no further
    // retry) leaves an existing durable checkpoint untouched. A real SQLite
    // reopen proves ordinary startup recovery finalizes only that durable
    // portion, without duplication or resurrecting the in-memory-only tail.
    void test_A3_exit_anyway_leaves_checkpoint_untouched_for_recovery();

    // A4 (R05-001): a checkpoint-discard failure's intent is durable across
    // a real close/reopen -- the rejected segment can never be resurrected
    // as credited history.
    void test_A4_watchdog_discard_failure_survives_reopen_never_credited();

    // M4: a real SqliteSessionStore + TimeTracker over a seeded cross-day
    // orphan (no discard intent) reports the non-discard policy-drop count
    // at the tracker boundary mainwin.cpp reads, while the old recovered-time
    // notification channel stays closed.
    void test_M4_tracker_reports_policy_dropped_count_for_cross_day_orphan();

    // D2 (R11-003/MEDIUM-8): a real SQLite store proves a backward-clock
    // Stop writes no row reflecting the reversed interval. HIGH-A (round-2
    // review): the already-checkpointed prefix must survive intact, not be
    // deleted -- proved by a real close/reopen where startup recovery
    // finalizes exactly that prefix.
    void test_D2_real_sqlite_backward_clock_stop_preserves_checkpoint_for_recovery();

    // D6 (R05-002/R15-004, carrying forward D2's marker-timestamp seam
    // obligation): a real ShutdownCoordinator/SqliteSessionStore shutdown
    // writes the clean-shutdown marker using the store's injected
    // now-provider (not the real clock), and that marker durably survives a
    // close and reopen of the database file.
    void test_D6_real_sqlite_shutdown_marker_uses_injected_clock_and_survives_reopen();
};

#endif // TEST_INTEGRATION_H
