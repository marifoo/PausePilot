/**
 * MainWin — the top-level Qt window.
 *
 * Responsibilities:
 * - Window management (System Tray, Always-on-Top, minimize behaviors)
 * - Global event handling (Shutdown, Sleep/Wake)
 * - User warning system (Health checks for too much work / no pause)
 *
 * Day-boundary policy: owned by TimeTracker::DayBoundaryWatcher.
 * MainWin only syncs the GUI when it observes the engine has stopped.
 */

#include "mainwin.h"
#include "shutdowncoordinator.h"
#include <QtDebug>
#include <QDateTime>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QPushButton>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QStatusBar>
#include "logger.h"
#include "timeformat.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif


MainWin::MainWin(Settings& settings, TimeTracker& timetracker, SessionStore& db,
	             ShutdownCoordinator& shutdown_coordinator, QWidget *parent)
	: QMainWindow(parent), settings_(settings), timetracker_(timetracker), db_(db),
	  shutdown_coordinator_(shutdown_coordinator),
	  health_monitor_(new HealthMonitor(settings, this))
{
	setupCentralWidget(settings, timetracker);

	setupIcon();

	setWindowTitle("µTimer");
	setWindowFlags(windowFlags() &(~Qt::WindowMaximizeButtonHint));

	connect(health_monitor_, &HealthMonitor::warningTriggered, this, &MainWin::showMsgBox);

	// Re-arm the health warnings when a session ends so they can fire again in
	// the next session (at most once per session, not once per app lifetime).
	connect(&timetracker_, &TimeTracker::stopped, health_monitor_, &HealthMonitor::reset);
}

void MainWin::setupCentralWidget(Settings& settings, TimeTracker& timetracker)
{
	content_widget_ = new ContentWidget(settings, timetracker, this);

	setCentralWidget(content_widget_);
	QObject::connect(content_widget_, &ContentWidget::startPausePressed,
		&timetracker, &TimeTracker::onStartPausePressed);
	QObject::connect(content_widget_, &ContentWidget::stopPressed,
		&timetracker, &TimeTracker::onStopPressed);
	QObject::connect(content_widget_, &ContentWidget::historyLoadReconciliationAvailable,
		this, &MainWin::showHistoryLoadReconciliation);
	QObject::connect(content_widget_, SIGNAL(minToTray()), this, SLOT(minToTray()));
	QObject::connect(content_widget_, SIGNAL(toggleAlwaysOnTop()), this, SLOT(toggleAlwaysOnTop()));
	
}

void MainWin::setupIcon()
{
	const QIcon icon(":/clock.png");

	setWindowIcon(icon);

	tray_icon_ = new QSystemTrayIcon(icon, this);
	tray_icon_->setToolTip("Timing Inactive");
	tray_icon_->show();

	QObject::connect(tray_icon_, SIGNAL(activated(QSystemTrayIcon::ActivationReason)), this, SLOT(iconActivated(QSystemTrayIcon::ActivationReason)));
}

void MainWin::onTick()
{
	content_widget_->updateTimes();
	tray_icon_->setToolTip(content_widget_->getTooltip());

	if (timetracker_.isActive()
	    && (settings_.showTooMuchActivityWarning() || settings_.showNoPauseWarning())) {
		health_monitor_->check(timetracker_.getActiveTime(), timetracker_.getPauseTime());
	}
}

void MainWin::showMsgBox(const QString &text)
{
	showMainWin();

	QMessageBox msgBox(this);
	msgBox.setWindowTitle("µTimer Warning");
	msgBox.setText(text);
	msgBox.setIcon(QMessageBox::Warning);
	msgBox.exec();
}

void MainWin::showUserWarning(const QString& text)
{
	if (tray_icon_ && tray_icon_->isVisible()) {
		tray_icon_->showMessage("uTimer Warning", text, QSystemTrayIcon::Warning, 10000);
	}

	statusBar()->showMessage(text, 10000);
}

void MainWin::showFatalError(const QString& text)
{
	// C4 (round-3 F9): only restore visibility when the window is not
	// already shown (e.g. minimized to tray). Calling showMainWin()
	// unconditionally raises/activates the main window even when it is
	// already visible behind an open modal dialog (HistoryDialog is the
	// one production path that reaches this), stealing focus from it.
	if (!isVisible()) {
		showMainWin();
	}
	QMessageBox::critical(this, "Database Error", text);
}

void MainWin::showHistoryLoadReconciliation(const QString& text)
{
	statusBar()->showMessage(text, 10000);
}

void MainWin::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
#ifdef Q_OS_LINUX
	// On Linux (especially KDE/Wayland), DoubleClick doesn't work - use single-click
	if (reason != QSystemTrayIcon::Trigger)
		return;
#else
	if (reason != QSystemTrayIcon::DoubleClick)
		return;
#endif

	if (isVisible())
		minToTray();
	else
		showMainWin();
}

void MainWin::showMainWin()
{
#ifdef Q_OS_LINUX
	setVisible(true);
	setWindowState(windowState() & ~Qt::WindowMinimized);
#else
	activateWindow();
	show();
#endif
}

void MainWin::minToTray()
{
#ifdef Q_OS_LINUX
	setVisible(false);
#else
	hide();
#endif
}

void MainWin::toggleAlwaysOnTop()
{
	toggleAlwaysOnTopFlag();
	showMainWin();
}

void MainWin::toggleAlwaysOnTopFlag()
{
	setWindowFlags(windowFlags() ^ (Qt::CustomizeWindowHint | Qt::WindowStaysOnTopHint));
}

void MainWin::start()
{
	if (settings_.isPinnedStartEnabled())
		toggleAlwaysOnTopFlag();

	if (settings_.isMinimizedStartEnabled())
		minToTray();
	else
		showMainWin();

	if (settings_.isAutostartTimingEnabled()) {
		content_widget_->pressedStartPauseButton();
		// The pressedButton signal will trigger midnight scheduling
	}

	// B2: the store returns exact recovered milliseconds; this formatter
	// consumes them directly -- no seconds conversion/rounding happens
	// anywhere in the path leading here.
	const qint64 recoveredMsec = timetracker_.getStartupRecoveredMsec();
	const int policyDropped = timetracker_.getStartupPolicyDroppedCount();
	QString message;
	if (recoveredMsec > 0 && timetracker_.shouldShowStartupRecoveryNotification()) {
		message = QString("Recovered %1 from the last session after an unclean shutdown.")
			.arg(convMSecToTimeStr(recoveredMsec));
	}
	if (policyDropped > 0) {
		// M4: rows recovery deleted on its own policy (too short, too old,
		// future-dated, spanning midnight, or overlapping existing history).
		// Discards the user asked for are excluded upstream and stay silent.
		const QString droppedText = QString("%1 unfinished %2 from the last session could not be restored and %3 discarded.")
			.arg(policyDropped)
			.arg(policyDropped == 1 ? "entry" : "entries")
			.arg(policyDropped == 1 ? "was" : "were");
		message = message.isEmpty() ? droppedText : message + " " + droppedText;
	}
	if (!message.isEmpty()) {
		statusBar()->showMessage(message, 10000);
		if (tray_icon_ && tray_icon_->isVisible()) {
			tray_icon_->showMessage("uTimer Recovery", message, QSystemTrayIcon::Information, 10000);
		}
	}

	health_monitor_->reset();
}

/**
 * Delegates the application shutdown sequence to ShutdownCoordinator.
 *
 * Normal quit (DrainEvents): one drain attempt; on failure, a modal
 * Retry / Exit Anyway choice is shown. Each Retry performs exactly one new
 * drain attempt. Exit Anyway stops asking and lets the process terminate
 * without a clean marker, leaving existing durable checkpoints untouched.
 *
 * A normal quit reaches this method TWICE -- closeEvent (X / Alt+F4) and then
 * QCoreApplication::aboutToQuit once the last window closes; SIGTERM likewise
 * calls onAboutToQuit() before quit() re-emits aboutToQuit. Since the
 * coordinator's run() is guarded and replays its cached (still Incomplete)
 * result, unsaved_loss_accepted_ makes the user's decline stick so the modal
 * cannot reappear on the second entry.
 *
 * OS-forced shutdown (Direct): exactly one attempt, no warning and no modal
 * on failure -- the OS event loop is restricted during this path.
 *
 * @param mode DrainEvents for normal quit; Direct for OS-restricted paths
 *             (Windows WM_QUERYENDSESSION / WM_ENDSESSION).
 */
void MainWin::shutdown(ShutdownMode mode)
{
	DrainResult result = shutdown_coordinator_.run(mode);
	if (mode == ShutdownMode::Direct) {
		return;
	}
	while (!result.clean()) {
		if (unsaved_loss_accepted_) {
			// The user already accepted this loss; never ask a second time.
			return;
		}
		QMessageBox box(this);
		box.setIcon(QMessageBox::Warning);
		if (result.markerOnlyFailure) {
			// Medium-3 (R05-002 follow-up): the drain itself was Complete --
			// session data is already saved. Only the clean-shutdown record
			// failed to write, so this must not be presented as data loss.
			box.setWindowTitle("uTimer - Shutdown Record Not Written");
			box.setText("Your session was saved, but uTimer could not write the clean-shutdown record. Retry, or exit anyway?");
		} else {
			box.setWindowTitle("uTimer - Could Not Save");
			box.setText("Could not save the current session. Retry, or exit and keep only what was already saved?");
		}
		QAbstractButton* retryButton = box.addButton("Retry", QMessageBox::AcceptRole);
		box.addButton("Exit Anyway", QMessageBox::DestructiveRole);
		box.setDefaultButton(static_cast<QPushButton*>(retryButton));
		box.exec();
		if (box.clickedButton() != retryButton) {
			// Exit Anyway: latch the decision so a later shutdown entry point
			// does not ask again, then let the caller terminate.
			unsaved_loss_accepted_ = true;
			break;
		}
		result = shutdown_coordinator_.retryDrain();
	}
}

void MainWin::onAboutToQuit()
{
	Logger::Log("[TIMER] AboutToQuit received");
	shutdown(ShutdownMode::DrainEvents);
}

void MainWin::closeEvent(QCloseEvent *event)
{
	Logger::Log("[TIMER] CloseEvent received");
	// Handle manual window closing (Alt+F4, X button, etc.)
	shutdown(ShutdownMode::DrainEvents);
	event->accept();
}

/**
 * D5 (R15-002): pure translation of a Windows shutdown-related message into
 * a decision, independent of MSG/WPARAM so it compiles and is unit-testable
 * on any platform. Mirrors LockStateWatcher::translateWtsSessionState (D3).
 */
WindowsShutdownDecision MainWin::translateWindowsShutdownMessage(quint32 message, quint64 wParam)
{
	constexpr quint32 kWmQueryEndSession = 0x0011;
	constexpr quint32 kWmEndSession = 0x0016;

	if (message == kWmQueryEndSession)
		return WindowsShutdownDecision::Query;
	if (message == kWmEndSession)
		return wParam != 0 ? WindowsShutdownDecision::EndSessionEnding
		                    : WindowsShutdownDecision::EndSessionCancelled;
	return WindowsShutdownDecision::Ignore;
}

bool MainWin::nativeEvent([[maybe_unused]]const QByteArray& eventType, void* message, long* result)
{
#ifdef Q_OS_WIN
	MSG* msg = static_cast<MSG*>(message);
	const WindowsShutdownDecision decision =
		translateWindowsShutdownMessage(static_cast<quint32>(msg->message), static_cast<quint64>(msg->wParam));

	switch (decision) {
	case WindowsShutdownDecision::Query:
		Logger::Log("[TIMER] WM_QUERYENDSESSION received - starting early save");
		// Windows is asking if we can shutdown
		// Best practice: start saving NOW to have more time before WM_ENDSESSION
		// Use Direct mode since the OS event loop is restricted during shutdown.
		shutdown(ShutdownMode::Direct);
		*result = TRUE;
		return true;
	case WindowsShutdownDecision::EndSessionEnding:
		Logger::Log("[TIMER] WM_ENDSESSION received (session_ending=true)");
		// Windows is actually shutting down - ensure timer is stopped
		// shutdown() guard will skip if already done in WM_QUERYENDSESSION
		shutdown(ShutdownMode::Direct);
		*result = 0;
		return true;
	case WindowsShutdownDecision::EndSessionCancelled:
		Logger::Log("[TIMER] WM_ENDSESSION received (session_ending=false)");
		// Shutdown was cancelled. WM_QUERYENDSESSION already ran the save
		// path; cancelShutdown() re-arms the coordinator's guard and clears
		// TimeTracker's terminal flag so the next real shutdown runs the
		// full save path again and History can save without a new Start
		// (the data saved earlier stays valid).
		shutdown_coordinator_.cancelShutdown();
		*result = 0;
		return true;
	case WindowsShutdownDecision::Ignore:
		break;
	}
#endif
	return QMainWindow::nativeEvent(eventType, message, result);
}

