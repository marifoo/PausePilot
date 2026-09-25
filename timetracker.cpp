/**
 * TimeTracker — Core timing engine managing Activity/Pause/None states.
 *
 * Architecture:
 * - State machine with three modes: Activity, Pause, None.
 * - Completed segments stored in session_.durations (deque); ongoing
 *   segment tracked via wall-clock times (session_.segment_start_time and now).
 * - Checkpoints persist the ongoing Activity segment every 5 min.
 * - Thread-safe via QRecursiveMutex; per-session mutable state grouped
 *   in SessionState with explicit logged transitions.
 *
 * Day-boundary policy:
 * - TimeTracker NEVER stores or persists a segment that crosses local
 *   midnight (D1: half-open [start, end) -- a segment ending exactly at
 *   local midnight is same-day and legal). Such segments are silently
 *   discarded by addDuration() and refused by saveCheckpoint_locked().
 * - The day-boundary rule is owned entirely by DayBoundaryWatcher: a
 *   scheduled stop at 23:59:59.500 and a 100 ms watchdog (via onTick())
 *   drive the engine stop path. MainWin has no midnight logic.
 * - Stops emit stopped(StopReason) so the GUI syncs without polling.
 *
 * Checkpoint behavior:
 * - Only saved during Activity mode (not Pause or Stopped)
 * - Suspended while PC is locked (is_locked_) or HistoryDialog is open (dialog_open_)
 * - Uses session_.segment_id to update same DB row instead of creating new rows
 *
 * Backpause behavior:
 * - When PC locked for longer than threshold, retroactively converts last N minutes to Pause
 * - Triggered by LockEvent::LongOngoingLock from LockStateWatcher
 *
 * History interaction:
 * - Any lock-watcher event (Lock, LongOngoingLock, Unlock) first revokes an
 *   open History edit (WP5), so lock/backpause bookkeeping always runs as
 *   if History had never been open.
 */

#include "timetracker.h"
#include <QtDebug>
#include <QDateTime>
#include <QMutexLocker>
#include <new>  // for std::bad_alloc
#include "logger.h"
#include "dayvalidity.h"

namespace {
// WP2: one neutral notice for every trigger that force-closes an open
// History edit. One neutral text for every revoke trigger.
const QString kHistoryRevokedNotice =
    "History was closed (screen lock, sleep, midnight or shutdown); any unsaved changes were discarded.";

// D10: the only translation needed between the pre-existing StopReason
// vocabulary (still used wherever a stop's own reason is threaded through --
// stopTimer's parameter, DrainResult, ShutdownCoordinator) and the newer,
// broader TransitionCause used by transitioned(). Every StopReason maps to
// exactly one to-None cause.
TimeTracker::TransitionCause causeFromStopReason(TimeTracker::StopReason reason)
{
    switch (reason) {
    case TimeTracker::StopReason::ButtonStop:         return TimeTracker::TransitionCause::ButtonStop;
    case TimeTracker::StopReason::MidnightScheduled:  return TimeTracker::TransitionCause::MidnightScheduled;
    case TimeTracker::StopReason::MidnightWatchdog:   return TimeTracker::TransitionCause::MidnightWatchdog;
    case TimeTracker::StopReason::Shutdown:           return TimeTracker::TransitionCause::Shutdown;
    case TimeTracker::StopReason::EditApplied:        return TimeTracker::TransitionCause::EditApplied;
    case TimeTracker::StopReason::FatalStoreError:    return TimeTracker::TransitionCause::FatalStoreError;
    }
    Q_UNREACHABLE();
}

// The inverse, for deriving stopped() from a transitioned() event whose
// `to` is Mode::None. Only ever called for the six causes that can reach
// None -- every other TransitionCause never produces a to-None transition.
// Every enumerator is listed explicitly (no `default:`) so that adding a
// new TransitionCause without updating this function is a -Wswitch build
// warning, matching causeFromStopReason() above -- a `default:` here would
// silently swallow that warning and defer the mistake to a release-build
// Q_UNREACHABLE() (undefined behaviour) instead.
TimeTracker::StopReason stopReasonFromCause(TimeTracker::TransitionCause cause)
{
    switch (cause) {
    case TimeTracker::TransitionCause::ButtonStop:        return TimeTracker::StopReason::ButtonStop;
    case TimeTracker::TransitionCause::MidnightScheduled: return TimeTracker::StopReason::MidnightScheduled;
    case TimeTracker::TransitionCause::MidnightWatchdog:  return TimeTracker::StopReason::MidnightWatchdog;
    case TimeTracker::TransitionCause::Shutdown:          return TimeTracker::StopReason::Shutdown;
    case TimeTracker::TransitionCause::EditApplied:       return TimeTracker::StopReason::EditApplied;
    case TimeTracker::TransitionCause::FatalStoreError:   return TimeTracker::StopReason::FatalStoreError;
    case TimeTracker::TransitionCause::ButtonStart:
    case TimeTracker::TransitionCause::ButtonPause:
    case TimeTracker::TransitionCause::LockAutopause:
    case TimeTracker::TransitionCause::LockResume:
        Q_UNREACHABLE();
    }
    Q_UNREACHABLE();
}
} // namespace

// ============================================================================
// DayBoundaryWatcher implementation
// ============================================================================

TimeTracker::DayBoundaryWatcher::DayBoundaryWatcher(TimeTracker& owner)
    : owner_(owner)
{
}

void TimeTracker::DayBoundaryWatcher::tick(const QDateTime& now)
{
    // The mutex is already held by the caller (TimeTracker::onTick).
    //
    // 1. Forward-date History revoke: runs even when stopped. WP4: if the
    // local date moves forward (>) while editing, revoke the edit first.
    const QDate nowLocalDate = now.toLocalTime().date();
    if (owner_.dialog_open_ && nowLocalDate > owner_.exclusive_edit_date_) {
        owner_.revokeExclusiveEdit();
    }

    // 2. Scheduled stop: only while a segment is running that started on
    // this same local date (persistence's civil-day rule, matching
    // isCrossMidnight/DayValidity). A segment from a prior local date never
    // takes this path -- it falls through to the watchdog below.
    if (owner_.mode_ != Mode::None && owner_.session_.segment_start_time.isValid()) {
        const QDateTime& segmentStart = owner_.session_.segment_start_time;
        if (nowLocalDate == segmentStart.toLocalTime().date()) {
            // Construct the cutoff from `now`'s own instant (via
            // toLocalTime()) rather than from date()+time()+a default
            // timeSpec() -- the latter would silently default the UTC
            // offset to 0 for a Qt::OffsetFromUTC/Qt::TimeZone now-provider
            // (D2/LOW-4).
            QDateTime cutoff = now.toLocalTime();
            cutoff.setTime(QTime(23, 59, 59, 500));
            if (now >= cutoff) {
                if (owner_.dialog_open_) {
                    owner_.revokeExclusiveEdit();
                }
                if (segmentStart < cutoff) {
                    // Segment began before the cutoff: stop exactly at the
                    // cutoff, never at a later heartbeat instant.
                    owner_.stopTimer(cutoff, StopReason::MidnightScheduled);
                } else if (now > segmentStart) {
                    // Segment began at/after the cutoff (e.g. Start pressed
                    // at 23:59:59.600): stop at the first valid same-day
                    // observed heartbeat instant, never before its own start.
                    owner_.stopTimer(now, StopReason::MidnightScheduled);
                }
                // else now <= segmentStart (same tick or a backward step):
                // do nothing until a later tick.
                return;
            }
        }
    }

    // 3. Watchdog: unchanged. A prior-date segment only reaches this path.
    owner_.discardCrossMidnightOngoingAndStop(now);
}

// ============================================================================
// SessionState transition methods
// ============================================================================

void SessionState::beginNewSegment(const QDateTime& startTime)
{
    SegmentId oldId = segment_id;
    QDateTime oldStart = segment_start_time;
    segment_id = SegmentId::mint();
    segment_start_time = startTime;
    Logger::Log(QString("[STATE] beginNewSegment: segId '%1' -> '%2', start %3 -> %4")
        .arg(oldId.toString(), segment_id.toString(),
             oldStart.toString(Qt::ISODate), startTime.toString(Qt::ISODate)));
}

void SessionState::clearSegment()
{
    SegmentId oldId = segment_id;
    QDateTime oldStart = segment_start_time;
    segment_id = SegmentId{};
    segment_start_time = QDateTime();
    Logger::Log(QString("[STATE] clearSegment: segId '%1' -> (empty), start %2 -> (invalid)")
        .arg(oldId.toString(), oldStart.toString(Qt::ISODate)));
}

void SessionState::updateSegmentStartTime(const QDateTime& newStart)
{
    QDateTime oldStart = segment_start_time;
    segment_start_time = newStart;
    Logger::Log(QString("[STATE] updateSegmentStartTime: %1 -> %2")
        .arg(oldStart.toString(Qt::ISODate), newStart.toString(Qt::ISODate)));
}

void SessionState::resetForNewSession()
{
    size_t oldSize = durations.size();
    durations.clear();
    Logger::Log(QString("[STATE] resetForNewSession: durations %1 -> 0").arg(oldSize));
}

void SessionState::adoptOngoingSegment(const TimeDuration& ongoing)
{
    SegmentId oldId = segment_id;
    QDateTime oldStart = segment_start_time;
    segment_id = ongoing.segment_id;
    segment_start_time = ongoing.startTime;
    Logger::Log(QString("[STATE] adoptOngoingSegment: segId '%1' -> '%2', start %3 -> %4")
        .arg(oldId.toString(), segment_id.toString(),
             oldStart.toString(Qt::ISODate), ongoing.startTime.toString(Qt::ISODate)));
}

// ============================================================================
// Debug-build state invariant checking
// ============================================================================

#ifndef QT_NO_DEBUG

/**
 * Checks structural invariants on the in-memory duration segments.
 *
 * Called in debug builds after every mutator that can reorder or add
 * duration segments (button/lock transitions, setDurationType,
 * commitEditedTimeline). Catches data-structure corruption early — before
 * it propagates to the DB or produces confusing UI behavior. Violations are
 * logged as warnings (not fatal) so tests surface them as QWARN output.
 *
 * Invariants checked:
 *   1. Segment ordering: startTimes must be non-decreasing.
 *   2. No overlapping segments of the same type on the same day.
 *   3. All segment_ids must be non-empty.
 *   4. When mode_ == Mode::None, the segment anchor must be clear:
 *      segment_id empty and segment_start_time invalid.
 */
void TimeTracker::checkDurationInvariants() const
{
    // Invariant 4: stopped means no anchor. Checked independently of the
    // durations deque below -- it must fire even with zero completed
    // segments.
    if (mode_ == Mode::None
        && (!session_.segment_id.isEmpty() || session_.segment_start_time.isValid())) {
        qWarning("[INVARIANT] Stopped (mode_ == None) but segment anchor is not clear: "
                 "segment_id=%s, segment_start_time=%s",
                 qPrintable(session_.segment_id.toString()),
                 qPrintable(session_.segment_start_time.toString(Qt::ISODate)));
    }

    const auto& durations = session_.durations;
    if (durations.empty()) {
        return;
    }

    for (size_t i = 0; i < durations.size(); ++i) {
        // Invariant 3: All segment_ids must be non-empty
        if (durations[i].segment_id.isEmpty()) {
            qWarning("[INVARIANT] Duration at index %zu has empty segment_id "
                     "(type=%d, start=%s, end=%s)",
                     i,
                     static_cast<int>(durations[i].type),
                     qPrintable(durations[i].startTime.toString(Qt::ISODate)),
                     qPrintable(durations[i].endTime.toString(Qt::ISODate)));
        }

        if (i == 0) {
            continue;
        }

        // Invariant 1: startTimes must be non-decreasing
        if (durations[i].startTime < durations[i - 1].startTime) {
            qWarning("[INVARIANT] Segment ordering violation at index %zu: "
                     "startTime %s < previous startTime %s",
                     i,
                     qPrintable(durations[i].startTime.toString(Qt::ISODate)),
                     qPrintable(durations[i - 1].startTime.toString(Qt::ISODate)));
        }

        // Invariant 2: No overlapping segments of the same type on the same day
        if (durations[i].type == durations[i - 1].type
            && !isCrossMidnight(durations[i - 1].startTime, durations[i].startTime)
            && durations[i].startTime < durations[i - 1].endTime) {
            qWarning("[INVARIANT] Overlapping same-type segments on %s at indices %zu and %zu: "
                     "prev=[%s..%s], curr=[%s..%s], type=%s",
                     qPrintable(durations[i].startTime.date().toString(Qt::ISODate)),
                     i - 1, i,
                     qPrintable(durations[i - 1].startTime.toString(Qt::ISODateWithMs)),
                     qPrintable(durations[i - 1].endTime.toString(Qt::ISODateWithMs)),
                     qPrintable(durations[i].startTime.toString(Qt::ISODateWithMs)),
                     qPrintable(durations[i].endTime.toString(Qt::ISODateWithMs)),
                     durations[i].type == DurationType::Activity ? "Activity" : "Pause");
        }
    }
}

/**
 * Cross-layer invariant: for each persisted segment, the DB must contain
 * exactly one finalized row with matching fields after a Success
 * commitSession(); no requirement is imposed after a Disabled one (history
 * storage off means the write was a deliberate no-op, so this checker has
 * nothing to verify against). Called after every okOrDisabled()
 * commitSession() in updateDurationsInDB() to catch divergence between
 * memory and storage.
 *
 * D13e (deferred from D11/R09-002): the "exactly one" requirement below is
 * only enforced for Success, since FakeSessionStore::loadDurations() now
 * (D13e) projects storedDurations so the double mirrors committed state --
 * previously it never did, so this had to accept zero matches
 * unconditionally to avoid failing ~165 fake-store tests whose loaded state
 * never reflected what commitSession() had just written.
 */
void TimeTracker::checkCrossLayerInvariants(const SessionStoreResult& commitResult) const
{
    if (session_.durations.empty()) {
        return;
    }

    // Compare against the normalized timeline — i.e. exactly what commitSession()
    // persisted. Normalization merges adjacent same-type segments (extending the
    // survivor's endTime), so iterating the raw session_.durations here would
    // report spurious endTime mismatches for any segment that was merged.
    const std::deque<TimeDuration> persisted =
        Timeline(session_.durations, std::nullopt).normalized().completed();

    // Load the current DB state once.
    LoadResult dbResult = db_.loadDurations();

    const bool expectPersisted = (commitResult.category == SessionStoreResult::Success);

    for (const auto& seg : persisted) {
        if (seg.segment_id.isEmpty()) {
            continue; // Already caught by checkDurationInvariants()
        }

        int matchCount = 0;
        for (const auto& dbRow : dbResult.durations) {
            if (dbRow.segment_id == seg.segment_id) {
                ++matchCount;
                // Fields must match.
                if (dbRow.type != seg.type
                    || dbRow.startTime != seg.startTime
                    || dbRow.endTime != seg.endTime) {
                    qWarning("[CROSS-LAYER] Segment %s: DB row fields mismatch memory "
                             "(type %d vs %d, start %s vs %s, end %s vs %s)",
                             qPrintable(seg.segment_id.toString()),
                             static_cast<int>(dbRow.type), static_cast<int>(seg.type),
                             qPrintable(dbRow.startTime.toString(Qt::ISODateWithMs)),
                             qPrintable(seg.startTime.toString(Qt::ISODateWithMs)),
                             qPrintable(dbRow.endTime.toString(Qt::ISODateWithMs)),
                             qPrintable(seg.endTime.toString(Qt::ISODateWithMs)));
                }
            }
        }

        if (matchCount > 1) {
            qWarning("[CROSS-LAYER] Segment %s: expected 0 or 1 DB rows, found %d",
                     qPrintable(seg.segment_id.toString()), matchCount);
        } else if (expectPersisted && matchCount == 0) {
            qWarning("[CROSS-LAYER] Segment %s: committed via Success commitSession() but "
                     "no matching DB row found",
                     qPrintable(seg.segment_id.toString()));
        }
    }
}

#endif // QT_NO_DEBUG

// ============================================================================
// TimeTracker implementation
// ============================================================================

TimeTracker::TimeTracker(const Settings &settings, SessionStore& db, QObject *parent)
    : QObject(parent), settings_(settings), now_provider_(QDateTime::currentDateTime),
      session_(), mode_(Mode::None),
      autopause_pending_resume_(false), is_locked_(false),
      dialog_open_(false), db_(db),
      startup_recovered_msec_(0), startup_recovery_notification_needed_(false),
      startup_policy_dropped_count_(0),
      day_boundary_watcher_(*this)
{
    checkpointTimer_.setInterval(kCheckpointIntervalMsec);
    checkpointTimer_.setSingleShot(false);
    connect(&checkpointTimer_, &QTimer::timeout, this, &TimeTracker::saveCheckpoint);

    // No persistence side effects in the constructor.
    // Call initializeFromStore() after checkSchemaOnStartup() returns Ready/Created.
}

StartupRecoveryResult TimeTracker::initializeFromStore()
{
    const StartupRecoveryResult recovery = db_.recoverStartupCheckpoints(now_provider_());
    if (recovery.okOrDisabled()) {
        startup_recovered_msec_ = recovery.recovered_msec;
        startup_recovery_notification_needed_ = recovery.notify_user;
        // M4: the discard-forced subset is work the user explicitly threw away;
        // only the remainder is a deletion the user never asked for.
        startup_policy_dropped_count_ = recovery.policy_dropped_count - recovery.discard_dropped_count;
    }
    return recovery;
}

TimeTracker::~TimeTracker()
{
    // Stop and disconnect checkpointTimer_ before acquiring the mutex so
    // that no in-flight slot can fire against a partially-destroyed object.
    checkpointTimer_.stop();
    checkpointTimer_.disconnect();
    QMutexLocker locker(&mutex_);
    // drainForShutdown() is idempotent: on every normal path
    // ShutdownCoordinator has already run it (directly or via a user Retry)
    // and this call is a no-op. If a pending commit or discard intent is
    // still here AND shutdown()/drainForShutdown() was never called on this
    // object before, ShutdownCoordinator was bypassed entirely -- an
    // orchestration bug, even though this last-resort drain may still
    // resolve it.
    const bool orchestrationBypassed = !shutdown_called_
        && (pending_commit_.hasPending() || pending_discards_.hasPending());
    const DrainResult result = drainForShutdown();
    if (orchestrationBypassed) {
        Logger::Log(QString("[SHUTDOWN] ~TimeTracker: found a pending commit at destruction "
                             "with no prior shutdown()/drainForShutdown() call -- "
                             "ShutdownCoordinator was bypassed (orchestration bug); "
                             "last-resort drain result: %1")
                     .arg(result.clean() ? "Complete" : "Incomplete"));
        Q_ASSERT_X(false, "~TimeTracker",
                   "drainForShutdown() must be invoked via ShutdownCoordinator before "
                   "TimeTracker is destroyed");
    }
}

/**
 * Retries the pending commit payload at the start of a new session (called
 * only from the None -> Activity transition in startTimer()). Performs
 * exactly one retry attempt against the payload PendingCommit already holds
 * — there is no retry counter or failure threshold.
 *
 * - No pending payload: no-op, returns true (startTimer continues).
 * - Success/Disabled: payload cleared, returns true (startTimer continues).
 * - Any other failure: payload retained unchanged, one concise error is
 *   emitted (Fatal already emitted its own message via logDbResultOrWarn),
 *   returns false (startTimer aborts; tracker remains stopped).
 */
bool TimeTracker::retryUnsavedDurations()
{
    // Best-effort: a pending checkpoint-discard intent must not block Start
    // (only clean-marking cares about it), but Start is a normal persistence
    // opportunity to retry it.
    retryPendingDiscards();

    if (!pending_commit_.hasPending()) {
        return true;
    }

    Logger::Log("[DB] Retrying pending commit at Start");
    const SessionStoreResult retryResult = appendDurationsChunkToDB(pending_commit_.payload());
    const bool ok = retryResult.okOrDisabled();
    pending_commit_.retry(ok);
    if (ok) {
        Logger::Log("[DB] Pending commit saved successfully");
        return true;
    }
    if (retryResult.category != SessionStoreResult::FatalError) {
        // FatalError already emitted "Database error: ..." via logDbResultOrWarn.
        emit userWarning("Could not save previous session data. Please retry Start.");
    }
    return false;
}

/**
 * Transitions timer to Activity mode. Behavior depends on current state:
 * - From Pause: Records pause duration, persists it immediately to DB for
 *   crash safety, then switches to Activity
 * - From None: Clears state, optionally adds boot time, starts fresh
 * - From Activity: No-op (logs debug message)
 *
 * @param now  Wall-clock timestamp captured once by the caller. All time
 *             calculations within this method (and its callees) use this
 *             single value, preventing races from thread preemption or
 *             clock skew between successive QDateTime::currentDateTime()
 *             calls.
 */
void TimeTracker::startTimer(const QDateTime& now, TransitionCause cause)
{
    if (shutdown_called_) {
        Logger::Log("[TIMER] Start refused: shutdown in progress, until cancelShutdown()");
        return;
    }

    // Clear autopause flag at the top of startTimer so both Pause→Activity
    // and None→Activity paths start with a clean state.
    autopause_pending_resume_ = false;

    if (mode_ == Mode::Pause) {
        Logger::Log("[DEBUG] Starting TimeTracker from Pause - D=" + QString::number(session_.durations.size()));

        // Record the completed Pause segment with its own fresh segment_id
        // (current_checkpoint_segment_id was assigned when transitioning to Pause mode).
        // Do NOT reuse or mutate any prior Activity segment_id — that was the
        // root cause of a previously fixed bug. Timeline::normalized() handles
        // merging adjacent same-type segments at DB-sync time.
        addDuration(DurationType::Pause, session_.segment_start_time, now, session_.segment_id);

        mode_ = Mode::Activity;
        session_.beginNewSegment(now);

        // Persist the completed Pause row immediately so it survives a crash.
        // Without this, a crash before the next pauseTimer/stopTimer would lose
        // the Pause entry, making the reloaded timeline show two Activities
        // back-to-back with no gap. The row is written as finalized (is_finalized=1)
        // because the Pause segment is complete the moment the user un-pauses.
        persistDurationsOrRetainForRetry();

        // C4 (finding 2): a Fatal commit above latches the engine but does
        // NOT stop it (enterFatalState() only latches) -- stop here, at the
        // first safe point after this transition's own state changes are
        // done, using the already-captured `now`. Without this, control
        // would fall through to the transitioned()/started() emission below
        // while mode_ has already been forced to None by a stop happening
        // mid-transition, desyncing the UI (started() with nothing actually
        // running). mode_ was already mutated to Activity above but that
        // transition was never announced -- pass Mode::Pause (what observers
        // actually last saw) so the nested stop's own emitted `from` is
        // truthful instead of the impossible Activity->None.
        if (stopIfLatched(now, Mode::Pause)) {
            return;
        }

        checkpointTimer_.start(); // Resume periodic checkpoint saving
        Logger::Log("[TIMER] > TimeTracker unpaused");
        emitTransition(Mode::Pause, Mode::Activity, cause);
        return;
    }
    if (mode_ == Mode::None) {
        Logger::Log("[DEBUG] Starting TimeTracker from Stopped - D=" + QString::number(session_.durations.size()));

        // C4 (R05-006): Start is disabled once a FatalError has latched the
        // engine into its one unavailable state; only a restart (a fresh
        // TimeTracker instance) clears it. No further dialog is shown here --
        // the transition already showed exactly one.
        if (fatal_state_latched_) {
            Logger::Log("[TIMER] Start refused: fatal store error latched, restart required");
            // C4 (finding 9): make the refusal observable -- without this,
            // Start silently does nothing and the UI has no way to know why.
            emit userWarning("Cannot start: a database error requires restarting uTimer.");
            return;
        }

        if (!retryUnsavedDurations()) return;

        // C4 (round-3 F1): retryUnsavedDurations() can latch fatal_state_
        // latched_ via retryPendingDiscards() -> discardCheckpoint() without
        // stopping anything (mode_ is still None, so stopIfLatched() is a
        // pure no-op stop). Without this check, a fatal that latches here
        // falls through to `mode_ = Mode::Activity` below and starts
        // tracking while latched -- the one enterFatalState() site with no
        // owning-transition post-check.
        if (fatal_state_latched_) return;

        unsigned int boot_time_sec = settings_.getBootTimeSec();
        bool shouldAddBootTime = false;
        if (boot_time_sec > 0) {
            QDate today = now.toLocalTime().date();
            bool hasEntriesInMemory = false;
            for (const auto &d : session_.durations) {
                if (DayValidity::overlapsLocalDate(d.startTime, d.endTime, today)) {
                    hasEntriesInMemory = true;
                    break;
                }
            }
            // Use tri-state result: only add boot time when the DB positively
            // confirms zero entries for today. When the result is Unknown
            // (history disabled or DB inaccessible), we err on the side of
            // caution and skip boot time to avoid double-counting.
            EntriesForDateResult dbResult = hasEntriesForDate(today);
            shouldAddBootTime = !hasEntriesInMemory && (dbResult == EntriesForDateResult::No);
            Logger::Log(shouldAddBootTime
                        ? "[TIMER] Will add boot time: " + QString::number(boot_time_sec) + " seconds (first session today)"
                        : "[TIMER] Boot time not added - entries already exist for today");
        }
        session_.resetForNewSession();
        if (shouldAddBootTime) {
            QDateTime bootStart = now.addMSecs(-static_cast<qint64>(boot_time_sec) * 1000);
            addDuration(DurationType::Activity, bootStart, now);
        }
        mode_ = Mode::Activity;
        session_.beginNewSegment(now);
        checkpointTimer_.start(); // Start periodic checkpoint saving
        Logger::Log("[TIMER] >> TimeTracker started");
        emitTransition(Mode::None, Mode::Activity, cause);
        return;
    }
    Logger::Log("[DEBUG] Trying to Start TimeTracker from Mode Activity - D=" + QString::number(session_.durations.size()));
}

void TimeTracker::pauseTimer(const QDateTime& now)
{
    if (mode_ != Mode::Activity) {
        Logger::Log("[DEBUG] Pause ignored; not in Activity");
        return;
    }
    // D2/R11-003 (HIGH-1): a backward wall-clock step (NTP/manual/DST) can
    // make `now` precede the live segment's start. Anchoring the new Pause
    // segment there -- whether at the stepped-back `now` or, worse, capped
    // to the Activity segment's own start -- re-identifies real Activity
    // time as Pause once that Pause segment later ends normally. Refuse the
    // whole transition instead: touch no state, so the unchanged anchor
    // lets the next Pause/Stop attempt compute a correct interval once wall
    // time is sane again.
    if (isClockReversed(now, session_.segment_start_time)) {
        Logger::Log(QString("[CLOCK] Pause rejected: now (%1) precedes segment start (%2) — backward clock step")
            .arg(now.toString(Qt::ISODateWithMs), session_.segment_start_time.toString(Qt::ISODateWithMs)));
        // D2/LOW-F: recoverable and the UI won't desync, but a silent no-op
        // button press is still worth telling the user about (C4 precedent).
        emit userWarning("Pause not applied: the system clock just moved backward. Try again.");
        return;
    }
    Logger::Log("[DEBUG] Pausing TimeTracker from Activity - D=" + QString::number(session_.durations.size()));
    addDuration(DurationType::Activity, session_.segment_start_time, now, session_.segment_id);

    finalizePauseTransition(now);
    // C4 (finding 2): finalizePauseTransition() may have stopped the tracker
    // itself (a Fatal commit during the transition) -- only report Pause if
    // it actually landed in Pause.
    if (mode_ == Mode::Pause) {
        Logger::Log("[TIMER] TimeTracker paused <");
        emitTransition(Mode::Activity, Mode::Pause, TransitionCause::ButtonPause);
    }
}

void TimeTracker::finalizePauseTransition(const QDateTime& now)
{
    mode_ = Mode::Pause;
    session_.updateSegmentStartTime(now);
    persistDurationsOrRetainForRetry();
    session_.beginNewSegment(now);
    checkpointTimer_.stop();
    // C4 (finding 2): a Fatal commit above latches the engine but does NOT
    // stop it (enterFatalState() only latches) -- stop here, now that the
    // pause transition has reached a fully consistent state (mode_/segment_
    // set together), rather than mid-transition where a nested stop would
    // leave segment_id/segment_start_time set while mode_ == None -- the
    // state SessionState documents as impossible. mode_ was already mutated
    // to Pause above but that transition was never announced -- both callers
    // (pauseTimer, backpauseTimer) guarantee the pre-transition mode was
    // Activity, so pass that as the nested stop's truthful `from`.
    stopIfLatched(now, Mode::Activity);
}

/**
 * Retroactively converts the last N minutes of Activity to Pause.
 *
 * Called when PC has been locked longer than the backpause threshold.
 * Splits current elapsed time into two segments:
 *   1. Activity segment: startTime = segment_start_time, endTime = now - backpause
 *   2. Pause segment: startTime = now - backpause, endTime = now
 *
 * This corrects any checkpoint data that was saved during the lock period,
 * since checkpoints are suspended while locked but the time before lock detection
 * may have been saved as Activity.
 */
void TimeTracker::backpauseTimer(const QDateTime& now, const QDateTime& lockTime)
{
    // C12b: production's caller (useTimerViaLockEvent) already rejects a
    // LongOngoingLock without a captured instant before this is reached;
    // this guard is for the direct debug entry point (backpauseTimer_dbg),
    // which can still be called with an invalid lockTime.
    if (!lockTime.isValid()) {
        Logger::Log("[WARNING] Backpause ignored; no lock timestamp");
        return;
    }
    if (mode_ != Mode::Activity) {
        Logger::Log("[DEBUG] Backpause ignored; not in Activity");
        return;
    }
    if (!settings_.isAutopauseEnabled()) {
        Logger::Log("[DEBUG] Autopause disabled; backpause ignored");
        return;
    }
    Logger::Log("[DEBUG] Backpausing TimeTracker from Activity - D=" + QString::number(session_.durations.size()));

    // Anchor the pause at the real wall-clock lock instant: the
    // LongOngoingLock event may arrive long after the lock (e.g. after a
    // suspend), so a now-relative calculation would credit the suspended
    // interval as Activity.
    QDateTime activity_end = lockTime;

    // Ensure activity_end is not before segment_start_time
    if (activity_end < session_.segment_start_time) {
        activity_end = session_.segment_start_time;
    }

    // D2/R11-003 (MEDIUM-6): the same backward-step guard as pauseTimer,
    // against activity_end (the Pause segment's own start) rather than
    // segment_start_time -- a step arriving during/after a lock can make
    // `now` precede activity_end. Refuse the whole transition rather than
    // re-anchor the new Pause segment at a stepped-back `now`.
    if (isClockReversed(now, activity_end)) {
        Logger::Log(QString("[CLOCK] Backpause rejected: now (%1) precedes activity_end (%2) — backward clock step")
            .arg(now.toString(Qt::ISODateWithMs), activity_end.toString(Qt::ISODateWithMs)));
        // D2/LOW-F: same rationale as pauseTimer's refusal -- tell the user
        // rather than silently doing nothing. D2/LOW-3 (round-3 review):
        // distinct wording -- this is the AUTOMATIC lock-driven path, so
        // user-action phrasing ("Try again") would misdescribe an event the
        // user never triggered.
        emit userWarning("Automatic pause skipped: the system clock moved backward.");
        return;
    }

    // Activity segment: segment_start_time to activity_end
    if (activity_end > session_.segment_start_time) {
        addDuration(DurationType::Activity, session_.segment_start_time, activity_end, session_.segment_id);
    }

    // Pause segment: activity_end to now
    QDateTime pause_start = activity_end;
    addDuration(DurationType::Pause, pause_start, now);

    finalizePauseTransition(now);
    // C4 (finding 2): finalizePauseTransition() may have stopped the tracker
    // itself (a Fatal commit during the transition).
    if (mode_ == Mode::Pause) {
        Logger::Log("[TIMER] TimeTracker retroactively paused <");
        emitTransition(Mode::Activity, Mode::Pause, TransitionCause::LockAutopause);
    }
}


/**
 * Ends the current timing session.
 *
 * Workflow:
 * 1. Calculates the final segment duration (Activity or Pause).
 * 2. Adds it to the durations list.
 * 3. Stops the periodic checkpoint timer.
 * 4. Persists the Session to the Database using updateDurationsInDB().
 *    (This updates the existing rows rather than creating new ones, preserving IDs).
 */
void TimeTracker::stopTimer(const QDateTime& now, StopReason reason, std::optional<Mode> observedFromOverride)
{
    if (mode_ == Mode::None) {
        Logger::Log("[DEBUG] Stop ignored; already stopped");
        return;
    }
    // Normally the live mode_ IS what observers last saw. The override
    // exists only for the nested fatal-latch stop fired from inside a
    // transition that already mutated mode_ to the new value before
    // persisting/checking the latch (startTimer's Pause branch,
    // finalizePauseTransition) -- there, mode_ is already the not-yet-
    // announced new mode, so the caller supplies the mode it actually
    // announced last.
    const Mode fromMode = observedFromOverride.value_or(mode_);

    // A cross-midnight ongoing segment cannot be safely closed -- its
    // closing interval is discarded by addDuration() below (TimeDuration::
    // create's CrossMidnight rejection). Capture its id BEFORE any store
    // call so its unfinalized checkpoint row is durably discarded too
    // (A4/R05-001): otherwise startup recovery could resurrect
    // deliberately-discarded time.
    //
    // A backward wall-clock step (R11-003/HIGH-A) is different: only the
    // CLOSING delta [segment_start, now) is invalid (NonPositive, dropped by
    // addDuration() below same as always) -- any already-checkpointed prefix
    // of that same segment_id is a valid, positive, same-day, truthfully
    // recorded interval and must NOT be discarded with it. Deleting it via
    // the same A4 tombstone mechanism used for cross-midnight would destroy
    // real recorded work the engine never rejected. Recovery already
    // finalizes an orphan checkpoint that doesn't overlap finalized history
    // (B1/B2), so leaving it alone here is both lossless and truthful --
    // no clean-marker gate is needed either: the marker only feeds
    // notify_user, never orphan finalization eligibility.
    const bool clockReversed = isClockReversed(now, session_.segment_start_time);
    SegmentId discardedSegmentId;
    if (session_.segment_start_time.isValid()
        && isCrossMidnight(session_.segment_start_time, now)) {
        discardedSegmentId = session_.segment_id;
    }
    if (clockReversed) {
        Logger::Log(QString("[CLOCK] Stop: backward wall-clock step detected (now=%1 < segment_start=%2) — "
                             "dropping the closing interval only; any already-checkpointed prefix is left "
                             "for startup recovery to finalize")
            .arg(now.toString(Qt::ISODateWithMs), session_.segment_start_time.toString(Qt::ISODateWithMs)));
    }

    if (mode_ == Mode::Pause) {
        Logger::Log("[DEBUG] Stopping from Pause - D=" + QString::number(session_.durations.size()));
        addDuration(DurationType::Pause, session_.segment_start_time, now, session_.segment_id);
    } else if (mode_ == Mode::Activity) {
        Logger::Log("[DEBUG] Stopping from Activity - D=" + QString::number(session_.durations.size()));
        addDuration(DurationType::Activity, session_.segment_start_time, now, session_.segment_id);
    }
    mode_ = Mode::None;
    session_.clearSegment();
    checkpointTimer_.stop();
    autopause_pending_resume_ = false;
    Logger::Log("[TIMER] TimeTracker stopped <<");

    // Register the discard intent BEFORE any store call (including the
    // ordinary commit below), so the identity is captured even if this
    // stop's own persistence attempt runs first (A4/R05-001).
    if (!discardedSegmentId.isEmpty()) {
        pending_discards_.begin(discardedSegmentId);
    }

    // Persist current session durations only (not entire history). History dialog does replace explicitly.
    // Use update interface to check for existing entries by start time and update them instead of creating duplicates.
    // This also retries any pending discard intent(s), including the one just registered above.
    const bool committed = persistDurationsOrRetainForRetry();
    if (committed) {
        session_.resetForNewSession();
        Logger::Log("[DB] Session durations updated");
    }
    // D6 (R05-002/R15-004): stopTimer() no longer writes the clean-shutdown
    // marker itself. Marking clean is now exclusively the store-level
    // durable shutdown operation ShutdownCoordinator invokes once
    // TimeTracker::drainForShutdown() reports the whole drain (commit AND
    // any pending discard intent) resolved -- see sessionstore.h's
    // markCleanShutdown().

    emitTransition(fromMode, Mode::None, causeFromStopReason(reason));
}

bool TimeTracker::stopIfLatched(const QDateTime& now, std::optional<Mode> observedFromOverride)
{
    if (fatal_state_latched_) {
        stopTimer(now, StopReason::FatalStoreError, observedFromOverride);
        return true;
    }
    return false;
}

void TimeTracker::emitTransition(Mode from, Mode to, TransitionCause cause)
{
    emit transitioned(Transition{from, to, cause});

    // stopped() is kept as the one explicitly separate terminal event
    // (R10-002): every cause that reaches None derives it here, from this
    // same one emission point, rather than from a second call site.
    if (to == Mode::None) {
        emit stopped(stopReasonFromCause(cause));
    }
}

void TimeTracker::useTimerViaButton(Button button)
{
    QMutexLocker locker(&mutex_);
    const QDateTime now = now_provider_();
    if (discardCrossMidnightOngoingAndStop(now)) {
        // Ongoing segment crossed midnight — engine is now in None state.
        // Whatever the user asked for, discard it: per the day-boundary
        // policy the timer must remain stopped until the user starts
        // again manually.
#ifndef QT_NO_DEBUG
        checkDurationInvariants();
#endif
        return;
    }
    switch (button) {
        case Button::Start: startTimer(now); break;
        case Button::Pause: pauseTimer(now); break;
        case Button::Stop:  stopTimer(now, StopReason::ButtonStop); break;
    }
#ifndef QT_NO_DEBUG
    checkDurationInvariants();
#endif
}

// D10 (R10-001): production's toggle entry point no longer repeats
// useTimerViaButton()'s boundary guard/switch -- it just translates the
// toggle to the one typed input (Button) and dispatches through the single
// production/test button surface. mutex_ is a QRecursiveMutex so re-entering
// useTimerViaButton()'s own lock from the same thread is safe (the same
// pattern onStopPressed() already relied on).
void TimeTracker::onStartPausePressed()
{
    QMutexLocker locker(&mutex_);
    const Button button = (mode_ == Mode::Activity) ? Button::Pause : Button::Start;
    useTimerViaButton(button);
}

void TimeTracker::onStopPressed()
{
    useTimerViaButton(Button::Stop);
}

/**
 * Handles desktop lock/unlock events from LockStateWatcher.
 *
 * WP5: any real event first revokes an open History edit, so the rest of
 * this function -- the cross-midnight check and the event switch -- always
 * runs exactly as it would with no dialog open.
 *
 * Event handling:
 * - Lock: Saves final checkpoint, suspends further checkpoints via is_locked_
 * - LongOngoingLock: Triggers backpause if timer was in Activity mode
 * - Unlock: Resumes checkpoints, restarts timer if it was auto-paused
 *
 * Note: Lock/Unlock events for checkpoint control are always processed.
 * Autopause behavior (LongOngoingLock handling) respects isAutopauseEnabled().
 *
 * C12b: production's LockStateWatcher always stamps a real captured instant
 * on LongOngoingLock (see lockstatewatcher.cpp), so a LongOngoingLock
 * without a valid lockTime can only be a malformed external/test call. It
 * is rejected up front -- before the History revoke, the midnight discard,
 * or any autopause mutation -- so it has no effect at all.
 */
void TimeTracker::useTimerViaLockEvent(LockEvent event, const QDateTime& lockTime)
{
    QMutexLocker locker(&mutex_);
    if (event == LockEvent::LongOngoingLock && !lockTime.isValid()) {
        Logger::Log("[WARNING] Malformed LongOngoingLock event (no timestamp) ignored");
        return;
    }
    const QDateTime now = now_provider_();
    if (dialog_open_ && event != LockEvent::None) revokeExclusiveEdit();
    if (discardCrossMidnightOngoingAndStop(now)) {
        // Already-forced stopped — ignore the lock event for engine
        // purposes. autopause_pending_resume_ is already cleared inside
        // the helper so a subsequent Unlock does not restart.
#ifndef QT_NO_DEBUG
        checkDurationInvariants();
#endif
        return;
    }
    // D10/plan4 B16: each event kind owns its complete behavior. The dialog
    // gate that used to live in each case (WP5) is gone: the revoke above
    // already closed History before this switch runs, so only the
    // autopause gate remains here.
    switch (event) {
    case LockEvent::Lock:            onLockDetected(now); break;
    case LockEvent::Unlock:          onUnlockDetected(now); break;
    case LockEvent::LongOngoingLock: onLongOngoingLockDetected(now, lockTime); break;
    case LockEvent::None:            break;
    }
#ifndef QT_NO_DEBUG
    checkDurationInvariants();
#endif
}

void TimeTracker::onLockDetected(const QDateTime& now)
{
    is_locked_ = true;
    // Save a checkpoint when lock is detected (only if actively tracking time)
    if (mode_ == Mode::Activity) {
        saveCheckpoint_locked(now);
        Logger::Log("[LOCK] Desktop locked - checkpoint saved, further checkpoints suspended");
    } else {
        Logger::Log("[LOCK] Desktop locked - no checkpoint (timer not in Activity mode)");
    }
}

void TimeTracker::onUnlockDetected(const QDateTime& now)
{
    is_locked_ = false;
    Logger::Log("[LOCK] Desktop unlocked - checkpoint saving resumed");
    if (!settings_.isAutopauseEnabled()) {
        Logger::Log("[DEBUG] Autopause disabled; lock event ignored");
        return;
    }
    if (autopause_pending_resume_) {
        startTimer(now, TransitionCause::LockResume);
    }
    autopause_pending_resume_ = false;
}

void TimeTracker::onLongOngoingLockDetected(const QDateTime& now, const QDateTime& lockTime)
{
    if (!settings_.isAutopauseEnabled()) {
        Logger::Log("[DEBUG] Autopause disabled; lock event ignored");
        return;
    }
    autopause_pending_resume_ = (mode_ == Mode::Activity);
    if (autopause_pending_resume_) {
        backpauseTimer(now, lockTime);
    }
}

void TimeTracker::onTick(const QDateTime& now)
{
    QMutexLocker locker(&mutex_);
    day_boundary_watcher_.tick(now);
}

qint64 TimeTracker::getActiveTime() const
{
    // Totals come from the wall-clock timeline (completed + ongoing) so the
    // display stays consistent with persistence across a suspend; a monotonic
    // elapsed timer would freeze during suspend and no longer drive the
    // displayed total.
    return snapshot().activeMsec();
}

qint64 TimeTracker::getPauseTime() const
{
    return snapshot().pauseMsec();
}

Timeline TimeTracker::snapshot() const
{
    QMutexLocker lock(&mutex_);
    return Timeline(session_.durations, getOngoingDuration_locked());
}

Timeline TimeTracker::completedTimeline() const
{
    QMutexLocker locker(&mutex_);
    return Timeline(session_.durations, std::nullopt);
}

void TimeTracker::setDurationType(size_t idx, DurationType type)
{
    QMutexLocker locker(&mutex_);
    if (idx < session_.durations.size()) {
        session_.durations[idx].type = type;
        Logger::Log(QString("[TIMER] Duration type changed at index %1 to %2").arg(idx).arg(type == DurationType::Activity ? "Activity" : "Pause"));
    } else {
        Logger::Log(QString("[TIMER] Invalid index %1 for setDurationType (size %2)").arg(idx).arg(session_.durations.size()));
    }
#ifndef QT_NO_DEBUG
    checkDurationInvariants();
#endif
}

/**
 * Attempts to write a fresh checkpoint re-anchored to the given ongoing segment.
 *
 * No-op (returns true) when checkpoints are not applicable: not in Activity
 * mode, the segment is not an Activity segment, the segment has no positive
 * duration / invalid timestamps, or a dialog is open / the session is
 * locked. Disabled history is also treated as success. Returns
 * false only on a real checkpoint write failure (caller bug or DB error).
 *
 * C4 fix round 2 (MEDIUM-4 audit): a Fatal result here calls enterFatalState()
 * directly (below) but, like every latch site, does not stop the tracker
 * itself. This function's ONLY caller is commitEditedTimeline(), which
 * already checks the latch and stops (via stopIfLatched()) right after this
 * call returns -- so no separate post-check stop is needed here. (It is NOT
 * reached from the periodic checkpoint timer -- that path is
 * saveCheckpoint_locked(), which has its own post-check stop.)
 */
bool TimeTracker::maybeReanchorCheckpoint(const TimeDuration& seg)
{
    if (mode_ != Mode::Activity || seg.type != DurationType::Activity)
        return true;
    if (seg.duration() <= 0 || !seg.startTime.isValid() || !seg.endTime.isValid())
        return true;
    if (dialog_open_ || is_locked_)
        return true;

    const SessionStoreResult cpResult = db_.saveCheckpoint(seg.type,
                                                           seg.startTime,
                                                           seg.endTime,
                                                           session_.segment_id);
    if (cpResult.okOrDisabled()) {
        // Disabled history is a no-op, not a checkpoint write failure.
        return true;
    } else if (cpResult.category == SessionStoreResult::CallerBug) {
        Logger::Log("[CHECKPOINT] maybeReanchorCheckpoint: saveCheckpoint caller bug: " + cpResult.message);
        return false;
    } else if (cpResult.category == SessionStoreResult::FatalError) {
        Logger::Log("[CHECKPOINT] maybeReanchorCheckpoint: Fatal DB error saving checkpoint: " + cpResult.message);
        // C4 (R05-006): route through the one shared latch/dialog handler.
        enterFatalState(cpResult.message);
        return false;
    } else {
        Logger::Log("[CHECKPOINT] maybeReanchorCheckpoint: saveCheckpoint failed: " + cpResult.message);
        return false;
    }
}

/**
 * Atomically replaces the in-memory durations and re-anchors checkpoint tracking
 * from an edited Timeline.
 *
 * This is the sole external entry point for overwriting session_.durations from
 * outside TimeTracker (e.g., when HistoryDialog saves edits). It couples the two
 * operations that must always happen together:
 *   1. Replace the completed-segment deque (edited.completed()).
 *   2. Re-anchor checkpoint tracking so the next checkpoint targets the correct
 *      DB row and wall-clock range for the ongoing segment (edited.ongoing()).
 *
 * Without the coupling, a caller could replace durations but forget to reset
 * checkpoint tracking, causing stale segment IDs or start times to persist — a
 * bug that the compiler cannot catch if the operations are separate public methods.
 *
 * When edited.ongoing() is std::nullopt and the engine is running (mode_ != None),
 * the segment anchor is cleared, the checkpoint timer is stopped, mode_ is set to
 * None, and stopped(StopReason::EditApplied) is emitted.
 */
TimeTracker::EditCommitResult TimeTracker::commitEditedTimeline(const Timeline& edited)
{
    QMutexLocker locker(&mutex_);
    const std::deque<TimeDuration>& newDurations = edited.completed();
    const std::optional<TimeDuration>& ongoing = edited.ongoing();

    session_.durations = newDurations;
    pending_commit_.clear();

    bool checkpointOk = true;
    if (ongoing.has_value() && mode_ != Mode::None) {
        const TimeDuration& seg = ongoing.value();
        session_.adoptOngoingSegment(seg);

        // Align mode_ with the edited ongoing segment type so the live row and
        // the next checkpoint/stop reflect the user's choice rather than the
        // pre-edit mode. (adoptOngoingSegment intentionally ignores type.)
        const Mode desired = (seg.type == DurationType::Activity) ? Mode::Activity : Mode::Pause;
        if (mode_ != desired) {
            const Mode from = mode_;
            mode_ = desired;
            if (desired == Mode::Activity) {
                if (!is_locked_ && !dialog_open_) {
                    checkpointTimer_.start();   // checkpoints run only in Activity and only while not suspended
                }
            } else {
                checkpointTimer_.stop();
            }
            emitTransition(from, desired, TransitionCause::EditApplied);
        }

        checkpointOk = maybeReanchorCheckpoint(seg);
        // C4 (finding 2): a Fatal checkpoint write above latches the engine
        // but does NOT stop it (enterFatalState() only latches) -- stop here,
        // now that this commit's own state changes are done, using a freshly
        // captured `now` (this operation has no earlier timestamp of its own
        // to reuse).
        stopIfLatched(now_provider_());
    } else if (ongoing.has_value()) {
        // mode_ == None: the engine is stopped. Adopting the segment identity
        // here would set segment_id/segment_start_time while mode_ == None —
        // the state SessionState documents as impossible (segment_id is
        // "Empty when the timer is stopped"). Refuse the half-adopt: the
        // ongoing segment is ignored and the engine stays stopped.
        Logger::Log("[HISTORY] commitEditedTimeline: ignoring edited ongoing segment; engine is already stopped (mode_ == None)");
    } else if (mode_ != Mode::None) {
        // The edited timeline has no ongoing segment: the engine must stop.
        // Clear the anchor so no stale segment_id remains after the commit.
        const Mode from = mode_;
        session_.clearSegment();
        checkpointTimer_.stop();
        mode_ = Mode::None;
        emitTransition(from, Mode::None, TransitionCause::EditApplied);
    }

#ifndef QT_NO_DEBUG
    checkDurationInvariants();
#endif
    return checkpointOk ? EditCommitResult::success()
                        : EditCommitResult::failure("checkpoint write failed after edit commit");
}

void TimeTracker::replaceCurrentDurations(const std::deque<TimeDuration>& newDurations,
                                    const std::optional<TimeDuration>& ongoing)
{
    commitEditedTimeline(Timeline(newDurations, ongoing));
}

void TimeTracker::commit(const Timeline& edited)
{
    commitEditedTimeline(edited);
}

LoadResult TimeTracker::loadHistoryFromStore()
{
    QMutexLocker locker(&mutex_);
    return db_.loadDurations();
}

std::optional<TimeDuration> TimeTracker::getOngoingDuration() const
{
    QMutexLocker locker(&mutex_);
    return getOngoingDuration_locked();
}

std::optional<TimeDuration> TimeTracker::getOngoingDuration_locked() const
{
    if (mode_ == Mode::None) return std::nullopt;
    if (!session_.segment_start_time.isValid()) return std::nullopt;
    const QDateTime now = now_provider_();
    const DurationType type = (mode_ == Mode::Activity) ? DurationType::Activity : DurationType::Pause;
    // D2/LOW-E: isClockReversed() covers the strict backward-step case;
    // `now == segment_start_time` (zero elapsed, not a reversal) gets the
    // same zero-length-capped treatment here, so the two are combined.
    if (isClockReversed(now, session_.segment_start_time) || now == session_.segment_start_time) {
        // A8: the wall clock jumped backward (NTP/DST step-back), so the running
        // segment appears to start in the "future". The engine is NOT stopped —
        // expose a zero-length ongoing capped at the start rather than nullopt,
        // so a history-edit save re-anchors the live segment instead of ending
        // the session and dropping the in-flight time. (Cross-midnight, handled
        // below, stays hidden — the watchdog owns force-stopping it.)
        return TimeDuration::fromLiveSession(type, session_.segment_start_time,
                                             session_.segment_start_time, session_.segment_id);
    }
    if (isCrossMidnight(session_.segment_start_time, now)) {
        // Cross-midnight ongoing — about to be force-stopped by the
        // watchdog within ~100 ms. Don't expose this transient state.
        return std::nullopt;
    }
    return TimeDuration::fromLiveSession(type, session_.segment_start_time, now, session_.segment_id);
}

qint64 TimeTracker::getStartupRecoveredMsec() const
{
    QMutexLocker locker(&mutex_);
    return startup_recovered_msec_;
}

bool TimeTracker::shouldShowStartupRecoveryNotification() const
{
    QMutexLocker locker(&mutex_);
    return startup_recovery_notification_needed_;
}

int TimeTracker::getStartupPolicyDroppedCount() const
{
    QMutexLocker locker(&mutex_);
    return startup_policy_dropped_count_;
}

bool TimeTracker::isFatalLatched() const
{
    QMutexLocker locker(&mutex_);
    return fatal_state_latched_;
}

void TimeTracker::cancelShutdown()
{
    QMutexLocker locker(&mutex_);
    shutdown_called_ = false;
}

TimeTracker::ShutdownResult TimeTracker::shutdown()
{
    QMutexLocker locker(&mutex_);
    if (dialog_open_) revokeExclusiveEdit();
    shutdown_called_ = true;
    ++shutdown_epoch_;
    if (mode_ != Mode::None) {
        stopTimer(now_provider_(), StopReason::Shutdown);
    }
    return {mode_ == Mode::None};
}

DrainResult TimeTracker::drainForShutdown()
{
    QMutexLocker locker(&mutex_);
    // Close any live segment first, using the one "now" captured inside
    // shutdown(). No-op if already None -- this is what makes a repeated
    // call idempotent on the close-segment half. D6: the clean-shutdown
    // marker is no longer written here or by stopTimer() -- it is written
    // exclusively by the store-level durable shutdown operation
    // ShutdownCoordinator invokes once this whole drain reports clean.
    shutdown();

    if (pending_commit_.hasPending()) {
        Logger::Log("[DB] Drain: retrying pending commit at shutdown");
        const SessionStoreResult retryResult = appendDurationsChunkToDB(pending_commit_.payload());
        const bool ok = retryResult.okOrDisabled();
        pending_commit_.retry(ok);
        if (!ok) {
            Logger::Log("[DB] Drain: pending commit still unresolved: " + retryResult.message);
            return DrainResult::incomplete(retryResult.category, retryResult.message);
        }
        Logger::Log("[DB] Drain: pending commit saved successfully");
    }

    // Retry any pending checkpoint-discard intent(s) (A4/R05-001). An
    // unresolved intent must block clean marking exactly like a pending
    // commit: a crash before it is durably resolved could let startup
    // recovery resurrect a segment the engine already deliberately rejected.
    retryPendingDiscards();
    if (pending_discards_.hasPending()) {
        Logger::Log("[DB] Drain: pending checkpoint discard(s) still unresolved");
        return DrainResult::incomplete(SessionStoreResult::TransientError,
                                        "A previously discarded checkpoint could not be removed from storage");
    }

    return DrainResult::complete();
}

bool TimeTracker::appendDurationsToDB()
{
    // Disabled history is a no-op, not a failure.
    return appendDurationsChunkToDB(session_.durations).okOrDisabled();
}

void TimeTracker::logDbResultOrWarn(const SessionStoreResult& result, const QString& context)
{
    if (result.category == SessionStoreResult::CallerBug) {
        Logger::Log("[DB] CRITICAL: " + context + " caller bug: " + result.message);
    } else if (result.category == SessionStoreResult::FatalError) {
        Logger::Log("[DB] CRITICAL: " + context + " fatal error: " + result.message);
        // C4 (R05-006): FatalError is handled ONCE, regardless of which
        // operation returned it -- see enterFatalState().
        enterFatalState(result.message);
    } else if (result.category == SessionStoreResult::TransientError) {
        Logger::Log("[DB] " + context + " transient error: " + result.message);
    }
    // Success: nothing to log/emit
}

void TimeTracker::enterFatalState(const QString& message)
{
    if (fatal_state_latched_) {
        // Later failures never repeat the dialog (C4/R05-006).
        Logger::Log("[DB] Fatal store error (already latched, no further dialog): " + message);
        return;
    }
    fatal_state_latched_ = true;
    Logger::Log("[DB] CRITICAL: entering fatal unavailable state (Start disabled until restart): " + message);
    // C4 fix round 2 (HIGH-1): fatalError, not userWarning -- this is the ONE
    // modal error C4/R05-006 requires; userWarning only reaches a tray
    // balloon/status-bar message, which is easily missed.
    emit fatalError("Database error: " + message);
    // C4 (finding 2): deliberately does NOT stop the tracker here. This can
    // run deep inside an in-progress transition (startTimer,
    // finalizePauseTransition, commitEditedTimeline, ...) that keeps
    // executing after logDbResultOrWarn()/this call returns; stopping mid-
    // transition desyncs mode_/session_ from whatever the transition does
    // next (e.g. emits started() after mode_ was already forced to None).
    // Each transition owner checks fatal_state_latched_ after its own
    // persistence call and stops itself once its own state changes are done.
}

SessionStoreResult TimeTracker::appendDurationsChunkToDB(const std::deque<TimeDuration>& durations)
{
    if (durations.empty())
        return SessionStoreResult::success();
    const SessionStoreResult result = db_.commitSession(Timeline(durations, std::nullopt));
    logDbResultOrWarn(result, "commitSession");
    return result;
}

SessionStoreResult TimeTracker::updateDurationsInDB()
{
    if (session_.durations.empty()) {
        return SessionStoreResult::success();
    }
    const SessionStoreResult result = db_.commitSession(Timeline(session_.durations, std::nullopt));
    logDbResultOrWarn(result, "commitSession");
    // Disabled history is a no-op, not a save failure: treat it as success so
    // we don't retain bogus unsaved data or skip the clean-shutdown marker.
#ifndef QT_NO_DEBUG
    if (result.okOrDisabled()) {
        checkCrossLayerInvariants(result);
    }
#endif
    return result;
}

bool TimeTracker::persistDurationsOrRetainForRetry()
{
    const SessionStoreResult result = updateDurationsInDB();
    // C4 fix round 2 (HIGH-2): CallerBug is NOT special-cased here. It falls
    // through to the normal okOrDisabled() path below like any other
    // failure -- the payload is retained for retry and surfaced through the
    // ordinary Retry/Exit-Anyway drain dialog (logDbResultOrWarn() above
    // already logged it CRITICAL). Round-1 dropped the payload and reported
    // success here, which propagated into stopTimer() as a false "committed"
    // and let it write the clean-shutdown marker for data that was never
    // actually persisted.
    //
    // C4 fix round 3 (F6): unlike a transient failure, CallerBug is
    // deterministic -- retrying the identical retained payload can never
    // succeed. In practice this means a CallerBug commit leaves Start
    // refused (retryUnsavedDurations() keeps failing the same way) and the
    // clean-shutdown marker unwritten for the rest of the process; only
    // Exit Anyway on the shutdown dialog can terminate. That is the accepted
    // trade against round-1's silent data loss, not a bug.
    const bool ok = result.okOrDisabled();
    pending_commit_.submit(ok, session_.durations);
    if (!ok) {
        Logger::Log("[DB] Session durations not persisted - retained for retry");
    }
    // A normal persistence opportunity is also a normal discard-retry
    // opportunity (A4/R05-001).
    retryPendingDiscards();
    return ok;
}

/**
 * Retries any durable-discard intents (A4/R05-001) against the store. Each
 * attempt is a normal discardCheckpoint() call; the store makes both halves
 * of that call idempotent (re-recording an already-recorded tombstone and
 * re-deleting an already-deleted row are both safe no-ops), so calling this
 * repeatedly cannot double-discard or resurrect anything.
 *
 * Success or Disabled resolves the intent. Every other category --
 * TransientError, FatalError, and CallerBug alike (C4 fix round 2 /
 * MEDIUM-HIGH-3) -- retains it for the next opportunity. FatalError also
 * latches the one tracker-unavailable state (C4); CallerBug is logged
 * CRITICAL by logDbResultOrWarn() above but is not treated as terminal --
 * dropping it would silently unblock clean marking for a checkpoint row
 * that was never actually removed from storage.
 *
 * C4 fix round 3 (F5): the FatalError branch above is defensive interface
 * handling for the SessionStore contract, not a live production path --
 * SqliteSessionStore::discardCheckpoint() no longer returns FatalError post-C4
 * (see sessionstore.h's discardCheckpoint doc); it is exercised here only via
 * FakeSessionStore in tests.
 */
void TimeTracker::retryPendingDiscards()
{
    if (!pending_discards_.hasPending()) return;
    // Copy: resolve() mutates the list pending_discards_.pending() refers to.
    for (const SegmentId& id : std::vector<SegmentId>(pending_discards_.pending())) {
        const SessionStoreResult r = db_.discardCheckpoint(id);
        logDbResultOrWarn(r, "discardCheckpoint");
        if (r.okOrDisabled()) {
            pending_discards_.resolve(id);
        }
    }
}

SessionStoreResult TimeTracker::replaceAll(const Timeline& history, const Timeline& session)
{
    QMutexLocker locker(&mutex_);
    const SessionStoreResult result = db_.replaceAll(history, session);
    // C4 (finding 1): route through the one shared latch/dialog handler --
    // previously a Fatal from replaceAll reached the caller (HistoryDialog)
    // directly and never latched fatal_state_latched_ at all.
    logDbResultOrWarn(result, "replaceAll");
    if (result.okOrDisabled()) {
        // DB is now authoritative; a stale pending commit must not survive past here.
        pending_commit_.replaceSucceeded();
    }
    // C4 (finding 2): a Fatal result above latches the engine but does NOT
    // stop it (enterFatalState() only latches) -- stop here, using a freshly
    // captured `now` (this operation has no earlier timestamp of its own).
    // stopTimer() itself is a no-op when already in Mode::None, so the
    // explicit mode_ check the pre-LOW-8 code had here was redundant.
    stopIfLatched(now_provider_());
    return result;
}

EntriesForDateResult TimeTracker::hasEntriesForDate(const QDate& date)
{
    return db_.hasEntriesForDate(date);
}

bool TimeTracker::isClockReversed(const QDateTime& now, const QDateTime& referenceStart)
{
    return referenceStart.isValid() && now < referenceStart;
}

/**
 * Appends a single same-day segment to session_.durations.
 *
 * DayBoundaryWatcher's scheduled stop and watchdog prevent cross-midnight
 * inputs from reaching here; this discard is a last-line defence.  Zero-
 * and negative-duration inputs (e.g. start == end) are also dropped.
 * Each rejection is logged with the specific reason.
 */
void TimeTracker::addDuration(DurationType type,
                              const QDateTime& startTime,
                              const QDateTime& endTime,
                              SegmentId segmentId)
{
    auto seg = TimeDuration::create(type, startTime, endTime, segmentId);
    if (!seg.has_value()) {
        switch (seg.error) {
        case DurationCreateError::InvalidTimestamp:
            Logger::Log(QString("[ADDURATION] Discarding segment with invalid timestamp: %1 → %2")
                .arg(startTime.toString(Qt::ISODateWithMs))
                .arg(endTime.toString(Qt::ISODateWithMs)));
            break;
        case DurationCreateError::NonPositive:
            Logger::Log(QString("[ADDURATION] Discarding zero/negative-duration segment: %1 → %2")
                .arg(startTime.toString(Qt::ISODateWithMs))
                .arg(endTime.toString(Qt::ISODateWithMs)));
            break;
        case DurationCreateError::CrossMidnight:
            Logger::Log(QString("[MIDNIGHT] Discarding cross-midnight segment: %1 → %2")
                .arg(startTime.toString(Qt::ISODateWithMs))
                .arg(endTime.toString(Qt::ISODateWithMs)));
            break;
        default:
            break;
        }
        return;
    }
    const qint64 dur = seg->duration();
    session_.durations.emplace_back(std::move(*seg));
    Logger::Log(QString("[DEBUG] Added duration (%1, %2 ms)")
        .arg(type == DurationType::Activity ? "Activity" : "Pause")
        .arg(dur));
}

bool TimeTracker::isOngoingSegmentCrossMidnight() const
{
    QMutexLocker locker(&mutex_);
    if (mode_ == Mode::None) return false;
    if (!session_.segment_start_time.isValid()) return false;
    return isCrossMidnight(session_.segment_start_time, now_provider_());
}

/**
 * Forces the engine into the stopped state when the ongoing segment has
 * crossed midnight, discarding the in-flight segment.
 *
 * If a History edit is open, first revokes it (revokeExclusiveEdit()),
 * then falls through: the discard below runs exactly as it would with no
 * dialog having intervened.
 *
 * Returns true when it fired (telling the caller to abandon any pending
 * transition). Safe to call multiple times; only the first call does real work.
 *
 * Emits stopped(MidnightWatchdog). The GUI is updated via the stopped() signal
 * connection in MainWin.
 */
bool TimeTracker::discardCrossMidnightOngoingAndStop(const QDateTime& now)
{
    if (mode_ == Mode::None) return false;
    if (!session_.segment_start_time.isValid()) return false;
    if (!isCrossMidnight(session_.segment_start_time, now)) return false;

    if (dialog_open_) revokeExclusiveEdit();

    Logger::Log(QString("[MIDNIGHT] Cross-midnight ongoing detected "
                        "(segment_start=%1, now=%2) — discarding segment, forcing stop")
        .arg(session_.segment_start_time.toString(Qt::ISODateWithMs))
        .arg(now.toString(Qt::ISODateWithMs)));

    // Best-effort: persist any already-completed (same-day) segments that
    // accumulated before midnight. Only the ONGOING segment is discarded.
    if (!session_.durations.empty()) {
        if (persistDurationsOrRetainForRetry()) {
            session_.resetForNewSession();
        }
    }

    // Reuse stopTimer for teardown. The cross-midnight ongoing segment is
    // discarded by addDuration (TimeDuration::create returns CrossMidnight).
    // durations is already empty (success path) or retained in unsaved buffer
    // (flush-failure path), so the updateDurationsInDB call inside stopTimer is a
    // no-op on success and a harmless retry on flush failure. stopTimer() also
    // owns deleting the discarded segment's checkpoint row (it detects the
    // same cross-midnight condition itself) — one owner, not two.
    stopTimer(now, StopReason::MidnightWatchdog);
    return true;
}

void TimeTracker::saveCheckpoint()
{
    QMutexLocker locker(&mutex_);

    // Don't save checkpoints while desktop is locked
    if (is_locked_) {
        Logger::Log("[CHECKPOINT] Skipped - desktop is locked");
        return;
    }

    // Don't save checkpoints while HistoryDialog is open
    if (dialog_open_) {
        Logger::Log("[CHECKPOINT] Skipped - dialog open");
        return;
    }

    // Capture wall-clock time once for the entire checkpoint operation.
    const QDateTime now = now_provider_();
    saveCheckpoint_locked(now);
}

void TimeTracker::saveCheckpoint_locked(const QDateTime& now)
{
    // NOTE: This function assumes the mutex is already held by the caller

    if (mode_ != Mode::Activity)        return;
    if (discardCrossMidnightOngoingAndStop(now)) return;

    // Use wall-clock elapsed rather than a monotonic elapsed timer to stay
    // correct across suspend. D2/LOW-E: routed through isClockReversed() for the
    // backward-step case explicitly; `wallClockMs == 0` (no time has passed
    // yet, not a reversal) is checked alongside it. Either way there is
    // nothing to save, no state is mutated, and the next tick (or a
    // Stop/Pause once wall time is sane) resolves it.
    const qint64 wallClockMs = session_.segment_start_time.msecsTo(now);
    if (isClockReversed(now, session_.segment_start_time) || wallClockMs == 0) {
        return;
    }

    // Always Activity - we return early above if mode_ != Mode::Activity
    DurationType type = DurationType::Activity;

    if (session_.segment_id.isEmpty()) {
        session_.beginNewSegment(session_.segment_start_time);
    }
    const SessionStoreResult result = db_.saveCheckpoint(type, session_.segment_start_time, now, session_.segment_id);

    if (result.ok()) {
        Logger::Log(QString("[CHECKPOINT] Saved checkpoint - Type: %1, Duration: %2ms, SegmentId: %3")
            .arg(type == DurationType::Activity ? "Activity" : "Pause")
            .arg(wallClockMs)
            .arg(session_.segment_id.toString()));
    } else {
        logDbResultOrWarn(result, "saveCheckpoint");
        // C4 (finding 2): a Fatal checkpoint write latches the engine but
        // does NOT stop it (enterFatalState() only latches) -- stop here.
        // This is already the last thing this function does, so it is a
        // safe point: nothing further reads mode_/session_ afterward.
        stopIfLatched(now);
    }
}

void TimeTracker::beginExclusiveEdit()
{
    QMutexLocker locker(&mutex_);
    dialog_open_ = true;
    exclusive_edit_date_ = now_provider_().toLocalTime().date();
    checkpointTimer_.stop();
    Logger::Log("[CHECKPOINT] Checkpoints paused (dialog open)");
}

/**
 * WP2: force-closes an open History edit so the engine can run its normal,
 * no-dialog code for whatever event is intervening (lock-watcher events,
 * forward date change, midnight, or shutdown). Called with mutex_ already held.
 * exclusiveEditRevoked() reaches HistoryDialog synchronously so it can flag
 * the refusal before the dialog is actually closed on the next event-loop
 * turn; endExclusiveEdit() then clears dialog_open_ so the caller's own
 * no-dialog code runs unimpeded.
 */
void TimeTracker::revokeExclusiveEdit()
{
    Logger::Log("[HISTORY-EDIT] revoked");
    emit exclusiveEditRevoked();
    endExclusiveEdit();
    emit userWarning(kHistoryRevokedNotice);
}

void TimeTracker::endExclusiveEdit()
{
    QMutexLocker locker(&mutex_);
    dialog_open_ = false;
    Logger::Log("[CHECKPOINT] Checkpoints resumed (dialog closed)");
    if (mode_ == Mode::Activity && !is_locked_) {
        checkpointTimer_.start();
    }
}

