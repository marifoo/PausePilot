#include "test_timeline.h"
#include "timeline.h"
#include "dayvalidity.h"
#include <QtTest>

using TestCommon::mk;

namespace {

QDateTime makeUtc(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC);
}

} // namespace

// ============================================================================
// Test M — activeMsec/pauseMsec sums including ongoing
// ============================================================================

void TimelineTest::test_M_totals()
{
    const qint64 base = 1'000'000;

    // Two completed segments: 500ms Activity, 300ms Pause
    std::deque<TimeDuration> comp;
    comp.push_back(mk(DurationType::Activity, base, base + 500));
    comp.push_back(mk(DurationType::Pause, base + 600, base + 900));

    // Ongoing: 200ms Activity
    TimeDuration ongoing = TimeDuration::fromPersistedRow(DurationType::Activity, makeUtc(base + 1000), makeUtc(base + 1200));

    Timeline t(comp, ongoing);

    QCOMPARE(t.activeMsec(), static_cast<qint64>(500 + 200));
    QCOMPARE(t.pauseMsec(), static_cast<qint64>(300));
}

// ============================================================================
// Test N — withSegmentType is pure
// ============================================================================

void TimelineTest::test_N_withSegmentType_is_pure()
{
    const qint64 base = 2'000'000;

    std::deque<TimeDuration> comp;
    comp.push_back(mk(DurationType::Activity, base, base + 1000));
    comp.push_back(mk(DurationType::Pause, base + 1100, base + 1600));

    Timeline original(comp, std::nullopt);
    Timeline modified = original.withSegmentType(0, DurationType::Pause);

    // Original is unchanged
    QCOMPARE(original.completed()[0].type, DurationType::Activity);
    QCOMPARE(original.completed()[1].type, DurationType::Pause);

    // Modified has the new type at index 0
    QCOMPARE(modified.completed()[0].type, DurationType::Pause);
    QCOMPARE(modified.completed()[1].type, DurationType::Pause);

    // Sizes unchanged
    QCOMPARE(modified.completed().size(), original.completed().size());
}

// ============================================================================
// Test O — withSplit preserves total duration and produces new segment_id
// ============================================================================

void TimelineTest::test_O_withSplit_preserves_total_duration()
{
    const qint64 base = 3'000'000;
    const SegmentId origId = SegmentId::fromString("original-segment-id");

    std::deque<TimeDuration> comp;
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, makeUtc(base), makeUtc(base + 10000), origId));

    Timeline original(comp, std::nullopt);
    QDateTime splitAt = makeUtc(base + 4000);

    Timeline split = original.withSplit(0, splitAt, DurationType::Activity, DurationType::Pause);

    // Two segments now
    QCOMPARE(split.completed().size(), static_cast<size_t>(2));

    const auto& first = split.completed()[0];
    const auto& second = split.completed()[1];

    // Times are correct
    QCOMPARE(first.startTime, makeUtc(base));
    QCOMPARE(first.endTime, splitAt);
    QCOMPARE(second.startTime, splitAt);
    QCOMPARE(second.endTime, makeUtc(base + 10000));

    // Total duration preserved
    QCOMPARE(first.duration() + second.duration(), static_cast<qint64>(10000));

    // First half retains original segment_id; second gets a new one
    QCOMPARE(first.segment_id, origId);
    QVERIFY(!second.segment_id.isEmpty());
    QVERIFY(second.segment_id != origId);

    // Original is unchanged
    QCOMPARE(original.completed().size(), static_cast<size_t>(1));
    QCOMPARE(original.completed()[0].segment_id, origId);
}

// ============================================================================
// Test P — normalized() matches legacy cleanDurations
// ============================================================================

void TimelineTest::test_P_normalized_matches_cleanDurations()
{
    const qint64 base = 4'000'000;

    // Use fixed segment IDs so both copies start identical
    const SegmentId id1 = SegmentId::fromString("id-activity-1");
    const SegmentId id2 = SegmentId::fromString("id-activity-2");
    const SegmentId id3 = SegmentId::fromString("id-pause-1");
    const SegmentId id4 = SegmentId::fromString("id-pause-2");

    auto makeDeque = [&]() {
        std::deque<TimeDuration> d;
        // Two Activity segments that touch exactly (gap == 0) — merge.
        d.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
            QDateTime::fromMSecsSinceEpoch(base, Qt::UTC),
            QDateTime::fromMSecsSinceEpoch(base + 1000, Qt::UTC), id1));
        d.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
            QDateTime::fromMSecsSinceEpoch(base + 1000, Qt::UTC),
            QDateTime::fromMSecsSinceEpoch(base + 2000, Qt::UTC), id2));
        // Overlapping Pause entries — merge.
        d.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
            QDateTime::fromMSecsSinceEpoch(base + 3000, Qt::UTC),
            QDateTime::fromMSecsSinceEpoch(base + 4000, Qt::UTC), id3));
        d.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
            QDateTime::fromMSecsSinceEpoch(base + 3010, Qt::UTC),
            QDateTime::fromMSecsSinceEpoch(base + 4020, Qt::UTC), id4));
        return d;
    };

    std::deque<TimeDuration> forClean = makeDeque();
    std::deque<TimeDuration> forTimeline = makeDeque();

    cleanDurations(&forClean);
    Timeline normed = Timeline(forTimeline, std::nullopt).normalized();

    const auto& normedComp = normed.completed();

    QCOMPARE(normedComp.size(), forClean.size());
    for (size_t i = 0; i < forClean.size(); ++i) {
        QCOMPARE(normedComp[i].segment_id, forClean[i].segment_id);
        QCOMPARE(normedComp[i].startTime, forClean[i].startTime);
        QCOMPARE(normedComp[i].endTime, forClean[i].endTime);
        QCOMPARE(normedComp[i].type, forClean[i].type);
        QCOMPARE(normedComp[i].duration(), forClean[i].duration());
    }
}

// ============================================================================
// Test R — TimeDuration::create factory invariants
// ============================================================================

void TimelineTest::test_R_factory_rejects_cross_midnight()
{
    QDateTime start(QDate(2025, 6, 15), QTime(23, 30, 0), Qt::LocalTime);
    QDateTime end(QDate(2025, 6, 16), QTime(0, 30, 0), Qt::LocalTime);
    auto result = TimeDuration::create(DurationType::Activity, start, end);
    QVERIFY(!result.has_value());
}

void TimelineTest::test_R_factory_accepts_same_day()
{
    QDateTime start(QDate(2025, 6, 15), QTime(9, 0, 0), Qt::LocalTime);
    QDateTime end(QDate(2025, 6, 15), QTime(10, 0, 0), Qt::LocalTime);
    auto result = TimeDuration::create(DurationType::Activity, start, end);
    QVERIFY(result.has_value());
    QVERIFY(result->duration() > 0);
}

void TimelineTest::test_R_factory_rejects_zero_duration()
{
    QDateTime dt(QDate(2025, 6, 15), QTime(9, 0, 0), Qt::LocalTime);
    auto result = TimeDuration::create(DurationType::Activity, dt, dt);
    QVERIFY(!result.has_value());
}

void TimelineTest::test_R_factory_rejects_negative_duration()
{
    QDateTime start(QDate(2025, 6, 15), QTime(10, 0, 0), Qt::LocalTime);
    QDateTime end(QDate(2025, 6, 15), QTime(9, 0, 0), Qt::LocalTime);
    auto result = TimeDuration::create(DurationType::Activity, start, end);
    QVERIFY(!result.has_value());
}

// ============================================================================
// Test R (9.1 extension) — typed DurationCreateError for each rejection cause
// ============================================================================

void TimelineTest::test_R_create_error_invalid_timestamp()
{
    QDateTime invalid;
    QDateTime valid(QDate(2025, 6, 15), QTime(9, 0, 0), Qt::LocalTime);

    auto r1 = TimeDuration::create(DurationType::Activity, invalid, valid);
    QVERIFY(!r1.has_value());
    QCOMPARE(r1.error, DurationCreateError::InvalidTimestamp);

    auto r2 = TimeDuration::create(DurationType::Activity, valid, invalid);
    QVERIFY(!r2.has_value());
    QCOMPARE(r2.error, DurationCreateError::InvalidTimestamp);
}

void TimelineTest::test_R_create_error_nonpositive()
{
    QDateTime dt(QDate(2025, 6, 15), QTime(9, 0, 0), Qt::LocalTime);

    // zero duration
    auto r1 = TimeDuration::create(DurationType::Activity, dt, dt);
    QVERIFY(!r1.has_value());
    QCOMPARE(r1.error, DurationCreateError::NonPositive);

    // negative duration
    auto r2 = TimeDuration::create(DurationType::Activity, dt.addSecs(1), dt);
    QVERIFY(!r2.has_value());
    QCOMPARE(r2.error, DurationCreateError::NonPositive);
}

void TimelineTest::test_R_create_error_cross_midnight()
{
    QDateTime start(QDate(2025, 6, 15), QTime(23, 30, 0), Qt::LocalTime);
    QDateTime end(QDate(2025, 6, 16), QTime(0, 30, 0), Qt::LocalTime);

    auto r = TimeDuration::create(DurationType::Activity, start, end);
    QVERIFY(!r.has_value());
    QCOMPARE(r.error, DurationCreateError::CrossMidnight);
}

// ============================================================================
// Test S — normalized() cross-day merge guard
// ============================================================================

void TimelineTest::test_S_normalized_no_cross_day_merge()
{
    // Segment A ends at 2025-06-15 23:59:59.500, segment B starts at 2025-06-16 00:00:00.000.
    // Each is individually same-day (assertSameDayInvariant passes for both).
    // gap = 500ms, a positive gap, so they must NOT merge (D7: a positive gap
    // is never filled, regardless of day-crossing).
    QDateTime aStart(QDate(2025, 6, 15), QTime(23, 0, 0), Qt::LocalTime);
    QDateTime aEnd(QDate(2025, 6, 15), QTime(23, 59, 59, 500), Qt::LocalTime);
    QDateTime bStart(QDate(2025, 6, 16), QTime(0, 0, 0), Qt::LocalTime);
    QDateTime bEnd(QDate(2025, 6, 16), QTime(1, 0, 0), Qt::LocalTime);

    auto segA = TimeDuration::create(DurationType::Activity, aStart, aEnd);
    auto segB = TimeDuration::create(DurationType::Activity, bStart, bEnd);
    QVERIFY(segA.has_value());
    QVERIFY(segB.has_value());

    std::deque<TimeDuration> comp;
    comp.push_back(std::move(*segA));
    comp.push_back(std::move(*segB));

    Timeline t(std::move(comp), std::nullopt);
    Timeline normed = t.normalized();

    QCOMPARE(normed.completed().size(), static_cast<size_t>(2));
}

void TimelineTest::test_S_normalized_overlap_still_merges()
{
    // Two same-type segments on the same day that genuinely overlap — merge.
    QDateTime aStart(QDate(2025, 6, 15), QTime(10, 0, 0), Qt::LocalTime);
    QDateTime aEnd(QDate(2025, 6, 15), QTime(10, 0, 30), Qt::LocalTime);
    QDateTime bStart(QDate(2025, 6, 15), QTime(10, 0, 15), Qt::LocalTime);
    QDateTime bEnd(QDate(2025, 6, 15), QTime(10, 30, 0), Qt::LocalTime);

    auto segA = TimeDuration::create(DurationType::Activity, aStart, aEnd);
    auto segB = TimeDuration::create(DurationType::Activity, bStart, bEnd);
    QVERIFY(segA.has_value());
    QVERIFY(segB.has_value());

    std::deque<TimeDuration> comp;
    comp.push_back(std::move(*segA));
    comp.push_back(std::move(*segB));

    Timeline t(std::move(comp), std::nullopt);
    Timeline normed = t.normalized();

    QCOMPARE(normed.completed().size(), static_cast<size_t>(1));
}

void TimelineTest::test_S_normalized_exactTouchAtMidnight_notMerged()
{
    // Segment A ends exactly at 2025-06-16 00:00:00.000 (the legal half-open
    // boundary of its own day, 2025-06-15 -- see isCrossMidnight's exception
    // in types.h); segment B starts at that exact same instant. gap == 0, an
    // exact touch, which normally merges -- but curr (B) starts on a
    // different calendar date than prev (A), so merging would splice a
    // cross-midnight interval. Must NOT merge.
    QDateTime aStart(QDate(2025, 6, 15), QTime(23, 0, 0), Qt::LocalTime);
    QDateTime aEnd(QDate(2025, 6, 16), QTime(0, 0, 0), Qt::LocalTime);
    QDateTime bStart(QDate(2025, 6, 16), QTime(0, 0, 0), Qt::LocalTime);
    QDateTime bEnd(QDate(2025, 6, 16), QTime(1, 0, 0), Qt::LocalTime);

    auto segA = TimeDuration::create(DurationType::Activity, aStart, aEnd);
    auto segB = TimeDuration::create(DurationType::Activity, bStart, bEnd);
    QVERIFY(segA.has_value());
    QVERIFY(segB.has_value());
    QCOMPARE(classifyMerge(*segA, *segB), MergeDecision::None);

    std::deque<TimeDuration> comp;
    comp.push_back(std::move(*segA));
    comp.push_back(std::move(*segB));

    Timeline t(std::move(comp), std::nullopt);
    Timeline normed = t.normalized();

    QCOMPARE(normed.completed().size(), static_cast<size_t>(2));
}

// ============================================================================
// 9.3: classifyMerge — only two outcomes are reachable once input is sorted
// by start (D7/R04-001): None and Merge.
// ============================================================================

namespace {

// Helper: same-day Activity segment from raw millisecond offsets.
TimeDuration makeAct(qint64 startMs, qint64 endMs)
{
    return TimeDuration::fromPersistedRow(DurationType::Activity,
                                          QDateTime::fromMSecsSinceEpoch(startMs, Qt::UTC),
                                          QDateTime::fromMSecsSinceEpoch(endMs, Qt::UTC));
}

} // namespace

void TimelineTest::test_classifyMerge_none_for_different_types()
{
    const qint64 base = 5'000'000;
    TimeDuration act = makeAct(base, base + 1000);
    TimeDuration pau = TimeDuration::fromPersistedRow(DurationType::Pause,
                     QDateTime::fromMSecsSinceEpoch(base + 2000, Qt::UTC),
                     QDateTime::fromMSecsSinceEpoch(base + 3000, Qt::UTC));
    QCOMPARE(classifyMerge(act, pau), MergeDecision::None);
}

void TimelineTest::test_classifyMerge_merge_for_overlap()
{
    const qint64 base = 9'000'000;
    TimeDuration prev = makeAct(base,       base + 1000);
    TimeDuration curr = makeAct(base + 500, base + 2000); // starts inside, ends after
    QCOMPARE(classifyMerge(prev, curr), MergeDecision::Merge);
}

void TimelineTest::test_classifyMerge_merge_for_subset()
{
    const qint64 base = 10'000'000;
    TimeDuration prev = makeAct(base,       base + 2000);
    TimeDuration curr = makeAct(base + 200, base + 1800); // fully inside
    QCOMPARE(classifyMerge(prev, curr), MergeDecision::Merge);
}

void TimelineTest::test_classifyMerge_merge_for_exact_touch()
{
    const qint64 base = 11'000'000;
    TimeDuration prev = makeAct(base, base + 1000);
    TimeDuration curr = makeAct(base + 1000, base + 2000); // gap == 0, exact touch
    QCOMPARE(classifyMerge(prev, curr), MergeDecision::Merge);
}

void TimelineTest::test_classifyMerge_none_for_any_positive_gap()
{
    const qint64 base = 13'000'000;
    TimeDuration prev = makeAct(base, base + 1000);
    // A single millisecond of positive gap is enough to block the merge —
    // there is no small-gap allowance any more.
    TimeDuration curr = makeAct(base + 1001, base + 2000);
    QCOMPARE(classifyMerge(prev, curr), MergeDecision::None);
}

// ============================================================================
// D7/R04-003: same-type merges only on overlap or exact touch; a positive
// gap, of any size, is never filled. Data-driven over the gap distance.
// ============================================================================

void TimelineTest::test_normalized_overlapOrTouch_vs_positiveGap_data()
{
    QTest::addColumn<qint64>("gapMs");
    QTest::addColumn<bool>("expectMerge");

    // Negative gapMs means overlap.
    QTest::newRow("deep overlap (-500ms)")    << (qint64)-500 << true;
    QTest::newRow("slight overlap (-1ms)")    << (qint64)-1   << true;
    QTest::newRow("exact touch (0ms)")        << (qint64)0    << true;
    QTest::newRow("1ms positive gap")         << (qint64)1    << false;
    QTest::newRow("100ms positive gap")       << (qint64)100  << false;
    QTest::newRow("499ms positive gap")       << (qint64)499  << false;
    QTest::newRow("500ms positive gap")       << (qint64)500  << false;
    QTest::newRow("large positive gap (5s)")  << (qint64)5000 << false;
}

void TimelineTest::test_normalized_overlapOrTouch_vs_positiveGap()
{
    QFETCH(qint64, gapMs);
    QFETCH(bool, expectMerge);

    const qint64 base = 20'000'000;
    const qint64 prevEnd = base + 1000;
    std::deque<TimeDuration> comp;
    comp.push_back(makeAct(base, prevEnd));
    comp.push_back(makeAct(prevEnd + gapMs, prevEnd + gapMs + 1000));

    Timeline t(comp, std::nullopt);
    Timeline normed = t.normalized();

    QCOMPARE(normed.completed().size(), expectMerge ? static_cast<size_t>(1) : static_cast<size_t>(2));
}

// ============================================================================
// D7/R04-002: earliest-starting ID survives; equal starts break
// lexicographically. Exact-duplicate survival is therefore deterministic.
// ============================================================================

void TimelineTest::test_normalized_earliestStart_idSurvives()
{
    // Two overlapping segments with different starts: the earlier-starting
    // id ("id-late" starts later despite its name) must survive regardless
    // of which one is longer or which was pushed first.
    const qint64 base = 30'000'000;
    std::deque<TimeDuration> comp;
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime::fromMSecsSinceEpoch(base + 500, Qt::UTC),
        QDateTime::fromMSecsSinceEpoch(base + 5000, Qt::UTC), SegmentId::fromString("id-late")));
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime::fromMSecsSinceEpoch(base, Qt::UTC),
        QDateTime::fromMSecsSinceEpoch(base + 1000, Qt::UTC), SegmentId::fromString("id-early")));

    Timeline t(comp, std::nullopt);
    Timeline normed = t.normalized();

    QCOMPARE(normed.completed().size(), static_cast<size_t>(1));
    QCOMPARE(normed.completed()[0].segment_id.toString(), QString("id-early"));
    QCOMPARE(normed.completed()[0].endTime.toMSecsSinceEpoch(), base + 5000);
}

void TimelineTest::test_normalized_equalStart_lexicographicIdSurvives()
{
    // Two overlapping segments with the SAME start: the lexicographically
    // smaller id survives, regardless of push order -- and regardless of
    // which end is later. The smaller id ("aaa...") deliberately gets the
    // LATER end here: under the old (start, end, duration) sort, the
    // shorter-ending "zzz..." row would sort first and "survive" purely by
    // having the earlier end, which would pass this assertion for the wrong
    // reason. Giving the smaller id the later end makes the assertion
    // actually discriminate the lexicographic-id tie-break it is named for.
    const qint64 base = 31'000'000;
    std::deque<TimeDuration> comp;
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime::fromMSecsSinceEpoch(base, Qt::UTC),
        QDateTime::fromMSecsSinceEpoch(base + 1000, Qt::UTC), SegmentId::fromString("zzz-later-pushed-shorter")));
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime::fromMSecsSinceEpoch(base, Qt::UTC),
        QDateTime::fromMSecsSinceEpoch(base + 9000, Qt::UTC), SegmentId::fromString("aaa-first-pushed-longer")));

    Timeline t(comp, std::nullopt);
    Timeline normed = t.normalized();

    QCOMPARE(normed.completed().size(), static_cast<size_t>(1));
    QCOMPARE(normed.completed()[0].segment_id.toString(), QString("aaa-first-pushed-longer"));
    // The surviving id keeps the union's end, not just its own original end.
    QCOMPARE(normed.completed()[0].endTime.toMSecsSinceEpoch(), base + 9000);
}

void TimelineTest::test_normalized_exactDuplicate_permutations_sameSurvivor()
{
    // Two byte-identical intervals with distinct ids: the same (lexicographically
    // smaller) id must survive no matter which order they're supplied in.
    const qint64 base = 32'000'000;
    auto build = [&](bool bFirst) {
        std::deque<TimeDuration> comp;
        auto a = TimeDuration::fromPersistedRow(DurationType::Activity,
            QDateTime::fromMSecsSinceEpoch(base, Qt::UTC),
            QDateTime::fromMSecsSinceEpoch(base + 1000, Qt::UTC), SegmentId::fromString("a-id"));
        auto b = TimeDuration::fromPersistedRow(DurationType::Activity,
            QDateTime::fromMSecsSinceEpoch(base, Qt::UTC),
            QDateTime::fromMSecsSinceEpoch(base + 1000, Qt::UTC), SegmentId::fromString("b-id"));
        if (bFirst) {
            comp.push_back(std::move(b));
            comp.push_back(std::move(a));
        } else {
            comp.push_back(std::move(a));
            comp.push_back(std::move(b));
        }
        return comp;
    };

    Timeline normedAFirst = Timeline(build(false), std::nullopt).normalized();
    Timeline normedBFirst = Timeline(build(true), std::nullopt).normalized();

    QCOMPARE(normedAFirst.completed().size(), static_cast<size_t>(1));
    QCOMPARE(normedBFirst.completed().size(), static_cast<size_t>(1));
    QCOMPARE(normedAFirst.completed()[0].segment_id.toString(), QString("a-id"));
    QCOMPARE(normedBFirst.completed()[0].segment_id.toString(), QString("a-id"));
}

// ============================================================================
// D7/R04-003: Timeline never merges differing types and never guesses. A
// cross-type overlap survives normalization unmerged, unrejected, and
// undropped -- rejecting it is DayValidity/SessionStore/HistoryEditSession's
// job (see dayvalidity.h and their own tests), not Timeline's.
// ============================================================================

void TimelineTest::test_normalized_crossTypeOverlap_neverMerged_flaggedByDayValidity()
{
    const qint64 base = 33'000'000;
    std::deque<TimeDuration> comp;
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime::fromMSecsSinceEpoch(base, Qt::UTC),
        QDateTime::fromMSecsSinceEpoch(base + 20000, Qt::UTC)));
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime::fromMSecsSinceEpoch(base + 10000, Qt::UTC),
        QDateTime::fromMSecsSinceEpoch(base + 15000, Qt::UTC)));

    Timeline t(comp, std::nullopt);
    Timeline normed = t.normalized();

    // Not merged, not dropped: both rows survive exactly as given.
    QCOMPARE(normed.completed().size(), static_cast<size_t>(2));
    QCOMPARE(normed.activeMsec(), static_cast<qint64>(20000));
    QCOMPARE(normed.pauseMsec(), static_cast<qint64>(5000));

    // The shared civil-interval primitive (owned by DayValidity, reused here
    // rather than duplicated) is what actually rejects this at a write
    // boundary -- Timeline's job stops at not corrupting it further.
    QCOMPARE(DayValidity::validatePayload(normed.completed()), DayValidity::Violation::TypeOverlap);
}

// ============================================================================
// D7: normalizedWithRemovedIds() reports exactly the merged-away IDs.
// ============================================================================

void TimelineTest::test_normalizedWithRemovedIds_reportsRemoved()
{
    const qint64 base = 34'000'000;
    std::deque<TimeDuration> comp;
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime::fromMSecsSinceEpoch(base, Qt::UTC),
        QDateTime::fromMSecsSinceEpoch(base + 1000, Qt::UTC), SegmentId::fromString("id-survivor")));
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime::fromMSecsSinceEpoch(base + 1000, Qt::UTC), // touches exactly
        QDateTime::fromMSecsSinceEpoch(base + 2000, Qt::UTC), SegmentId::fromString("id-merged-away")));
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime::fromMSecsSinceEpoch(base + 5000, Qt::UTC), // large gap, untouched
        QDateTime::fromMSecsSinceEpoch(base + 6000, Qt::UTC), SegmentId::fromString("id-untouched")));

    Timeline t(comp, std::nullopt);
    NormalizedResult result = t.normalizedWithRemovedIds();

    QCOMPARE(result.timeline.completed().size(), static_cast<size_t>(2));
    QCOMPARE((int)result.removedIds.size(), 1);
    QCOMPARE(result.removedIds[0], QString("id-merged-away"));
}

// ============================================================================
// D7: normalizing an already-normalized timeline changes nothing further.
// ============================================================================

void TimelineTest::test_normalized_isIdempotent()
{
    const qint64 base = 35'000'000;
    std::deque<TimeDuration> comp;
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime::fromMSecsSinceEpoch(base, Qt::UTC),
        QDateTime::fromMSecsSinceEpoch(base + 1000, Qt::UTC), SegmentId::fromString("id-a")));
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime::fromMSecsSinceEpoch(base + 1000, Qt::UTC),
        QDateTime::fromMSecsSinceEpoch(base + 2000, Qt::UTC), SegmentId::fromString("id-b")));
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime::fromMSecsSinceEpoch(base + 3000, Qt::UTC),
        QDateTime::fromMSecsSinceEpoch(base + 4000, Qt::UTC), SegmentId::fromString("id-c")));

    Timeline once = Timeline(comp, std::nullopt).normalized();
    Timeline twice = once.normalized();

    QCOMPARE(twice.completed().size(), once.completed().size());
    for (size_t i = 0; i < once.completed().size(); ++i) {
        QCOMPARE(twice.completed()[i].segment_id, once.completed()[i].segment_id);
        QCOMPARE(twice.completed()[i].startTime, once.completed()[i].startTime);
        QCOMPARE(twice.completed()[i].endTime, once.completed()[i].endTime);
    }
}

// ============================================================================
// Test Q — groupByDate assigns segments to correct dates
// ============================================================================

void TimelineTest::test_Q_groupByDate()
{
    // Day 1: 2024-01-10
    // Qt::LocalTime, not Qt::UTC: a 09:00-10:00 UTC row is 23:30-00:30 local
    // under Pacific/Marquesas (UTC-9:30), which straddles local midnight and
    // trips Timeline's assertSameDayInvariant() Q_ASSERT -- the same defect
    // fixed in test_database.cpp's test_A4_... (commit 4c54044). Local time
    // has no UTC->local conversion left to cross a boundary.
    const QDateTime day1Start(QDate(2024, 1, 10), QTime(9, 0, 0), Qt::LocalTime);
    const QDateTime day1End(QDate(2024, 1, 10), QTime(10, 0, 0), Qt::LocalTime);
    // Day 2: 2024-01-11
    const QDateTime day2Start(QDate(2024, 1, 11), QTime(14, 0, 0), Qt::UTC);
    const QDateTime day2End(QDate(2024, 1, 11), QTime(15, 30, 0), Qt::UTC);
    const QDateTime day2Start2(QDate(2024, 1, 11), QTime(16, 0, 0), Qt::UTC);
    const QDateTime day2End2(QDate(2024, 1, 11), QTime(17, 0, 0), Qt::UTC);

    std::deque<TimeDuration> comp;
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, day1Start, day1End));
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, day2Start, day2End));
    comp.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, day2Start2, day2End2));

    Timeline t(comp, std::nullopt);
    auto byDate = t.groupByDate();

    QCOMPARE(byDate.size(), 2);
    QVERIFY(byDate.contains(QDate(2024, 1, 10)));
    QVERIFY(byDate.contains(QDate(2024, 1, 11)));

    QCOMPARE(byDate[QDate(2024, 1, 10)].size(), static_cast<size_t>(1));
    QCOMPARE(byDate[QDate(2024, 1, 10)][0].startTime, day1Start);

    QCOMPARE(byDate[QDate(2024, 1, 11)].size(), static_cast<size_t>(2));
    QCOMPARE(byDate[QDate(2024, 1, 11)][0].startTime, day2Start);
    QCOMPARE(byDate[QDate(2024, 1, 11)][1].startTime, day2Start2);
}
