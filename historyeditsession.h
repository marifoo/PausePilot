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
#include "historyeligibility.h"

/**
 * HistoryEditSession — non-widget model for a single history-editing transaction.
 *
 * Owns all state that was previously scattered across HistoryDialog:
 *   - The page list (pendingTimelines_ + display-only cross-midnight rows per page)
 *   - Legacy cross-midnight rows are display-only, kept out of the scoped
 *     save diff (see buildScopedSave())
 *
 * No widget dependency. Can be constructed and exercised in tests without a QPA platform.
 *
 * Lifetime: constructed when HistoryDialog opens, destroyed when it closes.
 * The containing dialog calls buildScopedSave() on accept to get the
 * date-scoped diff, then calls TimeTracker::saveHistoryEdit() with it.
 */
class HistoryEditSession {
public:
    /**
     * A single "page" groups time entries for one calendar date.
     *   label:         page heading without the entry count (see pageTitle()).
     *   crossMidnight: cross-midnight rows starting on this day (display-only).
     *   continuations: cross-midnight rows ending on this day (display-only).
     *
     * Same-day editable rows are NOT duplicated here: they live only in
     * pendingTimelines_[i] (and, for the build-time snapshot, originalTimelines_[i]).
     * D9 (R14-002): a page has one representation of its rows; the ongoing
     * segment in particular is never stored both here and separately.
     */
    struct Page {
        QString label;
        std::deque<TimeDuration> crossMidnight;     ///< display-only canonical cross-midnight
        std::deque<TimeDuration> continuations;     ///< display-only spillover on end date
        bool isCurrent;
        QDate pageDate;                             ///< local calendar date this page represents
    };

    HistoryEditSession() = default;

    /// Populates pages_, pendingTimelines_ and the per-page permission/span
    /// state from an open-time HistoryEditContext (H5/WP4), without touching
    /// TimeTracker or the store. Finalized, well-formed stored rows sharing a
    /// calendar date become that page's editable candidates; rows crossing
    /// exactly one local-midnight boundary are display-only (crossMidnight/
    /// continuations); rows crossing more than one boundary are skipped from
    /// the pages entirely -- but kept internally so buildScopedSave()'s
    /// new-cross-type-overlap check can still see them (B4). Unfinalized and
    /// malformed stored rows are never displayed. Memory rows
    /// (ctx.live.completed()) are grouped by their own date and deduplicated
    /// against stored rows by id, memory winning. The ongoing row (if any)
    /// is attached to ctx.today, read-only.
    /// pageEditable(i) is set from ctx.isEditable(pages_[i].pageDate);
    /// daySpan(i) is DayValidity::localDayStart/localDayEnd(pageDate).toUTC(),
    /// captured once here.
    void buildFromContext(const HistoryEditContext& ctx);

    /// True iff pages_[pageIdx] may be mutated: ctx.isEditable(pageDate) as
    /// captured by buildFromContext().
    bool pageEditable(size_t pageIdx) const;

    /// The half-open UTC span of pages_[pageIdx]'s calendar date, captured at
    /// buildFromContext() time. Default-constructed (invalid QDateTimes) for
    /// an out-of-range index.
    struct DaySpan {
        QDateTime startUtc;
        QDateTime endUtc;
    };
    DaySpan daySpan(size_t pageIdx) const;

    /// Output of buildScopedSave() (H5/WP4): the date-scoped diff between
    /// every editable, modified page's pending rows and its open-time
    /// snapshot, ready for TimeTracker::saveHistoryEdit()/
    /// SessionStore::saveHistoryDays(). days is empty when nothing editable
    /// changed.
    struct ScopedSaveResult {
        std::vector<HistoryDayChange> days;
        bool needsMergeConfirmation = false;
        DayValidity::Violation violation = DayValidity::Violation::None;
    };

    /// Builds the date-scoped save diff (H5/WP4). For each page that is
    /// editable (pageEditable()) and modified (pageModified()):
    ///   - E = the page's rows at open (originalTimelines_); F = its pending
    ///     rows. A row is "changed" if its id is new or any field differs
    ///     from its E counterpart.
    ///   - Restricted merge: repeatedly merges two adjacent same-type rows of
    ///     F that touch or overlap, but only when at least one is changed.
    ///     The earlier-starting row survives (ties broken by the smaller id),
    ///     is extended to cover both, and becomes changed; a merge loser that
    ///     existed in E is recorded as deleted, a brand-new loser is simply
    ///     dropped. Any merge sets needsMergeConfirmation.
    ///   - upserts = the changed rows of the post-merge F; deletedIds = the E
    ///     ids missing from the post-merge F.
    ///   - violation is the first of: DayValidity::validatePayload(upserts);
    ///     an upsert lying outside the page's daySpan(); or
    ///     DayValidity::hasNewCrossTypeOverlap(upserts, retained), where
    ///     retained is the page's untouched E survivors plus its
    ///     crossMidnight/continuations rows plus the parsed multi-boundary
    ///     snapshot rows overlapping the page's date (U3).
    /// Never mutates pending state. Does not use whole-timeline normalized()
    /// or an overlap tie-break.
    ScopedSaveResult buildScopedSave() const;

    /// True iff the user changed any page's rows since the session was built
    /// (type toggle, split, etc.). When false the save is a no-op and must be
    /// skipped so an editless OK never rewrites or merges untouched DB history.
    /// Compares the ongoing's type only (its end-time advances on its own and is
    /// not a user edit).
    bool hasEdits() const;

    /// True iff pages_[pageIdx]'s completed rows or ongoing type differ from
    /// the build-time snapshot -- the same edit-relevant fields hasEdits()
    /// checks, scoped to one page. D9 (R14-002): this is the single dirty
    /// predicate for both hasEdits() and the dialog's per-page highlight, so
    /// no second, divergent comparison exists outside the model.
    bool pageModified(size_t pageIdx) const;

    /// Number of editable rows on pages_[pageIdx]: pending completed rows plus
    /// the pending ongoing row if present. Excludes the page's display-only
    /// cross-midnight/continuation rows (see displayCount()). D9 review
    /// (finding 5): shared by the dialog's row-count and highlight rendering
    /// and by its split-target validation, so a future row class cannot make
    /// the table and the title/context-menu bounds drift apart again.
    int editableRowCount(size_t pageIdx) const;

    /// Current display row count for pages_[pageIdx]: editableRowCount() plus
    /// this page's display-only cross-midnight/continuation rows. Recomputed
    /// from pendingTimelines_ so it reflects a split or toggle immediately
    /// (D9/R14-002) instead of the count baked into the page title at build time.
    int displayCount(size_t pageIdx) const;

    /// pages_[pageIdx].label plus the current displayCount(), e.g.
    /// "Today (entries: 3)".
    QString pageTitle(size_t pageIdx) const;

    // ---- Editing operations (pure; return new session state via mutation) ----

    /// Replaces the Timeline for page[pageIdx] wholesale. D9 review: no
    /// production caller uses this anymore -- setCompletedType() and
    /// splitRow() cover the dialog's actual operations directly. Kept as a
    /// general primitive for test setup. Returns false (no-op) for an
    /// out-of-range or read-only (H5/WP4) page.
    bool setPageTimeline(size_t pageIdx, const Timeline& tl);

    /// Changes the type of the completed row identified by `segId` on
    /// page[pageIdx]. Returns false (no-op) if the id is not found on that
    /// page, or if the page is not editable (H5/WP4).
    bool setCompletedType(size_t pageIdx, const SegmentId& segId, DurationType type);

    /// Why splitRow() refused a split. D9 review (finding 3): the minimum-
    /// length rules used to live in HistoryDialog with no typed result;
    /// splitRow() now enforces them itself and reports which rule failed so
    /// the dialog can render the corresponding message without re-deriving
    /// the reason.
    enum class SplitError {
        None,          ///< split applied; only meaningful when applied == true
        OutOfRange,    ///< pageIdx/rowIdx was not a valid completed row
        TooShort,      ///< the source row is below the minimum splittable length
        HalfTooShort,  ///< `at` would leave one half below the minimum half length
        ReadOnly,      ///< pages_[pageIdx] is not editable (H5/WP4)
    };

    /// Outcome of splitRow(): the two resulting segment ids when applied, or
    /// !applied with `error` set to the reason.
    struct SplitOutcome {
        bool applied = false;
        SplitError error = SplitError::None;
        SegmentId firstId;
        SegmentId secondId;
    };

    /// Whether a split of pages_[pageIdx]'s completed row `rowIdx` is
    /// possible at all, independent of where the split point falls: OutOfRange
    /// (bad index) or TooShort (the row itself is below
    /// kMinimumSplitDurationSeconds, types.h). splitRow() checks this first;
    /// D9 review (finding 1, round 3): HistoryDialog also calls it before
    /// constructing SplitDialog, so a too-short row never opens the modal in
    /// the first place, and the two callers share one predicate.
    SplitError splitRejection(size_t pageIdx, size_t rowIdx) const;

    /// Splits pages_[pageIdx]'s completed row `rowIdx` into two at `at`, with
    /// `firstType`/`secondType` for the resulting halves.
    ///
    /// Enforces splitRejection() first, then its own per-half rule: `at` must
    /// leave at least kMinSplitHalfMsec (historyeditsession.cpp) on each
    /// side -- this one depends on `at`, so splitRejection() cannot check it.
    /// Rejects with the corresponding SplitError instead of silently
    /// clamping/adjusting.
    SplitOutcome splitRow(size_t pageIdx, size_t rowIdx, const QDateTime& at,
                           DurationType firstType, DurationType secondType);

    // ---- Accessors ----
    const std::vector<Page>& pages() const { return pages_; }
    const std::vector<Timeline>& pendingTimelines() const { return pendingTimelines_; }
    size_t pageCount() const { return pages_.size(); }

    /// True iff loadHistoryRows() reported TransientError in
    /// buildFromContext() — Disabled is a legitimate empty state, not a failure.
    /// The model is then built from only whatever partial history did load,
    /// so saving it back would silently drop the rest (see HistoryDialog::saveChanges()).
    bool historyLoadFailed() const { return history_load_failed_; }

private:
    bool isPageEditable(size_t pageIdx) const {
        return pageIdx < pageEditable_.size() && pageEditable_[pageIdx];
    }

    std::vector<Page> pages_;
    std::vector<Timeline> pendingTimelines_;
    std::vector<Timeline> originalTimelines_; ///< snapshot of pendingTimelines_ at build (for hasEdits)
    bool history_load_failed_ = false;

    /// Parallel to pages_ (H5/WP4): set by buildFromContext() from
    /// ctx.isEditable()/DayValidity.
    std::vector<bool> pageEditable_;
    std::vector<DaySpan> daySpan_;
    /// Stored rows crossing more than one local-midnight boundary (H5/WP4):
    /// skipped from pages_ entirely, but kept here so buildScopedSave()'s
    /// new-cross-type-overlap check can still see them as retained context.
    /// Populated only by buildFromContext().
    std::vector<TimeDuration> multiBoundaryStoredRows_;
};

#endif // HISTORYEDITSESSION_H
