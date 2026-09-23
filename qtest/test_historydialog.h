#ifndef TEST_HISTORYDIALOG_H
#define TEST_HISTORYDIALOG_H

#include <QObject>
#include "testcommon.h"

class HistoryDialogTest : public QObject
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

    void test_historydialog_createPages_includes_current_db_ongoing();
    void test_historydialog_createPages_dedups_db_row_with_small_time_drift();
    void test_historydialog_createPages_groups_unsplit_cross_midnight_row_by_start_date();
    void test_historydialog_checkbox_toggle_updates_pending_and_totals();
    void test_historydialog_saveChanges_updates_timetracker_and_db();
    void test_historydialog_split_action_splits_row();
    void test_historydialog_split_today_mixed_origins_routes_to_correct_bucket();
    void test_historydialog_split_non_today_db_row_survives_save_roundtrip();
    void test_historydialog_save_unrelated_edit_preserves_row_and_creates_new_checkpoint();
    void test_historydialog_pauses_checkpoint_timer_for_dialog_lifetime();
    void test_historydialog_save_keeps_db_rows_for_history_plus_current_session();
    void test_historydialog_save_then_crash_reopen_retains_current_segment_row();
    void test_historydialog_save_uses_ongoing_snapshot_endtime_after_wait();
    void test_historydialog_save_failed_db_replace_keeps_runtime_state_unchanged();
    void test_historydialog_save_preserves_deferred_midnight_segment();

    void test_splitdialog_default_types_and_bounds();
    void test_splitdialog_short_duration_disables_slider();
    void test_splitdialog_minimum_duration_boundaries();
    void test_splitdialog_setters_affect_types();

    // Test R: round-trip edit via accept updates timetracker
    void test_R_round_trip_type_toggle_via_accept();

    // Test S: cancel preserves state
    void test_S_cancel_preserves_state();

    void test_saveChanges_deduplicates_cross_bucket_overlaps();
    void test_saveChanges_noop_save_unchanged();
    void test_saveChanges_editless_ok_leaves_mergeable_rows_untouched();
    void test_H3_ongoing_type_edit_preserved_when_engine_stops();
    void test_H4_overlap_cancel_aborts_save();
    void test_H2_midnight_crossing_shown_on_both_pages();
    void test_H6_split_dialog_preset_from_source_row_type();
    void test_H14_cross_midnight_totals_and_counts();

    // F1: a failed history load must refuse destructive edited saves
    void test_F1_saveChanges_refuses_when_history_load_failed();
    void test_F1_banner_shows_when_history_load_failed();

    // C3 (R13-001/R14-001): saveChanges() must refuse a payload the model
    // flags as invalid, before replaceAll() is ever called. MUST-FIX2 (user
    // decision, review packet C3) narrowed which pre-existing shapes count
    // as a violation (a pre-existing cross-type overlap is now dropped, not
    // flagged -- see the model-level tests in test_historyeditsession.cpp),
    // so this pins the dialog's refusal path with a violation an edit
    // itself creates, which must still block the save.
    void test_C3_saveChanges_refuses_when_edit_creates_violation();
    void test_C3_saveChanges_drops_pre_existing_overlap_and_succeeds();
    // Review gap: R13-001's literal headline trigger -- a >1-boundary
    // (multi-day) legacy cross-midnight row alongside an edited ordinary
    // row. buildFromTimer() already skips a >1-boundary row entirely (it
    // never becomes display-only, unlike a single-boundary row), so an
    // unrelated wholesale-replacement save silently drops it. Pins the
    // narrowed-contract policy (data loss for out-of-contract data is
    // accepted, no preservation logic) against real SQLite physical state.
    void test_R13_001_multiboundary_row_dropped_by_unrelated_save();

    // Finding 3 (C3 re-review): the >1-boundary case above is not the case
    // whose behavior changed in this packet -- the single-boundary case is
    // (preserved -> dropped). Pins that against real SQLite physical state.
    void test_C3_single_boundary_cross_midnight_dropped_by_unrelated_save();

    // C4 fix round 2 (MEDIUM-5): if the fatal latch was ALREADY set before
    // this saveChanges() call, enterFatalState() shows no dialog of its own
    // (it only logs on a repeat), so HistoryDialog's own critical dialog
    // must NOT be suppressed for this call.
    void test_C4_historydialog_shows_own_critical_when_already_latched();

    // C4 fix round 3 (F3): the mirror case -- latch NOT set before this
    // saveChanges() call, this call's own replaceAll() raises the latch and
    // fires TimeTracker's own fatalError modal, so HistoryDialog must show
    // NO dialog of its own for the same failure.
    void test_C4_historydialog_suppresses_own_critical_when_this_call_latches();

    // ========================================================================
    // D5 review fix (finding 1/2, R15-002): HistoryDialog's shutdown-refusal
    // gate must be per-dialog (TimeTracker::shutdownEpoch()), not global
    // (wasShutDown()) -- a cancelShutdown() must not unprotect a dialog that
    // spanned the shutdown, but must allow one opened only afterward.
    // ========================================================================

    // A dialog opened AFTER a shutdown was cancelled saves normally -- the
    // D5 requirement itself (History works immediately, no new Start needed).
    void test_D5_dialog_opened_after_cancelShutdown_saves_successfully();
    // A dialog open DURING a shutdown that is then cancelled must still
    // refuse to save -- the shutdown's own commit must not be silently
    // dropped by a stale dialog snapshot that never saw it.
    void test_D5_dialog_spanning_cancelled_shutdown_refuses_save();

    // ========================================================================
    // D9 (R14-002): dialog is a renderer/controller of typed model operations.
    // ========================================================================

    // Split: table row count, page title count, and highlight all reflect the
    // model's dynamic state instead of a frozen build-time count.
    void test_D9_split_updates_table_rowcount_and_title_and_highlights();
    // Completed-row type toggle: highlight applied, dirty state from the model.
    void test_D9_checkbox_toggle_highlights_row_and_marks_dirty();
    // Ongoing-row type toggle: previously received no highlight at all.
    void test_D9_ongoing_toggle_highlights_row();
    // Navigating away and back preserves each page's own dirty state.
    void test_D9_navigation_preserves_per_page_dirty_state();
    // Cancel (reject) leaves the model/runtime state untouched after edits.
    void test_D9_cancel_after_split_and_toggle_preserves_runtime_state();

    // D9 review (finding 2 (g)/finding 1): sequence regression tests where
    // hasEdits()/pageModified() and the rendered highlight must agree.
    void test_D9_toggle_off_then_on_clears_highlight_and_dirty_state();
    void test_D9_toggle_twice_clears_highlight();
    void test_D9_ongoing_toggle_then_navigate_away_and_back_highlights_ongoing_row();

    // D9 review (finding 1, round 3): a too-short row must be refused before
    // SplitDialog is ever constructed, not just before the split is applied.
    void test_D9_split_too_short_row_does_not_open_modal();

    // ========================================================================
    // M2: a successful save whose destructive replace ran without its
    // pre-write safety backup must warn the user, not stay silent.
    // ========================================================================
    void test_M2_saveChanges_warns_when_backup_missing();
    void test_M2_saveChanges_silent_when_backup_present();

    // ========================================================================
    // M3: a destructive History save that would silently drop a display-only
    // cross-midnight row or a pre-existing overlap row must ask first.
    // ========================================================================
    void test_M3_saveChanges_decline_cross_midnight_drop_preserves_rows();
    void test_M3_saveChanges_decline_pre_existing_overlap_preserves_rows();
    void test_M3_saveChanges_combined_drop_causes_single_prompt();
    void test_M3_saveChanges_no_prompt_when_nothing_is_replaced();
};

#endif // TEST_HISTORYDIALOG_H
