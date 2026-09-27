#include "historyeditsession.h"
#include <QSet>
#include <algorithm>

namespace {

// D9 review (finding 1, round 3): the total-length minimum is
// kMinimumSplitDurationSeconds (types.h), shared with SplitDialog so the two
// rules cannot drift apart. The per-half minimum has no other consumer, so
// it stays local here.
constexpr qint64 kMinSplitHalfMsec = 1000;

} // namespace

void HistoryEditSession::buildFromContext(const HistoryEditContext& ctx)
{
    // Classify stored rows from the raw loadHistoryRows() snapshot: only
    // finalized, well-formed rows are candidates at all (an unfinalized or
    // malformed row is context only, never displayed -- B4).
    QMap<QDate, std::deque<TimeDuration>> historyByDay;
    QMap<QDate, std::deque<TimeDuration>> crossMidnightByStartDay;
    QMap<QDate, std::deque<TimeDuration>> continuationsByEndDay;
    std::vector<TimeDuration> multiBoundary;

    for (const auto& row : ctx.stored.rows) {
        if (!row.finalized() || !row.parsed.has_value())
            continue;
        const TimeDuration& d = row.parsed.value();
        const QDate startDate = d.startTime.toLocalTime().date();
        if (DayValidity::overlapsLocalDate(d.startTime, d.endTime, startDate.addDays(2))) {
            // Spans more than one calendar boundary: skipped from the pages
            // entirely, but kept for the new-cross-type-overlap check below.
            multiBoundary.push_back(d);
        } else if (DayValidity::overlapsLocalDate(d.startTime, d.endTime, startDate.addDays(1))) {
            crossMidnightByStartDay[startDate].push_back(d);
            continuationsByEndDay[startDate.addDays(1)].push_back(d);
        } else {
            historyByDay[startDate].push_back(d);
        }
    }
    multiBoundaryStoredRows_ = std::move(multiBoundary);

    // Memory rows (B4): grouped by their own date, deduplicated against
    // stored rows by id -- the memory row wins wherever the id appeared.
    QSet<QString> memoryIds;
    QMap<QDate, std::deque<TimeDuration>> memoryByDay;
    for (const auto& d : ctx.live.completed()) {
        memoryIds.insert(d.segment_id.toString());
        memoryByDay[d.startTime.toLocalTime().date()].push_back(d);
    }
    if (!memoryIds.isEmpty()) {
        for (auto it = historyByDay.begin(); it != historyByDay.end(); ++it) {
            std::deque<TimeDuration> kept;
            for (const auto& d : it.value())
                if (!memoryIds.contains(d.segment_id.toString()))
                    kept.push_back(d);
            it.value() = std::move(kept);
        }
    }
    for (auto it = memoryByDay.begin(); it != memoryByDay.end(); ++it)
        for (const auto& d : it.value())
            historyByDay[it.key()].push_back(d);

    // Every date that needs a page; Today always gets one, even if empty.
    QSet<QDate> allDatesSet;
    allDatesSet.insert(ctx.today);
    for (const QDate& d : historyByDay.keys()) allDatesSet.insert(d);
    for (const QDate& d : crossMidnightByStartDay.keys()) allDatesSet.insert(d);
    for (const QDate& d : continuationsByEndDay.keys()) allDatesSet.insert(d);
    QList<QDate> dates = allDatesSet.values();
    std::sort(dates.begin(), dates.end(), std::greater<QDate>());

    auto sortByStartThenId = [](std::deque<TimeDuration>& rows) {
        std::sort(rows.begin(), rows.end(), [](const TimeDuration& a, const TimeDuration& b) {
            if (a.startTime != b.startTime)
                return a.startTime < b.startTime;
            return a.segment_id.toString() < b.segment_id.toString();
        });
    };

    for (const QDate& date : dates) {
        std::deque<TimeDuration> completed = historyByDay.value(date);
        sortByStartThenId(completed);

        std::optional<TimeDuration> ongoing;
        if (date == ctx.today && ctx.live.ongoing().has_value())
            ongoing = ctx.live.ongoing();

        pages_.push_back({
            date == ctx.today ? QString("Today") : date.toString("yyyy-MM-dd"),
            crossMidnightByStartDay.value(date),
            continuationsByEndDay.value(date),
            date == ctx.today,
            date
        });
        pendingTimelines_.push_back(Timeline(completed, ongoing));
        pageEditable_.push_back(ctx.isEditable(date));
        daySpan_.push_back({DayValidity::localDayStart(date).toUTC(), DayValidity::localDayEnd(date).toUTC()});
    }

    originalTimelines_ = pendingTimelines_;
    history_load_failed_ = ctx.stored.status == SessionStoreResult::TransientError;
}

bool HistoryEditSession::pageEditable(size_t pageIdx) const
{
    return isPageEditable(pageIdx);
}

HistoryEditSession::DaySpan HistoryEditSession::daySpan(size_t pageIdx) const
{
    if (pageIdx >= daySpan_.size())
        return DaySpan{};
    return daySpan_[pageIdx];
}

namespace {

// Compares the edit-relevant fields of two completed-row sequences. The ongoing
// row is compared separately (its end-time advances on its own, so only its
// existence and type are edit-relevant).
bool sameCompleted(const std::deque<TimeDuration>& a, const std::deque<TimeDuration>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].segment_id != b[i].segment_id ||
            a[i].type != b[i].type ||
            a[i].startTime != b[i].startTime ||
            a[i].endTime != b[i].endTime)
            return false;
    }
    return true;
}

} // namespace

bool HistoryEditSession::hasEdits() const
{
    if (pendingTimelines_.size() != originalTimelines_.size())
        return true;
    for (size_t i = 0; i < pendingTimelines_.size(); ++i) {
        if (pageModified(i))
            return true;
    }
    return false;
}

bool HistoryEditSession::pageModified(size_t pageIdx) const
{
    if (pageIdx >= pendingTimelines_.size() || pageIdx >= originalTimelines_.size())
        return false;
    if (!sameCompleted(pendingTimelines_[pageIdx].completed(), originalTimelines_[pageIdx].completed()))
        return true;
    const auto& curOng  = pendingTimelines_[pageIdx].ongoing();
    const auto& origOng = originalTimelines_[pageIdx].ongoing();
    if (curOng.has_value() != origOng.has_value())
        return true;
    // Only the ongoing's type is a user edit; its end-time advances on its own.
    if (curOng.has_value() && curOng->type != origOng->type)
        return true;
    return false;
}

int HistoryEditSession::editableRowCount(size_t pageIdx) const
{
    if (pageIdx >= pendingTimelines_.size())
        return 0;
    int count = static_cast<int>(pendingTimelines_[pageIdx].completed().size());
    if (pendingTimelines_[pageIdx].ongoing().has_value())
        ++count;
    return count;
}

int HistoryEditSession::displayCount(size_t pageIdx) const
{
    if (pageIdx >= pages_.size())
        return 0;
    return editableRowCount(pageIdx)
        + static_cast<int>(pages_[pageIdx].crossMidnight.size())
        + static_cast<int>(pages_[pageIdx].continuations.size());
}

QString HistoryEditSession::pageTitle(size_t pageIdx) const
{
    if (pageIdx >= pages_.size())
        return QString();
    return pages_[pageIdx].label + QString(" (entries: ")
        + QString::number(displayCount(pageIdx)) + QString(")");
}

namespace {

bool rowFieldsEqual(const TimeDuration& a, const TimeDuration& b)
{
    return a.segment_id == b.segment_id && a.type == b.type &&
        a.startTime == b.startTime && a.endTime == b.endTime;
}

} // namespace

// const: buildScopedSave() only reads pages_/pendingTimelines_/originalTimelines_;
// the session is single-use (built once, saved once, then destroyed), so no
// per-call state needs resetting between calls.
HistoryEditSession::ScopedSaveResult HistoryEditSession::buildScopedSave() const
{
    ScopedSaveResult result;

    for (size_t i = 0; i < pages_.size(); ++i) {
        if (!isPageEditable(i) || !pageModified(i))
            continue;

        QHash<QString, TimeDuration> eById;
        for (const auto& d : originalTimelines_[i].completed())
            eById.insert(d.segment_id.toString(), d);

        struct WorkRow { TimeDuration td; bool changed; };
        std::vector<WorkRow> rows;
        for (const auto& d : pendingTimelines_[i].completed()) {
            auto it = eById.find(d.segment_id.toString());
            const bool changed = (it == eById.end()) || !rowFieldsEqual(it.value(), d);
            rows.push_back({d, changed});
        }
        std::sort(rows.begin(), rows.end(), [](const WorkRow& a, const WorkRow& b) {
            if (a.td.startTime != b.td.startTime)
                return a.td.startTime < b.td.startTime;
            return a.td.segment_id.toString() < b.td.segment_id.toString();
        });

        // Restricted merge (B4): only a same-type, touching-or-overlapping
        // adjacent pair with at least one changed row merges. The earlier-
        // starting row survives (the sort above already breaks a start-time
        // tie by the smaller id, so `rows[j]` is always the survivor) and is
        // extended to cover both; repeat until no more pairs qualify.
        bool mergedAny = true;
        while (mergedAny) {
            mergedAny = false;
            for (size_t j = 0; j + 1 < rows.size(); ++j) {
                WorkRow& a = rows[j];
                WorkRow& b = rows[j + 1];
                if (a.td.type != b.td.type)
                    continue;
                if (b.td.startTime > a.td.endTime)
                    continue; // a positive gap: no touch/overlap
                if (!a.changed && !b.changed)
                    continue;

                a.td = TimeDuration::fromPersistedRow(a.td.type, a.td.startTime,
                    std::max(a.td.endTime, b.td.endTime), a.td.segment_id);
                a.changed = true;
                rows.erase(rows.begin() + static_cast<long>(j) + 1);
                result.needsMergeConfirmation = true;
                mergedAny = true;
                break;
            }
        }

        std::deque<TimeDuration> upserts;
        QSet<QString> finalIds;
        for (const auto& r : rows) {
            finalIds.insert(r.td.segment_id.toString());
            if (r.changed)
                upserts.push_back(r.td);
        }

        std::vector<SegmentId> deletedIds;
        for (auto it = eById.begin(); it != eById.end(); ++it)
            if (!finalIds.contains(it.key()))
                deletedIds.push_back(it.value().segment_id);

        HistoryDayChange change;
        change.date = pages_[i].pageDate;
        change.dayStartUtc = daySpan_[i].startUtc;
        change.dayEndUtc = daySpan_[i].endUtc;
        change.deletedIds = std::move(deletedIds);
        change.upserts = upserts;

        if (result.violation == DayValidity::Violation::None)
            result.violation = DayValidity::validatePayload(upserts);

        if (result.violation == DayValidity::Violation::None) {
            for (const auto& u : upserts) {
                if (u.startTime < daySpan_[i].startUtc || u.endTime > daySpan_[i].endUtc) {
                    result.violation = DayValidity::Violation::CrossDay;
                    break;
                }
            }
        }

        if (result.violation == DayValidity::Violation::None) {
            std::deque<TimeDuration> retained;
            for (const auto& r : rows)
                if (!r.changed)
                    retained.push_back(r.td);
            for (const auto& d : pages_[i].crossMidnight)
                retained.push_back(d);
            for (const auto& d : pages_[i].continuations)
                retained.push_back(d);
            for (const auto& d : multiBoundaryStoredRows_)
                if (DayValidity::overlapsLocalDate(d.startTime, d.endTime, pages_[i].pageDate))
                    retained.push_back(d);

            if (DayValidity::hasNewCrossTypeOverlap(upserts, retained))
                result.violation = DayValidity::Violation::TypeOverlap;
        }

        result.days.push_back(std::move(change));
    }

    return result;
}

bool HistoryEditSession::setPageTimeline(size_t pageIdx, const Timeline& tl)
{
    if (pageIdx >= pendingTimelines_.size() || !isPageEditable(pageIdx))
        return false;
    pendingTimelines_[pageIdx] = tl;
    return true;
}

bool HistoryEditSession::setCompletedType(size_t pageIdx, const SegmentId& segId, DurationType type)
{
    if (pageIdx >= pendingTimelines_.size() || !isPageEditable(pageIdx))
        return false;
    const auto& comp = pendingTimelines_[pageIdx].completed();
    auto it = std::find_if(comp.begin(), comp.end(),
        [&segId](const TimeDuration& td) { return td.segment_id == segId; });
    if (it == comp.end())
        return false;
    const size_t rowIdx = static_cast<size_t>(std::distance(comp.begin(), it));
    pendingTimelines_[pageIdx] = pendingTimelines_[pageIdx].withSegmentType(rowIdx, type);
    return true;
}

HistoryEditSession::SplitError HistoryEditSession::splitRejection(size_t pageIdx, size_t rowIdx) const
{
    if (pageIdx >= pendingTimelines_.size())
        return SplitError::OutOfRange;
    const auto& completed = pendingTimelines_[pageIdx].completed();
    if (rowIdx >= completed.size())
        return SplitError::OutOfRange;
    if (!isPageEditable(pageIdx))
        return SplitError::ReadOnly;

    const TimeDuration& source = completed[rowIdx];
    if (source.startTime.secsTo(source.endTime) < kMinimumSplitDurationSeconds)
        return SplitError::TooShort;

    return SplitError::None;
}

HistoryEditSession::SplitOutcome HistoryEditSession::splitRow(size_t pageIdx, size_t rowIdx,
    const QDateTime& at, DurationType firstType, DurationType secondType)
{
    // D9 review (finding 1, round 3): shares the one OutOfRange/TooShort
    // predicate with HistoryDialog's pre-dialog check, so the two callers
    // (and any future one) cannot classify the same row differently.
    const SplitError rejection = splitRejection(pageIdx, rowIdx);
    if (rejection != SplitError::None)
        return SplitOutcome{false, rejection, {}, {}};

    const auto& completed = pendingTimelines_[pageIdx].completed();
    const TimeDuration& source = completed[rowIdx];
    const QDateTime start = source.startTime;
    const QDateTime end = source.endTime;

    // The per-half minimum depends on `at`, which splitRejection() does not
    // take, so it stays here rather than folding into that shared predicate.
    const qint64 firstMsec = start.msecsTo(at);
    const qint64 secondMsec = at.msecsTo(end);
    if (firstMsec < kMinSplitHalfMsec || secondMsec < kMinSplitHalfMsec)
        return SplitOutcome{false, SplitError::HalfTooShort, {}, {}};

    const size_t prevSize = completed.size();

    pendingTimelines_[pageIdx] = pendingTimelines_[pageIdx].withSplit(rowIdx, at, firstType, secondType);

    const auto& newComp = pendingTimelines_[pageIdx].completed();
    if (newComp.size() <= prevSize || rowIdx + 1 >= newComp.size())
        return SplitOutcome{false, SplitError::OutOfRange, {}, {}};

    const SegmentId secondId = newComp[rowIdx + 1].segment_id;
    return SplitOutcome{true, SplitError::None, newComp[rowIdx].segment_id, secondId};
}
