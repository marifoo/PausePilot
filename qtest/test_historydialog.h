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
    void test_historydialog_split_action_splits_row();
    void test_historydialog_split_non_today_db_row_survives_save_roundtrip();
    void test_historydialog_save_unrelated_edit_preserves_row_and_creates_new_checkpoint();
    void test_historydialog_checkpoint_timer_keeps_running_for_dialog_lifetime();
    void test_historydialog_save_keeps_db_rows_for_history_plus_current_session();

    void test_splitdialog_default_types_and_bounds();
    void test_splitdialog_short_duration_disables_slider();
    void test_splitdialog_minimum_duration_boundaries();
    void test_splitdialog_setters_affect_types();

    // Test R: round-trip edit via accept updates timetracker
    void test_R_round_trip_type_toggle_via_accept();

    // Test S: cancel preserves state
    void test_S_cancel_preserves_state();

    void test_saveChanges_noop_save_unchanged();
    void test_saveChanges_editless_ok_leaves_mergeable_rows_untouched();
    void test_H4_overlap_cancel_aborts_save();
    void test_H2_midnight_crossing_shown_on_both_pages();
    void test_H6_split_dialog_preset_from_source_row_type();
    void test_H14_cross_midnight_totals_and_counts();

    // F1: a failed history load must refuse destructive edited saves
    void test_F1_saveChanges_refuses_when_history_load_failed();
    void test_F1_banner_shows_when_history_load_failed();

    // C3 (R13-001/R14-001): saveChanges() must refuse a diff the model
    // flags as invalid, before saveHistoryEdit() is ever called. MUST-FIX2 (user
    // decision, review packet C3) narrowed which pre-existing shapes count
    // as a violation (a pre-existing cross-type overlap is now dropped, not
    // flagged -- see the model-level tests in test_historyeditsession.cpp),
    // so this pins the dialog's refusal path with a violation an edit
    // itself creates, which must still block the save.
    void test_C3_saveChanges_refuses_when_edit_creates_violation();

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
    // Navigating away and back preserves each page's own dirty state.
    void test_D9_navigation_preserves_per_page_dirty_state();
    // Cancel (reject) leaves the model/runtime state untouched after edits.
    void test_D9_cancel_after_split_and_toggle_preserves_runtime_state();

    // D9 review (finding 2 (g)/finding 1): sequence regression tests where
    // hasEdits()/pageModified() and the rendered highlight must agree.
    void test_D9_toggle_off_then_on_clears_highlight_and_dirty_state();
    void test_D9_toggle_twice_clears_highlight();

    // D9 review (finding 1, round 3): a too-short row must be refused before
    // SplitDialog is ever constructed, not just before the split is applied.
    void test_D9_split_too_short_row_does_not_open_modal();

    // WP5 acceptance: the read-only split entry point is inert on the
    // ongoing row of a running Today page, even when targeted directly via
    // the debug seams (bypassing showContextMenu()'s own guard).
    void test_split_ongoing_row_entry_point_is_inert();

    // ========================================================================
    // WP1: History honours a revoked edit (inert until WP2).
    // ========================================================================
    void test_revoked_dialog_refuses_save_and_leaves_store_untouched();
    void test_revoked_dialog_accept_does_not_commit();
    void test_revoked_editless_accept_same_turn_never_accepts();

    // ========================================================================
    // S1: HistoryDialog's constructor reads the store only once (the
    // loadHistoryRows() in beginExclusiveEdit(); buildFromContext() does no I/O).
    // ========================================================================
    void test_historydialog_construction_loads_store_exactly_once();

    // ========================================================================
    // WP2 (midnight_close_plan.md): a live scheduled midnight stop revokes
    // an open edit and closes the dialog.
    // ========================================================================
    void test_D1_scheduled_midnight_closes_dialog_and_keeps_cutoff_segment();

    // ========================================================================
    // WP6 (midnight_close_plan.md): shutdown revokes an open edit.
    // ========================================================================
    void test_dialog_closes_on_shutdown_and_stays_closed_after_cancel();

    // ========================================================================
    // H5/WP5: date-scoped History save through the dialog.
    // ========================================================================
    void test_H5_scoped_save_touches_only_edited_date();
    void test_H5_unrelated_save_keeps_legacy_rows_and_overlap_byte_identical();
    void test_H5_malformed_row_on_edited_date_survives_open_and_save();
    void test_H5_split_and_confirmed_merge_on_past_date_round_trips();
    void test_H5_conflict_after_open_keeps_edits_and_writes_nothing_data();
    void test_H5_conflict_after_open_keeps_edits_and_writes_nothing();
    void test_H5_commit_failure_keeps_edits_and_retry_succeeds();
    void test_H5_revoke_from_save_error_ends_rejected_without_write();
    void test_H5_revoke_during_merge_prompt_never_saves();
    void test_H5_today_after_stop_saves();
    void test_H5_today_while_running_is_read_only();

    // ====================================================================
    // WP7c (step6_plan_revised.md): revoke History on any real mode
    // change (U1), exercised through the real HistoryDialog.
    // ====================================================================
    void test_WP7c_queued_start_with_pending_edits_ends_rejected_no_save();
    void test_WP7c_start_and_accept_same_turn_ends_rejected_no_write();
    void test_WP7c_merge_prompt_hook_real_mode_change_refuses_save();
    void test_WP7c_editable_today_dialog_then_start_revokes_and_start_proceeds();
};

#endif // TEST_HISTORYDIALOG_H
