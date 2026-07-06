#ifndef TEST_HISTORYEDITSESSION_H
#define TEST_HISTORYEDITSESSION_H

#include <QObject>

/**
 * HistoryEditSessionTest — non-widget unit tests for HistoryEditSession.
 *
 * These tests run with QT_QPA_PLATFORM=offscreen and must produce ZERO QWARN.
 * No QWidget is constructed; all test state is HistoryEditSession + TimeTracker + DB.
 */
class HistoryEditSessionTest : public QObject
{
    Q_OBJECT

private:
    QString db_path_;
    QString db_backup_path_;

    void resetDatabaseFile() const;

private slots:
    void initTestCase();
    void cleanupTestCase();

    // 5.1 — page creation
    void test_build_today_page_only_from_memory();
    void test_build_today_page_memory_plus_db();
    void test_build_historical_page_separate_from_today();
    void test_build_deduplicates_db_row_matching_memory_segment_id();

    // 5.1 — origin routing
    void test_origin_memory_rows_marked_true();
    void test_origin_db_rows_marked_false();

    // 5.1 — split op
    void test_split_registers_child_with_same_origin();
    void test_split_history_row_child_is_false_origin();

    // 5.1 — type toggle
    void test_type_toggle_updates_pending_timeline();

    // 5.1 — merge detection
    void test_no_merge_needed_for_non_overlapping();
    void test_merge_needed_for_overlapping_rows();

    // 5.1 — cross-midnight preservation
    void test_cross_midnight_preserved_in_payload();

    // 5.1 — ongoing refresh/clear behaviour
    void test_refresh_ongoing_updates_end_time();
    void test_refresh_ongoing_advances_end_time_and_preserves_user_type_edit();
    void test_refresh_ongoing_clears_when_engine_has_no_ongoing();

    // 5.1 — ongoing type toggle + refresh: end time advances, type preserved in payload
    void test_toggle_activity_to_pause_payload_end_utc_advances();
    void test_toggle_pause_to_activity_payload_end_utc_advances();

    // 5.1 — payload correctness (history / session / memory timelines)
    void test_payload_history_vs_session_buckets();
    void test_payload_memory_timeline_for_commit();
    void test_payload_memory_absorbs_merged_memory_row();

    // A8: a backward clock jump during a running session must not stop the engine
    void test_backward_clock_jump_keeps_engine_running();

    // F1: a failed history load must be remembered (Disabled is not a failure)
    void test_F1_historyLoadFailed_true_on_transient_error();
    void test_F1_historyLoadFailed_false_when_disabled();
};

#endif // TEST_HISTORYEDITSESSION_H
