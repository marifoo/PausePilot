#ifndef TIMER_H
#define TIMER_H

#include <QObject>
#include <QtGlobal>
#include <QDateTime>
#include <QMutex>
#include <QTimer>
#include <optional>
#include <deque>
#include <vector>
#include <algorithm>
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
        FatalStoreError,    ///< C4: a store operation returned FatalError; tracking latched unavailable.
    };

public:
    enum class Mode {Activity, Pause, None};

    /// D10 (R10-001/R10-002): the one cause vocabulary for every real mode
    /// transition, button/lock/midnight/edit/shutdown-driven alike. Carried
    /// by transitioned() together with the mode change itself, so a single
    /// event is sufficient for a UI reducer -- no pairing/exclusion rules to
    /// reconstruct from separate started()/paused()/stopped()/modeChanged()
    /// notifications.
    enum class TransitionCause {
        ButtonStart,        ///< None/Pause -> Activity via button or toggle.
        ButtonPause,        ///< Activity -> Pause via button or toggle.
        ButtonStop,         ///< -> None via the Stop button.
        LockAutopause,      ///< Activity -> Pause via a long desktop lock.
        LockResume,         ///< Pause -> Activity via desktop unlock.
        MidnightScheduled,  ///< -> None: scheduled 23:59:59.500 cutoff.
        MidnightWatchdog,   ///< -> None: watchdog cross-midnight discard.
        Shutdown,           ///< -> None: application shutdown.
        EditApplied,        ///< History edit committed a new mode.
        FatalStoreError,    ///< -> None: a store operation returned FatalError.
    };

    /// One transition: the mode it left, the mode it entered, and why.
    struct Transition {
        Mode from;
        Mode to;
        TransitionCause cause;
    };

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

        /// Directly invokes the private timer-fired handler without waiting
        /// for the real QTimer, so tests can drive scheduled-stop boundary
        /// revalidation deterministically via FakeClock (D2/R11-004).
        void fireMidnightTimer_dbg() { onMidnightTimerFired(); }
#endif // QT_NO_DEBUG

    private:
        TimeTracker& owner_;
        QTimer midnight_timer_;
        /// The intended civil-time boundary (23:59:59.500 on the arm day),
        /// captured once by armScheduledStop(). onMidnightTimerFired()
        /// revalidates wall time against this rather than trusting the
        /// QTimer's real-elapsed duration alone (D2/R11-004).
        QDateTime intended_boundary_;

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

    /**
     * PendingDiscards — durable-discard-intent tracking (A4/R05-001).
     *
     * TimeTracker deliberately rejects a live segment (currently: the
     * cross-midnight watchdog/scheduled-stop path) by asking the store to
     * discard its unfinalized checkpoint row. begin() records the segment's
     * stable identity BEFORE the store call is made, so the identity is
     * never lost even if the store call itself fails -- the identity is all
     * TimeTracker needs to keep retrying; the store owns making the discard
     * itself durable (a tombstone written before the physical row delete --
     * see SqliteSessionStore::discardCheckpoint()).
     *
     * Multiple intents can be outstanding at once (e.g. a second
     * cross-midnight discard before an earlier one resolves), so this holds
     * a small set rather than a single payload like PendingCommit. Each
     * entry is independent and safe to retry repeatedly.
     */
    class PendingDiscards {
    public:
        bool hasPending() const { return !ids_.empty(); }
        const std::vector<SegmentId>& pending() const { return ids_; }

        /// Records a new discard intent. No-op if already pending.
        void begin(const SegmentId& id) {
            if (std::find(ids_.begin(), ids_.end(), id) == ids_.end())
                ids_.push_back(id);
        }

        /// Marks one intent resolved (store confirms Success or Disabled).
        void resolve(const SegmentId& id) {
            ids_.erase(std::remove(ids_.begin(), ids_.end(), id), ids_.end());
        }

    private:
        std::vector<SegmentId> ids_;
    };

    const Settings & settings_;
    /// Wall-clock source for the engine. Defaults to QDateTime::currentDateTime;
    /// tests inject a controllable clock via setNowProvider_dbg(). Every internal
    /// "now" read in TimeTracker routes through this so the clock is fully drivable.
    std::function<QDateTime()> now_provider_;
    SessionState session_;
    PendingCommit pending_commit_;
    PendingDiscards pending_discards_;
    Mode mode_;
    bool autopause_pending_resume_;
    bool is_locked_;     // Track if desktop is currently locked (to prevent checkpoints while locked)
    bool dialog_open_;   // True while HistoryDialog is open. Midnight triggers now
                         // revoke it (WP2); other triggers still suspend mutation
                         // until later packages.
    QDate exclusive_edit_date_;  // WP4: the date when beginExclusiveEdit() was called.
                                 // Used to detect when the date moves forward (>) and revoke the edit.
    bool shutdown_called_ = false;  // Set once shutdown() has torn down the engine.
    /// D5 review fix (finding 1, R15-002): monotonic counter incremented on
    /// every shutdown() call (never by cancelShutdown()). HistoryDialog
    /// snapshots this at construction and refuses to save if it changed --
    /// a per-dialog guard against a shutdown that closed/committed the live
    /// segment while the dialog stayed open across it, which a cancellation
    /// afterward must not unprotect (unlike the global shutdown_called_ flag).
    qint64 shutdown_epoch_ = 0;
    /// C4 (R05-006): latched once ANY store operation returns FatalError.
    /// Stays true for the lifetime of this TimeTracker (cleared only by
    /// restart, i.e. constructing a new instance) -- there is no in-memory-
    /// only tracking mode and no unlatch path. See enterFatalState().
    ///
    /// C4 fix round 3 (F4): post-C4, SqliteSessionStore::replaceAll()'s
    /// post-backup reopen failure (sqlitesessionstore.cpp, ensureOpen()
    /// after the backup) is the ONLY production path that can produce a
    /// FatalError today -- commitSession, saveCheckpoint, discardCheckpoint,
    /// loadDurations, flushToDisc, setLastCleanShutdownMarker, and
    /// recoverStartupCheckpoints have all been consolidated onto
    /// TransientError/CallerBug (see sessionstore.h). The other
    /// stopIfLatched()/latch-check call sites in this class (saveCheckpoint_
    /// locked, maybeReanchorCheckpoint, startTimer's Pause and None
    /// branches, finalizePauseTransition) are therefore defence-in-depth
    /// against a future SessionStore implementation, not currently
    /// reachable from SqliteSessionStore, and are exercised in tests only
    /// via FakeSessionStore injecting FatalError from those operations.
    bool fatal_state_latched_ = false;
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
    /// A LongOngoingLock that arrived while HistoryDialog was open, to be applied
    /// when the dialog closes. unlock_time stays invalid while the desktop is
    /// still locked; an Unlock during the dialog stamps it instead of cancelling
    /// the correction (the locked interval still happened and must become Pause).
    struct PendingLockCorrection {
        QDateTime lock_time;
        QDateTime unlock_time;   // invalid ⇒ still locked at dialog close
    };

    /**
     * DeferredEditEvents — the one typed owner of exclusive-edit deferral
     * and its replay priority (D10/plan4 B4).
     *
     * A midnight boundary event and a LongOngoingLock correction can each be
     * captured at most once while dialog_open_, and both may be pending at
     * the same time. Previously this was two independent std::optional
     * members with the chronological-replay rule (A1/R11-002) written out
     * inline at every call site that needed it. Collapsing the bookkeeping
     * AND the ordering rule into one class keeps "which event replays first"
     * in a single place, while still allowing two concurrently pending
     * events of different types -- a literal single-slot design would
     * regress A1's "lock and midnight pending together replay
     * chronologically" invariant.
     */
    class DeferredEditEvents {
    public:
        /// Records a midnight deferral iff none is already pending
        /// (first-wins: the first-detected boundary is the true one).
        void deferMidnightIfAbsent(StopReason reason, const QDateTime& timestamp) {
            if (!midnight_.has_value())
                midnight_ = PendingMidnightEvent{reason, timestamp};
        }
        bool hasMidnight() const { return midnight_.has_value(); }
        std::optional<PendingMidnightEvent> midnight() const { return midnight_; }
#ifndef QT_NO_DEBUG
        /// Test-only direct injection (mirrors the old setPendingMidnightEvent_dbg()).
        void setMidnight_dbg(StopReason reason, const QDateTime& timestamp) {
            midnight_ = PendingMidnightEvent{reason, timestamp};
        }
#endif // QT_NO_DEBUG

        /// Begins (or supersedes) a pending LongOngoingLock correction.
        void beginLock(const QDateTime& lockTime) {
            lock_ = PendingLockCorrection{lockTime, QDateTime()};
        }
        bool hasLock() const { return lock_.has_value(); }
        std::optional<PendingLockCorrection> lock() const { return lock_; }
        /// Stamps the unlock time on an already-pending lock correction; a
        /// no-op if none is pending.
        void stampLockUnlock(const QDateTime& unlockTime) {
            if (lock_.has_value()) lock_->unlock_time = unlockTime;
        }

        /// Snapshots and clears both events (so a re-entrant/duplicate call
        /// cannot replay either twice), then invokes applyMidnight/applyLock
        /// in the A1 chronological order: earliest captured instant first. A
        /// lock correction's instant is when the LongOngoingLock fired.
        template <class ApplyMidnight, class ApplyLock>
        void replay(ApplyMidnight&& applyMidnight, ApplyLock&& applyLock) {
            const std::optional<PendingMidnightEvent> midnight = midnight_;
            midnight_.reset();
            const std::optional<PendingLockCorrection> lock = lock_;
            lock_.reset();

            if (midnight.has_value() && lock.has_value() && lock->lock_time < midnight->timestamp) {
                applyLock(*lock);
                applyMidnight(*midnight);
            } else if (midnight.has_value()) {
                applyMidnight(*midnight);
                if (lock.has_value()) applyLock(*lock);
            } else if (lock.has_value()) {
                applyLock(*lock);
            }
        }

    private:
        std::optional<PendingMidnightEvent> midnight_;
        std::optional<PendingLockCorrection> lock_;
    };
    DeferredEditEvents deferred_edit_events_;
    SessionStore& db_;
    mutable QRecursiveMutex mutex_;  // Protects state transitions from concurrent access
    QTimer checkpointTimer_;  // TimeTracker for periodic checkpoint saving
    qint64 checkpoint_interval_msec_; // Checkpoint interval (0 = disabled)
    qint64 startup_recovered_msec_;
    bool startup_recovery_notification_needed_;
    int startup_policy_dropped_count_;
    DayBoundaryWatcher day_boundary_watcher_; // Must be last: constructed after all other members

    // cause defaults to the button/toggle path; lock-resume callers (Unlock
    // handling, deferred-lock replay) pass TransitionCause::LockResume so the
    // one transitioned() emission this method performs carries the right cause.
    void startTimer(const QDateTime& now, TransitionCause cause = TransitionCause::ButtonStart);
    // observedFromOverride: the mode observers last saw via transitioned()/
    // stopped(), when it differs from the live mode_ at the moment this is
    // called. Needed only for the nested fatal-latch stop (via
    // stopIfLatched()) fired from inside startTimer()'s Pause branch and
    // finalizePauseTransition(): both mutate mode_ to the NEW mode before
    // persisting and checking the latch, so by the time the nested stop
    // runs, mode_ is already the not-yet-announced new mode, not the mode
    // observers actually last saw. Every other caller leaves it unset and
    // gets the (correct, unmutated) live mode_.
    void stopTimer(const QDateTime& now, StopReason reason,
                    std::optional<Mode> observedFromOverride = std::nullopt);
    /// True when `now` precedes `referenceStart` -- a backward wall-clock
    /// step (NTP/manual/DST) after `referenceStart` was captured. The one
    /// owner of this rule, used by stopTimer/pauseTimer/backpauseTimer,
    /// getOngoingDuration_locked(), and saveCheckpoint_locked()
    /// (D2/R11-003/MEDIUM-6/LOW-E); each caller supplies its own reference
    /// point (a live segment's start, or an intermediate split point such as
    /// backpauseTimer's activity_end), decides its own response, and -- where
    /// the caller's own guard is a `<=`/`>=` rather than a strict `</>` --
    /// checks the boundary-equal (zero-elapsed) case alongside it. This
    /// function only detects strict reversal.
    static bool isClockReversed(const QDateTime& now, const QDateTime& referenceStart);
    /// C4 fix round 2 (LOW-8): the ~6-times-repeated post-latch stop,
    /// factored out. enterFatalState() only latches; each transition owner
    /// must check the latch after its own state changes are done and stop
    /// itself here. Returns true iff it stopped (fatal_state_latched_ was
    /// set) so callers that need to short-circuit (e.g. return early) can.
    bool stopIfLatched(const QDateTime& now, std::optional<Mode> observedFromOverride = std::nullopt);
    void pauseTimer(const QDateTime& now);
    void backpauseTimer(const QDateTime& now, const QDateTime& lockTime = QDateTime());
    void finalizePauseTransition(const QDateTime& now);
    // Appends a single same-day segment to session_.durations.
    // Silently discards segments that cross local midnight (D1: half-open
    // [start, end) -- ending exactly at local midnight is same-day and
    // legal) (last-line defence for cross-midnight). DayBoundaryWatcher's
    // scheduled stop and watchdog are supposed to make cross-midnight
    // inputs unreachable.
    void addDuration(DurationType type,
                     const QDateTime& startTime,
                     const QDateTime& endTime,
                     SegmentId segmentId = SegmentId{});

    // If the ongoing segment is cross-midnight, forces the engine into None,
    // discarding the in-flight segment. If a History edit is open, first
    // revokes it (WP2), then re-evaluates, so the discard runs exactly as
    // it would with no dialog. Returns true when it fired. Safe to call
    // multiple times; only the first call does real work.
    bool discardCrossMidnightOngoingAndStop(const QDateTime& now);
    // WP2: force-closes an open History edit (see the definition for the
    // full contract) and runs the caller's own no-dialog code next.
    void revokeExclusiveEdit();
    EntriesForDateResult hasEntriesForDate(const QDate& date);
    void saveCheckpoint_locked(const QDateTime& now);  // Internal checkpoint save (called when mutex already held)
    SessionStoreResult appendDurationsChunkToDB(const std::deque<TimeDuration>& durations);
    /// Applies the standard log/warn policy for a SessionStoreResult in the
    /// named context.  Does NOT affect control flow — callers decide what to do
    /// after calling this.
    void logDbResultOrWarn(const SessionStoreResult& result, const QString& context);
    /// C4 (R05-006): the ONE place FatalError is handled, regardless of which
    /// store operation (checkpoint, commit, replace, discard) returned it.
    /// First call: latches fatal_state_latched_, logs, and emits exactly one
    /// fatalError (a modal dialog, C4 fix round 2 / HIGH-1) -- not
    /// userWarning. Later calls (already latched) only log. Does NOT stop the
    /// tracker itself -- this can run deep inside an in-progress transition
    /// (e.g. startTimer, finalizePauseTransition), and stopping here would
    /// desync mode_/session_ state from whatever the transition does next.
    /// Each transition owner is responsible for checking fatal_state_latched_
    /// after its own persistence call and stopping (via stopTimer()) itself,
    /// at a point where the transition has already reached a consistent
    /// state. mutex_ must already be held by the caller.
    void enterFatalState(const QString& message);
    // Retries the pending commit payload at the start of a new session
    // (None -> Activity only). Performs exactly one retry attempt.
    // Returns true if startTimer should continue, false if it should abort.
    bool retryUnsavedDurations();
    // Retries any durable-discard intents (A4/R05-001) against the store.
    // Safe to call any time, including when nothing is pending (no-op).
    void retryPendingDiscards();
    bool maybeReanchorCheckpoint(const TimeDuration& seg);
    /// Locked variant of getOngoingDuration() for callers that already hold mutex_.
    std::optional<TimeDuration> getOngoingDuration_locked() const;
    /// Appends session_.durations to the DB. Returns true on success.
    bool appendDurationsToDB();
    /// Rewrites all completed segments to the DB. Returns the store's result
    /// (needed by persistDurationsOrRetainForRetry() to apply the standard
    /// okOrDisabled() success/retain policy).
    SessionStoreResult updateDurationsInDB();
    /**
     * Persists session_.durations and keeps the pending-commit lifecycle
     * consistent: on success (or Disabled) any stale pending payload is
     * cleared; on any other failure -- including CallerBug (C4 fix round 2)
     * -- the durations become the pending payload so the next persistence
     * opportunity (Start, stop, drain) retries them. A CallerBug is logged
     * CRITICAL by logDbResultOrWarn() but is NOT dropped: it is retained and
     * surfaced through the normal Retry/Exit-Anyway drain dialog like any
     * other failure category, so an entire session's tracked time is never
     * silently discarded.
     * Returns true unless the payload is retained for retry.
     */
    bool persistDurationsOrRetainForRetry();
    void replaceCurrentDurations(const std::deque<TimeDuration>& newDurations,
                                 const std::optional<TimeDuration>& ongoing = std::nullopt);
    // Atomically replaces the live session with `edited`. Delegates to
    // commitEditedTimeline; use commitEditedTimeline directly from outside TimeTracker.
    void commit(const Timeline& edited);
    // Applies a deferred midnight event captured while dialog_open_. No-op if
    // an earlier-replayed event already stopped the engine (mode_ == None).
    void replayDeferredMidnightEvent(const PendingMidnightEvent& event, const QDateTime& now);
    // Applies a deferred LongOngoingLock/Unlock correction captured while
    // dialog_open_. Called by endExclusiveEdit() in chronological order
    // relative to a co-pending PendingMidnightEvent, if any.
    void replayDeferredLockCorrection(const PendingLockCorrection& corr, const QDateTime& now);

    // D10: the one place that emits a real mode transition. Emits the
    // transitioned(Transition) event, then derives the one remaining legacy
    // stopped() signal from the same cause centrally (R10-002) -- so, for
    // example, lock resume is one emission call producing one coherent
    // notification instead of two callers each emitting a piece of it.
    void emitTransition(Mode from, Mode to, TransitionCause cause);

    // D10/plan4 B16: useTimerViaLockEvent's per-event-kind behavior, each
    // owning its complete logic (including the autopause/dialog gates)
    // rather than a shared fall-through block.
    void onLockDetected(const QDateTime& now);
    void onUnlockDetected(const QDateTime& now);
    void onLongOngoingLockDetected(const QDateTime& now, const QDateTime& lockTime);

#ifndef QT_NO_DEBUG
    /// Checks structural invariants on session_.durations:
    ///   - Segment ordering: startTimes are non-decreasing
    ///   - No overlapping segments of the same type on the same day
    ///   - All segment_ids are non-empty
    /// Violations are logged via qWarning (not fatal) so they are visible
    /// in test output and debug runs without crashing the application.
    void checkDurationInvariants() const;

    /// Cross-layer invariant check: for each segment in session_.durations,
    /// queries the DB and asserts exactly one finalized row with matching
    /// fields after a Success commitSession(); no requirement is imposed
    /// after a Disabled one. Called after every okOrDisabled()
    /// db_.commitSession() in updateDurationsInDB(), passing that call's own
    /// result.
    void checkCrossLayerInvariants(const SessionStoreResult& commitResult) const;
#endif // QT_NO_DEBUG

public:
    bool isActive() const  { QMutexLocker lk(&mutex_); return mode_ == Mode::Activity; }
    bool isPaused() const  { QMutexLocker lk(&mutex_); return mode_ == Mode::Pause; }
    bool isStopped() const { QMutexLocker lk(&mutex_); return mode_ == Mode::None; }
    /// True once shutdown() has run and not yet cancelled (cleared by
    /// cancelShutdown()). Distinct from isStopped(): a normally stopped
    /// engine is still editable regardless of this flag. Test-only accessor
    /// today -- History's own per-dialog save gate is shutdownEpoch(), not
    /// this flag (D5 review fix, R15-002).
    bool wasShutDown() const { QMutexLocker lk(&mutex_); return shutdown_called_; }
    /// D5 (R15-002): reverses shutdown()'s terminal flag after a cancelled OS
    /// shutdown. Clears shutdown_called_ only -- mode_/session_/pending
    /// commit-or-discard state are untouched, so the ended session is neither
    /// reconstructed nor resumed and pending persistence stays governed by
    /// A2/A3 (retried at the next Start, not here).
    void cancelShutdown();
    /// D5 review fix (finding 1, R15-002): monotonic, never reset by
    /// cancelShutdown() -- lets a caller (HistoryDialog) detect "a shutdown
    /// happened since I opened" independent of whether it was later
    /// cancelled, so a dialog that spanned the shutdown stays refused even
    /// after cancelShutdown() clears wasShutDown() for everyone else.
    qint64 shutdownEpoch() const { QMutexLocker lk(&mutex_); return shutdown_epoch_; }

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
    /// Returns the store's typed recovery result so startup composition (see
    /// runStartupRecovery()) can decide whether to proceed, retry, or quit
    /// before the main UI is created. On Success/Disabled, the recovered-time
    /// and notification facts below reflect the result; on TransientError/
    /// FatalError they are left at their prior (default zero/false) value.
    StartupRecoveryResult initializeFromStore();

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
    // Cheap thread-safe predicate: true iff there is an ongoing segment that
    // has crossed local midnight as of now (isCrossMidnight(start, now)).
    // No production caller currently; exercised by tests only.
    bool isOngoingSegmentCrossMidnight() const;
    qint64 getStartupRecoveredMsec() const;
    bool shouldShowStartupRecoveryNotification() const;
    int getStartupPolicyDroppedCount() const;
    void setDurationType(size_t idx, DurationType type);
    SessionStoreResult replaceAll(const Timeline& history, const Timeline& session);
    /// Freezes checkpoints and defers midnight/lock events. Call before opening HistoryDialog.
    void beginExclusiveEdit();
    /// Resumes checkpoints and replays deferred events. Call after closing HistoryDialog.
    void endExclusiveEdit();
    /// True once a FatalError result has latched TimeTracker into the
    /// unavailable state (C4/R05-006). Start refuses to run while this is
    /// true; nothing but a fresh TimeTracker instance clears it. Public (C4
    /// fix round 2 / MEDIUM-5) so a caller like HistoryDialog can capture
    /// the latch state before its own store call, to tell "this call raised
    /// the modal" apart from "it was already latched".
    bool isFatalLatched() const;

    struct ShutdownResult {
        bool stopped;     ///< true iff timer is now in Mode::None
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
    /// WP1: the edit has been revoked (at lock, sleep, midnight, shutdown or
    /// fatal DB error). HistoryDialog's onExclusiveEditRevoked() slot receives
    /// this synchronously (direct connection) to set editRevoked_, then queues
    /// closeRevoked() to run on the next event-loop turn so nested modals
    /// (QMessageBox::question, QMenu::exec) can unwind first. If the dialog
    /// is destroyed before that turn, the queued call is dropped with it.
    void exclusiveEditRevoked();
    /// C4 fix round 2 (HIGH-1): the ONE modal error C4/R05-006 requires.
    /// Emitted by enterFatalState() on the first latch only, in place of
    /// (not in addition to) userWarning -- a tray balloon/status-bar message
    /// is trivially missed and is not a modal dialog. Connected in MainWin
    /// to QMessageBox::critical().
    void fatalError(const QString& text);
    /// D10 (R10-001/R10-002): the one cause-carrying transition event, emitted
    /// once per real mode change regardless of input source (button, lock,
    /// midnight, History edit, shutdown). Sufficient on its own for a UI
    /// reducer -- the previous started()/paused()/modeChanged() protocol is
    /// gone; observers that need per-cause behavior read t.cause.
    void transitioned(TimeTracker::Transition t);
    /// Kept as the one explicitly separate terminal event (R10-002's other
    /// permitted shape, alongside a fully unified event): every observer
    /// that only cares "did tracking stop, and why" -- today just
    /// HealthMonitor::reset -- can use this instead of filtering
    /// transitioned() on to == None. Derived centrally from the same one
    /// emission point as transitioned(); never emitted independently.
    void stopped(TimeTracker::StopReason reason);

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

    /// True iff a pending commit payload is currently retained (blocks a new
    /// session from starting until retried successfully). Test probe exposing
    /// PendingCommit behavior rather than a mutable field.
    bool hasPendingCommit_dbg() const { return pending_commit_.hasPending(); }

    /// Copy of the currently pending commit payload, or empty if none.
    std::deque<TimeDuration> pendingCommitPayload_dbg() const {
        return pending_commit_.hasPending() ? pending_commit_.payload() : std::deque<TimeDuration>{};
    }

    /// True iff one or more durable-discard intents (A4/R05-001) are still
    /// unresolved (blocks clean-shutdown marking until resolved).
    bool hasPendingDiscards_dbg() const { return pending_discards_.hasPending(); }

    /// Copy of the currently pending discard segment ids, or empty if none.
    std::vector<SegmentId> pendingDiscardIds_dbg() const { return pending_discards_.pending(); }

    /// Directly injects a discard intent, bypassing the normal discard path,
    /// for tests that need to arrange a pre-existing unresolved intent.
    void injectPendingDiscard_dbg(const SegmentId& id) { pending_discards_.begin(id); }

    /// Expose frequently-checked flags for test assertions.
    bool isDialogOpen_dbg() const { return dialog_open_; }
    bool isLocked_dbg() const { return is_locked_; }
    bool isAutopausePendingResume_dbg() const { return autopause_pending_resume_; }
    void setAutopausePendingResume_dbg(bool v) { autopause_pending_resume_ = v; }

    /// Simulates a process crash that skips the graceful shutdown drain:
    /// forgets the tracker was running (mode_ -> None, segment anchor
    /// cleared) without finalizing the ongoing segment, so its checkpoint
    /// row is left as an unfinalized orphan for startup recovery to
    /// reconcile. There is no behavioural route to this state -- letting
    /// the tracker destruct while still running invokes ~TimeTracker()'s
    /// last-resort drainForShutdown(), which gracefully finalizes the
    /// segment instead, the opposite of what a crash-recovery test needs.
    void simulateCrashWithoutShutdown_dbg() {
        mode_ = Mode::None;
        session_.clearSegment();
    }

    /// Exposes the internal checkpoint-save path for direct testing.
    void saveCheckpoint_locked_dbg(const QDateTime& now) { saveCheckpoint_locked(now); }

    /// Exposes the backpause path for direct testing.
    void backpauseTimer_dbg(const QDateTime& now, const QDateTime& lockTime = QDateTime()) { backpauseTimer(now, lockTime); }

    /// Exposes the addDuration path for direct testing.
    void addDuration_dbg(DurationType type, const QDateTime& start, const QDateTime& end) {
        addDuration(type, start, end);
    }

    /// Appends a single already-built TimeDuration directly onto the
    /// completed-segment deque, bypassing addDuration()'s create() guards
    /// (same-day, positive duration). No behavioural route can arrange a
    /// cross-midnight *completed* row: addDuration() discards it by design
    /// (TimeDuration::create returns CrossMidnight). CAUTION: a cross-midnight
    /// row seeded here makes the next completedTimeline()/snapshot() call
    /// abort the whole qtest binary -- Timeline's constructor Q_ASSERTs
    /// same-day per completed row (timeline.cpp:14-16, 107-113). Callers must
    /// ensure nothing reads the timeline while such a row is seeded.
    void seedCompletedDuration_dbg(const TimeDuration& d) {
        session_.durations.push_back(d);
    }

    /// Returns whether the periodic checkpoint QTimer is currently running.
    bool isCheckpointTimerActive_dbg() const { return checkpointTimer_.isActive(); }

    /// Exposes the pending deferred lock correction (set while dialog_open_).
    auto pendingLockCorrection_dbg() const { return deferred_edit_events_.lock(); }
    /// Exposes/injects the pending deferred midnight event (set while dialog_open_).
    void setPendingMidnightEvent_dbg(StopReason reason, const QDateTime& timestamp) {
        deferred_edit_events_.setMidnight_dbg(reason, timestamp);
    }
    auto pendingMidnightEvent_dbg() const { return deferred_edit_events_.midnight(); }

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
    bool updateDurationsInDB_dbg() { return updateDurationsInDB().okOrDisabled(); }
    void replaceCurrentDurations_dbg(const std::deque<TimeDuration>& d,
                                     const std::optional<TimeDuration>& o = std::nullopt) {
        replaceCurrentDurations(d, o);
    }
#endif // QT_NO_DEBUG
};

Q_DECLARE_METATYPE(TimeTracker::StopReason)
Q_DECLARE_METATYPE(TimeTracker::Mode)
Q_DECLARE_METATYPE(TimeTracker::TransitionCause)
Q_DECLARE_METATYPE(TimeTracker::Transition)

#endif // TIMER_H
