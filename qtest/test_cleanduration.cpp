#include "test_cleanduration.h"
#include <QtTest>
#include <algorithm>

using TestCommon::mk;

// Helper: create a TimeDuration with a specific segment_id for ID-tracking tests
static TimeDuration mkId(DurationType type, qint64 startMs, qint64 endMs, const QString& id)
{
    QDateTime start = QDateTime::fromMSecsSinceEpoch(startMs, Qt::UTC);
    QDateTime end = QDateTime::fromMSecsSinceEpoch(endMs, Qt::UTC);
    return TimeDuration::fromPersistedRow(type, start, end, SegmentId::fromString(id));
}

void CleanDurationsTest::test_cleanDurations_duplicateRemoval()
{
    // Description: Identical Activity entries -> keep one

    // Arrange
    std::deque<TimeDuration> d;
    const qint64 base = 1'000'000;
    d.push_back(mk(DurationType::Activity, base - 1000, base));
    d.push_back(mk(DurationType::Activity, base - 1000, base));
    QCOMPARE((int)d.size(), 2);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().type, DurationType::Activity);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), base);
    QCOMPARE(d.front().duration(), (qint64)1000);
}

void CleanDurationsTest::test_cleanDurations_overlappingNearDuplicate_extendsToUnion()
{
    // Description: D7 (R04-001): near-identical entries no longer get a special
    // "erase without extending" rule -- they merge like any other overlap, and
    // the survivor's end covers the union of both.

    // Arrange
    std::deque<TimeDuration> d;
    const qint64 base = 1'000'000;
    d.push_back(mk(DurationType::Activity, base - 1000, base));          // [base-1000, base]
    d.push_back(mk(DurationType::Activity, base - 990, base + 20));      // [base-990, base+20]
    QCOMPARE((int)d.size(), 2);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().type, DurationType::Activity);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), base + 20);
    QCOMPARE(d.front().duration(), (qint64)1020);
}

void CleanDurationsTest::test_cleanDurations_positiveGap_neverMerges()
{
    // Description: D7/R04-003 (plan section 2): a positive gap, however small,
    // is never filled -- same-type intervals merge only on overlap or exact touch.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));    // [0,1000]
    d.push_back(mk(DurationType::Activity, 1100, 1300)); // gap=100 -> no merge

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 2);
}

void CleanDurationsTest::test_cleanDurations_subsetRemoval()
{
    // Description: Second entry fully inside previous -> remove subset

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 1000, 2000)); // [1000,2000]
    d.push_back(mk(DurationType::Activity, 1500, 1700)); // inside previous
    QCOMPARE((int)d.size(), 2);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)2000);
    QCOMPARE(d.front().duration(), (qint64)1000);
}

void CleanDurationsTest::test_cleanDurations_overlapExtendForward()
{
    // Description: Overlap where second extends beyond previous end -> join and extend correctly (no double count)

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 1000, 1500)); // dur 500
    d.push_back(mk(DurationType::Activity, 1400, 1700)); // extends beyond
    QCOMPARE((int)d.size(), 2);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)1700);
    // After bugfix, duration must be union length: 1700 - 1000 = 700
    QCOMPARE(d.front().duration(), (qint64)700);
}

void CleanDurationsTest::test_cleanDurations_sameEndDifferentLength_earliestStartSurvives()
{
    // Description: Two overlapping Activity durations ending at the same time --
    // the earlier-starting (here, also longer) one survives (D7/R04-002).

    // Arrange
    std::deque<TimeDuration> d;
    const qint64 end = 10'000;
    d.push_back(mk(DurationType::Activity, end - 800, end));  // shorter [9200,10000]
    d.push_back(mk(DurationType::Activity, end - 1000, end)); // longer  [9000,10000]
    QCOMPARE((int)d.size(), 2);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().type, DurationType::Activity);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), end);
    QCOMPARE(d.front().duration(), (qint64)1000);
}

void CleanDurationsTest::test_cleanDurations_mergeTwoThenRemoveSuperset()
{
    // Description: [0,10000] and [10001,20000] have a positive 1ms gap (no
    // direct merge), but [0,20000] genuinely overlaps both -- everything
    // collapses to one entry via real overlap, not gap-filling.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 10000));     // dur 10000
    d.push_back(mk(DurationType::Activity, 10001, 20000)); // gap=1 to the first entry
    d.push_back(mk(DurationType::Activity, 0, 20000));     // overlaps both
    QCOMPARE((int)d.size(), 3);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().type, DurationType::Activity);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)20000);
    QCOMPARE(d.front().duration(), (qint64)20000);
}

void CleanDurationsTest::test_cleanDurations_gapEqualsThreshold_noMerge_supersetRemains()
{
    // Description: [0,10000], [10500,20000] (gap=500, no direct merge), then
    // [0,20500] overlaps both directly -> collapses to one via real overlap.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 10000));     // [0,10000]
    d.push_back(mk(DurationType::Activity, 10500, 20000)); // [10500,20000] gap=500
    d.push_back(mk(DurationType::Activity, 0, 20500));     // overlaps both
    QCOMPARE((int)d.size(), 3);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].type, DurationType::Activity);
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)20500);
    QCOMPARE(d[0].duration(), (qint64)20500);
}

void CleanDurationsTest::test_cleanDurations_gapEqualsThreshold_noMerge_allUntouched()
{
    // Description: [0,10000] Activity, [10500,20000] Pause (different type --
    // gap distance is irrelevant here since only same-type pairs are ever
    // considered for merging), then [0,20500] Activity overlaps the first
    // Activity row directly -> Activity collapses to [0,20500].
    //
    // D7/R04-003 (plan section 2): the resulting Activity [0,20500] and Pause
    // [10500,20000] intervals overlap. Timeline's own job stops at not
    // merging unlike types and not guessing which one is right -- it does
    // NOT reject this. Rejecting a cross-type overlap as a conflict is
    // DayValidity::validatePayload's job (see dayvalidity.h), invoked as a
    // final guard by SessionStore and by HistoryEditSession before either one
    // would ever write a payload like this
    // (test_normalized_crossTypeOverlap_neverMerged_flaggedByDayValidity in
    // test_timeline.cpp pins that guard's verdict on this exact shape).

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 10000));     // [0,10000]
    d.push_back(mk(DurationType::Pause, 10500, 20000));    // [10500,20000] gap=500
    d.push_back(mk(DurationType::Activity, 0, 20500));     // superset

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 2);
    QCOMPARE(d[0].type, DurationType::Activity);
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)20500);
    QCOMPARE(d[0].duration(), (qint64)20500);
    QCOMPARE(d[1].type, DurationType::Pause);
    QCOMPARE(d[1].endTime.toMSecsSinceEpoch(), (qint64)20000);
    QCOMPARE(d[1].duration(), (qint64)20000 - 10500);
}

void CleanDurationsTest::test_cleanDurations_chainMerge_threeTouchingSegments()
{
    // Description: [0,1000], [1000,2000], [2000,3000] touch exactly -> merge into single [0,3000]

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Activity, 1000, 2000));
    d.push_back(mk(DurationType::Activity, 2000, 3000));
    QCOMPARE((int)d.size(), 3);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)3000);
    QCOMPARE(d.front().duration(), (qint64)3000);
}

void CleanDurationsTest::test_cleanDurations_removeMultipleOverlapping_extendsToUnion()
{
    // Description: [0,1000], [10,1010], [20,1020] all overlap heavily -> collapse
    // to one entry covering the union, [0,1020].

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Activity, 10, 1010));
    d.push_back(mk(DurationType::Activity, 20, 1020));
    QCOMPARE((int)d.size(), 3);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)1020);
    QCOMPARE(d.front().duration(), (qint64)1020);
}

void CleanDurationsTest::test_cleanDurations_leftOverlapJoin()
{
    // Description: pushed out of chronological order; after normalized()'s
    // start-sort the earlier-starting entry becomes prev and is extended
    // forward to cover the later one -> [900,1600].

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 1000, 1600)); // pushed first, starts later
    d.push_back(mk(DurationType::Activity, 900, 1500));  // pushed second, starts earlier
    QCOMPARE((int)d.size(), 2);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)1600);
    QCOMPARE(d.front().duration(), (qint64)700);
}

void CleanDurationsTest::test_cleanDurations_chainMergeThenRemoveDuplicate()
{
    // Description: [0,1000], [1050,2000], [2050,3000], [0,3000] -> end with one.
    // The two touching-gap pairs don't merge directly (D7: positive gap never
    // fills), but [0,3000] genuinely overlaps every one of them, so all four
    // collapse into a single entry via real overlap.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Activity, 1050, 2000));
    d.push_back(mk(DurationType::Activity, 2050, 3000));
    d.push_back(mk(DurationType::Activity, 0, 3000));
    QCOMPARE((int)d.size(), 4);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)3000);
    QCOMPARE(d.front().duration(), (qint64)3000);
}

void CleanDurationsTest::test_cleanDurations_touchingIntervals_mergedByOverlap()
{
    // Description: [0,1000], [1000,1500] -> merged (exact touch, gap == 0)

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Activity, 1000, 1500));
    QCOMPARE((int)d.size(), 2);

    // Act
    cleanDurations(&d);

    // Assert (matches union length)
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), (qint64)1500);
    QCOMPARE(d.front().duration(), (qint64)1500);
}

void CleanDurationsTest::test_cleanDurations_longerFirst_shorterSecond_sameEnd()
{
    // Description: prev longer [9000,10000], it shorter [9500,10000] -> keep longer

    // Arrange
    std::deque<TimeDuration> d;
    const qint64 end = 10'000;
    d.push_back(mk(DurationType::Activity, end - 1000, end)); // prev longer
    d.push_back(mk(DurationType::Activity, end - 500, end));  // it shorter

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d.front().endTime.toMSecsSinceEpoch(), end);
    QCOMPARE(d.front().duration(), (qint64)1000);
}

void CleanDurationsTest::test_cleanDurations_disjointLargeGap_noMerge()
{
    // Description: [0,1000], [2000,3000] -> remain two

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Activity, 2000, 3000));

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 2);
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)1000);
    QCOMPARE(d[1].endTime.toMSecsSinceEpoch(), (qint64)3000);
}

void CleanDurationsTest::test_cleanDurations_positiveGap_justUnderOldThreshold_neverMerges()
{
    // Description: D7/R04-003: [0,1000], [1499,1600] gap=499 -- under the
    // pre-D7 500ms small-gap allowance this used to merge; a positive gap of
    // any size is never filled now, so both entries remain.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Activity, 1499, 1600));

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 2);
}

void CleanDurationsTest::test_cleanDurations_presorted_mixedTypes_touching_noChange()
{
    // Description: Activity [0,1000], Pause [1000,1500], Activity [1500,2000] -> unchanged

    // Arrange (already sorted)
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Pause, 1000, 1500));
    d.push_back(mk(DurationType::Activity, 1500, 2000));

    // Copy for comparison
    auto before = d;

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 3);
    QCOMPARE(d[0].type, before[0].type);
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), before[0].endTime.toMSecsSinceEpoch());
    QCOMPARE(d[0].duration(), before[0].duration());
    QCOMPARE(d[1].type, before[1].type);
    QCOMPARE(d[1].endTime.toMSecsSinceEpoch(), before[1].endTime.toMSecsSinceEpoch());
    QCOMPARE(d[1].duration(), before[1].duration());
    QCOMPARE(d[2].type, before[2].type);
    QCOMPARE(d[2].endTime.toMSecsSinceEpoch(), before[2].endTime.toMSecsSinceEpoch());
    QCOMPARE(d[2].duration(), before[2].duration());
}

void CleanDurationsTest::test_cleanDurations_presorted_mixedTypes_smallGaps_noChange()
{
    // Description: Activity [0,1000], Pause [1100,1300], Activity [1350,1600] -> unchanged
    // (types differ, so gap distance never matters for these pairs)

    // Arrange (already sorted)
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Pause, 1100, 1300));
    d.push_back(mk(DurationType::Activity, 1350, 1600));
    auto before = d;

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 3);
    for (size_t i = 0; i < d.size(); ++i) {
        QCOMPARE(d[i].type, before[i].type);
        QCOMPARE(d[i].endTime.toMSecsSinceEpoch(), before[i].endTime.toMSecsSinceEpoch());
        QCOMPARE(d[i].duration(), before[i].duration());
    }
}

void CleanDurationsTest::test_cleanDurations_presorted_identicalTimes_differentTypes_keepBoth()
{
    // Description: Activity [0,1000], Pause [0,1000] -> both remain (no same-type rule).
    // mk() mints a random id for each row, so nothing here may depend on a
    // specific output position -- assert set membership by type instead
    // (M1/H1 fix: the segment_id tie-break only ever decides among rows that
    // share start AND type, which always merge; an Activity/Pause pair here
    // is ordered by type, not id, but the test must not assume any
    // particular comparator to stay robust).

    // Arrange (already sorted by start/end)
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Pause, 0, 1000));

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 2);
    const TimeDuration* activity = d[0].type == DurationType::Activity ? &d[0] : &d[1];
    const TimeDuration* pause    = d[0].type == DurationType::Pause    ? &d[0] : &d[1];
    QVERIFY(activity->type == DurationType::Activity);
    QVERIFY(pause->type == DurationType::Pause);
    QCOMPARE(activity->endTime.toMSecsSinceEpoch(), (qint64)1000);
    QCOMPARE(activity->duration(), (qint64)1000);
    QCOMPARE(pause->endTime.toMSecsSinceEpoch(), (qint64)1000);
    QCOMPARE(pause->duration(), (qint64)1000);
}

void CleanDurationsTest::test_cleanDurations_differentTypesNotMerged()
{
    // Description: Different types must never be merged

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mk(DurationType::Activity, 0, 1000));
    d.push_back(mk(DurationType::Pause, 1000, 1200));
    QCOMPARE((int)d.size(), 2);

    // Act
    cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 2);
    QCOMPARE(d[0].type, DurationType::Activity);
    QCOMPARE(d[1].type, DurationType::Pause);
}

// ──────────────────────────────────────────────────────────────────
// Tests for removed segment_id tracking (T14)
// ──────────────────────────────────────────────────────────────────

void CleanDurationsTest::test_cleanDurations_removedIds_empty_when_no_merge()
{
    // Description: Two disjoint same-type entries with large gap -> no merge, empty removedIds

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-A"));
    d.push_back(mkId(DurationType::Activity, 5000, 6000, "id-B"));

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 2);
    QVERIFY(removed.empty());
}

void CleanDurationsTest::test_cleanDurations_removedIds_overlappingNearDuplicate_merges()
{
    // Description: Near-identical overlapping entries merge like any other
    // overlap; the erased entry (it) is the orphan.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-keep"));
    d.push_back(mkId(DurationType::Activity, 10, 1020, "id-dup"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-keep"));
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-dup"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_subsetRemoval_afterSort()
{
    // Description: [100,500] fully contains [200,400] -> subset is removed,
    // the containing entry's id survives.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 100, 500, "id-big"));
    d.push_back(mkId(DurationType::Activity, 200, 400, "id-subset"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert: subset removed, big kept
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-big"));
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-subset"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_overlapExtendForward_afterSort()
{
    // Description: [1000,1500] overlaps [1400,1700] -> merge, extending the
    // earlier-starting entry's end forward; the erased entry (it) is the orphan.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 1000, 1500, "id-prev"));
    d.push_back(mkId(DurationType::Activity, 1400, 1700, "id-overlap"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert: prevIt keeps its ID, it's ID is orphaned
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-prev"));
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)1700);
    QCOMPARE(d[0].duration(), (qint64)700);
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-overlap"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_exactTouch_merges()
{
    // Description: [0,1000] touching [1000,1500] exactly (gap == 0) -> merge.
    // The erased entry (it) is the orphan; prevIt keeps its segment_id.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-first"));
    d.push_back(mkId(DurationType::Activity, 1000, 1500, "id-second"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-first"));
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)1500);
    QCOMPARE(d[0].duration(), (qint64)1500);
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-second"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_subsetRemoval()
{
    // Description: current is a subset of previous -> erase current.
    // The erased entry (it) is the orphan; prevIt keeps its segment_id.

    // Arrange: [1000,3000] contains [1500,2500]
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 1000, 3000, "id-outer"));
    d.push_back(mkId(DurationType::Activity, 1500, 2500, "id-inner"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-outer"));
    QCOMPARE(d[0].duration(), (qint64)2000);
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-inner"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_positiveGap_noMerge_bothIdsRetained()
{
    // Description: D7/R04-003: adjacent disjoint entries with a positive gap
    // (200ms, previously under the old 500ms small-gap allowance) no longer
    // merge -- both ids are retained and removedIds is empty.

    // Arrange: [0,1000] and [1200,2000] -> gap=200, positive -> no merge
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-left"));
    d.push_back(mkId(DurationType::Activity, 1200, 2000, "id-right"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 2);
    QCOMPARE(d[0].segment_id.toString(), QString("id-left"));
    QCOMPARE(d[1].segment_id.toString(), QString("id-right"));
    QVERIFY(removed.empty());
}

void CleanDurationsTest::test_cleanDurations_removedIds_slightOverlap_merges()
{
    // Description: a small overlap still merges like any other overlap (there
    // is no separate "slight overlap" rule any more -- overlap of any size
    // merges, only a positive gap never does).

    // Arrange: [0,1000] and [950,1800] -> overlap by 50ms
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-first"));
    d.push_back(mkId(DurationType::Activity, 950, 1800, "id-second"));
    QCOMPARE((int)d.size(), 2);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-first"));
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)1800);
    QCOMPARE(d[0].duration(), (qint64)1800);
    QCOMPARE((int)removed.size(), 1);
    QCOMPARE(removed[0], QString("id-second"));
}

void CleanDurationsTest::test_cleanDurations_removedIds_chainMerge_returns_two_ids()
{
    // Description: Three touching entries merged into one via chain merge -> two IDs returned.
    // [0,1000] touches [1000,2000] touches [2000,3000].
    // First merge: id-B removed, prevIt=[0,2000,id-A]. Second merge: id-C removed.

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-A"));
    d.push_back(mkId(DurationType::Activity, 1000, 2000, "id-B"));
    d.push_back(mkId(DurationType::Activity, 2000, 3000, "id-C"));
    QCOMPARE((int)d.size(), 3);

    // Act
    auto removed = cleanDurations(&d);

    // Assert: one surviving entry with id-A, two removed
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-A"));
    QCOMPARE(d[0].endTime.toMSecsSinceEpoch(), (qint64)3000);
    QCOMPARE(d[0].duration(), (qint64)3000);
    QCOMPARE((int)removed.size(), 2);
    // Both id-B and id-C should be in removed (order matches iteration order)
    QVERIFY(std::find(removed.begin(), removed.end(), QString("id-B")) != removed.end());
    QVERIFY(std::find(removed.begin(), removed.end(), QString("id-C")) != removed.end());
}

void CleanDurationsTest::test_cleanDurations_removedIds_single_entry_returns_empty()
{
    // Description: Single entry -> nothing to merge, removedIds is empty

    // Arrange
    std::deque<TimeDuration> d;
    d.push_back(mkId(DurationType::Activity, 0, 1000, "id-only"));
    QCOMPARE((int)d.size(), 1);

    // Act
    auto removed = cleanDurations(&d);

    // Assert
    QCOMPARE((int)d.size(), 1);
    QCOMPARE(d[0].segment_id.toString(), QString("id-only"));
    QVERIFY(removed.empty());
}
