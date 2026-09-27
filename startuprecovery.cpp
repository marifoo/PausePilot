#include "startuprecovery.h"
#include "sessionstore.h"
#include "timetracker.h"

#include <QMessageBox>
#include <QPushButton>

bool runStartupRecovery(TimeTracker& tracker)
{
    StartupRecoveryResult result = tracker.initializeFromStore();
    while (!result.okOrDisabled()) {
        QMessageBox box;
        box.setWindowTitle("uTimer - Startup Recovery Failed");
        box.setIcon(QMessageBox::Warning);
        box.setText("Could not recover data from the last session. Retry, or quit and leave it for next time?");
        QAbstractButton* retryButton = box.addButton("Retry", QMessageBox::AcceptRole);
        box.addButton("Quit", QMessageBox::DestructiveRole);
        box.setDefaultButton(static_cast<QPushButton*>(retryButton));
        box.exec();
        if (box.clickedButton() != retryButton) {
            // Quit: existing rows/marker are left exactly as they are: no
            // recovery transaction committed on a TransientError path, so
            // there is nothing to undo.
            return false;
        }
        result = tracker.initializeFromStore();
    }
    return true;
}
