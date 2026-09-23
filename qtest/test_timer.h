#ifndef TEST_TIMER_H
#define TEST_TIMER_H

#include <QObject>
#include "testcommon.h"

class TimerTest : public QObject
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
    
    // Existing TimeTracker tests
    void test_timer_start_transitions_to_active();
    void test_timer_pause_captures_duration_and_keeps_ongoing();
    void test_timer_resume_from_pause_transitions_to_active();
    void test_timer_manual_checkpoint_preserves_ongoing_duration();
    void test_timer_stop_transitions_to_stopped();
    void test_timer_backpause_resets_checkpoint_and_splits();
    void test_timer_lock_events_checkpoint_and_resume();
    void test_timer_ongoing_duration();
    void test_timer_set_duration_type();
    void test_timer_checkpoints_paused();
    void test_timer_retry_append_failure_then_success_preserves_segments();
    void test_timer_retry_failure_keeps_unsaved_state_and_durations();

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
    void test_session_state_adopted_segment_start_time_anchors_committed_row();

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
    void test_getOngoingDuration_capped_at_deferred_scheduled_cutoff();
    void test_getOngoingDuration_stays_nullopt_for_deferred_watchdog_midnight();

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
    void test_D10_editApplied_pause_to_activity_emits_transitioned();

    // M7: zero checkpoint interval and exclusive-edit guards
    void test_M7_zero_checkpoint_interval_edit_to_activity_does_not_arm_timer();
    void test_M7_edit_to_activity_during_exclusive_edit_does_not_arm_timer();

    // Disabled history is a no-op, not a save/checkpoint failure
    void test_disabled_history_stop_keeps_clean_state_no_warning();
    void test_disabled_history_commitEditedTimeline_succeeds();

    // Item B: commitEditedTimeline clears anchor when edited timeline has no ongoing
    void test_commitEditedTimeline_clears_anchor_when_no_ongoing();
    void test_commitEditedTimeline_clears_anchor_when_no_ongoing_from_pause();

    // A14: commitEditedTimeline must refuse to half-adopt an ongoing segment
    // while mode_ == None (engine already stopped).
    void test_commitEditedTimeline_ignores_ongoing_when_already_stopped();

    // QF1: signal contract — cause distinguishes button vs. lock-driven
    // transitions. Originally pinned via which of started/paused/modeChanged
    // fired; migrated by D10 to assert TransitionCause on transitioned()
    // instead, since those three signals no longer exist (D10 finding 2).
    void test_QF1_explicit_pause_is_ButtonPause_cause();
    void test_QF1_explicit_resume_is_ButtonStart_cause();
    void test_QF1_lock_autopause_is_LockAutopause_cause();
    void test_QF1_lock_resume_is_LockResume_cause_emitted_once();

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

    // H2 (R11-003): useTimerViaLockEvent must defer a cross-midnight discard
    // when History holds the exclusive edit, exactly as the tick path does.
    void test_A1_lock_event_does_not_discard_deferred_scheduled_midnight();
    void test_A1_lock_event_defers_watchdog_when_no_midnight_pending();

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

    // ========================================================================
    // A4 (R05-001): a failed checkpoint discard must retain a durable intent
    // (not just log-and-forget) that blocks clean marking until resolved,
    // survives Transient/Fatal failures, retries at normal persistence
    // opportunities and at drainForShutdown(), and never mutates fake state
    // on Disabled or failure.
    // ========================================================================

    // A transient discard failure retains the intent and forbids clean marking.
    void test_A4_discard_transient_failure_retains_intent_and_blocks_clean_mark();
    // Same, but Fatal -- also retained (C4 owns the latch; A4 only retains).
    void test_A4_discard_fatal_failure_retains_intent_and_blocks_clean_mark();
    // Disabled resolves the intent as a contractual no-op and does not mutate
    // the fake's checkpoint records.
    void test_A4_discard_disabled_resolves_intent_without_mutating_checkpoints();
    // Start is a normal retry opportunity but must not itself be blocked by
    // an unresolved discard intent.
    void test_A4_start_retries_pending_discard_without_blocking_start();
    // drainForShutdown() retries an unresolved discard intent; once the store
    // recovers, the drain becomes clean and the intent clears.
    void test_A4_drain_retries_pending_discard_then_succeeds();
    // Same setup, but the store keeps failing: drain stays Incomplete and the
    // intent remains -- no clean marker may be written.
    void test_A4_drain_incomplete_while_discard_unresolved();

    // ========================================================================
    // C4 (R05-006): FatalError latches TimeTracker into one unavailable state.
    // A Fatal result from ANY store operation stops the current session
    // through the normal pending lifecycle, disables Start until restart,
    // and shows exactly one modal error; later failures (Fatal or not) never
    // repeat the dialog. A Transient periodic checkpoint failure, by
    // contrast, is a logged, UI-silent missed snapshot: tracking continues
    // and the next ordinary checkpoint interval retries through the same
    // call path.
    // ========================================================================

    // A Fatal periodic checkpoint failure stops tracking immediately and
    // shows exactly one modal error.
    void test_C4_fatal_checkpoint_failure_latches_and_stops_tracking();
    // Start is refused (no-op) once the fatal state is latched, even after
    // the store recovers -- only a fresh TimeTracker (i.e. a restart) can
    // clear it.
    void test_C4_fatal_latch_disables_start_even_after_store_recovers();
    // Two independent Fatal failures across different operations must show
    // exactly one modal error in total.
    void test_C4_fatal_error_shows_dialog_exactly_once();
    // A transient periodic checkpoint failure is silent (no dialog) and
    // tracking continues; the very next checkpoint interval retries through
    // the same call path and can succeed once the store recovers.
    void test_C4_transient_checkpoint_failure_is_silent_and_tracking_continues();

    // C4 finding 1: a Fatal from replaceAll() must latch the tracker exactly
    // like a Fatal from any other operation (previously it reached the
    // caller directly and never latched).
    void test_C4_fatal_replaceAll_latches_and_refuses_subsequent_start();
    // C4 finding 2: a Fatal commit on the Pause->Activity resume must stop
    // the tracker at a safe point AFTER the transition completes, not via a
    // nested stopTimer() mid-transition -- so no started() signal follows
    // the stopped() signal (the stuck-UI bug).
    void test_C4_fatal_pauseresume_commit_stops_without_started_after_stopped();
    // C4 finding 2: same defect on the Activity->Pause transition
    // (finalizePauseTransition, used by pauseTimer/backpauseTimer) -- the
    // tracker must end up cleanly stopped, never in the impossible state of
    // segment_id/segment_start_time set while mode_ == None.
    void test_C4_fatal_pause_transition_ends_stopped_not_impossible_state();
    // C4 finding 2: a Fatal commit encountered while discarding a
    // cross-midnight ongoing segment must still report
    // StopReason::MidnightWatchdog, not FatalStoreError -- the fatal latch
    // must not consume the transition out from under the watchdog's own stop.
    void test_C4_fatal_during_midnight_discard_still_reports_midnightwatchdog();
    // C4 fix round 2 (HIGH-2): a CallerBug on stop-commit is NOT dropped --
    // it retains the payload for retry like any other failure (normal
    // okOrDisabled() path), does not reset the session, and must not write
    // the clean-shutdown marker.
    void test_C4_callerBug_commit_payload_is_retained_not_dropped();
    // C4 fix round 3 (F6): a persistent (non-recovering) CallerBug on commit
    // must keep Start refused across repeated attempts -- the retried
    // payload is identical, so it fails the same deterministic way each time.
    void test_C4_callerBug_commit_keeps_start_refused_while_persistent();
    // C4 fix round 2 (MEDIUM-HIGH-3): a CallerBug on a pending checkpoint
    // discard retry is retained (not resolved) exactly like any other
    // failure -- it must keep blocking clean-shutdown marking.
    void test_C4_callerBug_discard_retry_is_retained_not_resolved();

    // C4 fix round 2 (HIGH-1): enterFatalState() emits fatalError (a single
    // modal dialog) instead of userWarning (a tray balloon/status message)
    // for the fatal transition itself.
    void test_C4_fatalError_emitted_once_userWarning_not_emitted_for_fatal();

    // C4 fix round 3 (F1): a Fatal discard-retry latch with no pending
    // commit payload must still refuse Start -- retryUnsavedDurations()
    // returning true (no pending commit) must not let startTimer()'s None
    // branch fall through to Mode::Activity while latched.
    void test_C4_start_refused_when_discard_retry_latches_with_no_pending_commit();

    // D2 (R11-003): a backward wall-clock step (NTP/manual/DST) makes `now`
    // precede the live segment's captured start. Stop drops only the
    // closing interval (zero DB mutation) and leaves nothing unresolved
    // (round-2 review MEDIUM-B; see the D2 packet's deviation note) -- so a
    // later drain can still clean-mark (D6: stopTimer() itself no longer
    // writes any marker; that is verified via drainForShutdown() here).
    void test_D2_stop_backward_clock_step_zero_mutation_drain_still_clean();
    // D2/HIGH-A (round-2 review): a backward-clock Stop must never register
    // a checkpoint-discard intent -- that would authorize deleting a valid
    // already-checkpointed prefix along with the invalid closing delta.
    void test_D2_stop_backward_clock_step_never_discards_checkpoint();
    // D2 (R11-003): a backward step onto the PRIOR calendar date must not be
    // misclassified as a cross-midnight watchdog condition.
    void test_D2_stop_rejects_backward_clock_step_across_date_not_misclassified();
    // D2/HIGH-1 (R11-003): the same backward step at Pause must refuse the
    // whole transition -- no state mutation at all -- rather than cap `now`
    // and still re-anchor the new Pause segment at the Activity segment's
    // own start (which would re-identify real Activity time as Pause).
    void test_D2_pause_refuses_backward_clock_step_no_state_mutated();
    // D2/MEDIUM-6: backpauseTimer (the automatic lock-driven pause path) is
    // exposed to the same anchor-regression risk and must refuse the same way.
    void test_D2_backpause_refuses_backward_clock_step_no_state_mutated();
    // D2/MEDIUM-8: a DST fall-back is just another backward step.
    void test_D2_pause_refuses_dst_fallback_backward_step();
    // D2/MEDIUM-8: a DST spring-forward is a forward step and must not be
    // mistaken for a reversal.
    void test_D2_pause_proceeds_normally_through_dst_springforward();
    // D2 (R11-004): a scheduled-stop callback that fires before wall time
    // actually reaches the captured boundary (backward clock/DST fall-back
    // after arming) must re-arm for the remaining time instead of stopping
    // early.
    void test_D2_scheduled_stop_fires_early_after_backward_step_rearms();
    // D2 (R11-004): continuing from a re-arm, once wall time reaches the
    // boundary the callback stops at exactly that boundary.
    void test_D2_scheduled_stop_reaches_boundary_after_rearm_then_stops();
    // D2 (R11-004): a same-civil-date late fire still applies the captured
    // boundary as the cutoff, not the later `now`.
    void test_D2_scheduled_stop_late_same_day_applies_captured_boundary();
    // D2/MEDIUM-5 (R11-004): a late fire that has rolled onto a FURTHER
    // civil date must defer to the watchdog's conservative discard, not
    // credit [segment_start, boundary] -- that would invent unattended work
    // time and race the watchdog tick.
    void test_D2_scheduled_stop_late_across_date_defers_to_watchdog_discard();

    // M6: DST spring-forward can cause an offset-blind QTime delay to
    // over-estimate the real duration to the midnight boundary, arming the
    // timer one real hour late. Mirror the offset-aware re-arm path to fix it.
    void test_M6_scheduled_stop_arm_is_offset_aware_across_spring_dst();

    // ========================================================================
    // D5 (R15-002): cancelShutdown() is the one lifecycle operation that
    // reverses shutdown()'s terminal state after a cancelled OS shutdown. It
    // must clear shutdown_called_ without reconstructing/resuming the ended
    // session or touching pending persistence (governed by A2/A3).
    // ========================================================================

    // Cancellation after shutdown() stopped a live segment: terminal state
    // clears, but the tracker stays in its ordinary stopped Mode::None --
    // the closed segment is not resumed.
    void test_D5_cancelShutdown_after_active_shutdown_stays_stopped_not_resumed();
    // Cancellation after shutdown() ran on an already-stopped tracker.
    void test_D5_cancelShutdown_while_already_stopped();
    // A pending commit payload retained from a failed Stop survives
    // cancellation unchanged -- A2/A3 still own its retry, at the next Start.
    void test_D5_cancelShutdown_retains_pending_commit_for_next_start();

    // ========================================================================
    // D10 (R10-001/R10-002): one typed input dispatcher, one cause-carrying
    // transition event sufficient for a UI reducer, and one typed owner of
    // deferred-event replay priority.
    // ========================================================================

    // Every valid mode/input/cause pair emits exactly one transitioned()
    // event with the expected (from, to, cause).
    void test_D10_transitioned_emits_one_event_per_real_transition_data();
    void test_D10_transitioned_emits_one_event_per_real_transition();

    // Invalid/no-op inputs (Pause from None/Pause, Start from Activity,
    // Stop from None) must not emit transitioned() at all.
    void test_D10_noop_inputs_emit_no_transitioned_data();
    void test_D10_noop_inputs_emit_no_transitioned();

    // R10-001: onStartPausePressed() is a pure toggle translation onto the
    // one production/test button dispatcher -- it must produce exactly the
    // same transition (and the same cross-midnight-discard behavior) as the
    // equivalent useTimerViaButton() call, not a second implementation of
    // the boundary guard/switch.
    void test_D10_onStartPausePressed_delegates_to_button_dispatch_data();
    void test_D10_onStartPausePressed_delegates_to_button_dispatch();

    // R10-002: lock resume is exactly one transitioned() emission, not two
    // independent notifications a caller must know not to double-act on.
    // Pinned by test_QF1_lock_resume_is_LockResume_cause_emitted_once() above.

    // D10/plan4 B4: a deferred midnight event and a deferred lock correction
    // pending together still replay chronologically through the new
    // DeferredEditEvents owner -- guards the consolidation against silently
    // dropping the A1 priority rule.
    void test_D10_deferred_events_replay_chronologically_lock_first();
    void test_D10_deferred_events_replay_chronologically_midnight_first();

    // Regression guard for a real ordering bug caught while restructuring
    // useTimerViaLockEvent (plan4 B16): when autopause is disabled, a
    // LongOngoingLock arriving while the History dialog is open must be
    // dropped outright, exactly as when the dialog is not open -- NOT
    // deferred and replayed once autopause is re-enabled.
    void test_D10_longlock_disabled_autopause_dialog_open_is_dropped_not_deferred();

    // Round-2 review (finding 1): every source of a transitioned() event
    // driven through its REAL live path (not dbg-injected), not just the
    // deferred-replay path exercised above.
    // A live scheduled midnight stop.
    void test_D10_midnight_scheduled_live_emits_transitioned();
    // A live watchdog cross-midnight discard, driven through the real
    // onTick() -> DayBoundaryWatcher::tick() path.
    void test_D10_midnight_watchdog_live_emits_transitioned();
    // shutdown()/drainForShutdown() emits transitioned(..., Shutdown), not
    // mis-mapped to ButtonStop by either the cause or the legacy stopped()
    // StopReason translation.
    void test_D10_shutdown_emits_transitioned();
    // A Fatal commit failure during a Button::Stop press must NOT relabel
    // the transition's cause as FatalStoreError -- stopTimer() never nests
    // through stopIfLatched(); the button's own reason (ButtonStop) is what
    // actually happened, while the latch still takes effect for the next Start.
    void test_D10_fatal_commit_on_stop_keeps_buttonstop_cause();

    // Round-2 review (finding 1, M8b): the delegation test must also cover
    // the cross-midnight-discard guard, not only the three ordinary modes --
    // a hand-rolled toggle body that dropped the guard would still pass the
    // three mode rows above.
    void test_D10_onStartPausePressed_cross_midnight_discard_matches_button_dispatch();
};

#endif // TEST_TIMER_H
