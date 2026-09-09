#ifndef TEST_CLEANDURATION_H
#define TEST_CLEANDURATION_H

#include <QObject>
#include "testcommon.h"

// D7 (R04-001/R04-003, plan section 2): these tests exercise cleanDurations()
// (a thin wrapper over Timeline::normalizedWithRemovedIds(), see
// testcommon.cpp), i.e. reachable end-to-end normalization behavior — not
// classifyMerge() called directly with hand-built unsorted/impossible pair
// states. Same-type intervals merge only when they overlap or touch exactly;
// a positive gap, of any size, is never filled.
class CleanDurationsTest : public QObject
{
    Q_OBJECT

private slots:
    void test_cleanDurations_duplicateRemoval();
    void test_cleanDurations_overlappingNearDuplicate_extendsToUnion();
    void test_cleanDurations_positiveGap_neverMerges();
    void test_cleanDurations_subsetRemoval();
    void test_cleanDurations_overlapExtendForward();
    void test_cleanDurations_sameEndDifferentLength_earliestStartSurvives();
    void test_cleanDurations_mergeTwoThenRemoveSuperset();
    void test_cleanDurations_gapEqualsThreshold_noMerge_supersetRemains();
    void test_cleanDurations_gapEqualsThreshold_noMerge_allUntouched();
    void test_cleanDurations_chainMerge_threeTouchingSegments();
    void test_cleanDurations_removeMultipleOverlapping_extendsToUnion();
    void test_cleanDurations_leftOverlapJoin();
    void test_cleanDurations_chainMergeThenRemoveDuplicate();
    void test_cleanDurations_touchingIntervals_mergedByOverlap();
    void test_cleanDurations_longerFirst_shorterSecond_sameEnd();
    void test_cleanDurations_disjointLargeGap_noMerge();
    void test_cleanDurations_positiveGap_justUnderOldThreshold_neverMerges();
    void test_cleanDurations_presorted_mixedTypes_touching_noChange();
    void test_cleanDurations_presorted_mixedTypes_smallGaps_noChange();
    void test_cleanDurations_presorted_identicalTimes_differentTypes_keepBoth();
    void test_cleanDurations_differentTypesNotMerged();

    // Tests for removed segment_id tracking (T14)
    void test_cleanDurations_removedIds_empty_when_no_merge();
    void test_cleanDurations_removedIds_overlappingNearDuplicate_merges();
    void test_cleanDurations_removedIds_subsetRemoval_afterSort();
    void test_cleanDurations_removedIds_overlapExtendForward_afterSort();
    void test_cleanDurations_removedIds_exactTouch_merges();
    void test_cleanDurations_removedIds_subsetRemoval();
    void test_cleanDurations_removedIds_positiveGap_noMerge_bothIdsRetained();
    void test_cleanDurations_removedIds_slightOverlap_merges();
    void test_cleanDurations_removedIds_chainMerge_returns_two_ids();
    void test_cleanDurations_removedIds_single_entry_returns_empty();
};

#endif // TEST_CLEANDURATION_H
