#include "shutdowncoordinator.h"
#include "timer.h"
#include "sessionstore.h"
#include "logger.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QCoreApplication>

ShutdownCoordinator::ShutdownCoordinator(Timer& timetracker, SessionStore& db)
    : timetracker_(timetracker), db_(db), shutdown_completed_(false)
{
}

void ShutdownCoordinator::pumpEvents(int budgetMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
}

void ShutdownCoordinator::reset()
{
    Logger::Log("[TIMER] Shutdown guard reset (OS shutdown cancelled)");
    shutdown_completed_ = false;
}

void ShutdownCoordinator::run(ShutdownMode mode)
{
    if (shutdown_completed_) {
        Logger::Log("[TIMER] Shutdown already completed, skipping");
        return;
    }
    // Set the guard immediately after the check: a re-entrant run() dispatched
    // inside pumpEvents(150) below must be short-circuited by the guard above,
    // otherwise it would flush (and pump) a second time.
    shutdown_completed_ = true;

    Logger::Log(QString("[TIMER] Shutdown requested (mode=%1)")
                .arg(mode == ShutdownMode::Direct ? "Direct" : "DrainEvents"));

    auto result = timetracker_.shutdown();

    if (mode == ShutdownMode::DrainEvents) {
        // Normal shutdown: pump the event loop so connected signals
        // (GUI sync, checkpoint flush) can fire before we proceed.
        pumpEvents(150);
        // Pump re-entrancy: a queued START click dispatched inside pumpEvents()
        // can restart the timer after shutdown(). Re-check and re-stop so we
        // never leave a live segment behind after shutdown.
        if (!timetracker_.isStopped()) {
            Logger::Log("[TIMER] Timer restarted during shutdown pump; re-stopping");
            result = timetracker_.shutdown();
        }
    }

    Logger::Log("[DB] Flushing database to disk before shutdown");
    const SessionStoreResult flushResult = db_.flushToDisc();
    if (!flushResult.ok() && flushResult.category != SessionStoreResult::Disabled) {
        Logger::Log("[DB] Warning: flushToDisc failed during shutdown: " + flushResult.message);
    }

    if (result.canCleanMark && !result.stopCalled) {
        const SessionStoreResult markerResult = db_.setLastCleanShutdownMarker(QDateTime::currentDateTime());
        if (!markerResult.ok() && markerResult.category != SessionStoreResult::Disabled) {
            Logger::Log("[DB] Warning: setLastCleanShutdownMarker failed during shutdown: " + markerResult.message);
        }
    }

    if (!result.stopped) {
        Logger::Log("[TIMER] Error: Timer did not stop correctly during shutdown");
    } else {
        Logger::Log("[TIMER] Shutdown completed successfully");
    }
}
