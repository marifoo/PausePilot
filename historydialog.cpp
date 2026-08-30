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
#include <QFileInfo>
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
    , exclusiveEditGuard_(timetracker)   // begins exclusive edit here
    , pageIndex_(0)
{
    session_.buildFromTimer(timetracker_);
    setupUI();
    updateTable(pageIndex_);

    // Enable right-click context menu for split functionality
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested, this, &HistoryDialog::showContextMenu);

    // Log initial state for debugging
    Logger::Log("[HISTORY] Dialog opened");
    {
        auto historyDurations = timetracker_.loadHistoryFromStore();
        Logger::Log(QString("[HISTORY] Loaded %1 total durations from DB").arg(historyDurations.durations.size()));

        const auto& pages = session_.pages();
        for (size_t i = 0; i < pages.size(); ++i) {
            Logger::Log(QString("[HISTORY] Page %1 - Title: %2, Entries: %3, IsCurrent: %4")
                .arg(i)
                .arg(pages[i].title)
                .arg(pages[i].durations.size())
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
 * 2. Refresh ongoing end-time from engine (preserving any user type edit).
 * 3. Delegate payload construction (normalization, origin re-split) to session_.
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

    // If the engine was shut down while this dialog stayed open (e.g. a
    // cancelled OS shutdown ran the save path), a save here would re-write the
    // now-finalized rows as unfinalized into a stopped engine. Refuse it.
    if (timetracker_.wasShutDown()) {
        Logger::Log("[HISTORY] Engine was shut down; discarding changes");
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

    // Step 3: Confirm if normalization merged overlapping segments.
    if (payload.needsMergeConfirmation) {
#ifndef QT_NO_DEBUG
        QMessageBox::StandardButton answer = QMessageBox::Yes;
        if (debugOverlapAnswer_.has_value()) {
            answer = *debugOverlapAnswer_;
            debugOverlapAnswer_ = std::nullopt;
        } else {
            answer = QMessageBox::question(this, "Overlapping Segments",
                "Overlapping segments were detected and will be merged. Continue?",
                QMessageBox::Yes | QMessageBox::No);
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
    } else {
        Logger::Log("[HISTORY] No historical durations to save");
    }

    // Step 5: Atomically update TimeTracker's in-memory durations and checkpoint tracking.
    const auto commitResult = timetracker_.commitEditedTimeline(memoryTimeline);
    if (!commitResult.ok) {
        Logger::Log("[HISTORY] commitEditedTimeline: checkpoint write failed after edit commit");
    }
    Logger::Log("[HISTORY] Updated TimeTracker current session and checkpoint tracking");
    return true;
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
    // Rich text enables the clickable log-file link embedded by buildLoadReconciliationMessage().
    // setOpenExternalLinks delegates the click to QDesktopServices::openUrl(), which
    // hands "file:///" URLs to the OS file manager without extra plumbing.
    loadReconciliationLabel_->setTextFormat(Qt::RichText);
    loadReconciliationLabel_->setOpenExternalLinks(true);
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
 *
 * If debug logging is enabled and the log file exists on disk, appends an HTML
 * anchor so the user can click through to the log for details. The label is
 * configured for Rich Text and external-link handling in setupUI(), so the
 * anchor is opened by the OS file manager / text editor automatically.
 */
QString HistoryDialog::buildLoadReconciliationMessage() const
{
    const int skipped = session_.historyLoadSkipped();
    const int repaired = session_.historyLoadRepaired();

    QString message;
    if (session_.historyLoadFailed()) {
        message = "History could not be loaded — showing today only. Saving is disabled.";
    }

    if (skipped <= 0 && repaired <= 0)
        return message;

    if (!message.isEmpty())
        message += "<br>";
    message += QString("%1 rows skipped due to corrupt data, %2 rows auto-repaired.")
        .arg(skipped)
        .arg(repaired);

    // Only show a link to the log file when logging is actually enabled; showing it
    // when logging is off is misleading (the file may be stale or absent).
    if (settings_.logToFile()) {
        const QString logPath = Logger::logFilePath();
        if (QFileInfo::exists(logPath)) {
            message += QString("<br><a href=\"file:///%1\">Open log file</a>")
                .arg(logPath);
        }
    }

    return message;
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

    const QDateTime dayStart(pages[idx].pageDate, QTime(0, 0, 0), Qt::LocalTime);
    const QDateTime dayEnd = dayStart.addDays(1);
    auto portionInDay = [&](const TimeDuration& d) -> qint64 {
        const QDateTime s = std::max(d.startTime.toLocalTime(), dayStart);
        const QDateTime e = std::min(d.endTime.toLocalTime(), dayEnd);
        return s < e ? s.msecsTo(e) : 0;
    };
    for (const auto& d : pages[idx].crossMidnight)
        (d.type == DurationType::Activity ? totalActivity : totalPause) += portionInDay(d);
    for (const auto& d : pages[idx].continuations)
        (d.type == DurationType::Activity ? totalActivity : totalPause) += portionInDay(d);

    QString totalsStr = QString("\nActivity: ") + convMSecToTimeStr(totalActivity)
                      + QString("  Pause: ") + convMSecToTimeStr(totalPause);
    pageLabel_->setText(pages[idx].title + totalsStr);
}

void HistoryDialog::addReadOnlyRow(QTableWidget* table, int row, const TimeDuration& d, const QString& suffix)
{
    const QColor grayBg(200, 200, 200, 180);
    QString typeStr = (d.type == DurationType::Activity ? "Activity  " : "Pause  ");
    typeStr += suffix;
    QTableWidgetItem* typeItem = new QTableWidgetItem(typeStr);
    QTableWidgetItem* startEndItem = new QTableWidgetItem(
        d.startTime.toString("hh:mm:ss") + " - " + d.endTime.toString("hh:mm:ss"));
    QTableWidgetItem* durationItem = new QTableWidgetItem(convMSecToTimeStr(d.duration) + "  ");
    for (auto* item : {typeItem, startEndItem, durationItem}) {
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        item->setBackground(grayBg);
    }
    table->setItem(row, 0, typeItem);
    table->setItem(row, 1, startEndItem);
    table->setItem(row, 2, durationItem);
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
    int rowCount = static_cast<int>(comp.size());
    if (hasOngoing) ++rowCount;
    rowCount += static_cast<int>(crossMidnight.size());
    rowCount += static_cast<int>(conts.size());
    table_->setRowCount(rowCount);

    updateTotalsLabel(idx);

    // Determine if this page has been modified (compare completed portion only)
    const auto& originalDurations = pages[idx].durations;
    // For page 0, original durations includes the ongoing row at the end; compare only completed
    size_t originalCompletedSize = originalDurations.size();
    if (idx == 0 && pendingTimelines[0].ongoing().has_value()) {
        originalCompletedSize = originalDurations.size() > 0 ? originalDurations.size() - 1 : 0;
    }
    bool pageModified = false;
    if (comp.size() != originalCompletedSize) {
        pageModified = true;
    } else {
        for (size_t i = 0; i < comp.size(); ++i) {
            const auto& pending  = comp[i];
            const auto& original = originalDurations[i];
            if (pending.type != original.type ||
                pending.duration != original.duration ||
                pending.startTime != original.startTime ||
                pending.endTime != original.endTime) {
                pageModified = true;
                break;
            }
        }
    }

    // Populate completed rows
    for (int row = 0; row < static_cast<int>(comp.size()); ++row) {
        const auto& d = comp[row];
        QString typeStr = d.type == DurationType::Activity ? "Activity  " : "Pause  ";
        QTableWidgetItem* typeItem = new QTableWidgetItem(typeStr);
        QTableWidgetItem* startEndItem = new QTableWidgetItem(
            d.startTime.toString("hh:mm:ss") + " - " + d.endTime.toString("hh:mm:ss"));
        QTableWidgetItem* durationItem = new QTableWidgetItem(convMSecToTimeStr(d.duration) + "  ");
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
            auto& pendingTimelines = session_.pendingTimelines();
            const auto& comp = pendingTimelines[capturedPageIdx].completed();
            auto it = std::find_if(comp.begin(), comp.end(),
                [&segId](const TimeDuration& td) { return td.segment_id == segId; });
            if (it == comp.end()) {
                Logger::Log(QString("[WARNING] Checkbox callback: segment_id not found"));
                return;
            }
            const size_t currentRow = static_cast<size_t>(std::distance(comp.begin(), it));
            DurationType newType = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
            pendingTimelines[capturedPageIdx] =
                pendingTimelines[capturedPageIdx].withSegmentType(currentRow, newType);
            const int displayRow = static_cast<int>(currentRow);
            QString ts = newType == DurationType::Activity ? "Activity  " : "Pause  ";
            if (table_->item(displayRow, 0)) {
                table_->item(displayRow, 0)->setText(ts);
                updateTotalsLabel(capturedPageIdx);
                for (int col = 0; col < table_->columnCount(); ++col) {
                    if (table_->item(displayRow, col))
                        table_->item(displayRow, col)->setBackground(QColor(180, 216, 228, 255));
                }
            }
        });

        if (pageModified) {
            for (int col = 0; col < table_->columnCount(); ++col) {
                if (table_->item(row, col))
                    table_->item(row, col)->setBackground(QColor(180, 216, 228, 255));
            }
        }
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
            convMSecToTimeStr(og.duration) + "  (Running)");
        table_->setItem(row, 0, typeItem);
        table_->setItem(row, 1, startEndItem);
        table_->setItem(row, 2, durationItem);

        QCheckBox* box = new QCheckBox(table_);
        box->setChecked(og.type == DurationType::Activity);
        box->setEnabled(true);
        table_->setCellWidget(row, 3, box);

        connect(box, &QCheckBox::stateChanged, this, [this, capturedPageIdx = idx, displayRow = row](int state) {
            if (capturedPageIdx != pageIndex_) return;
            auto& pendingTimelines = session_.pendingTimelines();
            if (!pendingTimelines[capturedPageIdx].ongoing().has_value()) return;
            DurationType newType = (state == Qt::Checked) ? DurationType::Activity : DurationType::Pause;
            auto ongoing = pendingTimelines[capturedPageIdx].ongoing().value();
            ongoing.type = newType;
            pendingTimelines[capturedPageIdx] =
                Timeline(pendingTimelines[capturedPageIdx].completed(), ongoing);
            const QString ts = newType == DurationType::Activity ? "Activity  " : "Pause  ";
            if (table_->item(displayRow, 0)) {
                table_->item(displayRow, 0)->setText(ts);
                updateTotalsLabel(capturedPageIdx);
            }
        });
    }

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

    // Don't allow split on continuation rows (display-only)
    if (pageIndex_ < session_.pageCount()) {
        const int canonicalRows =
            static_cast<int>(pendingTimelines[pageIndex_].completed().size())
            + (pendingTimelines[pageIndex_].ongoing().has_value() ? 1 : 0);
        if (row >= canonicalRows)
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
 * Handles the "Split" context menu action.
 *
 * Algorithm:
 * 1. Validates that the selected duration is long enough to be split (min 3s).
 * 2. Launches SplitDialog to get user input (split point and types).
 * 3. Validates the result to ensure no duration is lost or created (sum check).
 * 4. Atomically replaces the original entry with the two new entries in the deque.
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

    auto& pendingTimelines = session_.pendingTimelines();

    if (idx >= pendingTimelines.size()
        || row >= static_cast<int>(pendingTimelines[idx].completed().size())) {
        Logger::Log(QString("[HISTORY] Error: onSplitRow: Invalid indices - page: %1, row: %2")
            .arg(idx).arg(row));
        return;
    }
    // Don't allow split on the ongoing row
    if (row == static_cast<int>(pendingTimelines[idx].completed().size())
        && pendingTimelines[idx].ongoing().has_value()) {
        return;
    }

    const auto& completed = pendingTimelines[idx].completed();
    const TimeDuration& duration = completed[row];
    qint64 originalDuration = duration.duration;
    QDateTime start = duration.startTime;
    QDateTime end = duration.endTime;

    // Check if duration is long enough to split meaningfully (at least 3 seconds)
    if (start.secsTo(end) < kMinimumSplitDurationSeconds) {
        Logger::Log(QString("[HISTORY] Error: onSplitRow: Duration too short to split - only %1 seconds")
            .arg(start.secsTo(end)));
        QMessageBox::information(this, "Split Duration",
            "This duration is too short to split meaningfully (minimum 3 seconds required).");
        return;
    }

    // Show split dialog; preset type based on source row.
    SplitDialog dlg(start, end, this);
    presetSplitDialog(dlg, duration);
    if (dlg.exec() == QDialog::Accepted) {
        QDateTime splitTime = dlg.getSplitTime();
        qint64 firstDuration = start.msecsTo(splitTime);
        qint64 secondDuration = splitTime.msecsTo(end);

        // Validate split preserves total duration
        if (firstDuration + secondDuration != originalDuration) {
            Logger::Log(QString("[HISTORY] Warning: Split duration mismatch - original: %1ms, split sum: %2ms, adjusting")
                .arg(originalDuration).arg(firstDuration + secondDuration));
            secondDuration = originalDuration - firstDuration;
        }

        // Validate split results - ensure both segments have at least 1 second
        if (firstDuration < 1000 || secondDuration < 1000) {
            Logger::Log(QString("[HISTORY] Error: onSplitRow: Split results too small - first: %1ms, second: %2ms")
                .arg(firstDuration).arg(secondDuration));
            QMessageBox::warning(this, "Split Duration",
                "Invalid split: both segments must be at least 1 second long.");
            return;
        }

        DurationType firstType = dlg.getFirstSegmentType();
        DurationType secondType = dlg.getSecondSegmentType();

        // Capture the original segment_id before the split.
        const QString origId = completed[row].segment_id.toString();
        const bool wasMemory = session_.originIsMemory().value(origId, true);
        const size_t prevSize = completed.size();

        pendingTimelines[idx] = pendingTimelines[idx].withSplit(
            static_cast<size_t>(row), dlg.getSplitTime(), firstType, secondType);

        // Register the second-half's new segment_id with the same origin as the first.
        const auto& newComp = pendingTimelines[idx].completed();
        if (newComp.size() > prevSize && row + 1 < static_cast<int>(newComp.size()))
            session_.registerSplitChild(newComp[row + 1].segment_id.toString(), wasMemory);

        updateTable(idx);
    }
}

#ifndef QT_NO_DEBUG
/**
 * Applies a split without showing SplitDialog, for use in tests that run under
 * the offscreen QPA plugin (where exec() triggers propagateSizeHints() warnings).
 * Splits at the midpoint of the row, same logic as onSplitRow() after dialog acceptance.
 */
void HistoryDialog::applySplit_dbg(int row, int page, DurationType firstType, DurationType secondType)
{
    if (page < 0) return;
    uint idx = static_cast<uint>(page);
    auto& pendingTimelines = session_.pendingTimelines();
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

    const QString origId = completed[row].segment_id.toString();
    const bool wasMemory = session_.originIsMemory().value(origId, true);
    const size_t prevSize = completed.size();

    pendingTimelines[idx] = pendingTimelines[idx].withSplit(
        static_cast<size_t>(row), splitTime, firstType, secondType);

    const auto& newComp = pendingTimelines[idx].completed();
    if (newComp.size() > prevSize && row + 1 < static_cast<int>(newComp.size()))
        session_.registerSplitChild(newComp[row + 1].segment_id.toString(), wasMemory);
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
