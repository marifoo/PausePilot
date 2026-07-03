#ifndef TEST_TIMER_H
#define TEST_TIMER_H

#include <QObject>
#include "testcommon.h"

class TimerTest : public QObject
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
    
    // Existing Timer tests
    void test_timer_start_pause_resume_stop_and_checkpoints();
    void test_timer_backpause_resets_checkpoint_and_splits();
    void test_timer_lock_events_checkpoint_and_resume();
    void test_timer_ongoing_duration();
    void test_timer_set_duration_type();
    void test_timer_checkpoints_paused();
    void test_timer_retry_append_failure_then_success_preserves_segments();
    void test_timer_retry_failure_keeps_unsaved_state_and_durations();

    // SessionState transition tests
    void test_session_state_begin_new_segment();
    void test_session_state_clear_segment();
    void test_session_state_mark_and_clear_unsaved();
    void test_session_state_reset_for_new_session();
    void test_session_state_adopt_ongoing_segment();

    // Issue 9: retry cache cleared after successful replaceAll
    void test_replaceAll_success_clears_unsaved_cache();
    void test_replaceAll_failure_keeps_unsaved_cache();

    // State transition tests for public Timer methods
    void test_session_state_start_to_pause_transition();
    void test_session_state_pause_to_activity_transition();
    void test_session_state_stop_clears_segment();

    // Boot time + hasEntriesForDate tri-state tests (T18)
    void test_boot_time_not_added_when_history_disabled();
    void test_boot_time_added_once_on_empty_db();
    void test_boot_time_not_added_when_db_has_entries_for_today();

    // Pause persistence crash safety (T19)
    void test_pause_row_persisted_immediately_on_resume();

    // Midnight forced-stop and cross-midnight discard tests
    void test_addduration_appends_single_same_day_segment();
    void test_addduration_discards_cross_midnight_segment();
    void test_addduration_discards_zero_and_negative_duration();
    // 9.1: all three DurationCreateError causes discard the segment
    void test_addduration_discards_invalid_timestamp();

    void test_isOngoingSegmentCrossMidnight_returns_false_when_stopped();
    void test_isOngoingSegmentCrossMidnight_returns_false_for_same_day();
    void test_isOngoingSegmentCrossMidnight_returns_true_when_segment_started_yesterday();

    void test_useTimerViaButton_force_stops_on_cross_midnight_ongoing();
    void test_useTimerViaButton_pause_event_is_dropped_when_cross_midnight();

    void test_useTimerViaLockEvent_unlock_does_not_restart_after_cross_midnight_discard();

    void test_saveCheckpointInternal_does_not_write_cross_midnight_row();

    void test_stop_clears_was_active_before_autopause();

    void test_startTimer_drops_cross_midnight_boot_time_entry();

    void test_getOngoingDuration_returns_nullopt_when_cross_midnight();

    // Timer public surface regression guard and initializeFromStore tests
    void test_D_timetracker_public_surface_regression();
    // Boot-time gate via FakeSessionStore for each tri-state hasEntriesForDate result.
    void test_E_boot_time_gate_entries_yes_skips_boot_time();
    void test_E_boot_time_gate_entries_no_adds_boot_time();
    void test_E_boot_time_gate_entries_unknown_skips_boot_time();
    // In-memory boot-time check uses startTime (not endTime) — same-day guard.
    void test_E_boot_time_inmemory_start_date_skips_boot_time();

    // Stop persists state via commitSession only (no direct DB write methods).
    void test_X_stop_persists_via_commitSession_only();

    // beginExclusiveEdit suspends mutations while dialog is open
    void test_dialog_open_blocks_backpause();
    void test_dialog_open_defers_midnight_stop();
    void test_dialog_open_allows_lock_bookkeeping();

    // Unpause creates a new Pause segment with a fresh id (no segment reuse)
    void test_T1_unpause_creates_new_pause_segment_with_fresh_id();
    void test_T9_new_activity_segment_after_unpause_has_activity_type();

    // Autopause flag cleared in startTimer
    void test_T10_was_active_cleared_on_start_from_none();
    void test_T10_was_active_cleared_on_start_from_pause();

    // Destructor ordering: crash-absence smoke test
    void test_T8_destructor_does_not_crash_while_active();

    // beginExclusiveEdit/endExclusiveEdit checkpoint guard
    void test_T2_replaceCurrentDurations_skips_checkpoint_while_dialog_open();
    void test_C6_replaceCurrentDurations_writes_checkpoint_after_endExclusiveEdit();

    // MarkerResult::Error skips orphan reconciliation
    void test_S12_marker_error_skips_reconciliation();

    // Commit of an ongoing type edit aligns mode_
    void test_commit_ongoing_type_edit_changes_mode();
    // commitEditedTimeline returns success and aligns mode_ with edited ongoing type
    void test_commitEditedTimeline_mode_aligns_with_edited_ongoing_type();

    // Disabled history is a no-op, not a save/checkpoint failure
    void test_disabled_history_stop_keeps_clean_state_no_warning();
    void test_disabled_history_commitEditedTimeline_succeeds();

    // Item B: commitEditedTimeline clears anchor when edited timeline has no ongoing
    void test_commitEditedTimeline_clears_anchor_when_no_ongoing();
    void test_commitEditedTimeline_clears_anchor_when_no_ongoing_from_pause();

    // QF1: signal contract — started/paused/modeChanged emit counts
    void test_QF1_explicit_pause_emits_paused_not_modeChanged();
    void test_QF1_explicit_resume_emits_started_not_modeChanged();
    void test_QF1_lock_autopause_emits_modeChanged_not_paused();
    void test_QF1_lock_resume_emits_started_and_modeChanged_once_each();

    // Phase 2 clock seam: fake clock drives the ongoing duration with no sleep.
    void test_seam_fake_clock_drives_ongoing_duration();
};

#endif // TEST_TIMER_H
