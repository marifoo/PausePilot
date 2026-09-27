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
#include "historyeligibility.h"

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

};


class TimeTracker : public QObject
{
    Q_OBJECT
public:
    /// Reason a stop was initiated. Carried by the stopped() signal so
    /// observers (e.g. MainWin) can distinguish user-driven from engine-driven stops.
    enum class StopReason {
        ButtonStop,         ///< User pressed the Stop button.
        MidnightScheduled,  ///< DayBoundaryWatcher's heartbeat check reached the 23:59:59.500 cutoff.
        MidnightWatchdog,   ///< Watchdog detected a cross-midnight ongoing segment.
        Shutdown,           ///< Application shutdown (destructor or ShutdownCoordinator).
        FatalStoreError,    ///< C4: a store operation returned FatalError; tracking latched unavailable.
    };

public:
    enum class Mode {Activity, Pause, None};

    /// D10 (R10-001/R10-002): the one cause vocabulary for every real mode
    /// transition, button/lock/midnight/shutdown-driven alike. Carried
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
     * DayBoundaryWatcher — owns the scheduled 23:59:59.500 stop and the
     * cross-midnight watchdog logic, both driven purely from the 100 ms
     * heartbeat (tick()). There is no independent QTimer: a single owner
     * (the heartbeat) is sufficient because the old dedicated timer shared
     * the same Qt event loop and offered no independent wakeup when that
     * thread was blocked.
     */
    class DayBoundaryWatcher {
    public:
        explicit DayBoundaryWatcher(TimeTracker& owner);

        /// Called on every 100 ms heartbeat. Runs the forward-date History
        /// revoke check, the scheduled 23:59:59.500 stop, and the
        /// cross-midnight watchdog, in that order.
        void tick(const QDateTime& now);

    private:
        TimeTracker& owner_;
    };

    /**
     * PendingCommit — the one persistence payload TimeTracker may still owe
     * the store.
     *
     * Invariant: there is either no pending commit, or exactly one immutable
     * payload awaiting a durable write. Every complete successful
     * persistence (an ordinary commit, a Start-time retry, or a contractual
     * Disabled no-op) clears it. A pending
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

        void clear() { payload_.reset(); }

    private:
        std::optional<std::deque<TimeDuration>> payload_;
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
    bool dialog_open_;   // True while HistoryDialog is open. Checkpoints keep running (WP6); lock-watcher events, a forward date change, the midnight stop/discard and shutdown revoke it first (revokeExclusiveEdit()); any other real mode change revokes it from inside emitTransition() (H5/WP7c).
    QDate exclusive_edit_date_;  // WP4: the date when beginExclusiveEdit() was called.
                                 // Used to detect when the date moves forward (>) and revoke the edit.
    bool shutdown_called_ = false;  // Set once shutdown() has torn down the engine.
                                     // Cleared only by cancelShutdown(); while set, startTimer() refuses Start (log only).
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
    /// C4 fix round 3 (F4): post-C4, no production path in SqliteSessionStore
    /// produces a FatalError any more -- commitSession, saveCheckpoint,
    /// discardCheckpoint, loadDurations, flushToDisc,
    /// setLastCleanShutdownMarker, and recoverStartupCheckpoints have all
    /// been consolidated onto TransientError/CallerBug (see sessionstore.h).
    /// The stopIfLatched()/latch-check call sites in this class
    /// (saveCheckpoint_locked, startTimer's Pause and None branches,
    /// finalizePauseTransition) are therefore defence-in-depth against a
    /// future SessionStore implementation, not currently reachable from
    /// SqliteSessionStore, and are exercised in tests only via
    /// FakeSessionStore injecting FatalError from those operations. (The
    /// engine no longer calls replaceAll; WP10 removes the latch entirely.)
    bool fatal_state_latched_ = false;
    SessionStore& db_;
    mutable QRecursiveMutex mutex_;  // Protects state transitions from concurrent access
    QTimer checkpointTimer_;  // TimeTracker for periodic checkpoint saving
    static constexpr int kCheckpointIntervalMsec = 5 * 60 * 1000; // Fixed 5-minute Activity checkpoint cadence
    qint64 startup_recovered_msec_;
    bool startup_recovery_notification_needed_;
    int startup_policy_dropped_count_;
    DayBoundaryWatcher day_boundary_watcher_; // Must be last: constructed after all other members

    // cause defaults to the button/toggle path; the lock-resume caller (Unlock
    // handling) passes TransitionCause::LockResume so the
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
    void backpauseTimer(const QDateTime& now, const QDateTime& lockTime);
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
    // revokes it, then falls through to the discard exactly as with no dialog.
    // Returns true when it fired. Safe to call multiple times; only the first
    // call does real work.
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
    /// Locked variant of getOngoingDuration() for callers that already hold mutex_.
    std::optional<TimeDuration> getOngoingDuration_locked() const;
    /// Same, but built from a caller-supplied `now` instead of a fresh
    /// now_provider_() read (H5/WP3): beginExclusiveEdit() uses this so the
    /// ongoing row in the captured HistoryEditContext shares the dialog's
    /// one open-time clock read instead of taking a second one. The no-arg
    /// overload above forwards to this with now_provider_().
    std::optional<TimeDuration> getOngoingDuration_locked(const QDateTime& now) const;
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

    // D10: the one place that emits a real mode transition. Emits the
    // transitioned(Transition) event, then derives the one remaining legacy
    // stopped() signal from the same cause centrally (R10-002) -- so, for
    // example, lock resume is one emission call producing one coherent
    // notification instead of two callers each emitting a piece of it.
    void emitTransition(Mode from, Mode to, TransitionCause cause);

    // D10/plan4 B16: useTimerViaLockEvent's per-event-kind behavior, each
    // owning its complete logic (including the autopause gate; the dialog
    // gate is handled once, before the switch, by WP5's revoke) rather than
    // a shared fall-through block.
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
    /// engine is still editable regardless of this flag. While set, Start
    /// (button or lock-resume) is refused by startTimer() -- shutdown is
    /// terminal until cancelShutdown() (C9b). Test-only accessor
    /// today -- History's own per-dialog save gate is shutdownEpoch(), not
    /// this flag (D5 review fix, R15-002).
    bool wasShutDown() const { QMutexLocker lk(&mutex_); return shutdown_called_; }
    /// D5 (R15-002): reverses shutdown()'s terminal flag after a cancelled OS
    /// shutdown. Clears shutdown_called_ only -- mode_/session_/pending
    /// commit-or-discard state are untouched, so the ended session is neither
    /// reconstructed nor resumed and pending persistence stays governed by
    /// A2/A3 (retried at the next Start, not here). This is the only clearer
    /// of shutdown_called_; after this, Start works again (C9b).
    void cancelShutdown();
    /// D5 review fix (finding 1, R15-002): monotonic, never reset by
    /// cancelShutdown() -- lets a caller (HistoryDialog) detect "a shutdown
    /// happened since I opened" independent of whether it was later
    /// cancelled, so a dialog that spanned the shutdown stays refused even
    /// after cancelShutdown() clears wasShutDown() for everyone else.
    qint64 shutdownEpoch() const { QMutexLocker lk(&mutex_); return shutdown_epoch_; }

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
    std::optional<TimeDuration> getOngoingDuration() const;
    qint64 getStartupRecoveredMsec() const;
    bool shouldShowStartupRecoveryNotification() const;
    int getStartupPolicyDroppedCount() const;
    /// Marks the exclusive edit open and captures the open-time
    /// HistoryEditContext (H5/WP3): one now_provider_() read, one
    /// db_.loadHistoryRows() call, and a snapshot of the live/pending-commit/
    /// stopped state. Call before opening HistoryDialog. Checkpoints keep
    /// running while the edit is open (WP6).
    HistoryEditContext beginExclusiveEdit();
    /// Clears the exclusive-edit marker. Call after closing HistoryDialog.
    void endExclusiveEdit();
    /// Saves a scoped History edit (H5/WP3) built against the context
    /// `opened` returned by beginExclusiveEdit(). Holds mutex_ across the
    /// whole call and checks, in order:
    ///   1. the edit was revoked (dialog_open_ is now false) -> Conflict;
    ///   2. any day's date was not editable in `opened` -> CallerBug;
    ///   3. the current local date differs from opened.today (local date
    ///      changed, backward, or forward before the revoke tick) -> Conflict;
    ///   4. any day's captured UTC span no longer matches its local day's
    ///      span (timezone change) -> Conflict;
    ///   5. otherwise, db_.saveHistoryDays(opened.stored.rows, days).
    /// Checks 1-4 never call the store. Never touches session_,
    /// pending_commit_, mode_, the checkpoint anchor or the timer -- the
    /// snapshot in `opened` is the only input besides `days`.
    SessionStoreResult saveHistoryEdit(const HistoryEditContext& opened,
                                       const std::vector<HistoryDayChange>& days);
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
    /// Revokes an open History edit first (if any), then stops the timer if running
    /// and returns the resulting state. Thread-safe; may be called multiple times
    /// (idempotent on an already-stopped timer).
    ShutdownResult shutdown();

    /**
     * Terminal-shutdown drain: closes any live segment using one captured
     * `now` (via shutdown()), then retries any pending commit payload even
     * if mode_ is already None. Independent of tracker mode -- a pre-existing
     * pending payload from an earlier failed Stop is retried here too.
     *
     * Idempotent: a second call neither duplicates a committed segment nor
     * retries after a prior success. A repeated failure re-attempts the same
     * payload without corrupting its identity and returns that attempt's
     * outcome directly.
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
    DrainResult drainForShutdown();

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
    /// WP1: the edit has been revoked (at lock, sleep, midnight, shutdown, when
    /// the date moves forward, or on any other real mode change -- Start,
    /// Pause, unpause or Stop; H5/WP7c). HistoryDialog's onExclusiveEditRevoked()
    /// slot receives this synchronously (direct connection) to set editRevoked_,
    /// then queues closeRevoked() to run on the next event-loop turn so nested
    /// modals (QMessageBox::question, QMenu::exec) can unwind first. If the
    /// dialog is destroyed before that turn, the queued call is dropped with it.
    /// Contract: a slot on this signal must not call back into TimeTracker.
    /// The WP7c mode-change revoke fires mid-transition, inside
    /// emitTransition(), before transitioned() and stopped() are emitted, so
    /// the engine's own state for this transition is not yet fully published.
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
    void backpauseTimer_dbg(const QDateTime& now, const QDateTime& lockTime) { backpauseTimer(now, lockTime); }

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

    /// Returns the periodic checkpoint QTimer's configured interval, in milliseconds.
    int checkpointTimerIntervalMsec_dbg() const { return checkpointTimer_.interval(); }

    /// Triggers the periodic checkpoint slot directly (bypasses timer interval).
    void triggerCheckpoint_dbg() { saveCheckpoint(); }

    /// Calls the internal cross-midnight discard helper directly.
    bool discardCrossMidnightOngoingAndStop_dbg(const QDateTime& now) {
        return discardCrossMidnightOngoingAndStop(now);
    }

    /// Exposes the internal DB-write helpers for the regression surface test.
    bool updateDurationsInDB_dbg() { return updateDurationsInDB().okOrDisabled(); }
#endif // QT_NO_DEBUG
};

Q_DECLARE_METATYPE(TimeTracker::StopReason)
Q_DECLARE_METATYPE(TimeTracker::Mode)
Q_DECLARE_METATYPE(TimeTracker::TransitionCause)
Q_DECLARE_METATYPE(TimeTracker::Transition)

#endif // TIMER_H
