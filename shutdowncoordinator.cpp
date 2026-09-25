#include "shutdowncoordinator.h"
#include "timetracker.h"
#include "sessionstore.h"
#include "logger.h"

ShutdownCoordinator::ShutdownCoordinator(TimeTracker& timetracker, SessionStore& db)
    : timetracker_(timetracker), db_(db), shutdown_completed_(false)
{
}

void ShutdownCoordinator::cancelShutdown()
{
    Logger::Log("[TIMER] Shutdown cancelled -- re-arming guard and tracker terminal state");
    shutdown_completed_ = false;
    timetracker_.cancelShutdown();
}

DrainResult ShutdownCoordinator::finishDrain(DrainResult result)
{
    // D6: clean marking (marker write + FULL barrier) is one durable store
    // operation, attempted only when the drain itself is clean -- a failed
    // marker write never even attempts the barrier (see
    // SessionStore::markCleanShutdown()'s contract).
    if (result.clean()) {
        Logger::Log("[DB] Marking clean shutdown (durable marker + FULL barrier)");
        const MarkCleanShutdownResult markResult = db_.markCleanShutdown();
        if (!markResult.ok() && markResult.result.category != SessionStoreResult::Disabled) {
            Logger::Log("[DB] Warning: markCleanShutdown failed during shutdown: " + markResult.result.message);
            // M1: only a marker-WRITE failure (markerWritten == false) is
            // "the record itself was never committed" -- session data is
            // durable either way, but a barrier failure after a
            // successfully written marker is a genuine durability failure,
            // not a "record not written" one, and must not be presented
            // the same reassuring way (see DrainResult::markerOnlyFailure).
            result = markResult.markerWritten
                ? DrainResult::incomplete(markResult.result.category, markResult.result.message)
                : DrainResult::markerFailed(markResult.result.category, markResult.result.message);
        }
    } else {
        // Medium-2: an incomplete drain (Exit Anyway, a failed forced
        // shutdown) bypasses the clean MARKER only, never the durability
        // barrier itself -- plan section 2 promises existing durable
        // checkpoints remain "available for best-effort startup recovery",
        // which requires this fsync to still run.
        Logger::Log("[DB] Drain incomplete; running durability barrier without a clean marker");
        const SessionStoreResult flushResult = db_.flushToDisc();
        if (!flushResult.ok() && flushResult.category != SessionStoreResult::Disabled) {
            Logger::Log("[DB] Warning: flushToDisc failed during shutdown: " + flushResult.message);
        }
    }

    if (!timetracker_.isStopped()) {
        Logger::Log("[TIMER] Error: TimeTracker did not stop correctly during shutdown");
    } else if (!result.clean() && result.markerOnlyFailure) {
        // Medium-3: session data is durable -- only the clean-shutdown
        // record failed to write. Do not log this as unsaved data.
        Logger::Log("[DB] Shutdown completed; session data saved but clean-shutdown marker failed: " + result.message);
    } else if (!result.clean()) {
        // Do not claim success beside a truthful Incomplete result: the
        // engine stopped, but a commit payload is still only in memory.
        Logger::Log("[TIMER] Shutdown completed WITH UNSAVED DATA: " + result.message);
    } else {
        Logger::Log("[TIMER] Shutdown completed successfully");
    }
    return result;
}

DrainResult ShutdownCoordinator::run()
{
    if (shutdown_completed_) {
        Logger::Log("[TIMER] Shutdown already completed, skipping");
        return last_result_;
    }
    // Set the guard immediately after the check: any Start dispatched once
    // run() returns (e.g. from a queued signal) must be refused by
    // TimeTracker's own terminal shutdown_called_ flag (C9b) rather than
    // reopening this drain.
    shutdown_completed_ = true;

    Logger::Log("[TIMER] Shutdown requested");

    const DrainResult result = timetracker_.drainForShutdown();

    last_result_ = finishDrain(result);
    return last_result_;
}

DrainResult ShutdownCoordinator::retryDrain()
{
    Logger::Log("[TIMER] Shutdown retry requested");
    last_result_ = finishDrain(timetracker_.drainForShutdown());
    return last_result_;
}
