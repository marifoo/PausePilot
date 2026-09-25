#include "historyeditsession.h"
#include "timetracker.h"
#include "logger.h"
#include <QSet>
#include <algorithm>

namespace {

// D9 review (finding 1, round 3): the total-length minimum is
// kMinimumSplitDurationSeconds (types.h), shared with SplitDialog so the two
// rules cannot drift apart. The per-half minimum has no other consumer, so
// it stays local here.
constexpr qint64 kMinSplitHalfMsec = 1000;

bool isSameSegmentId(const TimeDuration& a, const TimeDuration& b)
{
    if (a.segment_id.isEmpty() || b.segment_id.isEmpty())
        return false;
    return a.segment_id == b.segment_id;
}

} // namespace

void HistoryEditSession::buildFromTimer(TimeTracker& timer, const QDate& today)
{
    Timeline sessionSnapshot = timer.snapshot();
    const auto& currentDurations = sessionSnapshot.completed();
    const auto& ongoingOpt = sessionSnapshot.ongoing();
    const LoadResult loaded = timer.loadHistoryFromStore();
    const auto& historyDurations = loaded.durations;
    history_load_failed_ = loaded.status == SessionStoreResult::TransientError
        || loaded.status == SessionStoreResult::FatalError;

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

        // Classify by which local date(s) [startTime, endTime) overlaps, half-open
        // (D1): a segment ending exactly at local midnight belongs to its start date
        // only, not the following one.
        const QDate startDate = d.startTime.toLocalTime().date();
        if (DayValidity::overlapsLocalDate(d.startTime, d.endTime, startDate.addDays(2))) {
            // Segments spanning more than one calendar boundary cannot occur in
            // normal TimeTracker operation — skip them entirely.
            qWarning("[HISTORY] buildFromTimer: skipping segment spanning >1 calendar boundary (segment_id: %s)",
                     qPrintable(d.segment_id.toString()));
            continue;
        }
        if (DayValidity::overlapsLocalDate(d.startTime, d.endTime, startDate.addDays(1))) {
            crossMidnightByStartDay[startDate].push_back(d);
            continuationsByEndDay[startDate.addDays(1)].push_back(d);
            originIsMemory_.insert(d.segment_id.toString(), false);
        } else {
            historyByDay[startDate].push_back(d);
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

    {
        std::deque<TimeDuration> todayCrossMidnight;
        auto cmIt = crossMidnightByStartDay.find(today);
        if (cmIt != crossMidnightByStartDay.end()) todayCrossMidnight = *cmIt;

        std::deque<TimeDuration> todayConts;
        auto contIt = continuationsByEndDay.find(today);
        if (contIt != continuationsByEndDay.end()) todayConts = *contIt;

        pages_.push_back({
            QString("Today"),
            todayCrossMidnight,
            todayConts,
            true,
            today
        });
    }
    pendingTimelines_.push_back(Timeline(currentPageCompleted, ongoingOpt));

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

        std::deque<TimeDuration> dayCompleted = historyByDay[date];
        for (const auto& d : dayCompleted)
            originIsMemory_.insert(d.segment_id.toString(), false);

        pages_.push_back({
            date.toString("yyyy-MM-dd"),
            crossMidnight,
            conts,
            false,
            date
        });
        pendingTimelines_.push_back(Timeline(dayCompleted, std::nullopt));
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
        // D7 (R03-002): take the whole validated snapshot from the engine
        // (its duration is always consistent with its own endpoints) rather
        // than poking endTime on the pending copy and re-deriving duration
        // by hand.
        TimeDuration updated = fresh.value();
        updated.type = cur->type;
        pendingTimelines_[0] = Timeline(pendingTimelines_[0].completed(), updated);
    } else if (!fresh.has_value() && cur.has_value()) {
        // Engine has no valid ongoing (e.g. the engine stopped while History was
        // open: midnight, shutdown, or a fatal store error during History's own save).
        // Clear it so commit() does not anchor a stale start time in session_,
        // which would orphan a checkpoint row.
        pendingTimelines_[0] = Timeline(pendingTimelines_[0].completed(), std::nullopt);
    }
}

// const: the session is single-use (built once, committed once, then destroyed),
// so no per-call state needs resetting between calls.
HistoryEditSession::SavePayload HistoryEditSession::buildSavePayload() const
{
    std::optional<TimeDuration> ongoingDurationForSave;
    std::deque<TimeDuration> unifiedCompleted;
    int preExistingOverlapDrops = 0;

    for (size_t i = 0; i < pages_.size(); ++i) {
        const auto& comp = pendingTimelines_[i].completed();
        for (const auto& d : comp)
            unifiedCompleted.push_back(d);
        if (pages_[i].isCurrent)
            ongoingDurationForSave = pendingTimelines_[i].ongoing();
    }

    // MUST-FIX2 (user decision, review packet C3): a pre-existing Activity/
    // Pause overlap between two rows loaded unmodified from the database must
    // not fail an otherwise-unrelated save. Drop the conflicting row
    // deterministically -- on each conflict the later-starting row is
    // dropped, and on equal starts the lexicographically larger SegmentId is
    // dropped (plan section 2's same-type-merge tie-break, reused here for
    // one uniform convention). The earliest-starting row of the candidate
    // set always survives; a row dropped by an earlier conflict may itself
    // be the earlier row of a later one.
    // This applies ONLY when BOTH rows are byte-identical to what
    // buildFromTimer() originally loaded: an overlap where either row was
    // actually touched this session is left as a violation below instead,
    // since preventing a user-authored overlap during editing is D9/D7 step
    // 4's job, not this method's.
    {
        QHash<QString, TimeDuration> originalById;
        for (const auto& tl : originalTimelines_)
            for (const auto& d : tl.completed())
                originalById.insert(d.segment_id.toString(), d);

        auto isUnedited = [&originalById](const TimeDuration& d) {
            auto it = originalById.find(d.segment_id.toString());
            return it != originalById.end() && it->type == d.type &&
                it->startTime == d.startTime && it->endTime == d.endTime;
        };

        // Finding 3 (C3 review): only a row genuinely persisted to the
        // database -- never a memory-origin row, even an unedited one -- may
        // be silently dropped here. A memory-origin row is never-persisted
        // current-session work; dropping it would be silent data loss worse
        // than dropping out-of-contract legacy DB data.
        auto isDroppable = [this, &isUnedited](const TimeDuration& d) {
            auto it = originIsMemory_.find(d.segment_id.toString());
            const bool isMemory = it != originIsMemory_.end() && it.value();
            return !isMemory && isUnedited(d);
        };

        // Sort the candidate set by (startTime, id) before the pairwise pass so
        // the drop outcome is independent of unifiedCompleted's incoming order
        // (Finding 1, C3 re-review): the earliest-starting row is then always
        // visited first and so always survives, rather than that being an
        // incidental property of today's memory-then-DB-order payload layout.
        std::vector<TimeDuration> sortedCandidates(unifiedCompleted.begin(), unifiedCompleted.end());
        std::sort(sortedCandidates.begin(), sortedCandidates.end(),
            [](const TimeDuration& a, const TimeDuration& b) {
                if (a.startTime != b.startTime)
                    return a.startTime < b.startTime;
                return a.segment_id.toString() < b.segment_id.toString();
            });

        QSet<QString> droppedIds;
        for (size_t i = 0; i < sortedCandidates.size(); ++i) {
            for (size_t j = i + 1; j < sortedCandidates.size(); ++j) {
                const TimeDuration& a = sortedCandidates[i];
                const TimeDuration& b = sortedCandidates[j];
                if (a.type == b.type)
                    continue;
                const QString aId = a.segment_id.toString();
                const QString bId = b.segment_id.toString();
                if (droppedIds.contains(aId) || droppedIds.contains(bId))
                    continue;
                if (!DayValidity::intervalsOverlap(a.startTime, a.endTime, b.startTime, b.endTime))
                    continue;
                if (!isDroppable(a) || !isDroppable(b))
                    continue;
                const bool bLoses = (a.startTime < b.startTime) ||
                    (a.startTime == b.startTime && aId < bId);
                droppedIds.insert(bLoses ? bId : aId);
            }
        }

        // Finding 2 (C3 review): a pre-existing overlap between an unedited
        // completed row and the unedited live ongoing segment must also be
        // resolvable by dropping, or the ongoing row's own type/interval
        // guarantees no route to save except Cancel. The ongoing row is
        // NEVER the one dropped -- ending the live session is not an option
        // here -- only the conflicting completed row can lose, and only when
        // it is itself droppable (genuinely persisted, unedited).
        if (ongoingDurationForSave.has_value() && !originalTimelines_.empty()) {
            const auto& origOngoing = originalTimelines_[0].ongoing();
            const bool ongoingUnedited = origOngoing.has_value() &&
                origOngoing->segment_id == ongoingDurationForSave->segment_id &&
                origOngoing->type == ongoingDurationForSave->type;
            if (ongoingUnedited) {
                const TimeDuration& ong = ongoingDurationForSave.value();
                for (const auto& d : unifiedCompleted) {
                    const QString dId = d.segment_id.toString();
                    if (droppedIds.contains(dId) || d.type == ong.type)
                        continue;
                    if (!DayValidity::intervalsOverlap(d.startTime, d.endTime, ong.startTime, ong.endTime))
                        continue;
                    if (!isDroppable(d))
                        continue;
                    droppedIds.insert(dId);
                }
            }
        }

        preExistingOverlapDrops = droppedIds.size();

        if (!droppedIds.empty()) {
            std::deque<TimeDuration> kept;
            for (const auto& d : unifiedCompleted)
                if (!droppedIds.contains(d.segment_id.toString()))
                    kept.push_back(d);
            unifiedCompleted = std::move(kept);
            Logger::Log(QString("[HISTORY] buildSavePayload: dropped %1 pre-existing cross-type overlap row(s)")
                .arg(droppedIds.size()));
        }
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

    // Ongoing goes into the session bucket for DB save. Finding 1 (C3 review):
    // a backward wall-clock step (A8) can leave the ongoing row zero-length
    // (start == end) so a save re-anchors the live segment instead of ending
    // it. That zero-length row has nothing to persist as an unfinalized DB
    // row -- maybeReanchorCheckpoint() (timetracker.cpp) uses the same
    // `duration <= 0` guard for the same reason -- but it still rides along
    // in memoryTimeline.ongoing() below via ongoingDurationForSave.
    if (ongoingDurationForSave.has_value() &&
        ongoingDurationForSave->startTime.msecsTo(ongoingDurationForSave->endTime) > 0)
        currentSessionDurations.push_back(ongoingDurationForSave.value());

    // C3 (R13-001/R14-001): classify the exact rows this save would send to
    // replaceAll(). A legacy cross-midnight row never enters pendingTimelines_
    // (buildFromTimer() keeps it display-only in crossMidnight/continuations),
    // so it is simply absent from historyDurations here rather than being
    // carried forward -- this uniformly rejects any remaining cross-day/
    // invalid/duplicate-id/overlap row exactly like every other uTimer write
    // boundary.
    std::deque<TimeDuration> combinedForValidation = historyDurations;
    combinedForValidation.insert(combinedForValidation.end(),
                                  currentSessionDurations.begin(), currentSessionDurations.end());
    const DayValidity::Violation violation =
        DayValidity::validatePayload(combinedForValidation);

    return SavePayload{
        Timeline::fromPersistedRows(historyDurations),
        Timeline(currentSessionDurations, std::nullopt),
        Timeline(currentMemoryDurations, ongoingDurationForSave),
        needsMergeConfirmation,
        preExistingOverlapDrops,
        violation
    };
}

void HistoryEditSession::setPageTimeline(size_t pageIdx, const Timeline& tl)
{
    if (pageIdx < pendingTimelines_.size())
        pendingTimelines_[pageIdx] = tl;
}

bool HistoryEditSession::setCompletedType(size_t pageIdx, const SegmentId& segId, DurationType type)
{
    if (pageIdx >= pendingTimelines_.size())
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

bool HistoryEditSession::setOngoingType(size_t pageIdx, DurationType type)
{
    if (pageIdx >= pendingTimelines_.size() || !pendingTimelines_[pageIdx].ongoing().has_value())
        return false;
    TimeDuration ongoing = pendingTimelines_[pageIdx].ongoing().value();
    ongoing.type = type;
    pendingTimelines_[pageIdx] = Timeline(pendingTimelines_[pageIdx].completed(), ongoing);
    return true;
}

HistoryEditSession::SplitError HistoryEditSession::splitRejection(size_t pageIdx, size_t rowIdx) const
{
    if (pageIdx >= pendingTimelines_.size())
        return SplitError::OutOfRange;
    const auto& completed = pendingTimelines_[pageIdx].completed();
    if (rowIdx >= completed.size())
        return SplitError::OutOfRange;

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

    const QString origId = source.segment_id.toString();
    const bool wasMemory = originIsMemory_.value(origId, true);
    const size_t prevSize = completed.size();

    pendingTimelines_[pageIdx] = pendingTimelines_[pageIdx].withSplit(rowIdx, at, firstType, secondType);

    const auto& newComp = pendingTimelines_[pageIdx].completed();
    if (newComp.size() <= prevSize || rowIdx + 1 >= newComp.size())
        return SplitOutcome{false, SplitError::OutOfRange, {}, {}};

    const SegmentId secondId = newComp[rowIdx + 1].segment_id;
    registerSplitChild(secondId.toString(), wasMemory);
    return SplitOutcome{true, SplitError::None, newComp[rowIdx].segment_id, secondId};
}

void HistoryEditSession::registerSplitChild(const QString& childSegmentId, bool wasMemory)
{
    originIsMemory_.insert(childSegmentId, wasMemory);
}
