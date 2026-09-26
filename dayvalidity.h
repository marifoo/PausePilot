#ifndef DAYVALIDITY_H
#define DAYVALIDITY_H

#include <QDateTime>
#include <QSet>
#include <QString>
#include <algorithm>
#include <deque>
#include "types.h"

/// DayValidity -- minimal Qt-only day-validity primitive (C3, R13-001/R14-001;
/// extended by D1, R16-002/R08-004/R13-002/R04-004).
///
/// Computes whether a batch of TimeDuration rows is safe to write: every row
/// must have a valid, positive interval and a non-empty, unique segment_id,
/// and no Activity interval may overlap a Pause interval. Used as the final
/// guard at uTimer's write boundaries (SqliteSessionStore's commitSession,
/// saveCheckpoint, replaceAll) and by HistoryEditSession's own save-payload
/// classification, so an invalid payload is rejected before mutation rather
/// than silently written.
///
/// D1 additionally provides exact overlap of an interval against a requested
/// local calendar date (localDayStart/localDayEnd/overlapsLocalDate) and the
/// clipped-portion helper UI totals need (clippedMsecInLocalDate).
/// SqliteSessionStore's hasEntriesForDate, boot-credit eligibility in
/// TimeTracker, HistoryDialog's per-page totals clipping, and
/// HistoryEditSession's page grouping call these directly. localDayStart and
/// localDayEnd themselves live in types.h (this namespace is reopened here),
/// since types.h's isCrossMidnight (used by TimeDuration::create, the
/// validatePayload above, Timeline legality, and TimeTracker's day-boundary
/// watcher) needs them and this header already includes types.h -- so there
/// is one definition, not a mirrored copy. All of it uses timezone-aware
/// QDateTime, never a fixed 24-hour/86400000ms constant, so a 23- or 25-hour
/// DST transition day is handled correctly.
namespace DayValidity {

/// Reason a payload was rejected by validatePayload(). None means acceptable.
enum class Violation {
    None,
    InvalidTimestamp,  ///< start or end is not a valid QDateTime.
    NonPositive,       ///< end <= start.
    CrossDay,          ///< end is beyond the exclusive end of start's local day.
    DuplicateId,       ///< the same segment_id appears more than once in the payload.
    TypeOverlap,       ///< an Activity interval overlaps a Pause interval.
};

// intervalsOverlap, localDayStart and localDayEnd live in types.h (this
// namespace is reopened here) -- they need only QDateTime, so keeping them
// there lets both this header and types.h's isCrossMidnight share one
// definition instead of two.

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

/// True iff any row in `changed` overlaps a row of a DIFFERENT type in
/// `retained` (U3: a new or edited History row must not create a NEW
/// Activity/Pause overlap with an untouched row). Shared by
/// SqliteSessionStore::saveHistoryDays()'s new-conflict check (H5/WP2) and
/// HistoryEditSession::buildScopedSave()'s per-page violation check, so both
/// use one definition of "new cross-type overlap" instead of two. Overlaps
/// between two rows already in `retained` are never examined -- only pairs
/// involving at least one row from `changed`.
inline bool hasNewCrossTypeOverlap(const std::deque<TimeDuration>& changed,
                                    const std::deque<TimeDuration>& retained)
{
    for (const auto& c : changed) {
        for (const auto& r : retained) {
            if (c.type != r.type && intervalsOverlap(c.startTime, c.endTime, r.startTime, r.endTime))
                return true;
        }
    }
    return false;
}

/// True iff half-open interval [start, end) overlaps local calendar date
/// `date` at all. An interval ending exactly at local midnight contributes
/// nothing to the following day (half-open semantics, via intervalsOverlap).
inline bool overlapsLocalDate(const QDateTime& start, const QDateTime& end, const QDate& date)
{
    return intervalsOverlap(start, end, localDayStart(date), localDayEnd(date));
}

/// Milliseconds of [start, end) that fall within local calendar date `date`,
/// clipped to the date's bounds. 0 when there is no overlap.
inline qint64 clippedMsecInLocalDate(const QDateTime& start, const QDateTime& end, const QDate& date)
{
    const QDateTime dayStart = localDayStart(date);
    const QDateTime dayEnd = localDayEnd(date);
    const QDateTime s = std::max(start, dayStart);
    const QDateTime e = std::min(end, dayEnd);
    return s < e ? s.msecsTo(e) : 0;
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
