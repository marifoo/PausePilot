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
