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
    void showContextMenu(const QPoint& pos);
    QString buildLoadReconciliationMessage() const;
    /// Presets a SplitDialog's first segment type from the source row before it is shown.
    void presetSplitDialog(SplitDialog& dlg, const TimeDuration& duration) const;

private slots:
    void onOlder();
    void onNewer();
    void onSplitRow();

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

    /// If true, saveChanges() skips QMessageBox::critical() on DB failure (still returns false).
    bool debugSkipCritical_ = false;
    /// C4 fix round 2 (MEDIUM-5/LOW-9): incremented each time saveChanges()'s
    /// replaceAll() failure path reaches its own critical-dialog branch,
    /// regardless of debugSkipCritical_ -- lets a test observe whether this
    /// dialog would have been shown without exec()-ing a real QMessageBox.
    int debugCriticalShownCount_ = 0;
#endif

    /**
     * Commits pending edits to the database and TimeTracker. Returns true on success.
     * Returns false (and keeps the dialog open / exits early) if:
     *   - The dialog result is not Accepted (cancelled path).
     *   - A merge confirmation was declined by the user.
     *   - The DB save failed (shows a critical error dialog).
     *
     * Called by accept() to block close on failure. Can also be called directly
     * in tests (via #define private public) to exercise the commit path explicitly.
     */
    bool saveChanges();

    /// Overrides QDialog::accept() to run saveChanges() before closing.
    /// If saveChanges() returns false the dialog stays open.
    void accept() override;
};

#endif // HISTORYDIALOG_H
