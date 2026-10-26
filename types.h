#ifndef TYPES_H
#define TYPES_H

#include <optional>
#include <QDateTime>
#include "localtime.h"
#include <QString>
#include <QUuid>

enum class Button {Start, Pause, Stop};

enum class LockEvent {None, Unlock, Lock, LongOngoingLock};

enum class DurationType { Activity, Pause };

/**
 * Opaque, immutable segment identity.
 *
 * SegmentId wraps a UUID string to prevent empty-string IDs from
 * propagating undetected.  fromString() asserts non-empty in debug
 * builds; mint() always produces a fresh non-empty UUID.
 *
 * The default constructor produces an empty SegmentId (used when no
 * segment is active, e.g. Mode::None in TimeTracker).
 */
class SegmentId {
public:
    SegmentId() = default;

    static SegmentId mint() {
        return SegmentId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }

    static SegmentId fromString(const QString& s) {
        Q_ASSERT_X(!s.isEmpty(), "SegmentId::fromString", "segment_id must not be empty");
        return SegmentId(s);
    }

    bool isEmpty() const { return value_.isEmpty(); }
    const QString& toString() const { return value_; }

    bool operator==(const SegmentId& o) const { return value_ == o.value_; }
    bool operator!=(const SegmentId& o) const { return value_ != o.value_; }
    bool operator<(const SegmentId& o) const { return value_ < o.value_; }

private:
    explicit SegmentId(const QString& s) : value_(s) {}
    QString value_;
};

/// DayValidity primitives shared with dayvalidity.h. These three need only
/// QDateTime, so they live here (rather than in dayvalidity.h, which
/// includes types.h for TimeDuration and so cannot be included back from
/// here) and dayvalidity.h uses this copy instead of duplicating it.
namespace DayValidity {

/// True iff half-open intervals [aStart, aEnd) and [bStart, bEnd) overlap.
inline bool intervalsOverlap(const QDateTime& aStart, const QDateTime& aEnd,
                             const QDateTime& bStart, const QDateTime& bEnd)
{
    return aStart < bEnd && bStart < aEnd;
}

/// Start of local calendar date `date`, i.e. local midnight. Timezone-aware:
/// computed via QDateTime rather than a fixed millisecond-per-day constant, so
/// it is correct across a DST transition.
///
/// On a spring-forward day where local midnight itself does not exist (e.g.
/// America/Santiago, Asia/Beirut), constructing QDateTime(date, QTime(0,0),
/// Qt::LocalTime) directly returns an *invalid* QDateTime on this Qt version
/// -- and an invalid QDateTime sorts as less than every valid one, which would
/// silently corrupt every overlapsLocalDate() comparison against that date
/// (an unrelated interval on the previous day would wrongly appear to
/// overlap it). Fall back to deriving the start from the previous day's local
/// midnight, which normalizes forward to the correct first valid instant --
/// exactly how localDayEnd() below already gets a valid answer.
inline QDateTime localDayStart(const QDate& date)
{
    QDateTime start = LocalTime::at(date, QTime(0, 0));
    if (!start.isValid())
        start = LocalTime::at(date.addDays(-1), QTime(0, 0)).addDays(1);
    return start;
}

/// Exclusive end of local calendar date `date`, i.e. the following local
/// midnight. May be 23 or 25 hours after localDayStart(date) on a DST
/// transition day -- computed as localDayStart() of the *next* date, never
/// as localDayStart(date) plus a fixed 24-hour offset. Deliberately not
/// localDayStart(date).addDays(1): when date's own local midnight fell in a
/// spring-forward gap, localDayStart(date) already normalized forward to
/// that day's first valid instant (e.g. 01:00), and addDays(1) on a
/// wall-clock time preserves that same time-of-day on the next date instead
/// of landing on its midnight -- computing the next date's start fresh
/// avoids that drift.
inline QDateTime localDayEnd(const QDate& date)
{
    return localDayStart(date.addDays(1));
}

} // namespace DayValidity

inline bool isCrossMidnight(const QDateTime& a, const QDateTime& b);

/// Reason a TimeDuration::create() call was rejected.
/// Returned in DurationCreateResult::error; None means creation succeeded.
enum class DurationCreateError {
    None,             ///< No error — creation succeeded.
    InvalidTimestamp, ///< start or end is not a valid QDateTime.
    NonPositive,      ///< duration is zero or negative (end <= start).
    CrossMidnight     ///< end is beyond the exclusive end of start's local day (same-day invariant violated).
};

struct TimeDuration {
    SegmentId segment_id;
    DurationType type;
    QDateTime startTime;
    QDateTime endTime;

    // D7 (R03-002): duration is derived from the endpoints rather than stored,
    // so it can never disagree with startTime/endTime -- there is no separate
    // field for a caller to forget to repair after moving an endpoint.
    qint64 duration() const { return startTime.msecsTo(endTime); }

    static SegmentId createSegmentId() { return SegmentId::mint(); }

    // Factory: returns a typed result whose error field names the rejection reason.
    // On success, error == DurationCreateError::None and duration is populated.
    struct CreateResult;
    static CreateResult create(DurationType type, QDateTime start, QDateTime end,
                               SegmentId segmentId = SegmentId{});

    // For rows loaded from the DB: already validated and persisted; cross-midnight rows
    // are preserved as-is without re-running the same-day guard.
    static TimeDuration fromPersistedRow(DurationType type, QDateTime start, QDateTime end,
                                         SegmentId segmentId = SegmentId{})
    {
        return TimeDuration(type, start, end, std::move(segmentId));
    }

    // For an in-flight segment snapshot generated by the engine: start/end come from live
    // session state whose validity is guaranteed by TimeTracker's own invariants.
    static TimeDuration fromLiveSession(DurationType type, QDateTime start, QDateTime end,
                                        SegmentId segmentId = SegmentId{})
    {
        return TimeDuration(type, start, end, std::move(segmentId));
    }

private:
    // Raw constructor — bypasses create()'s validation guards (same-day, positive duration).
    // Called only by the static factories (fromPersistedRow, fromLiveSession, create).
    explicit TimeDuration(DurationType type, QDateTime start, QDateTime end, SegmentId segmentId = SegmentId{})
        : segment_id(segmentId.isEmpty() ? SegmentId::mint() : std::move(segmentId)),
          type(type), startTime(start), endTime(end) {
    }

};

/// Result of TimeDuration::create().
/// Check has_value() (or error == DurationCreateError::None) for success.
struct TimeDuration::CreateResult {
    std::optional<TimeDuration> duration; ///< Populated on success.
    DurationCreateError error = DurationCreateError::None;

    bool has_value() const { return duration.has_value(); }
    TimeDuration& value()  { return *duration; }
    const TimeDuration& value() const { return *duration; }
    TimeDuration* operator->() { return &*duration; }
    const TimeDuration* operator->() const { return &*duration; }
    TimeDuration& operator*() { return *duration; }
    const TimeDuration& operator*() const { return *duration; }
};

inline TimeDuration::CreateResult TimeDuration::create(DurationType type, QDateTime start, QDateTime end,
                                                       SegmentId segmentId)
{
    if (!start.isValid() || !end.isValid())
        return {{}, DurationCreateError::InvalidTimestamp};
    if (start.msecsTo(end) <= 0)
        return {{}, DurationCreateError::NonPositive};
    if (isCrossMidnight(start, end))
        return {{}, DurationCreateError::CrossMidnight};
    return {TimeDuration(type, start, end, std::move(segmentId)), DurationCreateError::None};
}

// Half-open same-local-day rule (D1): b may equal the exclusive end of a's
// local day (the following local midnight) without crossing into it, so an
// interval ending exactly at midnight is same-day and legal. Same
// computation as DayValidity::localDayEnd(a's local date), above.
inline bool isCrossMidnight(const QDateTime& a, const QDateTime& b)
{
    return b > DayValidity::localDayEnd(LocalTime::fromInstant(a).date());
}

/**
 * Tri-state result for hasEntriesForDate.
 *
 * Distinguishes between "definitely no entries" (safe to add boot time)
 * and "we don't know" (history disabled or DB inaccessible — never add
 * boot time to avoid double-counting).
 *
 * - Yes:     A finalized entry overlaps the given local date (D1: overlap,
 *            not merely start-date match -- see hasEntriesForDate()).
 * - No:      The DB was queried successfully and no entry overlaps.
 * - Unknown: The DB could not be queried (history disabled or open failed).
 */
enum class EntriesForDateResult { Yes, No, Unknown };

/// Minimum whole seconds a completed row must span to be splittable at all.
/// D9 review (finding 1, round 3): shared by HistoryEditSession::splitRow()/
/// splitRejection() (the authoritative rule) and SplitDialog (which disables
/// its slider below this) so the two constants cannot drift apart. Declared
/// here rather than in splitdialog.h so the model stays widget-free.
constexpr int kMinimumSplitDurationSeconds = 3;

#endif // TYPES_H
