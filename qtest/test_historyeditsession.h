#ifndef TEST_HISTORYEDITSESSION_H
#define TEST_HISTORYEDITSESSION_H

#include <QObject>

/**
 * HistoryEditSessionTest — non-widget unit tests for HistoryEditSession.
 *
 * These tests run with QT_QPA_PLATFORM=offscreen and must produce ZERO QWARN.
 * No QWidget is constructed; all test state is a HistoryEditContext built
 * directly (via the local makeContext helper) plus the HistoryEditSession
 * under test.
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

    // 5.1 — type toggle
    void test_type_toggle_updates_pending_timeline();

    // 5.1 — cross-midnight rows are display-only, absent from the scoped save
    void test_WP4_cross_midnight_absent_from_scopedSave_unrelatedEditSucceeds();

    // F1: a failed history load must be remembered (Disabled is not a failure)
    void test_F1_historyLoadFailed_true_on_transient_error();
    void test_F1_historyLoadFailed_false_when_disabled();

    // D2/MEDIUM-C: buildFromContext()'s ctx.today, not the real system date,
    // decides page grouping/editability -- a pinned date far from the real
    // one must still be classified as the current page.
    void test_D2_buildFromContext_uses_ctx_today_not_real_date();

    // D2 test list ("History rejection"): an edit that produces a reversed
    // interval must be flagged by buildScopedSave(), never silently written.
    void test_D2_scopedSave_flags_reversed_interval_created_by_edit();

    // D9 (R14-002): split/type-change operations, dynamic display count, and
    // per-page dirty state are owned by HistoryEditSession.
    void test_D9_displayCount_reflects_split_immediately();
    void test_D9_pageTitle_reflects_dynamic_count();
    void test_D9_splitRow_returns_original_and_new_ids();
    void test_D9_splitRow_out_of_range_reports_not_applied();
    void test_D9_setCompletedType_updates_type_and_marks_page_modified();
    void test_D9_setCompletedType_unknown_id_returns_false();
    void test_D9_pageModified_false_before_any_edit();
    void test_D9_pageModified_scoped_to_single_page();
    void test_D9_page_has_one_ongoing_representation();

    // D9 review (finding 3): splitRow() itself enforces the minimum-length
    // rules with a typed reason -- these pin the exact boundaries.
    void test_D9_splitRow_rejects_source_row_under_3_seconds_as_TooShort();
    void test_D9_splitRow_accepts_source_row_at_exactly_3_seconds();
    void test_D9_splitRow_rejects_first_half_under_1_second_as_HalfTooShort();
    void test_D9_splitRow_rejects_second_half_under_1_second_as_HalfTooShort();
    void test_D9_splitRow_accepts_first_half_at_exactly_1_second();
    // D9 review (finding 2, round 3): the original "both halves" test only
    // pinned the first half at the boundary (second half was 2000ms) -- add
    // the second-half boundary separately so a `<` -> `<=` mutation on
    // either comparison is caught.
    void test_D9_splitRow_accepts_second_half_at_exactly_1_second();

    // D9 review (finding 1, round 3): the split modal must not open for a
    // row splitRejection() already knows is too short.
    void test_D9_splitRejection_none_for_valid_row();
    void test_D9_splitRejection_TooShort_for_short_row();
    void test_D9_splitRejection_OutOfRange_for_bad_indices();

    // M3: an ordinary timezone change can turn a valid same-day UTC row into
    // a single-boundary cross-midnight row on reload.
    void test_M3_timezone_change_makes_row_single_boundary_cross_midnight();

    // H5/WP4 — buildFromContext() page building
    void test_WP4_pastMemoryRow_isReadOnly_todayUnaffected();
    void test_WP4_ongoingRow_onToday_notEditable();
    void test_WP4_mutationsOnReadOnlyPage_leaveItUnchanged();
    void test_WP4_rowsOutOfOrder_sortedOnPage();
    void test_WP4_futureDatedPage_isEditable();
    void test_WP4_daySpan_matchesLocalDayStartEnd();

    // H5/WP4 — buildScopedSave() scoped diff
    void test_WP4_scopedSave_toggle_yieldsOneUpsert();
    void test_WP4_scopedSave_split_yieldsOriginalPlusNewId();
    void test_WP4_scopedSave_toggleNextToSameTypeNeighbour_merges();
    void test_WP4_scopedSave_untouchedTouchingPair_absentFromDays_idsKept();
    void test_WP4_scopedSave_toggleBackToOriginal_yieldsEmptyDays();
    void test_WP4_scopedSave_editsOnTwoDates_yieldTwoDaysEntries();

    // H5/WP4 — buildScopedSave() violations
    void test_WP4_scopedSave_overlapsRetainedCrossMidnight_givesTypeOverlap();
    void test_WP4_scopedSave_overlapsRetainedMultiBoundary_givesTypeOverlap();
    void test_WP4_scopedSave_unrelatedPreExistingOverlap_doesNotBlockOtherEdit();
    void test_WP4_scopedSave_editedRowOverlapsUntouchedRetained_givesTypeOverlap();
};

#endif // TEST_HISTORYEDITSESSION_H
