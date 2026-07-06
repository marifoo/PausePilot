#include "historyeditsession.h"
#include "timetracker.h"
#include "logger.h"
#include <QSet>
#include <algorithm>

namespace {

bool isSameSegmentId(const TimeDuration& a, const TimeDuration& b)
{
    if (a.segment_id.isEmpty() || b.segment_id.isEmpty())
        return false;
    return a.segment_id == b.segment_id;
}

} // namespace

void HistoryEditSession::buildFromTimer(TimeTracker& timer)
{
    Timeline sessionSnapshot = timer.snapshot();
    const auto& currentDurations = sessionSnapshot.completed();
    const auto& ongoingOpt = sessionSnapshot.ongoing();
    const LoadResult loaded = timer.loadHistoryFromStore();
    const auto& historyDurations = loaded.durations;
    history_load_failed_ = loaded.status == SessionStoreResult::TransientError
        || loaded.status == SessionStoreResult::FatalError;
    history_load_skipped_ = loaded.skipped;
    history_load_repaired_ = loaded.repaired;
    const QDate today = QDate::currentDate();

    // Build a list of all current (in-memory) durations for dedup comparison.
    std::vector<TimeDuration> currentComparableDurations;
    currentComparableDurations.reserve(
        currentDurations.size() + (ongoingOpt.has_value() ? 1 : 0));
    currentComparableDurations.insert(
        currentComparableDurations.end(), currentDurations.begin(), currentDurations.end());
    if (ongoingOpt.has_value())
        currentComparableDurations.push_back(ongoingOpt.value());

    // Group historical durations by date.
    // Same-day rows go into historyByDay (editable via Timeline/pendingTimelines_).
    // Cross-midnight rows go into crossMidnightByStartDay (display-only canonical)
    // and continuationsByEndDay (display-only spillover on end date).
    QMap<QDate, std::deque<TimeDuration>> historyByDay;
    QMap<QDate, std::deque<TimeDuration>> crossMidnightByStartDay;
    QMap<QDate, std::deque<TimeDuration>> continuationsByEndDay;
    for (const auto& d : historyDurations) {
        const bool isDuplicateOfCurrent = std::any_of(
            currentComparableDurations.begin(),
            currentComparableDurations.end(),
            [&d](const TimeDuration& cur) { return isSameSegmentId(d, cur); });

        if (isDuplicateOfCurrent)
            continue;

        if (isCrossMidnight(d.startTime, d.endTime)) {
            // Segments spanning more than one calendar boundary cannot occur in
            // normal TimeTracker operation — skip them entirely.
            if (d.endTime.toLocalTime().date() > d.startTime.toLocalTime().date().addDays(1)) {
                qWarning("[HISTORY] buildFromTimer: skipping segment spanning >1 calendar boundary (segment_id: %s)",
                         qPrintable(d.segment_id.toString()));
                continue;
            }
            crossMidnightByStartDay[d.startTime.date()].push_back(d);
            continuationsByEndDay[d.endTime.date()].push_back(d);
            crossMidnightRows_.push_back(d);
            originIsMemory_.insert(d.segment_id.toString(), false);
        } else {
            historyByDay[d.startTime.date()].push_back(d);
        }
    }

    // Collect all dates that need a page.
    QSet<QDate> allDatesSet;
    for (const QDate& d : historyByDay.keys()) allDatesSet.insert(d);
    for (const QDate& d : crossMidnightByStartDay.keys()) allDatesSet.insert(d);
    for (const QDate& d : continuationsByEndDay.keys()) allDatesSet.insert(d);
    QList<QDate> historyDates = allDatesSet.values();
    std::sort(historyDates.begin(), historyDates.end(), std::greater<QDate>());

    // Build "today" page: in-memory + today's DB + ongoing.
    std::deque<TimeDuration> currentPageCompleted;
    currentPageCompleted.insert(
        currentPageCompleted.end(), currentDurations.begin(), currentDurations.end());
    for (const auto& d : currentDurations)
        originIsMemory_.insert(d.segment_id.toString(), true);

    auto todayIt = historyByDay.find(today);
    if (todayIt != historyByDay.end()) {
        currentPageCompleted.insert(
            currentPageCompleted.end(), todayIt->begin(), todayIt->end());
        for (const auto& d : *todayIt)
            originIsMemory_.insert(d.segment_id.toString(), false);
    }

    // currentPageDurations includes ongoing for the Page title count.
    std::deque<TimeDuration> currentPageDurations = currentPageCompleted;
    if (ongoingOpt.has_value())
        currentPageDurations.push_back(ongoingOpt.value());

    {
        std::deque<TimeDuration> todayCrossMidnight;
        auto cmIt = crossMidnightByStartDay.find(today);
        if (cmIt != crossMidnightByStartDay.end()) todayCrossMidnight = *cmIt;

        std::deque<TimeDuration> todayConts;
        auto contIt = continuationsByEndDay.find(today);
        if (contIt != continuationsByEndDay.end()) todayConts = *contIt;

        const int todayShown = static_cast<int>(
            currentPageDurations.size() + todayCrossMidnight.size() + todayConts.size());
        pages_.push_back({
            QString("Today (entries: ") + QString::number(todayShown) + QString(")"),
            currentPageDurations,
            todayCrossMidnight,
            todayConts,
            true,
            today
        });
    }

    // Add historical pages (one per day, most recent first, skip today).
    for (const QDate& date : historyDates) {
        if (date == today)
            continue;

        std::deque<TimeDuration> crossMidnight;
        auto cmIt = crossMidnightByStartDay.find(date);
        if (cmIt != crossMidnightByStartDay.end()) crossMidnight = *cmIt;

        std::deque<TimeDuration> conts;
        auto contIt = continuationsByEndDay.find(date);
        if (contIt != continuationsByEndDay.end()) conts = *contIt;

        const int histShown = static_cast<int>(
            historyByDay[date].size() + crossMidnight.size() + conts.size());
        pages_.push_back({
            date.toString("yyyy-MM-dd") + QString(" (entries: ") +
                QString::number(histShown) + QString(")"),
            historyByDay[date],
            crossMidnight,
            conts,
            false,
            date
        });
    }

    // Build pendingTimelines_: page 0 = completed + ongoing, pages 1..N = completed only.
    pendingTimelines_.reserve(pages_.size());
    pendingTimelines_.push_back(Timeline(currentPageCompleted, ongoingOpt));

    for (size_t i = 1; i < pages_.size(); ++i) {
        pendingTimelines_.push_back(Timeline(pages_[i].durations, std::nullopt));
        for (const auto& d : pages_[i].durations)
            originIsMemory_.insert(d.segment_id.toString(), false);
    }

    // Snapshot the freshly built timelines so hasEdits() can detect whether the
    // user changed anything before an OK-triggered save.
    originalTimelines_ = pendingTimelines_;
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
        if (!sameCompleted(pendingTimelines_[i].completed(), originalTimelines_[i].completed()))
            return true;
        const auto& curOng  = pendingTimelines_[i].ongoing();
        const auto& origOng = originalTimelines_[i].ongoing();
        if (curOng.has_value() != origOng.has_value())
            return true;
        // Only the ongoing's type is a user edit; its end-time advances on its own.
        if (curOng.has_value() && curOng->type != origOng->type)
            return true;
    }
    return false;
}

void HistoryEditSession::refreshOngoing(TimeTracker& timer)
{
    if (pendingTimelines_.empty() || !pages_[0].isCurrent)
        return;

    const auto fresh = timer.getOngoingDuration();
    const auto& cur  = pendingTimelines_[0].ongoing();
    const bool stillSameOngoing =
        fresh.has_value() && cur.has_value() &&
        fresh->segment_id == cur->segment_id;
    if (stillSameOngoing) {
        // Advance the end time from the engine snapshot while preserving any
        // type edit the user may have made to the pending ongoing segment.
        TimeDuration updated = cur.value();
        updated.endTime = fresh->endTime;
        updated.duration = updated.startTime.msecsTo(updated.endTime);
        pendingTimelines_[0] = Timeline(pendingTimelines_[0].completed(), updated);
    } else if (!fresh.has_value() && cur.has_value()) {
        // Engine has no valid ongoing (e.g. cross-midnight after a deferred
        // midnight stop). Clear it so commit() does not anchor a stale
        // start time in session_, which would orphan a checkpoint row.
        pendingTimelines_[0] = Timeline(pendingTimelines_[0].completed(), std::nullopt);
    }
}

// const: unlike the old saveChanges(), this does not clear crossMidnightRows_.
// The session is single-use (built once, committed once, then destroyed), so the
// rows never need resetting between calls.
HistoryEditSession::SavePayload HistoryEditSession::buildSavePayload() const
{
    std::optional<TimeDuration> ongoingDurationForSave;
    std::deque<TimeDuration> unifiedCompleted;

    for (size_t i = 0; i < pages_.size(); ++i) {
        const auto& comp = pendingTimelines_[i].completed();
        for (const auto& d : comp)
            unifiedCompleted.push_back(d);
        if (pages_[i].isCurrent)
            ongoingDurationForSave = pendingTimelines_[i].ongoing();
    }

    // Capture memory-origin intervals before normalization so we can re-route
    // merged rows that absorbed current-session time back into the live session.
    struct OriginInterval { DurationType type; QDateTime start, end; };
    std::vector<OriginInterval> memoryIntervals;
    for (const auto& d : unifiedCompleted) {
        auto it = originIsMemory_.find(d.segment_id.toString());
        if (it != originIsMemory_.end() && it.value())
            memoryIntervals.push_back({d.type, d.startTime, d.endTime});
    }

    // Normalize once across all buckets; detect merges for confirmation.
    // Cross-midnight rows are not included in normalization (Timeline rejects them);
    // they are preserved as-is and appended after normalization.
    const size_t preNormalizeSize = unifiedCompleted.size();
    Timeline normalised = Timeline(unifiedCompleted, std::nullopt).normalized();
    const bool needsMergeConfirmation = (normalised.completed().size() < preNormalizeSize);

    // Re-split using originIsMemory_:
    //   historyDurations    → finalized rows for DB (all completed rows)
    //   currentMemoryDurations → live session for TimeTracker commit (memory-origin rows)
    //   currentSessionDurations → unfinalized rows for DB (ongoing only)
    //
    // A9: completed rows are finalized on save regardless of origin. Writing a
    // completed memory-origin row as unfinalized would let the stale-orphan
    // policy delete it after a crash + relaunch >24 h later. Only the genuinely
    // in-flight ongoing segment stays unfinalized (appended below).
    std::deque<TimeDuration> historyDurations;
    std::deque<TimeDuration> currentMemoryDurations;
    std::deque<TimeDuration> currentSessionDurations;

    for (const auto& d : normalised.completed()) {
        auto idIt = originIsMemory_.find(d.segment_id.toString());
        Q_ASSERT(idIt != originIsMemory_.end());
        bool isMemory = (idIt != originIsMemory_.end()) && idIt.value();
        if (!isMemory) {
            // The surviving id is a history id, but the merged span may have
            // absorbed a current-session row. Keep it in the live session if
            // it overlaps a memory interval.
            for (const auto& m : memoryIntervals) {
                if (m.type == d.type && m.start < d.endTime && d.startTime < m.end) {
                    isMemory = true;
                    break;
                }
            }
        }
        // Every completed row is finalized in the DB; memory-origin rows also
        // stay in the live session for TimeTracker::commitEditedTimeline().
        historyDurations.push_back(d);
        if (isMemory) {
            currentMemoryDurations.push_back(d);
        }
    }

    // Cross-midnight rows are preserved as-is in the history bucket.
    for (const auto& d : crossMidnightRows_)
        historyDurations.push_back(d);

    // Ongoing goes into the session bucket for DB save.
    if (ongoingDurationForSave.has_value())
        currentSessionDurations.push_back(ongoingDurationForSave.value());

    return SavePayload{
        Timeline::fromPersistedRows(historyDurations),
        Timeline(currentSessionDurations, std::nullopt),
        Timeline(currentMemoryDurations, ongoingDurationForSave),
        needsMergeConfirmation
    };
}

void HistoryEditSession::setPageTimeline(size_t pageIdx, const Timeline& tl)
{
    if (pageIdx < pendingTimelines_.size())
        pendingTimelines_[pageIdx] = tl;
}

void HistoryEditSession::registerSplitChild(const QString& childSegmentId, bool wasMemory)
{
    originIsMemory_.insert(childSegmentId, wasMemory);
}
