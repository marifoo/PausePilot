#ifndef SHUTDOWNCOORDINATOR_H
#define SHUTDOWNCOORDINATOR_H

#include "sessionstore.h"  // DrainResult, SessionStoreResult

class TimeTracker;

/// Controls which shutdown path ShutdownCoordinator::run() takes.
/// DrainEvents: pump the Qt event loop for 150 ms so queued signals
///              (GUI sync, checkpoint flush) can fire before cleanup.
/// Direct:      skip the event-loop pump — required when the OS event loop
///              is restricted (Windows WM_QUERYENDSESSION / WM_ENDSESSION).
enum class ShutdownMode { DrainEvents, Direct };

class ShutdownCoordinator
{
public:
    ShutdownCoordinator(TimeTracker& timetracker, SessionStore& db);

    /// Runs the drain-flush-marker sequence once per shutdown (guarded by an
    /// internal completion flag; a second call is a no-op that returns the
    /// previous result). Returns the typed drain outcome so MainWin can
    /// decide whether to present a Retry/Exit Anyway choice.
    DrainResult run(ShutdownMode mode = ShutdownMode::DrainEvents);

    /// One additional drain attempt requested by the user via Retry, after
    /// run() reported an incomplete drain. Not guarded by run()'s completion
    /// flag -- run() already completed (unsuccessfully) and this performs
    /// exactly one more attempt, flushing and writing the marker again if it
    /// now succeeds.
    DrainResult retryDrain();

    /// D5 (R15-002): the one cancellation operation owned by the shutdown
    /// lifecycle. Called when an OS shutdown is cancelled (Windows
    /// WM_ENDSESSION with wParam=FALSE) after WM_QUERYENDSESSION already ran
    /// the save path. Atomically re-arms the coordinator's own guard so a
    /// later run() executes the full save path again, AND clears
    /// TimeTracker's terminal shutdown_called_ flag so the application
    /// returns to an ordinary, fully usable stopped state (History can save
    /// immediately, with no new Start required). Does not reconstruct or
    /// resume the ended session -- pending persistence stays governed by
    /// A2/A3 and is retried at the next Start, not here.
    void cancelShutdown();

private:
    void pumpEvents(int budgetMs);
    /// Shared flush + marker-write tail used by both run() and retryDrain().
    DrainResult finishDrain(DrainResult result);

    TimeTracker& timetracker_;
    SessionStore& db_;
    bool shutdown_completed_;
    DrainResult last_result_;
};

#endif // SHUTDOWNCOORDINATOR_H
