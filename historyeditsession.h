#ifndef HISTORYEDITSESSION_H
#define HISTORYEDITSESSION_H

#include <QDate>
#include <QHash>
#include <optional>
#include <vector>
#include <deque>
#include "types.h"
#include "timeline.h"

class TimeTracker;

/**
 * HistoryEditSession — non-widget model for a single history-editing transaction.
 *
 * Owns all state that was previously scattered across HistoryDialog:
 *   - The page list (pendingTimelines_ + display-only cross-midnight rows per page)
 *   - Origin tracking (which segments came from in-memory session vs DB)
 *   - Cross-midnight rows preserved unchanged through save
 *
 * No widget dependency. Can be constructed and exercised in tests without a QPA platform.
 *
 * Lifetime: constructed when HistoryDialog opens, destroyed when it closes.
 * The containing dialog calls buildSavePayload() on accept to get the data to
 * persist, then calls TimeTracker::commitEditedTimeline() with the resulting timelines.
 */
class HistoryEditSession {
public:
    /**
     * A single "page" groups time entries for one calendar date.
     *   durations:     same-day editable rows (backed by pendingTimelines_[i]).
     *   crossMidnight: cross-midnight rows starting on this day (display-only).
     *   continuations: cross-midnight rows ending on this day (display-only).
     */
    struct Page {
        QString title;
        std::deque<TimeDuration> durations;         ///< same-day canonical (editable) rows
        std::deque<TimeDuration> crossMidnight;     ///< display-only canonical cross-midnight
        std::deque<TimeDuration> continuations;     ///< display-only spillover on end date
        bool isCurrent;
        QDate pageDate;                             ///< local calendar date this page represents
    };

    /**
     * Output of buildSavePayload() — the three buckets HistoryDialog hands to
     * TimeTracker::replaceAll() and TimeTracker::commitEditedTimeline().
     *
     * needsMergeConfirmation is true when normalization reduced the segment count,
     * meaning the user must confirm before the save proceeds.
     */
    struct SavePayload {
        Timeline historyTimeline;        ///< finalized rows (non-current-session)
        Timeline sessionTimeline;        ///< rows to save to DB as unfinalized (ongoing only)
        Timeline memoryTimeline;         ///< rows to commit into TimeTracker's live session (current-session + ongoing)
        bool needsMergeConfirmation;     ///< true iff normalization merged overlapping rows
    };

    HistoryEditSession() = default;

    /// Populates pages_, pendingTimelines_, originIsMemory_, crossMidnightRows_
    /// from the TimeTracker's current state and its DB history.
    void buildFromTimer(TimeTracker& timer);

    /// Refreshes the ongoing segment's end-time from the engine. If the user has
    /// edited the type, the type edit is preserved while the end-time advances.
    /// Call just before save.
    void refreshOngoing(TimeTracker& timer);

    /// Builds the three timeline buckets ready for persistence and TimeTracker commit.
    /// Does NOT perform the save — the caller (HistoryDialog::accept()) owns that.
    SavePayload buildSavePayload() const;

    /// True iff the user changed any page's rows since the session was built
    /// (type toggle, split, etc.). When false the save is a no-op and must be
    /// skipped so an editless OK never rewrites or merges untouched DB history.
    /// Compares the ongoing's type only (its end-time advances on its own and is
    /// not a user edit).
    bool hasEdits() const;

    // ---- Editing operations (pure; return new session state via mutation) ----

    /// Replaces the Timeline for page[pageIdx] (used by type-toggle checkboxes).
    void setPageTimeline(size_t pageIdx, const Timeline& tl);

    /// Registers the second half's segment_id with the same origin as the first
    /// half after a split. Called by HistoryDialog::onSplitRow().
    void registerSplitChild(const QString& childSegmentId, bool wasMemory);

    // ---- Accessors ----
    const std::vector<Page>& pages() const { return pages_; }
    const std::vector<Timeline>& pendingTimelines() const { return pendingTimelines_; }
    std::vector<Timeline>& pendingTimelines() { return pendingTimelines_; }
    const QHash<QString, bool>& originIsMemory() const { return originIsMemory_; }
    const std::deque<TimeDuration>& crossMidnightRows() const { return crossMidnightRows_; }
    size_t pageCount() const { return pages_.size(); }

    /// True iff loadHistoryFromStore() reported TransientError/FatalError in
    /// buildFromTimer() — Disabled is a legitimate empty state, not a failure.
    /// The model is then built from only whatever partial history did load,
    /// so saving it back would silently drop the rest (see HistoryDialog::saveChanges()).
    bool historyLoadFailed() const { return history_load_failed_; }
    /// Number of rows skipped / auto-repaired by the last load, for the dialog's banner.
    int historyLoadSkipped() const { return history_load_skipped_; }
    int historyLoadRepaired() const { return history_load_repaired_; }

private:
    std::vector<Page> pages_;
    std::vector<Timeline> pendingTimelines_;
    std::vector<Timeline> originalTimelines_; ///< snapshot of pendingTimelines_ at build (for hasEdits)
    QHash<QString, bool> originIsMemory_; ///< segment_id.toString() → true if memory row
    std::deque<TimeDuration> crossMidnightRows_; ///< cross-midnight rows preserved as-is through save
    bool history_load_failed_ = false;
    int history_load_skipped_ = 0;
    int history_load_repaired_ = 0;
};

#endif // HISTORYEDITSESSION_H
