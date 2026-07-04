#ifndef SHUTDOWNCOORDINATOR_H
#define SHUTDOWNCOORDINATOR_H

class Timer;
class SessionStore;

/// Controls which shutdown path ShutdownCoordinator::run() takes.
/// DrainEvents: pump the Qt event loop for 150 ms so queued signals
///              (GUI sync, checkpoint flush) can fire before cleanup.
/// Direct:      skip the event-loop pump — required when the OS event loop
///              is restricted (Windows WM_QUERYENDSESSION / WM_ENDSESSION).
enum class ShutdownMode { DrainEvents, Direct };

class ShutdownCoordinator
{
public:
    ShutdownCoordinator(Timer& timetracker, SessionStore& db);

    void run(ShutdownMode mode = ShutdownMode::DrainEvents);

    /// Re-arms the shutdown guard so a later run() executes the full save path
    /// again. Called when an OS shutdown is cancelled (Windows WM_ENDSESSION
    /// with wParam=FALSE) after WM_QUERYENDSESSION already ran the save path.
    void reset();

private:
    void pumpEvents(int budgetMs);

    Timer& timetracker_;
    SessionStore& db_;
    bool shutdown_completed_;
};

#endif // SHUTDOWNCOORDINATOR_H
