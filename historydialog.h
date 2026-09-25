#ifndef HISTORYDIALOG_H
#define HISTORYDIALOG_H

#include <QDialog>
#include <QDate>
#include <QMessageBox>
#include <optional>
#include "types.h"
#include "timetracker.h"
#include "timeline.h"
#include "splitdialog.h"
#include "historyeditsession.h"

class QLabel;
class QTableWidget;
class QPushButton;

/**
 * TimerExclusiveEditGuard — RAII guard for TimeTracker::beginExclusiveEdit / endExclusiveEdit.
 *
 * Calls beginExclusiveEdit() on construction and endExclusiveEdit() on destruction,
 * so resume cannot be forgotten on accept, reject, or early-return paths.
 */
class TimerExclusiveEditGuard {
public:
    explicit TimerExclusiveEditGuard(TimeTracker& timer) : timer_(timer) {
        timer_.beginExclusiveEdit();
    }
    ~TimerExclusiveEditGuard() {
        timer_.endExclusiveEdit();
    }

    TimerExclusiveEditGuard(const TimerExclusiveEditGuard&) = delete;
    TimerExclusiveEditGuard& operator=(const TimerExclusiveEditGuard&) = delete;

private:
    TimeTracker& timer_;
};

class HistoryDialog : public QDialog
{
    Q_OBJECT

private:
    TimeTracker& timetracker_;
    const Settings& settings_;
    /// D5 review fix (finding 1, R15-002): TimeTracker::shutdownEpoch()
    /// captured at construction. saveChanges() refuses to save if this
    /// changed -- a shutdown ran (and closed/committed the live segment)
    /// while this dialog stayed open across it. Unlike TimeTracker::
    /// wasShutDown(), a later cancelShutdown() does not clear this: only a
    /// dialog opened AFTER the shutdown (and any cancellation) may save.
    const qint64 shutdownEpochAtOpen_;

    /// RAII guard: beginExclusiveEdit on construction, endExclusiveEdit on destruction.
    TimerExclusiveEditGuard exclusiveEditGuard_;

    /// WP1: set synchronously by onExclusiveEditRevoked() when the engine
    /// revokes the edit (at lock, sleep, midnight, shutdown, or when the date
    /// moves forward). saveChanges() refuses with a single check placed before
    /// Step 4, also gating Step 5's memory-only commit when nothing is written.
    bool editRevoked_ = false;

    /// Non-widget model owning all edit state: pages, pending timelines, origin tracking.
    HistoryEditSession session_;

    uint pageIndex_;
    int contextMenuRow_ = -1;
    int contextMenuPage_ = -1;

    QLabel* pageLabel_;
    QLabel* loadReconciliationLabel_;
    QTableWidget* table_;
    QPushButton* prevButton_;
    QPushButton* nextButton_;

    void setupUI();
    void updateTotalsLabel(uint idx);
    void updateTable(uint idx);
    void addReadOnlyRow(QTableWidget* table, int row, const TimeDuration& d, const QString& suffix);
    /// D9 review (finding 1): the single dirty-highlight rule, sourced only
    /// from session_.pageModified(idx). Highlighting is page-level because
    /// pageModified() itself is page-scoped: once any row on a page changed,
    /// every editable row on that page is highlighted; a per-row highlight
    /// would need a per-row dirty predicate the model does not expose. Sets
    /// or clears every editable (completed + ongoing) row's Type/Start-End/
    /// Duration cells so a toggle back to the original value un-highlights,
    /// unlike the old handler-local "always highlight, never clear" rule.
    void applyDirtyHighlight(uint idx);
    void showContextMenu(const QPoint& pos);
    QString buildLoadReconciliationMessage() const;
    /// Presets a SplitDialog's first segment type from the source row before it is shown.
    void presetSplitDialog(SplitDialog& dlg, const TimeDuration& duration) const;
    /// Shared tail of onSplitRow() and applySplit_dbg(): applies session_.splitRow()
    /// and, on success, rerenders via updateTable(idx). D9 review (finding 2):
    /// having both callers go through this means a debug-only test path
    /// cannot silently diverge from what a real split renders.
    HistoryEditSession::SplitOutcome applySplitAt(uint idx, int row, const QDateTime& at,
                                                   DurationType firstType, DurationType secondType);

private slots:
    void onOlder();
    void onNewer();
    void onSplitRow();
    /// WP1: receives exclusiveEditRevoked() from TimeTracker. Sets editRevoked_
    /// synchronously, then queues closeRevoked() to run on the next event-loop
    /// turn (so nested modals can unwind first).
    void onExclusiveEditRevoked();
    /// WP1: queued by onExclusiveEditRevoked() to run after nested modals unwind.
    /// Rejects every visible child QDialog and closes every visible child QMenu,
    /// then rejects this dialog itself.
    void closeRevoked();

public:
    explicit HistoryDialog(TimeTracker& timetracker, const Settings& settings, QWidget* parent = nullptr);
    ~HistoryDialog() override;
    QString getLoadReconciliationMessage() const;

#ifndef QT_NO_DEBUG
    // ---- Debug-build test probes ----
    /// Mutable access to the edit session for test assertions and setup.
    HistoryEditSession& editSession_dbg() { return session_; }
    const HistoryEditSession& editSession_dbg() const { return session_; }
    /// Widget accessors for smoke tests.
    QLabel* pageLabel_dbg() const { return pageLabel_; }
    QLabel* loadReconciliationLabel_dbg() const { return loadReconciliationLabel_; }
    QTableWidget* table_dbg() const { return table_; }
    /// Set the context menu row/page for split tests.
    void setContextMenuTarget_dbg(int row, int page) { contextMenuRow_ = row; contextMenuPage_ = page; }
    /// Triggers a split-row operation directly (as if the context menu was used).
    void onSplitRow_dbg() { onSplitRow(); }
    /// Updates the page totals label for the given page index.
    void updateTotalsLabel_dbg(uint idx) { updateTotalsLabel(idx); }
    /// Navigation wrappers for tests (onOlder()/onNewer() are private slots).
    void onOlder_dbg() { onOlder(); }
    void onNewer_dbg() { onNewer(); }

    /// Applies a split without showing SplitDialog (avoids offscreen QPA warnings in tests).
    /// Splits row `row` on page `page` into `firstType` / `secondType` at the midpoint.
    void applySplit_dbg(int row, int page, DurationType firstType, DurationType secondType);

    /// Performs onSplitRow()'s preset step (construct SplitDialog, preset its first
    /// segment type from the source row) and returns the dialog's resulting first
    /// segment type, without exec()/show() — so it covers the production preset wiring
    /// without realizing a window (which emits offscreen QPA warnings).
    DurationType splitDialogPresetFirstType_dbg(int row, int page);

    /// If set, saveChanges() uses this answer instead of showing QMessageBox::question()
    /// for the overlap confirmation.  Reset to nullopt after each saveChanges() call.
    std::optional<QMessageBox::StandardButton> debugOverlapAnswer_;

    /// M3: if set, saveChanges() uses this answer instead of showing
    /// QMessageBox::question() for the display-only-row/pre-existing-overlap
    /// drop confirmation (Step 2.6). Defaults to No under offscreen QPA when
    /// unset -- that dialog has nothing to dismiss it and would hang the test
    /// run instead of failing the assertion on saveChanges()'s return value.
    /// Reset to nullopt after each saveChanges() call.
    std::optional<QMessageBox::StandardButton> debugDropAnswer_;
    /// M3: the composed Step 2.6 prompt text, captured for tests to inspect
    /// which clauses (cross-midnight/overlap) were included.
    QString debugDropPromptText_;

    /// If true, saveChanges() skips QMessageBox::critical() on DB failure (still returns false).
    /// Defaults to true: under offscreen QPA a real QMessageBox::critical() has nothing to
    /// dismiss it, so an unexpected failure would hang the test run instead of failing the
    /// assertion on saveChanges()'s (unaffected) false return. A test that wants to observe
    /// the "would have shown" signal still can, via debugCriticalShownCount_. Also suppresses
    /// the M2 backup-missing warning (see debugBackupWarningShownCount_), for the same reason.
    bool debugSkipCritical_ = true;
    /// C4 fix round 2 (MEDIUM-5/LOW-9): incremented each time saveChanges()'s
    /// replaceAll() failure path reaches its own critical-dialog branch,
    /// regardless of debugSkipCritical_ -- lets a test observe whether this
    /// dialog would have been shown without exec()-ing a real QMessageBox.
    int debugCriticalShownCount_ = 0;
    /// M2: incremented each time saveChanges() shows (or, under
    /// debugSkipCritical_, would show) the backup-missing warning.
    int debugBackupWarningShownCount_ = 0;

    /// D9 review (finding 1, round 3): if true, onSplitRow() skips its
    /// QMessageBox::information()/warning() calls for a rejected split
    /// (still logs and returns normally) -- lets a test exercise the
    /// rejection path without exec()-ing a real QMessageBox under the
    /// offscreen QPA plugin.
    bool debugSkipSplitMessages_ = false;
    /// D11 (D9 debt/R09-002): the one outcome onSplitRow() reached, recorded
    /// UNCONDITIONALLY -- even when debugSkipSplitMessages_ suppresses the
    /// actual QMessageBox -- so a test can distinguish "too-short row refused
    /// before SplitDialog opened" from "SplitDialog opened", which is what
    /// the old debugSplitDialogConstructedCount_ counter could not do (it only
    /// proved the modal path was NOT taken).
    enum class DebugSplitUi { None, TooShortInfo, DialogOpened };
    DebugSplitUi debugLastSplitUi_ = DebugSplitUi::None;
    /// D11 HIGH-1 fix: the exact text argument passed to (or that would be
    /// passed to) the too-short QMessageBox::information() call, captured
    /// from the same local variable the real call uses -- so a test can
    /// assert on the actual message content, not just an opaque branch tag.
    QString debugLastSplitMessage_;
#endif

    /**
     * Commits pending edits to the database and TimeTracker. Returns true on success.
     * Returns false (and keeps the dialog open / exits early) if:
     *   - The dialog result is not Accepted (cancelled path).
     *   - The edit was revoked (exclusiveEditRevoked() signal received).
     *   - A merge confirmation was declined by the user.
     *   - The DB save failed (shows a critical error dialog).
     *
     * Called by accept() to block close on failure. Can also be called directly
     * in tests (via #define private public) to exercise the commit path explicitly.
     */
    bool saveChanges();

    /// Overrides QDialog::accept() to run saveChanges() before closing.
    /// If saveChanges() returns false the dialog stays open.
    /// If the edit is revoked before saveChanges() completes, closeRevoked()
    /// will reject the dialog on the next event-loop turn.
    void accept() override;
};

#endif // HISTORYDIALOG_H
