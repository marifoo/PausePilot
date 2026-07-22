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
    
    // Existing TimeTracker tests
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
    void test_session_state_reset_for_new_session();
    void test_session_state_adopt_ongoing_segment();

    // Issue 9: retry cache cleared after successful replaceAll
    void test_replaceAll_success_clears_unsaved_cache();
    void test_replaceAll_failure_keeps_unsaved_cache();

    // ========================================================================
    // A2 (R16-001/R09-001): PendingCommit replaces the retry flag/cache/
    // counter with one immutable-payload lifecycle. There is no retry
    // threshold: a pending commit always blocks Start until exactly one
    // retry either succeeds (payload cleared) or fails (payload retained
    // unchanged, Start refused).
    // ========================================================================

    // Table-driven: outcome of a first persist attempt (Stop) determines the
    // resulting pending state and whether a subsequent Start is permitted.
    void test_A2_first_submit_outcome_data();
    void test_A2_first_submit_outcome();

    // A single Start-time retry: failure keeps the exact same payload and
    // refuses Start; success clears it and permits Start.
    void test_A2_retry_failure_retains_same_payload_and_refuses_start();
    void test_A2_retry_success_clears_payload_and_permits_start();

    // Regression: no stale failure count. Three independent, non-consecutive
    // fail/recover cycles must each behave exactly like a first failure —
    // never escalate or refuse permanently.
    void test_A2_no_stale_failure_state_across_intermittent_successes();

    // Repeated failed Start attempts while a payload is pending must neither
    // begin tracking nor mutate the retained payload.
    void test_A2_repeated_failed_start_neither_starts_nor_mutates_payload();

    // State transition tests for public TimeTracker methods
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

    void test_saveCheckpoint_locked_does_not_write_cross_midnight_row();

    void test_stop_clears_was_active_before_autopause();

    void test_startTimer_drops_cross_midnight_boot_time_entry();

    void test_getOngoingDuration_returns_nullopt_when_cross_midnight();

    // TimeTracker public surface regression guard and initializeFromStore tests
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

    // F4: Unlock during a dialog-open LongOngoingLock must replay as Pause at
    // true timestamps, not cancel the correction.
    void test_F4_unlock_during_dialog_replays_lock_interval_at_true_timestamps();
    void test_F4_still_locked_at_dialog_close_replays_unchanged();
    void test_F4_ongoing_edited_away_skips_lock_correction_replay();
    void test_F4_replay_rearms_midnight_stop_at_real_time();

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

    // A14: commitEditedTimeline must refuse to half-adopt an ongoing segment
    // while mode_ == None (engine already stopped).
    void test_commitEditedTimeline_ignores_ongoing_when_already_stopped();

    // QF1: signal contract — started/paused/modeChanged emit counts
    void test_QF1_explicit_pause_emits_paused_not_modeChanged();
    void test_QF1_explicit_resume_emits_started_not_modeChanged();
    void test_QF1_lock_autopause_emits_modeChanged_not_paused();
    void test_QF1_lock_resume_emits_started_and_modeChanged_once_each();

    // Phase 2 clock seam: fake clock drives the ongoing duration with no sleep.
    void test_seam_fake_clock_drives_ongoing_duration();

    // A2: backpause anchors the Activity→Pause boundary at the real wall-clock
    // lock instant, not now−backpause, so a late (post-suspend) LongOngoingLock
    // does not credit suspended time as Activity.
    void test_A2_backpause_anchors_at_lock_time_across_suspend();
    // A2: display totals come from wall-clock segment_start_time, so they match
    // persisted totals across a suspend instead of tracking the frozen monotonic timer.
    void test_A2_display_total_matches_wallclock_across_suspend();

    // A3: the cross-midnight watchdog discard deletes the in-flight segment's
    // unfinalized checkpoint row so it cannot be resurrected or dropped as stale.
    void test_A3_watchdog_discard_deletes_checkpoint_row();

    // A4: shutdown_called_ is cleared on fresh session start to prevent silent data loss
    void test_A4_shutdown_called_cleared_on_fresh_session_start();

    // F2: a late (post-resume) stop that crosses midnight must discard the
    // in-flight segment's checkpoint row itself, not rely on the watchdog path.
    void test_F2_shutdown_after_midnight_discards_checkpoint();
    void test_F2_ontime_stop_does_not_discard_checkpoint();

    // F3: pause/backpause persistence failures must feed the same
    // unsaved-data bookkeeping stopTimer() already uses.
    void test_F3_pause_failure_marks_unsaved();
    void test_F3_backpause_failure_marks_unsaved();
    void test_F3_recovery_after_pause_failure_clears_unsaved();

    // F1: loadHistoryFromStore() forwards the store's LoadResult status
    // instead of discarding it (the contract violation the rest of F1 follows from).
    void test_F1_loadHistoryFromStore_forwards_status();

    // ========================================================================
    // A1 (R11-002): a deferred midnight event replayed at endExclusiveEdit()
    // must preserve the original policy — scheduled deferral credits exactly
    // through the captured 23:59:59.500 cutoff; watchdog deferral
    // conservatively discards the whole ongoing segment. Neither may use the
    // post-dialog-close "now" as a segment endpoint.
    // ========================================================================

    // Production capture path: onMidnightTimerFired()/tick() must record a
    // typed event (not just a bool) while dialog_open_, without stopping.
    void test_A1_onMidnightTimerFired_captures_typed_event_while_dialog_open();
    void test_A1_tick_captures_watchdog_event_while_dialog_open();

    // Scheduled deferral credits exactly through the captured cutoff.
    void test_A1_scheduled_deferred_credits_activity_through_cutoff_with_checkpoint();
    void test_A1_scheduled_deferred_credits_activity_through_cutoff_no_checkpoint();
    void test_A1_scheduled_deferred_credits_pause_through_cutoff();

    // Watchdog deferral conservatively discards the whole segment.
    void test_A1_watchdog_deferred_discards_entire_segment_with_checkpoint();
    void test_A1_watchdog_deferred_discards_entire_segment_no_checkpoint();

    // An unrelated in-memory History edit made during the deferral survives replay.
    void test_A1_unrelated_history_edit_preserved_during_deferred_midnight();

    // endExclusiveEdit() is idempotent: a second call must not replay again.
    void test_A1_repeated_endExclusiveEdit_does_not_replay_twice();

    // A deferred lock correction and a deferred midnight event pending
    // together replay chronologically by captured timestamp; both orders.
    void test_A1_lock_before_midnight_replays_lock_first();
    void test_A1_midnight_before_lock_replays_midnight_first();

    // ========================================================================
    // A3 (R11-001): TimeTracker::drainForShutdown() retries a pending commit
    // payload independent of tracker mode. Shutdown cleanliness derives from
    // the typed DrainResult, not from mode_ == None.
    // ========================================================================

    // Already-stopped, no pending payload: a no-op success.
    void test_A3_drain_already_stopped_no_pending_is_noop_success();
    // A pre-existing pending payload (from an earlier failed, already-stopped
    // Stop) is retried and resolved by drainForShutdown() even though
    // mode_ == None throughout -- the exact R11-001 scenario.
    void test_A3_drain_retries_preexisting_pending_payload_while_stopped();
    // Same setup, but the retry also fails: Incomplete, payload retained,
    // message preserved.
    void test_A3_drain_retry_failure_keeps_incomplete_and_payload();
    // A running session whose own Stop-time commit fails is drained (stopped
    // + retried) by one drainForShutdown() call.
    void test_A3_drain_running_session_stop_commit_fails_then_drain_succeeds();
    void test_A3_drain_running_session_stop_commit_fails_then_drain_fails();
    // Idempotent: a second call after success does not resubmit/duplicate;
    // a second call after failure retries again without losing/mutating the
    // payload identity or the failure category.
    void test_A3_drain_idempotent_repeated_calls_after_success();
    void test_A3_drain_idempotent_repeated_calls_after_failure();
    // An explicit user Retry is a new attempt, so it reports a fresh message
    // rather than the one latched by the drain that prompted it.
    void test_A3_user_retry_reports_fresh_failure_message();
    // The destructor is a no-op on the normal path (drain already resolved).
    void test_A3_destructor_is_noop_after_successful_drain();
};

#endif // TEST_TIMER_H
