/**
 * HistoryDialog - Interface for viewing and editing past time logs.
 *
 * Architecture:
 * - Transactional Editing: All changes (type toggling, splitting) are performed
 *   on local copies inside HistoryEditSession (session_). The actual database and
 *   TimeTracker are only updated when the user clicks "OK" and saveChanges() succeeds.
 * - Paging Model: Data is grouped by Date (Pages). Page 0 is always "Today"
 *   (in-memory + today's DB + ongoing), while subsequent pages are historical (DB).
 * - Safety: Checkpoints are paused while this dialog is open via
 *   TimerExclusiveEditGuard (beginExclusiveEdit on construction, endExclusiveEdit
 *   on destruction — guaranteed on all exit paths).
 * - Close blocking: accept() runs saveChanges() before closing. A failed DB
 *   save or a declined merge confirmation leaves the dialog open so the user
 *   can correct the situation or cancel explicitly.
 * - Close on edit revoke (WP1): when the engine revokes the edit (at lock,
 *   sleep, midnight, shutdown, or when the date moves forward),
 *   onExclusiveEditRevoked() sets editRevoked_, queues closeRevoked() to
 *   reject child dialogs/menus, then rejects History itself. saveChanges()
 *   checks editRevoked_ twice: once right after the result() guard, so an
 *   accept() landing before the queued closeRevoked() runs cannot slip past
 *   the no-edit early return, and again immediately before Step 4 (DB
 *   persist), since the Step 2.6/Step 3 prompts run nested event loops that
 *   can revoke after the early check.
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
    session_.buildFromTimer(timetracker_, QDate::currentDate());
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
 * Commits pending edits to the Application State and Database.
 *
 * Returns true on success (dialog may close), false if the save failed or
 * the user declined a merge confirmation or the result is not Accepted (dialog stays open).
 *
 * Strategy:
 * 1. Check result() — return false if Rejected (cancel path).
 * 1a. Check editRevoked_ — return false if the edit was revoked (early check;
 *     see the class-level "Close on edit revoke" note above).
 * 2. Refresh ongoing end-time from engine (preserving any user type edit).
 * 3. Delegate payload construction (normalization, origin re-split) to session_.
 * 3a. If the save would replace the DB and drop a display-only cross-midnight
 *     row or a pre-existing overlap row, ask for confirmation; abort on No.
 * 4. If a merge will occur, ask for confirmation; abort on No.
 * 5. Persist via replaceAll().
 * 6. Update TimeTracker runtime state via commitEditedTimeline() — only after persistence succeeds.
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
    // return below and finish Accepted. The Step 4 check further down stays
    // for edited sessions, since the Step 2.6/Step 3 prompts run nested
    // event loops that can revoke after this early check.
    if (editRevoked_) {
        Logger::Log("[HISTORY] Edit was revoked; discarding changes");
        return false;
    }

    // D5 review fix (finding 1, R15-002): if the engine was shut down while
    // this dialog stayed open (e.g. a cancelled OS shutdown ran the save
    // path, closing/committing the live segment), a save here would silently
    // drop that segment (absent from this dialog's pre-shutdown snapshot) and
    // re-insert the current-session bucket as unfinalized into a stopped
    // engine. Per-dialog, not global: cancelShutdown() clears
    // TimeTracker::wasShutDown() for everyone, but shutdownEpoch() is
    // monotonic, so a dialog that spans the shutdown stays refused even
    // after cancellation, while one opened afterward saves normally.
    if (timetracker_.shutdownEpoch() != shutdownEpochAtOpen_) {
        Logger::Log("[HISTORY] Engine was shut down while this dialog was open; discarding changes");
        return false;
    }

    // A failed history load means the model is incomplete; saving would
    // replaceAll() the DB with only the rows we managed to load, wiping the
    // rest. Refuse; the user can cancel and retry.
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

    // Step 0: If the user made no edits, the save is a true no-op. Skip it
    // entirely so an editless OK never rewrites or merges untouched DB history.
    if (!session_.hasEdits()) {
        Logger::Log("[HISTORY] No edits; skipping save (no-op)");
        return true;
    }

    // Step 1: Refresh the ongoing segment's end-time from the engine.
    session_.refreshOngoing(timetracker_);

    // Step 2: Build the three save buckets from the session model.
    const HistoryEditSession::SavePayload payload = session_.buildSavePayload();

    // Step 2.5 (C3/R13-001/R14-001): refuse a save that would write an
    // unsupported row (invalid/non-positive interval, empty/duplicate id, or
    // an Activity/Pause overlap). SessionStore::replaceAll() also rejects this
    // as a final guard, but refusing here avoids reaching the DB at all.
    if (payload.violation != DayValidity::Violation::None) {
        Logger::Log("[HISTORY] buildSavePayload rejected: " + DayValidity::describe(payload.violation));
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

    // Step 2.6 (M3): replaceAll() below is a whole-table replace that can
    // silently remove rows the user never touched -- a single-boundary
    // cross-midnight row (display-only, never carried into the payload; see
    // buildSavePayload()) or a pre-existing Activity/Pause overlap between two
    // persisted, unedited rows (category 3, payload.preExistingOverlapDrops).
    // Ask before either loss, but only when the save will actually call
    // replaceAll() (same predicate as Step 4 below).
    int crossMidnightDrops = 0;
    for (const auto& page : session_.pages())
        crossMidnightDrops += static_cast<int>(page.crossMidnight.size());
    const int overlapDrops = payload.preExistingOverlapDrops;
    const bool willReplace = !payload.historyTimeline.completed().empty() ||
        !payload.sessionTimeline.completed().empty();

    if (willReplace && (crossMidnightDrops > 0 || overlapDrops > 0)) {
        QString dropText;
        if (crossMidnightDrops > 0)
            dropText += QString("%1 entries spanning midnight will be removed.").arg(crossMidnightDrops);
        if (overlapDrops > 0) {
            if (!dropText.isEmpty())
                dropText += " ";
            dropText += QString("%1 pre-existing overlapping entries will be removed.").arg(overlapDrops);
        }
        dropText += " Continue saving?";

#ifndef QT_NO_DEBUG
        debugDropPromptText_ = dropText;
        // Default to No (abort the save) rather than exec()-ing a real
        // QMessageBox::question() when a test hits this path without setting
        // debugDropAnswer_ -- under offscreen QPA that dialog has nothing to
        // dismiss it and would hang the run instead of failing the assertion on
        // saveChanges()'s return value.
        QMessageBox::StandardButton dropAnswer = QMessageBox::No;
        if (debugDropAnswer_.has_value()) {
            dropAnswer = *debugDropAnswer_;
            debugDropAnswer_ = std::nullopt;
        }
#else
        const auto dropAnswer = QMessageBox::question(this, "Entries Will Be Removed",
            dropText, QMessageBox::Yes | QMessageBox::No);
#endif
        if (dropAnswer != QMessageBox::Yes) {
            return false;
        }
    }

    // Step 3: Confirm if normalization merged overlapping segments.
    if (payload.needsMergeConfirmation) {
#ifndef QT_NO_DEBUG
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

    const Timeline& historyTimeline  = payload.historyTimeline;
    const Timeline& sessionTimeline  = payload.sessionTimeline;
    const Timeline& memoryTimeline   = payload.memoryTimeline;

    bool backupMissing = false;

    // WP1: if the edit was revoked (at lock, sleep, midnight, shutdown, or
    // when the date moves forward), refuse the save and discard all edits.
    // This check gates both Step 4 (DB persist) and Step 5 (memory-only commit).
    if (editRevoked_) {
        Logger::Log("[HISTORY] Edit was revoked; discarding changes");
        return false;
    }

    // Step 4: Persist to DB.
    if (!historyTimeline.completed().empty() || !sessionTimeline.completed().empty()) {
        Logger::Log(QString("[HISTORY] Saving %1 historical + %2 current-session durations to DB")
            .arg(historyTimeline.completed().size())
            .arg(sessionTimeline.completed().size()));

        // C4 fix round 2 (MEDIUM-5): capture the latch state BEFORE the
        // store call. If it was already latched, enterFatalState() returns
        // early and shows nothing for THIS call -- so a FatalError here
        // still needs this dialog. Only suppress it when this very call is
        // the one that raised the modal.
        const bool wasLatchedBefore = timetracker_.isFatalLatched();
        const SessionStoreResult dbResult = timetracker_.replaceAll(historyTimeline, sessionTimeline);
        if (!dbResult.ok() && dbResult.category != SessionStoreResult::Disabled) {
            Logger::Log("[HISTORY] CRITICAL: Failed to save durations to DB: " + dbResult.message);
            // C4 (finding 1): a FatalError that THIS call raised already
            // latched TimeTracker and showed its own single modal error via
            // fatalError -- do not show a second dialog for the same
            // failure. Every other failure category still shows this dialog
            // as before, and so does a FatalError that arrives while the
            // latch was already set (this call raised no dialog of its own).
            if (!(dbResult.category == SessionStoreResult::FatalError && !wasLatchedBefore)) {
#ifndef QT_NO_DEBUG
                ++debugCriticalShownCount_;
                if (!debugSkipCritical_) {
                    QMessageBox::critical(this, "Database Error",
                        "Failed to save changes to the database. Your changes to historical entries may be lost.");
                }
#else
                QMessageBox::critical(this, "Database Error",
                    "Failed to save changes to the database. Your changes to historical entries may be lost.");
#endif
            }
            return false;
        }
        Logger::Log("[HISTORY] Successfully saved historical durations to DB");
        backupMissing = dbResult.backupMissing;
    } else {
        Logger::Log("[HISTORY] No historical durations to save");
    }

    // Step 5: Atomically update TimeTracker's in-memory durations and checkpoint tracking.
    const auto commitResult = timetracker_.commitEditedTimeline(memoryTimeline);
    if (!commitResult.ok) {
        Logger::Log("[HISTORY] commitEditedTimeline: checkpoint write failed after edit commit");
    }
    Logger::Log("[HISTORY] Updated TimeTracker current session and checkpoint tracking");

    if (backupMissing) {
#ifndef QT_NO_DEBUG
        ++debugBackupWarningShownCount_;
        if (!debugSkipCritical_) {
            QMessageBox::warning(this, "Database Warning",
                "Your changes were saved, but no safety backup of the previous history "
                "could be created (e.g. low disk space or a copy failure).");
        }
#else
        QMessageBox::warning(this, "Database Warning",
            "Your changes were saved, but no safety backup of the previous history "
            "could be created (e.g. low disk space or a copy failure).");
#endif
    }

    return true;
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
    pageLabel_->setText(session_.pageTitle(idx) + totalsStr);
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
    const int editableRows = session_.editableRowCount(idx);
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
        box->setEnabled(true);
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

    // Populate ongoing row if present
    if (hasOngoing) {
        int row = static_cast<int>(comp.size());
        const auto& og = pendingTimelines[idx].ongoing().value();
        QString typeStr = og.type == DurationType::Activity ? "Activity  " : "Pause  ";
        QTableWidgetItem* typeItem = new QTableWidgetItem(typeStr);
        QTableWidgetItem* startEndItem = new QTableWidgetItem(
            og.startTime.toString("hh:mm:ss") + " - " + og.endTime.toString("hh:mm:ss"));
        QTableWidgetItem* durationItem = new QTableWidgetItem(
            convMSecToTimeStr(og.duration()) + "  (Running)");
        table_->setItem(row, 0, typeItem);
        table_->setItem(row, 1, startEndItem);
        table_->setItem(row, 2, durationItem);

        QCheckBox* box = new QCheckBox(table_);
        box->setChecked(og.type == DurationType::Activity);
        box->setEnabled(true);
        table_->setCellWidget(row, 3, box);

        connect(box, &QCheckBox::stateChanged, this, [this, capturedPageIdx = idx, displayRow = row](int state) {
            if (capturedPageIdx != pageIndex_) return;
            const DurationType newType = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
            if (!session_.setOngoingType(capturedPageIdx, newType))
                return;
            const QString ts = newType == DurationType::Activity ? "Activity  " : "Pause  ";
            if (table_->item(displayRow, 0)) {
                table_->item(displayRow, 0)->setText(ts);
                updateTotalsLabel(capturedPageIdx);
                // D9 review (finding 1): the ongoing row previously received
                // no dirty highlight at all, unlike completed rows -- the one
                // applyDirtyHighlight() rule now covers both.
                applyDirtyHighlight(capturedPageIdx);
            }
        });
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
        // Defensive: buildFromTimer() (still the only build this dialog uses)
        // marks every page editable, so this cannot fire yet -- H5/WP5 wires
        // the scoped-save model in.
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
