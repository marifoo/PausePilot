#ifndef SHUTDOWNCOORDINATOR_H
#define SHUTDOWNCOORDINATOR_H

#include "sessionstore.h"  // DrainResult, SessionStoreResult

class TimeTracker;

class ShutdownCoordinator
{
public:
    ShutdownCoordinator(TimeTracker& timetracker, SessionStore& db);

    /// Runs the drain-flush-marker sequence once per shutdown (guarded by an
    /// internal completion flag; a second call is a no-op that returns the
    /// previous result). Returns the typed drain outcome so MainWin can
    /// decide whether to present a Retry/Exit Anyway choice.
    DrainResult run();

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
    /// returns to an ordinary, fully usable stopped state, with no new Start
    /// required. Does not reconstruct or resume the ended session -- pending
    /// persistence stays governed by
    /// A2/A3 and is retried at the next Start, not here.
    void cancelShutdown();

private:
    /// Shared tail used by both run() and retryDrain(): iff the drain
    /// result is clean, invokes the store's single durable shutdown
    /// operation (D6: marker write + FULL durability barrier); a failure
    /// there downgrades the result to non-clean, marked markerOnlyFailure
    /// only when the marker-WRITE step itself failed (M1) -- a barrier
    /// failure after a successfully written marker is a genuine durability
    /// failure and is deliberately NOT markerOnlyFailure. An
    /// already-incomplete drain still runs the durability barrier alone
    /// (Medium-2) -- it just never attempts the clean marker.
    DrainResult finishDrain(DrainResult result);

    TimeTracker& timetracker_;
    SessionStore& db_;
    bool shutdown_completed_;
    DrainResult last_result_;
};

#endif // SHUTDOWNCOORDINATOR_H
