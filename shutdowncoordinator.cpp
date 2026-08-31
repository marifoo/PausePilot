#include "shutdowncoordinator.h"
#include "timetracker.h"
#include "sessionstore.h"
#include "logger.h"
#include <QDateTime>
#include <QEventLoop>
#include <QTimer>

ShutdownCoordinator::ShutdownCoordinator(TimeTracker& timetracker, SessionStore& db)
    : timetracker_(timetracker), db_(db), shutdown_completed_(false)
{
}

void ShutdownCoordinator::pumpEvents(int budgetMs)
{
    QEventLoop loop;
    QTimer::singleShot(budgetMs, &loop, &QEventLoop::quit);
    loop.exec();
}

void ShutdownCoordinator::cancelShutdown()
{
    Logger::Log("[TIMER] Shutdown cancelled -- re-arming guard and tracker terminal state");
    shutdown_completed_ = false;
    timetracker_.cancelShutdown();
}

DrainResult ShutdownCoordinator::finishDrain(DrainResult result)
{
    Logger::Log("[DB] Flushing database to disk before shutdown");
    const SessionStoreResult flushResult = db_.flushToDisc();
    if (!flushResult.ok() && flushResult.category != SessionStoreResult::Disabled) {
        Logger::Log("[DB] Warning: flushToDisc failed during shutdown: " + flushResult.message);
    }

    // Eligibility is the drain's own outcome, not whether this call happened
    // to stop a live segment. markerAlreadyWritten only avoids a redundant
    // second write when stopTimer() already wrote it synchronously.
    if (result.clean() && !result.markerAlreadyWritten) {
        const SessionStoreResult markerResult = db_.setLastCleanShutdownMarker(QDateTime::currentDateTime());
        if (!markerResult.ok() && markerResult.category != SessionStoreResult::Disabled) {
            Logger::Log("[DB] Warning: setLastCleanShutdownMarker failed during shutdown: " + markerResult.message);
        }
    }

    if (!timetracker_.isStopped()) {
        Logger::Log("[TIMER] Error: TimeTracker did not stop correctly during shutdown");
    } else if (!result.clean()) {
        // Do not claim success beside a truthful Incomplete result: the
        // engine stopped, but a commit payload is still only in memory.
        Logger::Log("[TIMER] Shutdown completed WITH UNSAVED DATA: " + result.message);
    } else {
        Logger::Log("[TIMER] Shutdown completed successfully");
    }
    return result;
}

DrainResult ShutdownCoordinator::run(ShutdownMode mode)
{
    if (shutdown_completed_) {
        Logger::Log("[TIMER] Shutdown already completed, skipping");
        return last_result_;
    }
    // Set the guard immediately after the check: a re-entrant run() dispatched
    // inside pumpEvents(150) below must be short-circuited by the guard above,
    // otherwise it would flush (and pump) a second time.
    shutdown_completed_ = true;

    Logger::Log(QString("[TIMER] Shutdown requested (mode=%1)")
                .arg(mode == ShutdownMode::Direct ? "Direct" : "DrainEvents"));

    DrainResult result = timetracker_.drainForShutdown();

    if (mode == ShutdownMode::DrainEvents) {
        // Normal shutdown: pump the event loop so connected signals
        // (GUI sync, checkpoint flush) can fire before we proceed.
        pumpEvents(150);
        // Pump re-entrancy: a queued START click dispatched inside pumpEvents()
        // can restart the timer after the drain. Re-check and re-drain so we
        // never leave a live segment behind after shutdown.
        if (!timetracker_.isStopped()) {
            Logger::Log("[TIMER] TimeTracker restarted during shutdown pump; re-stopping");
            result = timetracker_.drainForShutdown();
        }
    }

    last_result_ = finishDrain(result);
    return last_result_;
}

DrainResult ShutdownCoordinator::retryDrain()
{
    Logger::Log("[TIMER] Shutdown retry requested");
    // A user-driven Retry is a genuinely new attempt, so it reports its own
    // fresh outcome rather than the first failure latched by run()'s drain.
    last_result_ = finishDrain(timetracker_.drainForShutdown(TimeTracker::DrainAttempt::UserRetry));
    return last_result_;
}
