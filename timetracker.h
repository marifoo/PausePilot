#ifndef TIMER_H
#define TIMER_H

#include <QObject>
#include <QtGlobal>
#include <QElapsedTimer>
#include <QDateTime>
#include <QMutex>
#include <QTimer>
#include <optional>
#include <deque>
#include <memory>
#include <utility>
#include <functional>
#include "settings.h"
#include "types.h"
#include "sessionstore.h"
#include "timeline.h"

/**
 * SessionState — groups the mutable per-session data that TimeTracker manages.
 *
 * Previously these fields were scattered as raw members on TimeTracker,
 * making it easy for code paths to mutate them as side effects without the
 * caller being aware. Grouping them into a struct with explicit transition
 * methods ensures:
 *   - Every mutation is named and logged (in debug builds)
 *   - State changes are traceable during debugging
 *   - Callers like addDuration can compute results without touching the struct
 *     directly, and the caller applies changes explicitly
 *
 * The struct is intentionally not a class — TimeTracker is its sole owner
 * and accesses fields directly. The transition methods are convenience
 * helpers that enforce logging discipline, not access control.
 */
struct SessionState {
    /// Completed duration segments for the current session (in-memory).
    std::deque<TimeDuration> durations;

    /// Segment identity for checkpoint saves.  Empty when the timer
    /// is stopped (Mode::None).  Rotated on every mode transition so each
    /// segment gets its own DB row.
    SegmentId segment_id;

    /// Wall-clock time when the current segment started. Invalid when the
    /// timer is stopped. Used together with "now" to compute the ongoing
    /// segment's duration.
    QDateTime segment_start_time;

    // ---- Explicit transition methods ----
    // Each method logs the old→new state at debug level so that state
    // mutations are always traceable in the log output.

    /// Starts a new segment: assigns a fresh segment_id and records the
    /// wall-clock start time.
    void beginNewSegment(const QDateTime& startTime);

    /// Clears the segment_id and start time (used when the timer stops).
    void clearSegment();

    /// Updates segment_start_time (e.g. after a pause transition).
    void updateSegmentStartTime(const QDateTime& newStart);

    /// Clears durations for a fresh session. Pending-commit bookkeeping is
    /// owned separately by TimeTracker::PendingCommit.
    void resetForNewSession();

    /// Adopts the segment identity from an externally-provided ongoing
    /// duration (used when HistoryDialog replaces durations).
    void adoptOngoingSegment(const TimeDuration& ongoing);
};


class TimeTracker : public QObject
{
    Q_OBJECT
public:
    /// Reason a stop was initiated. Carried by the stopped() signal so
    /// observers (e.g. MainWin) can distinguish user-driven from engine-driven stops.
    enum class StopReason {
        ButtonStop,         ///< User pressed the Stop button.
        MidnightScheduled,  ///< DayBoundaryWatcher scheduled timer fired at 23:59:59.500.
        MidnightWatchdog,   ///< Watchdog detected a cross-midnight ongoing segment.
        Shutdown,           ///< Application shutdown (destructor or ShutdownCoordinator).
        EditApplied,        ///< History edit committed with no ongoing segment: tracking stopped.
    };

    /// Cause of a pause/resume transition. Carried by modeChanged() so MainWin
    /// can distinguish lock-driven autopause from user-driven pause.
    enum class PauseCause {
        UserAction,    ///< User pressed Pause or Start.
        LockAutopause, ///< Desktop locked and backpause triggered.
        LockResume,    ///< Desktop unlocked and timer was auto-resumed.
    };

public:
    enum class Mode {Activity, Pause, None};

private:

    /**
     * DayBoundaryWatcher — owns the 23:59:59.500 single-shot QTimer and the
     * cross-midnight watchdog logic.
     *
     * tick() arms/cancels the internal timer, calls the engine's stop path
     * when it fires, and contains the watchdog poll (moved from
     * MainWin::update()).
     */
    class DayBoundaryWatcher {
    public:
        explicit DayBoundaryWatcher(TimeTracker& owner);

        /// Called on every 100 ms heartbeat. Runs the cross-midnight watchdog.
        void tick(const QDateTime& now);

        /// Arms the single-shot timer to fire at 23:59:59.500 today.
        void armScheduledStop(const QDateTime& now);

        /// Cancels the single-shot timer (call on any stop).
        void cancel();

#ifndef QT_NO_DEBUG
        /// Remaining ms until the scheduled stop fires (-1 if not armed). Test probe.
        qint64 remainingMs_dbg() const { return midnight_timer_.remainingTime(); }
#endif // QT_NO_DEBUG

    private:
        TimeTracker& owner_;
        QTimer midnight_timer_;

        void onMidnightTimerFired();
    };

    /**
     * PendingCommit — the one persistence payload TimeTracker may still owe
     * the store.
     *
     * Invariant: there is either no pending commit, or exactly one immutable
     * payload awaiting a durable write. Every complete successful
     * persistence (an ordinary commit, a Start-time retry, a successful
     * replaceAll, or a contractual Disabled no-op) clears it. A pending
     * payload blocks a new session from starting (see retryUnsavedDurations()).
     *
     * submit() is used by ordinary in-session persistence (Pause/Backpause/
     * Stop): it may overwrite an already-pending payload with the current
     * (larger) snapshot, because within one still-running session that
     * snapshot is always a superset of anything already pending — segment
     * ids are stable and commitSession() is an idempotent upsert. retry() is
     * used only when mode_ == None; it takes no new content, so a failed
     * retry can never replace the payload with newer mutable session
     * contents — it is left byte-for-byte as submitted.
     */
    class PendingCommit {
    public:
        bool hasPending() const { return payload_.has_value(); }
        const std::deque<TimeDuration>& payload() const { return *payload_; }

        /// Records an ordinary persist attempt's outcome.
        void submit(bool success, const std::deque<TimeDuration>& durations) {
            if (success) { clear(); return; }
            payload_ = durations;
        }

        /// Records a Start-time retry outcome against the payload already held.
        void retry(bool success) {
            if (success) clear();
        }

        /// DB is now authoritative after a successful replaceAll.
        void replaceSucceeded() { clear(); }

        void clear() { payload_.reset(); first_drain_failure_.reset(); }

        /// Remembers why this payload's FIRST shutdown drain failed. Later
        /// idempotent repeats of drainForShutdown() re-attempt the store but
        /// keep reporting this original diagnosis, so a repeated call cannot
        /// lose the first failure message. Lives here (rather than on
        /// TimeTracker) because its validity is exactly the payload's
        /// lifetime: every path that clears the payload clears it too.
        void recordDrainFailure(SessionStoreResult::Category category, const QString& message) {
            if (!first_drain_failure_.has_value())
                first_drain_failure_ = DrainResult::incomplete(category, message);
        }

        /// Discards the remembered first failure so the next drain reports its
        /// own outcome. Used for an explicit user-driven Retry, which is a
        /// genuinely new attempt rather than an idempotent repeat.
        void forgetDrainFailure() { first_drain_failure_.reset(); }

        const std::optional<DrainResult>& firstDrainFailure() const { return first_drain_failure_; }

    private:
        std::optional<std::deque<TimeDuration>> payload_;
        std::optional<DrainResult> first_drain_failure_;
    };

    const Settings & settings_;
    /// Wall-clock source for the engine. Defaults to QDateTime::currentDateTime;
    /// tests inject a controllable clock via setNowProvider_dbg(). Every internal
    /// "now" read in TimeTracker routes through this so the clock is fully drivable.
    std::function<QDateTime()> now_provider_;
    QElapsedTimer timer_;
    SessionState session_;
    PendingCommit pending_commit_;
    Mode mode_;
    bool autopause_pending_resume_;
    bool is_locked_;     // Track if desktop is currently locked (to prevent checkpoints while locked)
    bool dialog_open_;   // True while HistoryDialog is open (suspends mutation)
    bool shutdown_called_ = false;  // Set once shutdown() has torn down the engine.
    /// A midnight boundary event captured while dialog_open_, to be replayed
    /// exactly once by endExclusiveEdit(). Distinguishes the two deferral
    /// policies: a scheduled cutoff credits the segment through the captured
    /// instant (nominally 23:59:59.500); a watchdog detection conservatively
    /// discards the whole ongoing segment, matching the non-deferred watchdog
    /// path. `timestamp` also orders replay against a simultaneously deferred
    /// PendingLockCorrection (chronological, earliest captured event first).
    struct PendingMidnightEvent {
        StopReason reason;    // MidnightScheduled or MidnightWatchdog
        QDateTime timestamp;  // Scheduled: the captured cutoff. Watchdog: the
                               // instant the crossing was detected.
    };
    std::optional<PendingMidnightEvent> pending_midnight_event_;
    /// A LongOngoingLock that arrived while HistoryDialog was open, to be applied
    /// when the dialog closes. unlock_time stays invalid while the desktop is
    /// still locked; an Unlock during the dialog stamps it instead of cancelling
    /// the correction (the locked interval still happened and must become Pause).
    struct PendingLockCorrection {
        QDateTime lock_time;
        QDateTime unlock_time;   // invalid ⇒ still locked at dialog close
    };
    std::optional<PendingLockCorrection> pending_lock_correction_;
    SessionStore& db_;
    mutable QRecursiveMutex mutex_;  // Protects state transitions from concurrent access
    QTimer checkpointTimer_;  // TimeTracker for periodic checkpoint saving
    qint64 checkpoint_interval_msec_; // Checkpoint interval (0 = disabled)
    qint64 startup_recovered_seconds_;
    bool startup_recovery_notification_needed_;
    DayBoundaryWatcher day_boundary_watcher_; // Must be last: constructed after all other members

    void startTimer(const QDateTime& now);
    void stopTimer(const QDateTime& now, StopReason reason);
    void pauseTimer(const QDateTime& now);
    void backpauseTimer(const QDateTime& now, const QDateTime& lockTime = QDateTime());
    void finalizePauseTransition(const QDateTime& now);
    // Appends a single same-day segment to session_.durations.
    // Silently discards segments where startTime.date() != endTime.date()
    // (last-line defence for cross-midnight). DayBoundaryWatcher's scheduled
    // stop and watchdog are supposed to make cross-midnight inputs unreachable.
    void addDuration(DurationType type,
                     const QDateTime& startTime,
                     const QDateTime& endTime,
                     SegmentId segmentId = SegmentId{});

    // If the ongoing segment is cross-midnight, forces the engine into None,
    // discarding the in-flight segment. Returns true when it fired.
    // Safe to call multiple times; only the first call does real work.
    bool discardCrossMidnightOngoingAndStop(const QDateTime& now);
    EntriesForDateResult hasEntriesForDate(const QDate& date);
    void saveCheckpoint_locked(const QDateTime& now);  // Internal checkpoint save (called when mutex already held)
    SessionStoreResult appendDurationsChunkToDB(const std::deque<TimeDuration>& durations);
    /// Applies the standard log/warn policy for a SessionStoreResult in the
    /// named context.  Does NOT affect control flow — callers decide what to do
    /// after calling this.
    void logDbResultOrWarn(const SessionStoreResult& result, const QString& context);
    // Retries the pending commit payload at the start of a new session
    // (None -> Activity only). Performs exactly one retry attempt.
    // Returns true if startTimer should continue, false if it should abort.
    bool retryUnsavedDurations();
    bool maybeReanchorCheckpoint(const TimeDuration& seg);
    /// Locked variant of getOngoingDuration() for callers that already hold mutex_.
    std::optional<TimeDuration> getOngoingDuration_locked() const;
    /// Appends session_.durations to the DB. Returns true on success.
    bool appendDurationsToDB();
    /// Rewrites all completed segments to the DB. Returns true on success.
    bool updateDurationsInDB();
    /**
     * Persists session_.durations and keeps the pending-commit lifecycle
     * consistent: on success any stale pending payload is cleared; on
     * failure the durations become the pending payload so the next session
     * start retries them exactly once.
     * Returns updateDurationsInDB()'s result.
     */
    bool persistDurationsOrRetainForRetry();
    void replaceCurrentDurations(const std::deque<TimeDuration>& newDurations,
                                 const std::optional<TimeDuration>& ongoing = std::nullopt);
    // Atomically replaces the live session with `edited`. Delegates to
    // commitEditedTimeline; use commitEditedTimeline directly from outside TimeTracker.
    void commit(const Timeline& edited);
    // Applies a deferred midnight event captured while dialog_open_. No-op if
    // an earlier-replayed event already stopped the engine (mode_ == None).
    void replayDeferredMidnightEvent(const PendingMidnightEvent& event);
    // Applies a deferred LongOngoingLock/Unlock correction captured while
    // dialog_open_. Called by endExclusiveEdit() in chronological order
    // relative to a co-pending PendingMidnightEvent, if any.
    void replayDeferredLockCorrection(const PendingLockCorrection& corr);

#ifndef QT_NO_DEBUG
    /// Debug-build state snapshot used by StateGuard to detect unlogged mutations.
    struct StateSnapshot {
        size_t durations_size;
        SegmentId segment_id;
        QDateTime segment_start_time;
        bool has_pending_commit;
        Mode mode;
    };
    StateSnapshot takeStateSnapshot() const;

    /// RAII guard that captures state on entry to a public method and checks
    /// for unexpected mutations on exit. Only active in debug builds.
    class StateGuard {
    public:
        StateGuard(const TimeTracker& tracker, const char* methodName);
        ~StateGuard();
        /// Call this to acknowledge that the method intentionally modified state.
        void markTransitioned();
    private:
        const TimeTracker& tracker_;
        const char* method_;
        StateSnapshot entry_;
        bool transitioned_ = false;
    };

    /// Checks structural invariants on session_.durations:
    ///   - Segment ordering: startTimes are non-decreasing
    ///   - No overlapping segments of the same type on the same day
    ///   - All segment_ids are non-empty
    /// Violations are logged via qWarning (not fatal) so they are visible
    /// in test output and debug runs without crashing the application.
    void checkDurationInvariants() const;

    /// Cross-layer invariant check: for each segment in session_.durations,
    /// queries the DB and asserts either no row exists for that segment_id, or
    /// exactly one finalized row with matching fields. Called after successful
    /// db_.commitSession() calls in updateDurationsInDB().
    void checkCrossLayerInvariants() const;
#endif // QT_NO_DEBUG

public:
    bool isActive() const  { QMutexLocker lk(&mutex_); return mode_ == Mode::Activity; }
    bool isPaused() const  { QMutexLocker lk(&mutex_); return mode_ == Mode::Pause; }
    bool isStopped() const { QMutexLocker lk(&mutex_); return mode_ == Mode::None; }
    /// True once shutdown() has run. Distinct from isStopped(): a normally
    /// stopped engine is still editable, but a shut-down engine must not accept
    /// a HistoryDialog save (it would re-write finalized rows as unfinalized).
    bool wasShutDown() const { QMutexLocker lk(&mutex_); return shutdown_called_; }

    /**
     * Result returned by commitEditedTimeline().
     *
     *   ok == true  — all operations succeeded (durations replaced, mode aligned,
     *                 checkpoint re-anchored if applicable).
     *   ok == false — the checkpoint write after re-anchoring failed; durations
     *                 are already replaced in memory but the checkpoint row may
     *                 be stale.  The error is also logged and emitted as
     *                 userWarning by the internal handler.
     */
    struct EditCommitResult {
        bool ok;
        QString message;
        static EditCommitResult success() { return {true, {}}; }
        static EditCommitResult failure(QString msg) { return {false, std::move(msg)}; }
    };

    explicit TimeTracker(const Settings & settings, SessionStore& db, QObject *parent = nullptr);
    ~TimeTracker();

    /// Consumes the clean-shutdown marker and reconciles orphan checkpoints.
    /// Must be called once after checkSchemaOnStartup() returns Ready or Created,
    /// and before the event loop starts.  The TimeTracker constructor performs no
    /// persistence side effects; this method is where recovery happens.
    void initializeFromStore();

    qint64 getActiveTime() const;
    qint64 getPauseTime() const;
    /// Returns completed + ongoing segments. Use for history display.
    Timeline snapshot() const;
    /// Returns completed segments only (excludes ongoing). Use for checkpoint saves.
    Timeline completedTimeline() const;
    /**
     * Atomically replaces the live session with `edited`, aligning mode_,
     * re-anchoring checkpoint state, and deciding whether checkpoint writes
     * are allowed.  Thread-safe; caller must hold no TimeTracker locks.
     *
     * Owns internally:
     *   - replacing completed segments and clearing the retry cache,
     *   - adopting or clearing the ongoing segment,
     *   - aligning mode_ with the edited ongoing segment type,
     *   - re-anchoring checkpoint tracking (segment id, start time, DB row),
     *   - deciding whether checkpoint writes are allowed (suspended while
     *     dialog_open_ or is_locked_).
     *
     * Observer behavior:
     *   - ongoing present, type differs from current mode_: emits started(true)
     *     or paused() so the UI reflects the new mode.
     *   - ongoing absent, mode_ was not None: emits stopped(EditApplied) so
     *     the tray/UI learns that tracking ended due to the edit.
     *   - ongoing present but mode_ is already None: refused. Adopting the
     *     segment identity would leave segment_id set while the engine stays
     *     stopped — a state SessionState documents as impossible. The
     *     ongoing segment is ignored; no signal emitted.
     *   - no mode change: no signal emitted.
     *
     * Returns EditCommitResult::failure if the checkpoint write fails;
     * success otherwise.
     */
    EditCommitResult commitEditedTimeline(const Timeline& edited);
    LoadResult loadHistoryFromStore();
    std::optional<TimeDuration> getOngoingDuration() const;
    // Cheap thread-safe predicate used by the MainWin watchdog. Returns true
    // iff there is an ongoing segment whose start date is earlier than today.
    bool isOngoingSegmentCrossMidnight() const;
    qint64 getStartupRecoveredSeconds() const;
    bool shouldShowStartupRecoveryNotification() const;
    void setDurationType(size_t idx, DurationType type);
    SessionStoreResult replaceAll(const Timeline& history, const Timeline& session);
    /// Freezes checkpoints and defers midnight/lock events. Call before opening HistoryDialog.
    void beginExclusiveEdit();
    /// Resumes checkpoints and replays deferred events. Call after closing HistoryDialog.
    void endExclusiveEdit();
    bool canMarkCleanShutdown() const;

    struct ShutdownResult {
        bool stopped;     ///< true iff timer is now in Mode::None
        bool canCleanMark; ///< true iff state is clean enough for a shutdown marker
        bool stopCalled;   ///< true iff shutdown called stopTimer (which writes the marker)
    };
    /// Stops the timer if running and returns the resulting state.
    /// Thread-safe; may be called multiple times (idempotent on an already-stopped timer).
    ShutdownResult shutdown();

    /// Why drainForShutdown() is being called.
    ///   Idempotent — an automatic repeat (coordinator re-entry, destructor
    ///                safety net). Re-attempts the store, but keeps reporting
    ///                the FIRST failure message for this payload.
    ///   UserRetry   — an explicit user-driven Retry, i.e. a genuinely new
    ///                attempt, so it reports its own fresh outcome.
    enum class DrainAttempt { Idempotent, UserRetry };

    /**
     * Terminal-shutdown drain: closes any live segment using one captured
     * `now` (via shutdown()), then retries any pending commit payload even
     * if mode_ is already None. Independent of tracker mode -- a pre-existing
     * pending payload from an earlier failed Stop is retried here too.
     *
     * Idempotent: a second call neither duplicates a committed segment nor
     * retries after a prior success, and never loses the first failure
     * message (see DrainAttempt). A repeated failure re-attempts the same
     * payload without corrupting its identity.
     *
     * Called by ShutdownCoordinator (normal exit, OS-forced shutdown, and
     * user Retry) and by the destructor as a last-resort safety net.
     *
     * Ownership/ordering requirement: TimeTracker must be destroyed BEFORE
     * the SessionStore it was constructed with. The destructor runs this
     * drain as a final safety net, so the store must still be alive at that
     * point; main.cpp satisfies this by declaring the store first (later
     * declarations are destroyed first).
     */
    DrainResult drainForShutdown(DrainAttempt attempt = DrainAttempt::Idempotent);

public slots:
    void useTimerViaButton(Button button);
    void useTimerViaLockEvent(LockEvent event, const QDateTime& lockTime = QDateTime());
    void onTick(const QDateTime& now);  // 100 ms heartbeat for the watchdog
    /// Start if stopped/paused, pause if active. Called by the start/pause button.
    void onStartPausePressed();
    /// Stop the timer. Called by the stop button.
    void onStopPressed();

signals:
    void userWarning(const QString& text);
    void stopped(TimeTracker::StopReason reason);
    /// Emitted after a successful None→Activity or Pause→Activity transition,
    /// including lock-driven resume. fromPause is true when the previous mode
    /// was Pause, false for a fresh start from None.
    void started(bool fromPause);
    /// Emitted after a successful Activity→Pause transition (user-driven only;
    /// lock-driven autopause is signalled by modeChanged(LockAutopause)).
    void paused();
    /// Emitted for lock-driven autopause (LockAutopause) and autoresume
    /// (LockResume). On LockResume, started(true) also fires — GUI must not
    /// double-act on both.
    void modeChanged(TimeTracker::PauseCause cause);

private slots:
    void saveCheckpoint();  // Periodic checkpoint saving every 5 minutes

#ifndef QT_NO_DEBUG
public:
    // ---- Debug-build test probes ----
    // Narrow accessors for white-box tests. NOT compiled in release builds.

    /// Injects a controllable wall-clock source so tests can drive the engine
    /// without sleeping. Pass a lambda over a FakeClock; every internal "now"
    /// read (checkpoints, ongoing duration, stop/backpause, watchdog) uses it.
    void setNowProvider_dbg(std::function<QDateTime()> provider) { now_provider_ = std::move(provider); }

    /// Direct access to the SessionState for test setup and assertions.
    SessionState& sessionState_dbg() { return session_; }
    const SessionState& sessionState_dbg() const { return session_; }

    /// True iff a pending commit payload is currently retained (blocks a new
    /// session from starting until retried successfully). Test probe exposing
    /// PendingCommit behavior rather than a mutable field.
    bool hasPendingCommit_dbg() const { return pending_commit_.hasPending(); }

    /// Copy of the currently pending commit payload, or empty if none.
    std::deque<TimeDuration> pendingCommitPayload_dbg() const {
        return pending_commit_.hasPending() ? pending_commit_.payload() : std::deque<TimeDuration>{};
    }

    /// Expose frequently-checked flags for test assertions.
    bool isDialogOpen_dbg() const { return dialog_open_; }
    bool isLocked_dbg() const { return is_locked_; }
    bool isAutopausePendingResume_dbg() const { return autopause_pending_resume_; }
    void setAutopausePendingResume_dbg(bool v) { autopause_pending_resume_ = v; }

    /// Forces the engine into a specific mode without transition logic.
    /// Use only in tests that need to arrange a pre-condition state.
    void forceMode_dbg(Mode m) { mode_ = m; }

    /// Expose the QElapsedTimer so tests can reset it to control elapsed time.
    QElapsedTimer& elapsedTimer_dbg() { return timer_; }

    /// Exposes the internal checkpoint-save path for direct testing.
    void saveCheckpoint_locked_dbg(const QDateTime& now) { saveCheckpoint_locked(now); }

    /// Exposes the backpause path for direct testing.
    void backpauseTimer_dbg(const QDateTime& now, const QDateTime& lockTime = QDateTime()) { backpauseTimer(now, lockTime); }

    /// Exposes the addDuration path for direct testing.
    void addDuration_dbg(DurationType type, const QDateTime& start, const QDateTime& end) {
        addDuration(type, start, end);
    }

    /// Returns whether the periodic checkpoint QTimer is currently running.
    bool isCheckpointTimerActive_dbg() const { return checkpointTimer_.isActive(); }

    /// Exposes the pending deferred lock correction (set while dialog_open_).
    auto pendingLockCorrection_dbg() const { return pending_lock_correction_; }
    /// Exposes/injects the pending deferred midnight event (set while dialog_open_).
    void setPendingMidnightEvent_dbg(StopReason reason, const QDateTime& timestamp) {
        pending_midnight_event_ = PendingMidnightEvent{reason, timestamp};
    }
    auto pendingMidnightEvent_dbg() const { return pending_midnight_event_; }

    /// Triggers the periodic checkpoint slot directly (bypasses timer interval).
    void triggerCheckpoint_dbg() { saveCheckpoint(); }

    /// Calls the internal cross-midnight discard helper directly.
    bool discardCrossMidnightOngoingAndStop_dbg(const QDateTime& now) {
        return discardCrossMidnightOngoingAndStop(now);
    }

    /// Exposes the DayBoundaryWatcher for test control of the scheduled-stop timer.
    DayBoundaryWatcher& dayBoundaryWatcher_dbg() { return day_boundary_watcher_; }

    /// Exposes the internal DB-write helpers for the regression surface test.
    bool appendDurationsToDB_dbg() { return appendDurationsToDB(); }
    bool updateDurationsInDB_dbg() { return updateDurationsInDB(); }
    void replaceCurrentDurations_dbg(const std::deque<TimeDuration>& d,
                                     const std::optional<TimeDuration>& o = std::nullopt) {
        replaceCurrentDurations(d, o);
    }
#endif // QT_NO_DEBUG
};

Q_DECLARE_METATYPE(TimeTracker::StopReason)
Q_DECLARE_METATYPE(TimeTracker::PauseCause)

#endif // TIMER_H
