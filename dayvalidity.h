#ifndef DAYVALIDITY_H
#define DAYVALIDITY_H

#include <QDateTime>
#include <QSet>
#include <QString>
#include <deque>
#include "types.h"

/// DayValidity -- minimal Qt-only day-validity primitive (C3, R13-001/R14-001).
///
/// Computes whether a batch of TimeDuration rows is safe to write: every row
/// must have a valid, positive interval and a non-empty, unique segment_id,
/// and no Activity interval may overlap a Pause interval. Used as the final
/// guard at uTimer's write boundaries (SqliteSessionStore's commitSession,
/// saveCheckpoint, replaceAll) and by HistoryEditSession's own save-payload
/// classification, so an invalid payload is rejected before mutation rather
/// than silently written.
///
/// Does NOT do (see D1): validate an interval against a specific requested
/// calendar date, apply DST-aware endpoint semantics, or decide boot-credit
/// eligibility. D1 extends this primitive with that logic and migrates the
/// remaining ad hoc day-membership checks (TimeDuration::create, Timeline,
/// hasEntriesForDate, ...) onto it.
namespace DayValidity {

/// Reason a payload was rejected by validatePayload(). None means acceptable.
enum class Violation {
    None,
    InvalidTimestamp,  ///< start or end is not a valid QDateTime.
    NonPositive,       ///< end <= start.
    CrossDay,          ///< start.date() != end.date().
    DuplicateId,       ///< the same segment_id appears more than once in the payload.
    TypeOverlap,       ///< an Activity interval overlaps a Pause interval.
};

/// True iff half-open intervals [aStart, aEnd) and [bStart, bEnd) overlap.
inline bool intervalsOverlap(const QDateTime& aStart, const QDateTime& aEnd,
                             const QDateTime& bStart, const QDateTime& bEnd)
{
    return aStart < bEnd && bStart < aEnd;
}

/// Validates a complete batch of rows before any of them may be written.
///
/// A row spanning local midnight is always a violation, at every write
/// boundary (commitSession, saveCheckpoint, replaceAll) -- uTimer adds no
/// split, pass-through, or repair mode for manually introduced multi-day
/// data (plan section 2). A pre-existing legacy cross-day row loaded by
/// HistoryEditSession is dropped from the save payload before it ever
/// reaches this guard (see historyeditsession.cpp's buildSavePayload()),
/// so replaceAll() never needs an exception for it.
///
/// segment_id is not checked for emptiness: TimeDuration's private
/// constructor mints a fresh id whenever the caller passes an empty one, so
/// every TimeDuration this batch can contain already has a non-empty id.
///
/// Rows are not assumed sorted or deduplicated. Returns the first violation
/// found, or Violation::None if the whole batch is acceptable.
inline Violation validatePayload(const std::deque<TimeDuration>& rows)
{
    QSet<QString> seenIds;
    for (const auto& d : rows) {
        if (!d.startTime.isValid() || !d.endTime.isValid())
            return Violation::InvalidTimestamp;
        if (d.startTime.msecsTo(d.endTime) <= 0)
            return Violation::NonPositive;
        if (isCrossMidnight(d.startTime, d.endTime))
            return Violation::CrossDay;
        const QString idStr = d.segment_id.toString();
        if (seenIds.contains(idStr))
            return Violation::DuplicateId;
        seenIds.insert(idStr);
    }

    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = i + 1; j < rows.size(); ++j) {
            if (rows[i].type != rows[j].type &&
                intervalsOverlap(rows[i].startTime, rows[i].endTime, rows[j].startTime, rows[j].endTime))
                return Violation::TypeOverlap;
        }
    }

    return Violation::None;
}

/// Human-readable reason, for log lines only (not shown to end users).
inline QString describe(Violation v)
{
    switch (v) {
    case Violation::None:             return "none";
    case Violation::InvalidTimestamp: return "invalid timestamp";
    case Violation::NonPositive:      return "non-positive interval";
    case Violation::CrossDay:         return "crosses local midnight";
    case Violation::DuplicateId:      return "duplicate segment_id";
    case Violation::TypeOverlap:      return "Activity/Pause overlap";
    }
    return QString();
}

} // namespace DayValidity

#endif // DAYVALIDITY_H
