#ifndef MAINWIN_H
#define MAINWIN_H

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QString>
#include "contentwidget.h"
#include "healthmonitor.h"
#include "settings.h"
#include "types.h"
#include "sessionstore.h"
#include "shutdowncoordinator.h"

/// Controls which shutdown path MainWin::shutdown() takes.
/// Interactive: normal quit path; on drain failure, shows the Retry / Exit
///              Anyway modal.
/// Direct:      OS-forced shutdown path -- no modal, since the OS event loop
///              is restricted (Windows WM_QUERYENDSESSION / WM_ENDSESSION).
enum class ShutdownMode { Interactive, Direct };

/// D5 (R15-002): the pure decision produced from a Windows shutdown-related
/// native message, independent of MSG/WPARAM types so it is unit-testable on
/// any platform (mirrors LockStateWatcher::translateWtsSessionState, D3).
enum class WindowsShutdownDecision {
	Ignore,              ///< not a shutdown-related message.
	Query,                ///< WM_QUERYENDSESSION: run Direct shutdown, allow (TRUE).
	EndSessionEnding,     ///< WM_ENDSESSION, wParam != 0: session is actually ending.
	EndSessionCancelled,  ///< WM_ENDSESSION, wParam == 0: shutdown/logoff was cancelled.
};

class MainWin : public QMainWindow
{
	Q_OBJECT
private:
	ContentWidget *content_widget_;
	QSystemTrayIcon *tray_icon_;
	const Settings & settings_;
	TimeTracker& timetracker_;
	SessionStore& db_;
	ShutdownCoordinator& shutdown_coordinator_;
	HealthMonitor* health_monitor_;
	/// Latched once the user answers "Exit Anyway" to the unsaved-data modal.
	/// That answer is a decision about terminating the application, not about
	/// one entry point: a normal quit reaches shutdown() twice (closeEvent,
	/// then aboutToQuit once the last window closes), and ShutdownCoordinator
	/// replays its cached Incomplete result on the guarded second run(). The
	/// latch guarantees the user is asked at most once per termination.
	bool unsaved_loss_accepted_ = false;


	void showMsgBox(const QString &text);
	void showMainWin();
	void toggleAlwaysOnTopFlag();
	void setupIcon();
	void setupCentralWidget(Settings &settings, TimeTracker &timetracker);
	void shutdown(ShutdownMode mode);
	/// Pure decision from a Windows shutdown-related native message. Takes
	/// plain integers (not MSG/WPARAM) so it compiles and is unit-testable
	/// on any platform; nativeEvent() only dispatches on its result.
	static WindowsShutdownDecision translateWindowsShutdownMessage(quint32 message, quint64 wParam);

protected:
	bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
	void closeEvent(QCloseEvent *event) override;

public:
	explicit MainWin(Settings &settings, TimeTracker &timetracker, SessionStore &db,
	                 ShutdownCoordinator &shutdown_coordinator, QWidget *parent = nullptr);
	/// Applies startup preferences (pin, minimize, auto-start) and shows crash-recovery notification.
	void start();

#ifndef QT_NO_DEBUG
	/// Runs the shutdown sequence for `mode`. Test seam only: the Direct
	/// (OS-forced) path is otherwise reachable exclusively from the
	/// Windows-only nativeEvent handler, so tests on other platforms have no
	/// way to exercise its no-warning/no-marker contract.
	void shutdownForTest_dbg(ShutdownMode mode) { shutdown(mode); }
	/// Exposes the pure Windows shutdown-message decision seam for direct
	/// unit testing on any platform (D5, R15-002).
	static WindowsShutdownDecision translateWindowsShutdownMessage_dbg(quint32 message, quint64 wParam) {
		return translateWindowsShutdownMessage(message, wParam);
	}
#endif // QT_NO_DEBUG

public slots:
	/// 100ms heartbeat: refreshes display and runs health checks.
	void onTick();
	void iconActivated(QSystemTrayIcon::ActivationReason reason);
	void minToTray();
	void toggleAlwaysOnTop();
	void showUserWarning(const QString& text);
	void showHistoryLoadReconciliation(const QString& text);
	void onAboutToQuit();
};

#endif // MAINWIN_H
