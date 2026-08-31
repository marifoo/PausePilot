#ifndef HISTORYEDITSESSION_H
#define HISTORYEDITSESSION_H

#include <QDate>
#include <QHash>
#include <optional>
#include <vector>
#include <deque>
#include "types.h"
#include "timeline.h"
#include "dayvalidity.h"

class TimeTracker;

/**
 * HistoryEditSession — non-widget model for a single history-editing transaction.
 *
 * Owns all state that was previously scattered across HistoryDialog:
 *   - The page list (pendingTimelines_ + display-only cross-midnight rows per page)
 *   - Origin tracking (which segments came from in-memory session vs DB)
 *   - Legacy cross-midnight rows are display-only and dropped from the save
 *     payload (see buildSavePayload())
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
        /// C3 (R13-001/R14-001): non-None means split/type-change/ongoing-conversion
        /// produced (or left in place) an unsupported row -- invalid/non-positive/
        /// cross-day interval, duplicate id, or an Activity/Pause overlap the user's
        /// own edit created. The caller (HistoryDialog) must refuse to save rather
        /// than call replaceAll(). Two categories of out-of-contract input never
        /// reach this violation: a legacy cross-day row loaded unmodified from the
        /// database is silently dropped from the payload (not preserved, not
        /// rejected -- see buildSavePayload()), and a pre-existing cross-type
        /// overlap between two genuinely persisted (never memory-origin) rows --
        /// completed or the live ongoing segment -- loaded/held unmodified is
        /// resolved the same way (see buildSavePayload()'s pre-existing-overlap
        /// drop rule).
        DayValidity::Violation violation = DayValidity::Violation::None;
    };

    HistoryEditSession() = default;

    /// Populates pages_, pendingTimelines_, and originIsMemory_ from the
    /// TimeTracker's current state and its DB history. `today` decides page
    /// grouping/editability (which page is "today" vs. read-only history) --
    /// an explicit parameter (D2/MEDIUM-C) rather than a TimeTracker clock
    /// accessor, so this stays testable without adding engine API surface.
    void buildFromTimer(TimeTracker& timer, const QDate& today);

    /// Refreshes the ongoing segment's end-time from the engine. If the user has
    /// edited the type, the type edit is preserved while the end-time advances.
    /// Call just before save.
    void refreshOngoing(TimeTracker& timer);

    /// Builds the three timeline buckets ready for persistence and TimeTracker commit.
    /// Does NOT perform the save — the caller (HistoryDialog::accept()) owns that.
    ///
    /// Two categories of out-of-contract data loaded unmodified from the
    /// database are silently dropped here rather than surfacing a violation
    /// (plan C3 step 5: this loss is explicitly accepted for out-of-contract
    /// data, however it arose -- manual database modification, or uTimer's
    /// own recording across a wall-clock step (in either direction), e.g. an
    /// NTP correction or a DST transition; D2 owns removing the generating
    /// condition, not this method):
    ///   - a legacy single-boundary cross-midnight row (buildFromTimer() already
    ///     drops a row spanning more than one boundary; this drops the
    ///     supported one-boundary case too, instead of carrying it forward);
    ///   - a pre-existing Activity/Pause overlap between two completed rows,
    ///     or between a completed row and the live ongoing segment, resolved
    ///     by dropping one deterministically: on each conflict the later-
    ///     starting row is dropped, and on equal starts the lexicographically
    ///     larger SegmentId is dropped (the same tie-break plan section 2
    ///     uses for same-type merges). The earliest-starting row of the
    ///     candidate set always survives; a row dropped by an earlier
    ///     conflict may itself be the earlier row of a later one. The
    ///     ongoing row is NEVER the one dropped -- dropping it would end the
    ///     live session -- so a conflict against it always drops the
    ///     completed row instead.
    /// Either drop rule applies ONLY when every row involved is byte-identical
    /// to what buildFromTimer() originally loaded (or, for the ongoing row,
    /// unchanged in id and type) AND is genuinely persisted -- never
    /// memory-origin, even if unedited, since a memory-origin row is
    /// never-persisted current-session work and dropping it would be silent
    /// data loss (finding 3, C3 review). An overlap involving a row the user
    /// actually edited this session, or a memory-origin row, is left as a
    /// violation instead; preventing a user-authored overlap during editing
    /// is D9/D7 step 4's job, not this method's.
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
    bool history_load_failed_ = false;
    int history_load_skipped_ = 0;
    int history_load_repaired_ = 0;
};

#endif // HISTORYEDITSESSION_H
