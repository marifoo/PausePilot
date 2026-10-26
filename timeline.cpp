#include "timeline.h"
#include "logger.h"
#include <algorithm>
#include <cassert>

Timeline::Timeline()
{
}

Timeline::Timeline(std::deque<TimeDuration> completed,
                   std::optional<TimeDuration> ongoing)
    : completed_(std::move(completed)), ongoing_(std::move(ongoing))
{
#ifndef QT_NO_DEBUG
    assertSameDayInvariant();
#endif
}

Timeline Timeline::fromPersistedRows(std::deque<TimeDuration> completed,
                                     std::optional<TimeDuration> ongoing)
{
    Timeline t;
    t.completed_ = std::move(completed);
    t.ongoing_ = std::move(ongoing);
    return t;
}

const std::deque<TimeDuration>& Timeline::completed() const
{
    return completed_;
}

const std::optional<TimeDuration>& Timeline::ongoing() const
{
    return ongoing_;
}

qint64 Timeline::activeMsec() const
{
    qint64 sum = 0;
    for (const auto& d : completed_) {
        if (d.type == DurationType::Activity)
            sum += d.duration();
    }
    if (ongoing_.has_value() && ongoing_->type == DurationType::Activity)
        sum += ongoing_->duration();
    return sum;
}

qint64 Timeline::pauseMsec() const
{
    qint64 sum = 0;
    for (const auto& d : completed_) {
        if (d.type == DurationType::Pause)
            sum += d.duration();
    }
    if (ongoing_.has_value() && ongoing_->type == DurationType::Pause)
        sum += ongoing_->duration();
    return sum;
}

QMap<QDate, std::vector<TimeDuration>> Timeline::groupByDate() const
{
    QMap<QDate, std::vector<TimeDuration>> result;
    for (const auto& d : completed_) {
        result[d.startTime.date()].push_back(d);
    }
    return result;
}

Timeline Timeline::withSegmentType(size_t index, DurationType t) const
{
    assert(index < completed_.size());
    Timeline copy(*this);
    copy.completed_[index].type = t;
    return copy;
}

Timeline Timeline::withSplit(size_t index, QDateTime at,
                              DurationType first, DurationType second) const
{
    assert(index < completed_.size());
    Timeline copy(*this);
    const TimeDuration& original = copy.completed_[index];
    QDateTime origStart = original.startTime;
    QDateTime origEnd = original.endTime;
    SegmentId origId = original.segment_id;

    auto firstHalf = TimeDuration::create(first, origStart, at, origId);
    if (!firstHalf.has_value()) {
        Logger::Log("[SPLIT] Cross-day first half, aborting split");
        return *this;
    }
    auto secondHalf = TimeDuration::create(second, at, origEnd, SegmentId::mint());
    if (!secondHalf.has_value()) {
        Logger::Log("[SPLIT] Cross-day second half, aborting split");
        return *this;
    }

    copy.completed_.erase(copy.completed_.begin() + static_cast<std::ptrdiff_t>(index));
    copy.completed_.insert(copy.completed_.begin() + static_cast<std::ptrdiff_t>(index), std::move(*secondHalf));
    copy.completed_.insert(copy.completed_.begin() + static_cast<std::ptrdiff_t>(index), std::move(*firstHalf));

    return copy;
}

#ifndef QT_NO_DEBUG
void Timeline::assertSameDayInvariant() const
{
    for (const auto& d : completed_) {
        Q_ASSERT(!isCrossMidnight(d.startTime, d.endTime));
    }
}
#endif

/**
 * Classify how prev and curr should be merged during normalization.
 *
 * Precondition (established by normalized()'s sort): prev.startTime <=
 * curr.startTime. Given that, a same-type pair has exactly two reachable
 * outcomes (D7/R04-001, plan section 2's resolved policy):
 *
 *  - They overlap or touch exactly (curr starts at or before prev's end) →
 *    Merge: extend prev to cover curr, then erase curr.
 *  - There is a positive gap between them, however small → None: a positive
 *    gap is never filled.
 *
 * curr.startTime landing on a different local date than prev.startTime also
 * forces None, even on exact touch (gap == 0): each input row is already
 * individually same-day valid, so curr's own end can only be bounded by
 * *its* day's boundary, not prev's -- merging them could otherwise splice a
 * cross-midnight interval past release builds' disabled same-day assertion
 * (assertSameDayInvariant() is #ifndef QT_NO_DEBUG only). This is the one
 * case where two same-type, individually-valid rows can still touch exactly
 * at a boundary (a segment legally ending exactly at the next local
 * midnight, isCrossMidnight's half-open exception) without being mergeable.
 */
MergeDecision classifyMerge(const TimeDuration& prev, const TimeDuration& curr)
{
    if (prev.type != curr.type)
        return MergeDecision::None;
    if (LocalTime::fromInstant(prev.startTime).date() != LocalTime::fromInstant(curr.startTime).date())
        return MergeDecision::None;

    const qint64 prev_end   = prev.endTime.toMSecsSinceEpoch();
    const qint64 curr_start = curr.startTime.toMSecsSinceEpoch();

    if (curr_start > prev_end)
        return MergeDecision::None;
    return MergeDecision::Merge;
}

/// Apply a merge decision to the (prev, curr) iterator pair.
/// On Merge, extends prev to cover curr (keeping prev's identity) and erases
/// curr; returns the resulting valid iterator.
static std::deque<TimeDuration>::iterator applyMergeDecision(
    MergeDecision decision,
    std::deque<TimeDuration>::iterator prevIt,
    std::deque<TimeDuration>::iterator it,
    std::deque<TimeDuration>& durations)
{
    if (decision == MergeDecision::None)
        return std::next(it);

    if (prevIt->endTime < it->endTime)
        prevIt->endTime = it->endTime;
    return durations.erase(it);
}

Timeline Timeline::normalized() const
{
    // Precondition: input entries are sorted by (start, type, segment_id) —
    // the sort below establishes this. classifyMerge() relies on ascending
    // start order.
    //
    // Two entries that share a start and a type always overlap (both have a
    // positive duration, so curr_start == prev_start < prev_end) and
    // therefore always merge -- for that pair, segment_id is the tie-break
    // that decides which identity survives (D7/R04-002, plan section 2:
    // "equal starts use the lexicographically smaller ID" -- stated
    // unconditionally, not only for byte-identical/exact-duplicate rows, so
    // the tie-break must win regardless of which of the two has the later
    // end).
    //
    // Two entries that share a start but differ in type never merge
    // (classifyMerge() checks type first), so their relative order has no
    // survivor to decide -- but it must still be deterministic, or two
    // byte-identical inputs (e.g. an Activity/Pause pair at the same
    // instant) would come out in a different order every run depending on
    // each row's randomly-minted id. type is ordered ahead of segment_id
    // for exactly this pair, so id is never consulted for it.
    std::deque<TimeDuration> durations = completed_;

    if (durations.size() < 2) {
        return Timeline(durations, ongoing_);
    }

    std::sort(durations.begin(), durations.end(), [](const TimeDuration& a, const TimeDuration& b) {
        if (a.startTime != b.startTime) return a.startTime < b.startTime;
        if (a.type != b.type) return a.type < b.type;
        return a.segment_id.toString() < b.segment_id.toString();
    });

    for (auto it = durations.begin() + 1; it != durations.end(); ) {
        auto prevIt = std::prev(it);
        const MergeDecision decision = classifyMerge(*prevIt, *it);
        if (decision == MergeDecision::None) {
            ++it;
        } else {
            it = applyMergeDecision(decision, prevIt, it, durations);
        }
    }

    Timeline result(durations, ongoing_);
#ifndef QT_NO_DEBUG
    result.assertSameDayInvariant();
#endif
    return result;
}

NormalizedResult Timeline::normalizedWithRemovedIds() const
{
    std::vector<QString> before;
    before.reserve(completed_.size());
    for (const auto& d : completed_)
        before.push_back(d.segment_id.toString());

    Timeline normed = normalized();

    std::vector<QString> removed;
    for (const auto& id : before) {
        bool found = false;
        for (const auto& d : normed.completed_)
            if (d.segment_id.toString() == id) { found = true; break; }
        if (!found)
            removed.push_back(id);
    }

    return {normed, removed};
}
