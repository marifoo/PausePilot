#ifndef TIMELINE_H
#define TIMELINE_H

#include <deque>
#include <optional>
#include <vector>
#include <QDate>
#include <QMap>
#include <QDateTime>
#include <QString>
#include "types.h"

struct NormalizedResult;

class Timeline {
public:
    Timeline();
    Timeline(std::deque<TimeDuration> completed,
             std::optional<TimeDuration> ongoing);

    // Factory for Timelines built from persisted DB rows: bypasses the same-day invariant
    // check because DB rows are already stored and may legitimately include cross-midnight
    // entries that must be preserved as-is.
    static Timeline fromPersistedRows(std::deque<TimeDuration> completed,
                                      std::optional<TimeDuration> ongoing = std::nullopt);

    const std::deque<TimeDuration>& completed() const;
    const std::optional<TimeDuration>& ongoing() const;

    // Sums durations by type across completed + ongoing
    qint64 activeMsec() const;
    qint64 pauseMsec() const;

    // Groups completed segments by start date
    QMap<QDate, std::vector<TimeDuration>> groupByDate() const;

    // Pure editing operations — return new Timeline, do not modify *this
    Timeline withSegmentType(size_t index, DurationType t) const;
    Timeline withSplit(size_t index, QDateTime at,
                       DurationType first, DurationType second) const;

    // Normalization: equivalent to cleanDurations from helpers.cpp
    Timeline normalized() const;

    /// Normalizes and returns both the merged timeline and the segment_id strings
    /// of completed segments that were merged away during normalization.
    NormalizedResult normalizedWithRemovedIds() const;

#ifndef QT_NO_DEBUG
    void assertSameDayInvariant() const;
#endif

private:
    std::deque<TimeDuration> completed_;
    std::optional<TimeDuration> ongoing_;
};

struct NormalizedResult {
    Timeline timeline;
    std::vector<QString> removedIds;
};

/// Merge decision returned by classifyMerge().
/// Describes what normalized() should do with the (prev, curr) pair.
///
/// D7 (R04-001/R04-003, plan section 2): normalized() sorts its input by
/// (start, type, segment_id) before scanning adjacent pairs, so
/// prev.startTime <= curr.startTime always holds here. That leaves exactly
/// two reachable outcomes for a same-type pair: they overlap or touch
/// exactly (merge, extending prev to cover curr and keeping prev's
/// identity), or they don't (a positive gap, however small, is never
/// filled -- plan section 2's resolved policy). The earlier four/seven-way
/// branch table collapsed into this because every one of its
/// "extend"/"erase" outcomes was just this one merge computed differently,
/// and its curr-starts-earlier branches were unreachable once the sort
/// guarantees ascending start order.
enum class MergeDecision {
    None, ///< No merge — types differ, or curr starts after a positive gap.
    Merge ///< prev and curr overlap or touch exactly — extend prev to cover curr, erase curr.
};

/// Classify how prev and curr should be merged in normalized().
/// Only meaningful when prev.type == curr.type and prev.startTime <= curr.startTime
/// (normalized()'s sort guarantees the latter); returns None for differing types.
MergeDecision classifyMerge(const TimeDuration& prev, const TimeDuration& curr);

#endif // TIMELINE_H
