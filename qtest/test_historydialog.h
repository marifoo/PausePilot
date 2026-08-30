#ifndef TEST_HISTORYDIALOG_H
#define TEST_HISTORYDIALOG_H

#include <QObject>
#include "testcommon.h"

class HistoryDialogTest : public QObject
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

    void test_historydialog_createPages_includes_current_db_ongoing();
    void test_historydialog_createPages_dedups_db_row_with_small_time_drift();
    void test_historydialog_createPages_groups_unsplit_cross_midnight_row_by_start_date();
    void test_historydialog_checkbox_toggle_updates_pending_and_totals();
    void test_historydialog_saveChanges_updates_timetracker_and_db();
    void test_historydialog_split_action_splits_row();
    void test_historydialog_split_today_mixed_origins_routes_to_correct_bucket();
    void test_historydialog_split_non_today_db_row_survives_save_roundtrip();
    void test_historydialog_shows_load_reconciliation_banner();
    void test_historydialog_save_unrelated_edit_preserves_row_and_creates_new_checkpoint();
    void test_historydialog_pauses_checkpoint_timer_for_dialog_lifetime();
    void test_historydialog_save_keeps_db_rows_for_history_plus_current_session();
    void test_historydialog_save_then_crash_reopen_retains_current_segment_row();
    void test_historydialog_save_uses_ongoing_snapshot_endtime_after_wait();
    void test_historydialog_save_failed_db_replace_keeps_runtime_state_unchanged();

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
};

#endif // TEST_HISTORYDIALOG_H
