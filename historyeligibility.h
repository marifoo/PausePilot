#ifndef HISTORYELIGIBILITY_H
#define HISTORYELIGIBILITY_H

#include <QDate>
#include <QDateTime>
#include <deque>
#include "sessionstore.h"
#include "timeline.h"
#include "dayvalidity.h"
#include "types.h"

struct HistoryEditContext;

/// Pure per-date permission rule for an open History edit (H5/WP3), evaluated
/// against the open-time snapshot HistoryEditContext captured once by
/// TimeTracker::beginExclusiveEdit(). Nothing here re-reads the store or the
/// clock: every input is whatever was true at open.
namespace HistoryEligibility {

/// True iff `date` may be edited, given the open-time context `ctx` (B2).
///
/// A date is NOT editable if any of:
///   - the open-time store read failed (ctx.stored.status != Success);
///   - a live row (completed or ongoing) or a pending-commit row overlaps
///     the date;
///   - a stored row that is unfinalized, well-formed, and overlaps the date
///     exists (covers the live checkpoint, orphan rows, and rows behind
///     pending discards);
///   - date == ctx.today, and either the tracker was not stopped at open, or
///     no stored row is finalized, well-formed and fully inside today's
///     span (a cross-midnight continuation does not qualify).
///
/// Every other date is editable, past or future.
inline bool isEditable(const QDate& date, const HistoryEditContext& ctx);

} // namespace HistoryEligibility

/**
 * HistoryEditContext -- the open-time snapshot TimeTracker::beginExclusiveEdit()
 * captures under mutex_ (H5/WP3): one now_provider_() read (openedAt) and one
 * db_.loadHistoryRows() call (stored). Nothing in it is refreshed for the
 * lifetime of the dialog -- HistoryEligibility::isEditable() and
 * TimeTracker::saveHistoryEdit() consult this snapshot only, never the live
 * engine state.
 */
struct HistoryEditContext {
    QDateTime openedAt;                     ///< The one engine-clock read at open.
    QDate today;                            ///< openedAt.toLocalTime().date().
    HistoryRowsResult stored;               ///< The one store read for this dialog.
    Timeline live;                          ///< session_.durations + ongoing, built from openedAt.
    std::deque<TimeDuration> pendingCommit; ///< PendingCommit payload at open, empty if none.
    bool stopped = false;                   ///< mode_ == None at open.

    bool isEditable(const QDate& d) const { return HistoryEligibility::isEditable(d, *this); }
};

namespace HistoryEligibility {

inline bool isEditable(const QDate& date, const HistoryEditContext& ctx)
{
    if (ctx.stored.status != SessionStoreResult::Success)
        return false;

    for (const auto& d : ctx.live.completed())
        if (DayValidity::overlapsLocalDate(d.startTime, d.endTime, date))
            return false;
    if (ctx.live.ongoing() &&
        DayValidity::overlapsLocalDate(ctx.live.ongoing()->startTime, ctx.live.ongoing()->endTime, date))
        return false;

    for (const auto& d : ctx.pendingCommit)
        if (DayValidity::overlapsLocalDate(d.startTime, d.endTime, date))
            return false;

    for (const auto& row : ctx.stored.rows)
        if (!row.finalized() && row.parsed.has_value() &&
            DayValidity::overlapsLocalDate(row.parsed->startTime, row.parsed->endTime, date))
            return false;

    if (date == ctx.today) {
        if (!ctx.stopped)
            return false;
        const QDateTime dayStart = DayValidity::localDayStart(date);
        const QDateTime dayEnd = DayValidity::localDayEnd(date);
        bool hasFinalizedFullyInsideToday = false;
        for (const auto& row : ctx.stored.rows) {
            if (row.finalized() && row.parsed.has_value() &&
                row.parsed->startTime >= dayStart && row.parsed->endTime <= dayEnd) {
                hasFinalizedFullyInsideToday = true;
                break;
            }
        }
        if (!hasFinalizedFullyInsideToday)
            return false;
    }

    return true;
}

} // namespace HistoryEligibility

#endif // HISTORYELIGIBILITY_H
