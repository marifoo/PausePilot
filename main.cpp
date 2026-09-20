#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QGuiApplication>
#include <QLockFile>
#include <QMessageBox>
#include <QStyleFactory>
#include <QTimer>
#include <cstdio>
#ifdef Q_OS_LINUX
#include <QLoggingCategory>
#include <QSocketNotifier>
#endif

#include "apppaths.h"
#include "settings.h"
#include "mainwin.h"
#include "shutdowncoordinator.h"
#include "timetracker.h"
#include "sqlitesessionstore.h"
#include "lockstatewatcher.h"
#include "logger.h"
#include "types.h"
#include "startuprecovery.h"
#include "lockdecision.h"
#include "unixsignalbridge.h"

/**
 * Application Entry Point.
 *
 * Startup Sequence:
 * 1. Initialize Signal Handlers (Linux-specific for graceful shutdown).
 * 2. Setup Qt Application and Style.
 * 3. Instantiate Core Components:
 *    - Settings (Configuration)
 *    - LockStateWatcher (Hardware abstraction)
 *    - TimeTracker (Business Logic)
 *    - MainWin (Presentation)
 * 4. Wire up Signals/Slots to connect the components.
 * 5. Start the main event loop.
 */
int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
	// Suppress KDE Framework warnings about missing platform plugin
	QLoggingCategory::setFilterRules("kf.windowsystem=false");

	// Set up the Unix signal bridge before creating QApplication. On
	// failure, continue startup without UI, a fallback signal mechanism, or
	// a degraded-mode flag (D4, section 2 "Application shell and
	// diagnostics"); Logger::Log() is a silent no-op until Logger is
	// configured below, which is exactly "if available" at this point.
	UnixSignalBridge signal_bridge;
	if (!signal_bridge.setup()) {
		const QString msg = "Failed to set up Unix signal bridge; graceful shutdown on SIGTERM/SIGINT/SIGHUP will not work";
		std::fprintf(stderr, "[SIGNAL] %s\n", msg.toLocal8Bit().constData());
		std::fflush(stderr);
		Logger::Log("[SIGNAL] " + msg);
	}
#endif

	QCoreApplication::setApplicationName("µTimer");

	QApplication application(argc, argv);
	application.setStyle(QStyleFactory::create("Fusion"));

	// Refuse a second instance: two instances racing recoverStartupCheckpoints()
	// against each other's live checkpoint row (and createBackup()'s close-copy-
	// reopen against a concurrently-written file) can corrupt the database.
	// QLockFile also detects and clears a stale lock left by a crashed process,
	// so a crash does not lock the user out permanently.
	//
	// Fail closed on every tryLock() failure (R15-001): the data directory
	// (schema check, recovery, store writes, window construction -- all
	// below this block) must only be touched once exclusive ownership is
	// proven, not merely when the failure happens to look like "another
	// instance is running". The diagnostic is always printed to stderr so a
	// non-interactive session (no window manager, offscreen QPA, a service
	// supervisor) still gets it and startup is never blocked waiting on a
	// modal; the message box is shown in addition wherever a user is present
	// to dismiss it.
	QLockFile instanceLock(AppPaths::instanceLockFile());
	if (!instanceLock.tryLock(100)) {
		const LockDecision::Outcome outcome = LockDecision::classify(instanceLock.error());
		const QString text = LockDecision::message(outcome);

		std::fprintf(stderr, "[LOCK] %s\n", text.toLocal8Bit().constData());
		std::fflush(stderr);

		// "minimal" has no user to dismiss a modal either (like "offscreen")
		// but still creates windows, so QMessageBox::critical would block
		// forever there instead of returning immediately.
		const QString platform = QGuiApplication::platformName();
		if (platform != "offscreen" && platform != "minimal")
			QMessageBox::critical(nullptr, "µTimer", text);

		return LockDecision::exitCode(outcome);
	}

	QTimer timer;

	Settings settings(AppPaths::settingsFile());
	Logger::configure(settings.logToFile());
	LockStateWatcher lockstate_watcher(settings);
	SqliteSessionStore session_store(settings);
	TimeTracker time_tracker(settings, session_store);

	// Validate schema before any recovery or data access.
	const SchemaStatus schemaStatus = session_store.checkSchemaOnStartup();
	if (schemaStatus == SchemaStatus::Outdated) {
		QMessageBox::critical(nullptr, "Database Schema Mismatch",
			"The database schema is not compatible with this version of µTimer.\n\n"
			"Please rename or delete the following file and restart:\n"
			+ AppPaths::databaseFile());
		return 1;
	}
	if (schemaStatus == SchemaStatus::Inaccessible) {
		QMessageBox::critical(nullptr, "Database Error",
			"Could not open or initialize the database.\n\n"
			"Please check the following file and restart:\n"
			+ AppPaths::databaseFile());
		return 1;
	}

	// Schema is Ready or Created — safe to recover from the store.
	// runStartupRecovery() completes (or the user Quits) before the main
	// window is created below, so no tracking is possible before recovery
	// succeeds (B2, section 2 "Resolved product and domain policies").
	if (!runStartupRecovery(time_tracker)) {
		return 1;
	}

	ShutdownCoordinator shutdown_coordinator(time_tracker, session_store);
	MainWin main_win(settings, time_tracker, session_store, shutdown_coordinator);

#ifdef Q_OS_LINUX
	// Set up socket notifier to handle Unix signals in the Qt event loop.
	// drainPending() coalesces however many signals arrived between two
	// event-loop turns into this one shutdown invocation (D4 step 3).
	QSocketNotifier* signalNotifier = nullptr;
	if (signal_bridge.notifierFd() != -1) {
		signalNotifier = new QSocketNotifier(signal_bridge.notifierFd(), QSocketNotifier::Read, &application);
		QObject::connect(signalNotifier, &QSocketNotifier::activated, [&](int) {
			if (signal_bridge.drainPending()) {
				Logger::Log("[SIGNAL] Received Unix signal - initiating shutdown");

				// Trigger graceful shutdown
				main_win.onAboutToQuit();
				QCoreApplication::quit();
			}
		});
	}
#endif

	QObject::connect(&timer, SIGNAL(timeout()), &main_win, SLOT(onTick()));
	QObject::connect(&timer, &QTimer::timeout, [&time_tracker]() {
		time_tracker.onTick(QDateTime::currentDateTime());
	});

	QObject::connect(&timer, SIGNAL(timeout()), &lockstate_watcher, SLOT(update()));
	QObject::connect(&lockstate_watcher, SIGNAL(desktopLockEvent(LockEvent,QDateTime)),	&time_tracker, SLOT(useTimerViaLockEvent(LockEvent,QDateTime)));
	QObject::connect(&time_tracker, &TimeTracker::userWarning, &main_win, &MainWin::showUserWarning);
	// C4 (round-3 F2): must stay QueuedConnection. A direct connection would run
	// QMessageBox::critical()'s nested event loop synchronously inside the
	// still-in-progress TimeTracker transition that emitted fatalError (the
	// mutex is recursive, so it would not block), letting onTick()/lock events
	// re-enter and mutate mode_/session_ underneath the half-finished caller.
	QObject::connect(&time_tracker, &TimeTracker::fatalError, &main_win, &MainWin::showFatalError, Qt::QueuedConnection);

	QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &main_win, &MainWin::onAboutToQuit);

	timer.setInterval(100);
	timer.start();

	main_win.start();

	return application.exec();
}
