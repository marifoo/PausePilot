/**
 * HistoryDialog - Interface for viewing and editing past time logs.
 *
 * Architecture:
 * - Transactional Editing: All changes (type toggling, splitting) are performed
 *   on local copies inside HistoryEditSession (session_). The actual database and
 *   TimeTracker are only updated when the user clicks "OK" and saveChanges() succeeds.
 * - Paging Model (H5/WP5): session_.buildFromContext() groups pages from the
 *   open-time HistoryEditContext captured by beginExclusiveEdit(). Pages are
 *   sorted newest-first, so Today is NOT necessarily page 0 (a future-dated
 *   page sorts ahead of it); use pages()[i].isCurrent/pageDate, never an
 *   assumed index. A page is editable exactly when session_.pageEditable(i)
 *   is true (H5/WP2's per-date permission rule); the ongoing row (if any) is
 *   always read-only and never gets a type box.
 * - Safety: Checkpoints are paused while this dialog is open via
 *   TimerExclusiveEditGuard (beginExclusiveEdit on construction, endExclusiveEdit
 *   on destruction — guaranteed on all exit paths).
 * - Close blocking: accept() runs saveChanges() before closing. A failed DB
 *   save or a declined merge confirmation leaves the dialog open so the user
 *   can correct the situation or cancel explicitly.
 * - Date-scoped save (H5/WP5): saveChanges() builds session_.buildScopedSave()'s
 *   per-date diff and persists it via TimeTracker::saveHistoryEdit(), instead
 *   of the old whole-table replaceAll(). Conflict (history changed underneath
 *   this dialog since open) and any other failure both keep the edits and
 *   leave the dialog open; neither ever partially writes.
 * - Close on edit revoke (WP1): when the engine revokes the edit (at lock,
 *   sleep, midnight, shutdown, or when the date moves forward),
 *   onExclusiveEditRevoked() sets editRevoked_, queues closeRevoked() to
 *   reject child dialogs/menus, then rejects History itself. saveChanges()
 *   checks editRevoked_ twice: once right after the result() guard, so an
 *   accept() landing before the queued closeRevoked() runs cannot slip past
 *   the no-edit early return, and again immediately before persisting, since
 *   the Step 3 merge-confirmation prompt runs a nested event loop that can
 *   revoke after the early check.
 */

#include "historydialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QCheckBox>
#include <QPushButton>
#include <QMenu>
#include <QMessageBox>
#include <QApplication>
#include "timeformat.h"
#include "logger.h"
#include "dayvalidity.h"
#include <QSlider>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QGroupBox>
#include <QButtonGroup>
#include <algorithm>
#include <QHash>

HistoryDialog::HistoryDialog(TimeTracker& timetracker, const Settings& settings, QWidget* parent)
    : QDialog(parent)
    , timetracker_(timetracker)
    , settings_(settings)
    , shutdownEpochAtOpen_(timetracker.shutdownEpoch())
    , exclusiveEditGuard_(timetracker)   // begins exclusive edit here
    , pageIndex_(0)
{
    session_.buildFromContext(exclusiveEditGuard_.context());
    setupUI();
    updateTable(pageIndex_);

    // Enable right-click context menu for split functionality
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested, this, &HistoryDialog::showContextMenu);

    // WP1: connect exclusiveEditRevoked signal. Direct connection ensures
    // editRevoked_ is set synchronously before any nested modals unwind.
    connect(&timetracker_, &TimeTracker::exclusiveEditRevoked, this,
            &HistoryDialog::onExclusiveEditRevoked, Qt::DirectConnection);

    // Log initial state for debugging
    Logger::Log("[HISTORY] Dialog opened");
    {
        const auto& pages = session_.pages();
        for (size_t i = 0; i < pages.size(); ++i) {
            Logger::Log(QString("[HISTORY] Page %1 - Title: %2, IsCurrent: %3")
                .arg(i)
                .arg(session_.pageTitle(i))
                .arg(pages[i].isCurrent));
        }
    }
}

HistoryDialog::~HistoryDialog()
{
    Logger::Log("[HISTORY] Dialog closing");

    // Clean up cell widgets to prevent memory leaks
    if (table_) {
        for (int row = 0; row < table_->rowCount(); ++row) {
            QWidget* cellWidget = table_->cellWidget(row, 3);
            if (cellWidget) {
                cellWidget->disconnect();
                table_->removeCellWidget(row, 3);
            }
        }
    }
    // exclusiveEditGuard_ destructor calls endExclusiveEdit() automatically.
}

void HistoryDialog::accept()
{
    // Temporarily set result to Accepted so saveChanges() does not short-circuit
    // on the result() guard (which exists for direct test calls with done(Rejected)).
    // Reset to 0 if the commit fails so the dialog stays open.
    setResult(QDialog::Accepted);
    if (!saveChanges()) {
        setResult(0);
        return;
    }
    // saveChanges() succeeded — call QDialog::accept() to hide and finalize.
    QDialog::accept();
}

/**
 * Commits pending edits via the date-scoped save (H5/WP5).
 *
 * Returns true on success or on a true no-op, false if the save failed, the
 * user declined a merge confirmation, or the result is not Accepted (dialog
 * stays open in every false case).
 *
 * Strategy:
 * 1. Check result() — return false if Rejected (cancel path).
 * 1a. Check editRevoked_ — return false if the edit was revoked (early check;
 *     see the class-level "Close on edit revoke" note above).
 * 1b. Check the shutdown epoch and the earlier history-load-failed state.
 * 2. If there are no edits at all, the save is a true no-op.
 * 3. Build the per-date diff via session_.buildScopedSave().
 *    - A violation shows "Invalid Entry" and refuses.
 *    - An empty days list (edits that scope to no actual change) is also a
 *      true no-op.
 *    - A merge confirmation asks the user; abort on No.
 * 4. Re-check editRevoked_ (the Step 3 prompt runs a nested event loop that
 *    can revoke after the early check).
 * 5. Persist via TimeTracker::saveHistoryEdit(). Conflict and any other
 *    non-Success category both keep the edits and refuse; only Success closes
 *    the dialog.
 */
bool HistoryDialog::saveChanges()
{
    if (result() != QDialog::Accepted) {
        Logger::Log("[HISTORY] Dialog cancelled, discarding changes");
        return false;
    }

    // WP1 (H1 fix): if the edit was revoked, refuse immediately -- even for
    // an editless dialog. onExclusiveEditRevoked() sets editRevoked_
    // synchronously (direct connection) but only queues closeRevoked() to
    // reject the dialog; an accept() landing in the same event turn, before
    // that queued reject runs, must not slip through the no-edit early
    // return below and finish Accepted. The late check further down stays
    // for edited sessions, since the Step 3 prompt runs a nested event loop
    // that can revoke after this early check.
    if (editRevoked_) {
        Logger::Log("[HISTORY] Edit was revoked; discarding changes");
        return false;
    }

    // D5 review fix (finding 1, R15-002): if the engine was shut down while
    // this dialog stayed open (e.g. a cancelled OS shutdown ran the save
    // path, closing/committing the live segment), a save here would act on a
    // stale open-time snapshot. Per-dialog, not global: cancelShutdown()
    // clears TimeTracker::wasShutDown() for everyone, but shutdownEpoch() is
    // monotonic, so a dialog that spans the shutdown stays refused even
    // after cancellation, while one opened afterward saves normally.
    if (timetracker_.shutdownEpoch() != shutdownEpochAtOpen_) {
        Logger::Log("[HISTORY] Engine was shut down while this dialog was open; discarding changes");
        return false;
    }

    // A failed history load means the model is incomplete; refuse so the
    // user can cancel and retry rather than saving from a partial snapshot.
    if (session_.historyLoadFailed()) {
        Logger::Log("[HISTORY] History load failed earlier; refusing to save edits");
#ifndef QT_NO_DEBUG
        if (!debugSkipCritical_) {
            QMessageBox::critical(this, "History Unavailable",
                "History could not be loaded from the database, so changes cannot "
                "be saved safely. Close this dialog and try again.");
        }
#else
        QMessageBox::critical(this, "History Unavailable",
            "History could not be loaded from the database, so changes cannot "
            "be saved safely. Close this dialog and try again.");
#endif
        return false;
    }

    // If the user made no edits, the save is a true no-op. Skip it entirely
    // so an editless OK never touches the store.
    if (!session_.hasEdits()) {
        Logger::Log("[HISTORY] No edits; skipping save (no-op)");
        return true;
    }

    // Build the date-scoped diff against the open-time snapshot.
    const HistoryEditSession::ScopedSaveResult scoped = session_.buildScopedSave();

    if (scoped.violation != DayValidity::Violation::None) {
        Logger::Log("[HISTORY] buildScopedSave rejected: " + DayValidity::describe(scoped.violation));
#ifndef QT_NO_DEBUG
        if (!debugSkipCritical_) {
            QMessageBox::critical(this, "Invalid Entry",
                "This change cannot be saved because it contains an invalid time entry.");
        }
#else
        QMessageBox::critical(this, "Invalid Entry",
            "This change cannot be saved because it contains an invalid time entry.");
#endif
        return false;
    }

    // H5/WP5: an editable page can be "modified" (hasEdits() true) yet scope
    // to no actual change, e.g. a toggle back to its original value. Treat
    // this like no edits -- it is not a failure.
    if (scoped.days.empty()) {
        Logger::Log("[HISTORY] No scoped changes after diffing; skipping save (no-op)");
        return true;
    }

    // Confirm if the diff merged overlapping same-type rows.
    if (scoped.needsMergeConfirmation) {
#ifndef QT_NO_DEBUG
        if (debugOnMergePromptShown_)
            debugOnMergePromptShown_();
        // Default to No (abort the save) rather than exec()-ing a real
        // QMessageBox::question() when a test hits this path without setting
        // debugOverlapAnswer_ -- under offscreen QPA that dialog has nothing to
        // dismiss it and would hang the run instead of failing the assertion on
        // saveChanges()'s return value.
        QMessageBox::StandardButton answer = QMessageBox::No;
        if (debugOverlapAnswer_.has_value()) {
            answer = *debugOverlapAnswer_;
            debugOverlapAnswer_ = std::nullopt;
        }
#else
        const auto answer = QMessageBox::question(this, "Overlapping Segments",
            "Overlapping segments were detected and will be merged. Continue?",
            QMessageBox::Yes | QMessageBox::No);
#endif
        if (answer != QMessageBox::Yes) {
            return false;
        }
    }

    // WP1: if the edit was revoked (at lock, sleep, midnight, shutdown, or
    // when the date moves forward, or now also any mode change from WP7c),
    // refuse the save and discard all edits. The Step 3 prompt above runs a
    // nested event loop that can revoke after the early check.
    if (editRevoked_) {
        Logger::Log("[HISTORY] Edit was revoked; discarding changes");
        return false;
    }

    // Persist the scoped diff.
    const SessionStoreResult dbResult = timetracker_.saveHistoryEdit(exclusiveEditGuard_.context(), scoped.days);
    if (dbResult.ok()) {
        Logger::Log("[HISTORY] Successfully saved scoped history edit");
        return true;
    }

    if (dbResult.category == SessionStoreResult::Conflict) {
        Logger::Log("[HISTORY] saveHistoryEdit: Conflict: " + dbResult.message);
#ifndef QT_NO_DEBUG
        ++debugConflictShownCount_;
        if (debugOnSaveErrorShown_)
            debugOnSaveErrorShown_();
        if (!debugSkipCritical_) {
            QMessageBox::critical(this, "History Changed",
                "History changed since this window was opened. Your edits were not "
                "saved. Close and reopen History to review the current data.");
        }
#else
        QMessageBox::critical(this, "History Changed",
            "History changed since this window was opened. Your edits were not "
            "saved. Close and reopen History to review the current data.");
#endif
        return false;
    }

    Logger::Log("[HISTORY] saveHistoryEdit failed: " + dbResult.message);
#ifndef QT_NO_DEBUG
    ++debugCriticalShownCount_;
    if (debugOnSaveErrorShown_)
        debugOnSaveErrorShown_();
    if (!debugSkipCritical_) {
        QMessageBox::critical(this, "Database Error",
            "Could not save your changes to the database. Your edits are still "
            "here -- you can retry or cancel.");
    }
#else
    QMessageBox::critical(this, "Database Error",
        "Could not save your changes to the database. Your edits are still "
        "here -- you can retry or cancel.");
#endif
    return false;
}

void HistoryDialog::onExclusiveEditRevoked()
{
    // WP1: set editRevoked_ synchronously so any prompts in-flight
    // (QMessageBox::question, QMenu::exec) can unwind and be seen by
    // closeRevoked(). Then queue closeRevoked() to run after they unwind.
    editRevoked_ = true;
    Logger::Log("[HISTORY-EDIT] revoked");
    QMetaObject::invokeMethod(this, &HistoryDialog::closeRevoked, Qt::QueuedConnection);
}

void HistoryDialog::closeRevoked()
{
    // WP1: close all visible child dialogs and menus so they unwind before
    // this dialog itself closes, then reject this dialog.
    // Walk through all top-level dialogs; find those parented to this dialog.
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        QDialog* dlg = qobject_cast<QDialog*>(widget);
        if (dlg && dlg != this && dlg->isVisible() && dlg->parentWidget() == this) {
            dlg->reject();
        }
        QMenu* menu = qobject_cast<QMenu*>(widget);
        if (menu && menu->isVisible()) {
            // Check if the menu is a child of this dialog or any child dialog
            QWidget* w = menu->parentWidget();
            while (w) {
                if (w == this) {
                    menu->close();
                    break;
                }
                w = w->parentWidget();
            }
        }
    }
    reject();
}

void HistoryDialog::setupUI()
{
    setWindowTitle("History");
    QVBoxLayout* layout = new QVBoxLayout(this);

    // Page title and totals label
    pageLabel_ = new QLabel(this);
    layout->addWidget(pageLabel_);

    loadReconciliationLabel_ = new QLabel(this);
    loadReconciliationLabel_->setWordWrap(true);
    loadReconciliationLabel_->setStyleSheet("QLabel { color: #9A6B00; }");
    const QString reconciliationMessage = buildLoadReconciliationMessage();
    loadReconciliationLabel_->setText(reconciliationMessage);
    loadReconciliationLabel_->setVisible(!reconciliationMessage.isEmpty());
    layout->addWidget(loadReconciliationLabel_);

    // Main table showing duration entries
    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({ "Type      ", "Start - End   ", "Duration   ", "Activity   " });
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table_);

    // Navigation buttons for page switching
    QHBoxLayout* navLayout = new QHBoxLayout();
    prevButton_ = new QPushButton("Previous", this);
    nextButton_ = new QPushButton("Next", this);
    navLayout->addWidget(prevButton_);
    navLayout->addWidget(nextButton_);
    layout->addLayout(navLayout);

    QHBoxLayout* buttonLayout = new QHBoxLayout();
    QPushButton* okButton = new QPushButton("OK", this);
    okButton->setEnabled(!session_.historyLoadFailed());
    QPushButton* cancelButton = new QPushButton("Cancel", this);
    buttonLayout->addStretch();
    buttonLayout->addWidget(okButton);
    buttonLayout->addWidget(cancelButton);
    layout->addLayout(buttonLayout);

    connect(prevButton_, &QPushButton::clicked, this, &HistoryDialog::onOlder);
    connect(nextButton_, &QPushButton::clicked, this, &HistoryDialog::onNewer);
    connect(okButton, &QPushButton::clicked, this, &HistoryDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    resize(400, 400);
}

/**
 * Builds the banner message shown when loadDurations encountered corrupt rows.
 */
QString HistoryDialog::buildLoadReconciliationMessage() const
{
    if (session_.historyLoadFailed())
        return "History could not be loaded — showing today only. Saving is disabled.";
    return QString();
}

QString HistoryDialog::getLoadReconciliationMessage() const
{
    return buildLoadReconciliationMessage();
}

void HistoryDialog::updateTotalsLabel(uint idx)
{
    const auto& pages = session_.pages();
    const auto& pendingTimelines = session_.pendingTimelines();

    if (idx >= pendingTimelines.size() || idx >= pages.size()) {
        Logger::Log(QString("[HISTORY] Error: updateTotalsLabel: Invalid index %1").arg(idx));
        return;
    }

    qint64 totalActivity = pendingTimelines[idx].activeMsec();
    qint64 totalPause    = pendingTimelines[idx].pauseMsec();

    const QDate& pageDate = pages[idx].pageDate;
    auto portionInDay = [&](const TimeDuration& d) -> qint64 {
        return DayValidity::clippedMsecInLocalDate(d.startTime.toLocalTime(), d.endTime.toLocalTime(), pageDate);
    };
    for (const auto& d : pages[idx].crossMidnight)
        (d.type == DurationType::Activity ? totalActivity : totalPause) += portionInDay(d);
    for (const auto& d : pages[idx].continuations)
        (d.type == DurationType::Activity ? totalActivity : totalPause) += portionInDay(d);

    QString totalsStr = QString("\nActivity: ") + convMSecToTimeStr(totalActivity)
                      + QString("  Pause: ") + convMSecToTimeStr(totalPause);
    // H5/WP5: a read-only page (Today while running, or any date the open-time
    // permission rule excluded) gets a " (read-only)" title suffix.
    QString title = session_.pageTitle(idx);
    if (!session_.pageEditable(idx))
        title += " (read-only)";
    pageLabel_->setText(title + totalsStr);
}

void HistoryDialog::addReadOnlyRow(QTableWidget* table, int row, const TimeDuration& d, const QString& suffix)
{
    const QColor grayBg(200, 200, 200, 180);
    QString typeStr = (d.type == DurationType::Activity ? "Activity  " : "Pause  ");
    typeStr += suffix;
    QTableWidgetItem* typeItem = new QTableWidgetItem(typeStr);
    QTableWidgetItem* startEndItem = new QTableWidgetItem(
        d.startTime.toString("hh:mm:ss") + " - " + d.endTime.toString("hh:mm:ss"));
    QTableWidgetItem* durationItem = new QTableWidgetItem(convMSecToTimeStr(d.duration()) + "  ");
    for (auto* item : {typeItem, startEndItem, durationItem}) {
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        item->setBackground(grayBg);
    }
    table->setItem(row, 0, typeItem);
    table->setItem(row, 1, startEndItem);
    table->setItem(row, 2, durationItem);
}

void HistoryDialog::applyDirtyHighlight(uint idx)
{
    const bool modified = session_.pageModified(idx);
    const QBrush highlight(QColor(180, 216, 228, 255));
    const QBrush clear; // default (invalid) brush: falls back to the item's normal background
    // H5/WP5: the ongoing row never gets a type box and is rendered fully
    // read-only (addReadOnlyRow), so only the completed rows above it are
    // ever highlighted -- unlike editableRowCount(), which still counts it.
    const auto& pendingTimelines = session_.pendingTimelines();
    const int editableRows = idx < pendingTimelines.size()
        ? static_cast<int>(pendingTimelines[idx].completed().size()) : 0;
    for (int row = 0; row < editableRows; ++row) {
        for (int col = 0; col < table_->columnCount(); ++col) {
            if (table_->item(row, col))
                table_->item(row, col)->setBackground(modified ? highlight : clear);
        }
    }
}

/**
 * Refreshes the UI table with data from the current page.
 *
 * Responsibilities:
 * 1. Clears old data and widgets.
 * 2. Compares pendingChanges against original data to detect modifications.
 * 3. Re-populates the table.
 * 4. Binds Checkboxes for Type Toggling (Activity <-> Pause).
 * 5. Applies visual highlighting if the page has unsaved changes.
 */
void HistoryDialog::updateTable(uint idx)
{
    const auto& pages = session_.pages();
    const auto& pendingTimelines = session_.pendingTimelines();

    if (idx >= pendingTimelines.size() || idx >= pages.size()) {
        Logger::Log(QString("[HISTORY] Error: updateTable: Invalid index %1").arg(idx));
        return;
    }

    // Clean up existing checkboxes to prevent memory leaks
    for (int row = 0; row < table_->rowCount(); ++row) {
        QWidget* cellWidget = table_->cellWidget(row, 3);
        if (cellWidget) {
            cellWidget->disconnect();
            table_->removeCellWidget(row, 3);
        }
    }

    table_->clearContents();

    const auto& comp = pendingTimelines[idx].completed();
    const bool hasOngoing = pendingTimelines[idx].ongoing().has_value();
    const auto& crossMidnight = pages[idx].crossMidnight;
    const auto& conts = pages[idx].continuations;
    // D9 review (finding 5): route through the model's own row-count formula
    // instead of recomputing the same sum here (and again in showContextMenu()).
    table_->setRowCount(session_.displayCount(idx));

    updateTotalsLabel(idx);

    // H5/WP5: a read-only page (Today while running, or any date the
    // open-time permission rule excluded) shows disabled type boxes.
    const bool editable = session_.pageEditable(idx);

    // Populate completed rows
    for (int row = 0; row < static_cast<int>(comp.size()); ++row) {
        const auto& d = comp[row];
        QString typeStr = d.type == DurationType::Activity ? "Activity  " : "Pause  ";
        QTableWidgetItem* typeItem = new QTableWidgetItem(typeStr);
        QTableWidgetItem* startEndItem = new QTableWidgetItem(
            d.startTime.toString("hh:mm:ss") + " - " + d.endTime.toString("hh:mm:ss"));
        QTableWidgetItem* durationItem = new QTableWidgetItem(convMSecToTimeStr(d.duration()) + "  ");
        table_->setItem(row, 0, typeItem);
        table_->setItem(row, 1, startEndItem);
        table_->setItem(row, 2, durationItem);

        QCheckBox* box = new QCheckBox(table_);
        box->setChecked(d.type == DurationType::Activity);
        box->setEnabled(editable);
        table_->setCellWidget(row, 3, box);

        const SegmentId segId = d.segment_id;
        connect(box, &QCheckBox::stateChanged, this, [this, capturedPageIdx = idx, segId](int state) {
            if (capturedPageIdx != pageIndex_)
                return;
            const DurationType newType = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
            if (!session_.setCompletedType(capturedPageIdx, segId, newType)) {
                Logger::Log(QString("[WARNING] Checkbox callback: segment_id not found"));
                return;
            }
            const auto& comp = session_.pendingTimelines()[capturedPageIdx].completed();
            auto it = std::find_if(comp.begin(), comp.end(),
                [&segId](const TimeDuration& td) { return td.segment_id == segId; });
            if (it == comp.end())
                return;
            const int displayRow = static_cast<int>(std::distance(comp.begin(), it));
            const QString ts = newType == DurationType::Activity ? "Activity  " : "Pause  ";
            if (table_->item(displayRow, 0)) {
                table_->item(displayRow, 0)->setText(ts);
                updateTotalsLabel(capturedPageIdx);
                applyDirtyHighlight(capturedPageIdx);
            }
        });
    }

    // Populate ongoing row if present. H5/WP5: the ongoing row never gets a
    // type box -- it is always read-only (setOngoingType() is dead API from
    // this dialog's perspective; the page it sits on is read-only anyway
    // whenever tracking is running, by the open-time permission rule).
    if (hasOngoing) {
        int row = static_cast<int>(comp.size());
        const auto& og = pendingTimelines[idx].ongoing().value();
        addReadOnlyRow(table_, row, og, "(Running)");
    }

    // D9 review (finding 1): apply the single dirty-highlight rule once, to
    // every editable row, now that both the completed and ongoing rows exist.
    applyDirtyHighlight(idx);

    // Populate cross-midnight canonical rows (spans midnight, display-only on start-day page)
    {
        int baseRow = static_cast<int>(comp.size()) + (hasOngoing ? 1 : 0);
        for (int i = 0; i < static_cast<int>(crossMidnight.size()); ++i)
            addReadOnlyRow(table_, baseRow + i, crossMidnight[i], "(spans midnight)");
    }

    // Populate continuation rows (display-only cross-midnight spillover on end-day page)
    if (!conts.empty()) {
        int baseRow = static_cast<int>(comp.size()) + (hasOngoing ? 1 : 0)
                    + static_cast<int>(crossMidnight.size());
        for (int i = 0; i < static_cast<int>(conts.size()); ++i)
            addReadOnlyRow(table_, baseRow + i, conts[i], "(cont.)");
    }

    // Update navigation button states
    prevButton_->setEnabled(pageIndex_ < session_.pageCount() - 1);
    nextButton_->setEnabled(pageIndex_ > 0);
}

void HistoryDialog::onOlder()
{
    if (pageIndex_ < session_.pageCount() - 1) {
        pageIndex_++;
        updateTable(pageIndex_);
    }
}

void HistoryDialog::onNewer()
{
    if (pageIndex_ > 0) {
        pageIndex_--;
        updateTable(pageIndex_);
    }
}

void HistoryDialog::showContextMenu(const QPoint& pos)
{
    int row = table_->rowAt(pos.y());
    if (row < 0) return;

    // H5/WP5: no split menu on a read-only page.
    if (pageIndex_ < session_.pageCount() && !session_.pageEditable(pageIndex_))
        return;

    const auto& pendingTimelines = session_.pendingTimelines();

    // Don't allow split on the ongoing row
    if (pageIndex_ < pendingTimelines.size()
        && row == static_cast<int>(pendingTimelines[pageIndex_].completed().size())
        && pendingTimelines[pageIndex_].ongoing().has_value()) {
        return;
    }

    // Don't allow split on continuation rows (display-only). D9 review
    // (finding 5): route through the model's own editable-row count instead
    // of recomputing the same sum here (and again in updateTable()).
    if (pageIndex_ < session_.pageCount()) {
        if (row >= session_.editableRowCount(pageIndex_))
            return;
    }

    contextMenuRow_ = row;
    contextMenuPage_ = static_cast<int>(pageIndex_);
    QMenu menu(this);
    QAction* splitAction = menu.addAction("Split..");
    connect(splitAction, &QAction::triggered, this, &HistoryDialog::onSplitRow);
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void HistoryDialog::presetSplitDialog(SplitDialog& dlg, const TimeDuration& duration) const
{
    dlg.setFirstSegmentType(duration.type);
}

/**
 * Shared tail of onSplitRow() and applySplit_dbg(): calls session_.splitRow()
 * and, on success, rerenders via updateTable(idx). D9 review (finding 2):
 * the minimum-length/per-half validation and the origin bookkeeping both
 * live in HistoryEditSession::splitRow() now -- this method only applies the
 * typed outcome and keeps the debug and production paths' rendering in sync.
 */
HistoryEditSession::SplitOutcome HistoryDialog::applySplitAt(uint idx, int row, const QDateTime& at,
    DurationType firstType, DurationType secondType)
{
    const auto outcome = session_.splitRow(idx, static_cast<size_t>(row), at, firstType, secondType);
    if (outcome.applied)
        updateTable(idx);
    return outcome;
}

/**
 * Handles the "Split" context menu action.
 *
 * Algorithm:
 * 1. Refuses up front (no modal) if session_.splitRejection() already knows
 *    the row is out of range or too short (D9 review, finding 1, round 3) --
 *    the same predicate splitRow() itself checks first, so the dialog and
 *    the model can never classify a row differently.
 * 2. Otherwise launches SplitDialog to get user input (split point and types).
 * 3. Delegates the split -- and its per-half minimum-length validation -- to
 *    session_.splitRow() via applySplitAt(), and maps its typed error to the
 *    matching message box.
 */
void HistoryDialog::onSplitRow()
{
    if (contextMenuRow_ < 0) return;

    // Capture and reset row immediately to prevent stale state
    int row = contextMenuRow_;
    contextMenuRow_ = -1;

    if (contextMenuPage_ < 0)
        return;
    uint idx = static_cast<uint>(contextMenuPage_);
    contextMenuPage_ = -1;

    const auto& pendingTimelines = session_.pendingTimelines();

    if (idx >= pendingTimelines.size()
        || row >= static_cast<int>(pendingTimelines[idx].completed().size())) {
        Logger::Log(QString("[HISTORY] Error: onSplitRow: Invalid indices - page: %1, row: %2")
            .arg(idx).arg(row));
        return;
    }

    // D9 review (finding 1, round 3): refuse before opening SplitDialog for
    // any row splitRow() itself would reject regardless of split point --
    // previously the modal opened with a disabled-but-live slider and fully
    // live type radios/OK, and the user only learned the row was too short
    // after choosing a type and pressing OK.
    const auto rejection = session_.splitRejection(idx, static_cast<size_t>(row));
    if (rejection == HistoryEditSession::SplitError::TooShort) {
        Logger::Log(QString("[HISTORY] Error: onSplitRow: duration too short to split - page: %1, row: %2")
            .arg(idx).arg(row));
        const QString tooShortText =
            "This duration is too short to split meaningfully (minimum 3 seconds required).";
#ifndef QT_NO_DEBUG
        debugLastSplitUi_ = DebugSplitUi::TooShortInfo;
        debugLastSplitMessage_ = tooShortText;
        if (!debugSkipSplitMessages_)
#endif
        QMessageBox::information(this, "Split Duration", tooShortText);
        return;
    }
    if (rejection == HistoryEditSession::SplitError::OutOfRange) {
        Logger::Log(QString("[HISTORY] Error: onSplitRow: split rejected - page: %1, row: %2")
            .arg(idx).arg(row));
        return;
    }
    // H5/WP5: the split context menu is already suppressed on a read-only
    // page (showContextMenu()), but refuse defensively here too -- e.g. a
    // stale contextMenuRow_/contextMenuPage_ from before a revoke -- so
    // SplitDialog never opens on a read-only page.
    if (rejection == HistoryEditSession::SplitError::ReadOnly) {
        Logger::Log(QString("[HISTORY] Error: onSplitRow: page not editable - page: %1, row: %2")
            .arg(idx).arg(row));
        return;
    }

    const auto& completed = pendingTimelines[idx].completed();
    const TimeDuration& duration = completed[row];
    const QDateTime start = duration.startTime;
    const QDateTime end = duration.endTime;

#ifndef QT_NO_DEBUG
    debugLastSplitUi_ = DebugSplitUi::DialogOpened;
#endif
    SplitDialog dlg(start, end, this);
    presetSplitDialog(dlg, duration);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const auto outcome = applySplitAt(idx, row, dlg.getSplitTime(),
        dlg.getFirstSegmentType(), dlg.getSecondSegmentType());

    switch (outcome.error) {
    case HistoryEditSession::SplitError::None:
        break;
    case HistoryEditSession::SplitError::TooShort:
        // Defensive: already refused above before the dialog was shown.
        Logger::Log(QString("[HISTORY] Error: onSplitRow: duration too short to split - page: %1, row: %2")
            .arg(idx).arg(row));
#ifndef QT_NO_DEBUG
        debugLastSplitUi_ = DebugSplitUi::TooShortInfo;
        if (!debugSkipSplitMessages_)
#endif
        QMessageBox::information(this, "Split Duration",
            "This duration is too short to split meaningfully (minimum 3 seconds required).");
        break;
    case HistoryEditSession::SplitError::HalfTooShort:
        Logger::Log(QString("[HISTORY] Error: onSplitRow: split results too small - page: %1, row: %2")
            .arg(idx).arg(row));
#ifndef QT_NO_DEBUG
        if (!debugSkipSplitMessages_)
#endif
        QMessageBox::warning(this, "Split Duration",
            "Invalid split: both segments must be at least 1 second long.");
        break;
    case HistoryEditSession::SplitError::OutOfRange:
        Logger::Log(QString("[HISTORY] Error: onSplitRow: split failed - page: %1, row: %2")
            .arg(idx).arg(row));
        break;
    case HistoryEditSession::SplitError::ReadOnly:
        // Defensive: already refused above (the ReadOnly check before
        // SplitDialog is even shown) and the split menu itself is
        // suppressed on a read-only page (showContextMenu()), so this
        // cannot fire in production.
        Logger::Log(QString("[HISTORY] Error: onSplitRow: page not editable - page: %1, row: %2")
            .arg(idx).arg(row));
        break;
    }
}

#ifndef QT_NO_DEBUG
/**
 * Applies a split without showing SplitDialog, for use in tests that run under
 * the offscreen QPA plugin (where exec() triggers propagateSizeHints() warnings).
 * Splits at the midpoint of the row and shares onSplitRow()'s applySplitAt()
 * tail, so the debug path cannot silently skip the production rerender.
 */
void HistoryDialog::applySplit_dbg(int row, int page, DurationType firstType, DurationType secondType)
{
    if (page < 0) return;
    uint idx = static_cast<uint>(page);
    const auto& pendingTimelines = session_.pendingTimelines();
    if (idx >= pendingTimelines.size()
        || row < 0
        || row >= static_cast<int>(pendingTimelines[idx].completed().size()))
        return;

    const auto& completed = pendingTimelines[idx].completed();
    const TimeDuration& duration = completed[row];
    const QDateTime start = duration.startTime;
    const QDateTime end = duration.endTime;
    // Split at midpoint (same default as SplitDialog).
    const QDateTime splitTime = start.addSecs(start.secsTo(end) / 2);

    applySplitAt(idx, row, splitTime, firstType, secondType);
}

/**
 * Runs onSplitRow()'s real preset routine (presetSplitDialog) on a SplitDialog for the
 * source row and reads the value back, without exec()/show() — so it covers the
 * production preset wiring without realizing a window (which emits offscreen QPA warnings).
 */
DurationType HistoryDialog::splitDialogPresetFirstType_dbg(int row, int page)
{
    uint idx = static_cast<uint>(page);
    const auto& completed = session_.pendingTimelines()[idx].completed();
    const TimeDuration& duration = completed[row];

    SplitDialog dlg(duration.startTime, duration.endTime, this);
    presetSplitDialog(dlg, duration);
    return dlg.getFirstSegmentType();
}
#endif // !QT_NO_DEBUG
