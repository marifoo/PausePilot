#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QLockFile>
#include <QMessageBox>
#include <QStyleFactory>
#include <QTimer>
#ifdef Q_OS_LINUX
#include <QLoggingCategory>
#include <QSocketNotifier>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "apppaths.h"
#include "settings.h"
#include "mainwin.h"
#include "shutdowncoordinator.h"
#include "timer.h"
#include "sqlitesessionstore.h"
#include "lockstatewatcher.h"
#include "logger.h"
#include "types.h"

#ifdef Q_OS_LINUX
// Unix signal handling - safe pattern using socketpair
// Signal handlers can only call async-signal-safe functions,
// so we write to a socket and let Qt handle it in the event loop

static int signalFd[2] = {-1, -1};

static void unixSignalHandler(int signum)
{
    // Write the signal number to the socket (async-signal-safe)
    char sig = static_cast<char>(signum);
    if (write(signalFd[0], &sig, sizeof(sig)) == -1) {
        // Can't do much here - we're in a signal handler
    }
}

static bool setupUnixSignalHandlers()
{
    // Create socket pair for signal communication
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, signalFd) != 0) {
        qWarning() << "Failed to create socketpair for signal handling";
        return false;
    }

    // Install signal handlers
    struct sigaction sa;
    sa.sa_handler = unixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;  // Restart interrupted system calls

    if (sigaction(SIGTERM, &sa, nullptr) != 0) {
        qWarning() << "Failed to install SIGTERM handler";
        return false;
    }
    if (sigaction(SIGINT, &sa, nullptr) != 0) {
        qWarning() << "Failed to install SIGINT handler";
        return false;
    }
    if (sigaction(SIGHUP, &sa, nullptr) != 0) {
        qWarning() << "Failed to install SIGHUP handler";
        return false;
    }

    return true;
}
#endif

/**
 * Application Entry Point.
 *
 * Startup Sequence:
 * 1. Initialize Signal Handlers (Linux-specific for graceful shutdown).
 * 2. Setup Qt Application and Style.
 * 3. Instantiate Core Components:
 *    - Settings (Configuration)
 *    - LockStateWatcher (Hardware abstraction)
 *    - Timer (Business Logic)
 *    - MainWin (Presentation)
 * 4. Wire up Signals/Slots to connect the components.
 * 5. Start the main event loop.
 */
int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
	// Suppress KDE Framework warnings about missing platform plugin
	QLoggingCategory::setFilterRules("kf.windowsystem=false");

	// Set up Unix signal handlers before creating QApplication
	setupUnixSignalHandlers();
#endif

	QCoreApplication::setApplicationName("µTimer");

	QApplication application(argc, argv);
	application.setStyle(QStyleFactory::create("Fusion"));

	// Refuse a second instance: two instances racing recoverStartupCheckpoints()
	// against each other's live checkpoint row (and createBackup()'s close-copy-
	// reopen against a concurrently-written file) can corrupt the database.
	// QLockFile also detects and clears a stale lock left by a crashed process,
	// so a crash does not lock the user out permanently.
	QLockFile instanceLock(AppPaths::instanceLockFile());
	if (!instanceLock.tryLock(100)) {
		if (instanceLock.error() == QLockFile::LockFailedError) {
			QMessageBox::critical(nullptr, "µTimer Already Running",
				"Another instance of µTimer is already running.\n\n"
				"Please close it before starting a new one.");
			return 1;
		}
		// Any other error (e.g. the app directory is not writable) is not a
		// second-instance conflict -- fall through and let the existing
		// DB-writability check in checkSchemaOnStartup() report it accurately.
		qWarning() << "Could not create instance lock file, error code:" << instanceLock.error();
	}

	QTimer timer;

	Settings settings(AppPaths::settingsFile());
	Logger::registerSettings(&settings);
	LockStateWatcher lockstate_watcher(settings);
	SqliteSessionStore session_store(settings);
	Timer time_tracker(settings, session_store);

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
	time_tracker.initializeFromStore();

	ShutdownCoordinator shutdown_coordinator(time_tracker, session_store);
	MainWin main_win(settings, time_tracker, session_store, shutdown_coordinator);

#ifdef Q_OS_LINUX
	// Set up socket notifier to handle Unix signals in the Qt event loop
	QSocketNotifier* signalNotifier = nullptr;
	if (signalFd[1] != -1) {
		signalNotifier = new QSocketNotifier(signalFd[1], QSocketNotifier::Read, &application);
		QObject::connect(signalNotifier, &QSocketNotifier::activated, [&](int) {
			// Read and discard the signal number
			char sig;
			if (read(signalFd[1], &sig, sizeof(sig)) > 0) {
				{
					QString signalName;
					switch (sig) {
						case SIGTERM: signalName = "SIGTERM"; break;
						case SIGINT:  signalName = "SIGINT"; break;
						case SIGHUP:  signalName = "SIGHUP"; break;
						default:      signalName = QString::number(sig); break;
					}
					Logger::Log("[SIGNAL] Received " + signalName + " - initiating shutdown");
				}

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
	QObject::connect(&time_tracker, &Timer::userWarning, &main_win, &MainWin::showUserWarning);

	QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &main_win, &MainWin::onAboutToQuit);

	timer.setInterval(100);
	timer.start();

	main_win.start();

	return application.exec();
}
