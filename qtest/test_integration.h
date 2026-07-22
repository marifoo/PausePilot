#ifndef TEST_INTEGRATION_H
#define TEST_INTEGRATION_H

#include <QObject>
#include "testcommon.h"

class IntegrationTest : public QObject
{
    Q_OBJECT

private:
    QString db_path_;
    QString db_backup_path_;
    
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

    // Shutdown sequence end-to-end: stop → flush → clean-shutdown marker.
    void test_F_shutdown_sequence_stop_flush_marker();

    // DayBoundaryWatcher regression: cross-midnight behavior
    void test_5_0a_normal_same_day_no_discard();
    void test_5_0a_cross_midnight_ongoing_discarded_completed_preserved();
    void test_5_0a_discard_is_idempotent();
    void test_5_0a_watchdog_helper_returns_false_when_not_crossed();

    // DayBoundaryWatcher: scheduled vs watchdog stop signals
    void test_Y_engine_scheduled_stop_emits_MidnightScheduled();
    void test_Z_engine_watchdog_emits_MidnightWatchdog();
    void test_AA_no_duplicate_stop_signal();
    void test_AB_scheduleMidnightStop_is_gone();

    // recoverStartupCheckpoints: overlap rejection and count reporting
    void test_recoverStartupCheckpoints_overlap_rejected_not_counted();
    void test_recoverStartupCheckpoints_reports_finalized_and_dropped_counts();
    void test_timer_startup_recovery_uses_store_recovered_seconds();

    // Step 19 (C9 timezone): parameterized timezone tests for hasEntriesForDate.
    void test_hasEntriesForDate_timezone_data();
    void test_hasEntriesForDate_timezone();
    void test_midnight_boundary_timezone_data();
    void test_midnight_boundary_timezone();

    // Item A: toggle ongoing type then crash — refreshed end_utc survives recovery.
    void test_A_toggle_ongoing_to_pause_crash_recovery_uses_refreshed_end_utc();

    // A3 (R11-001): Exit Anyway (drain attempted and failed, no further
    // retry) leaves an existing durable checkpoint untouched. A real SQLite
    // reopen proves ordinary startup recovery finalizes only that durable
    // portion, without duplication or resurrecting the in-memory-only tail.
    void test_A3_exit_anyway_leaves_checkpoint_untouched_for_recovery();
};

#endif // TEST_INTEGRATION_H
