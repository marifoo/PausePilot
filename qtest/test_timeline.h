#ifndef TEST_TIMELINE_H
#define TEST_TIMELINE_H

#include <QObject>
#include "testcommon.h"

class TimelineTest : public QObject
{
    Q_OBJECT

private slots:
    // Test M: activeMsec/pauseMsec sums including ongoing
    void test_M_totals();

    // Test N: withSegmentType is pure — original unchanged, returned has new type
    void test_N_withSegmentType_is_pure();

    // Test O: withSplit preserves total duration and produces new segment_id for second half
    void test_O_withSplit_preserves_total_duration();

    // Test P: normalized() matches legacy cleanDurations
    void test_P_normalized_matches_cleanDurations();

    // Test Q: groupByDate assigns segments to correct dates
    void test_Q_groupByDate();

    // Test R: TimeDuration::create factory invariants (return type + error field)
    void test_R_factory_rejects_cross_midnight();
    void test_R_factory_accepts_same_day();
    void test_R_factory_rejects_zero_duration();
    void test_R_factory_rejects_negative_duration();
    // 9.1: typed error field for each rejection cause
    void test_R_create_error_invalid_timestamp();
    void test_R_create_error_nonpositive();
    void test_R_create_error_cross_midnight();

    // Test S: normalized() cross-day merge guard
    void test_S_normalized_no_cross_day_merge();
    void test_S_normalized_overlap_still_merges();
    void test_S_normalized_exactTouchAtMidnight_notMerged();

    // 9.3: classifyMerge — only two outcomes are reachable once input is sorted
    // by start (D7/R04-001): None (differing types, or a positive gap) and
    // Merge (overlap or exact touch).
    void test_classifyMerge_none_for_different_types();
    void test_classifyMerge_merge_for_overlap();
    void test_classifyMerge_merge_for_subset();
    void test_classifyMerge_merge_for_exact_touch();
    void test_classifyMerge_none_for_any_positive_gap();

    // D7 (R04-003, plan section 2): same-type merges only on overlap or exact
    // touch; a positive gap, of any size, is never filled. Data-driven table.
    void test_normalized_overlapOrTouch_vs_positiveGap_data();
    void test_normalized_overlapOrTouch_vs_positiveGap();

    // D7 (R04-002): earliest-starting ID survives; equal starts break
    // lexicographically. Exact-duplicate survival is therefore deterministic.
    void test_normalized_earliestStart_idSurvives();
    void test_normalized_equalStart_lexicographicIdSurvives();
    void test_normalized_exactDuplicate_permutations_sameSurvivor();

    // D7 (R04-003): Timeline never merges differing types and never guesses —
    // a cross-type overlap survives normalization unmerged; rejecting it is
    // DayValidity/SessionStore/HistoryEditSession's job (already covered by
    // their own tests), not Timeline's.
    void test_normalized_crossTypeOverlap_neverMerged_flaggedByDayValidity();

    // D7: normalizedWithRemovedIds() reports exactly the merged-away IDs.
    void test_normalizedWithRemovedIds_reportsRemoved();

    // D7: normalizing an already-normalized timeline is a no-op (idempotence).
    void test_normalized_isIdempotent();
};

#endif // TEST_TIMELINE_H
