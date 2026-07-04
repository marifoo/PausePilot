/**
 * Timer — Core timing engine managing Activity/Pause/None states.
 *
 * Architecture:
 * - State machine with three modes: Activity, Pause, None.
 * - Completed segments stored in session_.durations (deque); ongoing
 *   segment tracked by timer_.elapsed() and session_.segment_start_time.
 * - Checkpoints persist the ongoing Activity segment every 5 min.
 * - Thread-safe via QRecursiveMutex; per-session mutable state grouped
 *   in SessionState with explicit logged transitions.
 *
 * Day-boundary policy:
 * - Timer NEVER stores or persists a segment whose
 *   startTime.date() != endTime.date(). Such segments are silently
 *   discarded by addDuration() and refused by saveCheckpointInternal().
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
 */

#include "timer.h"
#include <QtDebug>
#include <QDateTime>
#include <QMutexLocker>
#include <new>  // for std::bad_alloc
#include "logger.h"

// ============================================================================
// DayBoundaryWatcher implementation
// ============================================================================

Timer::DayBoundaryWatcher::DayBoundaryWatcher(Timer& owner)
    : owner_(owner)
{
    midnight_timer_.setSingleShot(true);
}

void Timer::DayBoundaryWatcher::tick(const QDateTime& now)
{
    // Watchdog: if the ongoing segment has crossed midnight (e.g. due to sleep
    // bypassing the scheduled stop), force the engine to stopped state.
    // The mutex is already held by the caller (Timer::onTick).
    if (owner_.dialog_open_) {
        return;
    }
    owner_.discardCrossMidnightOngoingAndStop(now);
}

void Timer::DayBoundaryWatcher::armScheduledStop(const QDateTime& now)
{
    QTime current_time = now.time();
    QTime midnight_stop_time(23, 59, 59, 500);

    qint64 msecs_until_stop;
    if (current_time < midnight_stop_time) {
        msecs_until_stop = current_time.msecsTo(midnight_stop_time);
    } else {
        // Already past 23:59:59.500 — fire in 1 ms to trigger the watchdog path.
        msecs_until_stop = 1;
    }

    Logger::Log(QString("[MIDNIGHT] Scheduled engine auto-stop in %1 seconds")
        .arg(msecs_until_stop / 1000.0, 0, 'f', 1));

    midnight_timer_.disconnect();
    QObject::connect(&midnight_timer_, &QTimer::timeout,
                     [this]() { onMidnightTimerFired(); });
    midnight_timer_.start(static_cast<int>(msecs_until_stop));
}

void Timer::DayBoundaryWatcher::cancel()
{
    midnight_timer_.stop();
    Logger::Log("[MIDNIGHT] Scheduled stop timer cancelled");
}

void Timer::DayBoundaryWatcher::onMidnightTimerFired()
{
    Logger::Log("[MIDNIGHT] Scheduled stop: engine forcing timer off at end of day");
    QMutexLocker locker(&owner_.mutex_);
    if (owner_.mode_ == Mode::None) {
        Logger::Log("[MIDNIGHT] Scheduled stop fired but engine already stopped - ignoring");
        return;
    }
    if (owner_.dialog_open_) {
        owner_.pending_midnight_stop_ = true;
        Logger::Log("[MIDNIGHT] suspended during history edit");
        return;
    }
    owner_.stopTimer(owner_.now_provider_(), StopReason::MidnightScheduled);
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

void SessionState::markUnsaved()
{
    if (!has_unsaved_data) {
        has_unsaved_data = true;
        Logger::Log("[STATE] markUnsaved: false -> true");
    }
}

void SessionState::clearUnsaved()
{
    bool old = has_unsaved_data;
    has_unsaved_data = false;
    unsaved_durations.clear();
    if (old) {
        Logger::Log("[STATE] clearUnsaved: true -> false");
    }
}

void SessionState::resetForNewSession()
{
    size_t oldSize = durations.size();
    bool oldUnsaved = has_unsaved_data;
    durations.clear();
    has_unsaved_data = false;
    unsaved_durations.clear();
    Logger::Log(QString("[STATE] resetForNewSession: durations %1 -> 0, unsaved %2 -> false")
        .arg(oldSize).arg(oldUnsaved ? "true" : "false"));
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

Timer::StateSnapshot Timer::takeStateSnapshot() const
{
    return {
        session_.durations.size(),
        session_.segment_id,
        session_.segment_start_time,
        session_.has_unsaved_data,
        mode_
    };
}

Timer::StateGuard::StateGuard(const Timer& tracker, const char* methodName)
    : tracker_(tracker), method_(methodName), entry_(tracker.takeStateSnapshot())
{
}

Timer::StateGuard::~StateGuard()
{
    if (transitioned_) {
        return; // Caller acknowledged the mutation
    }
    auto exit = tracker_.takeStateSnapshot();
    bool changed = (exit.durations_size != entry_.durations_size)
                || (exit.segment_id != entry_.segment_id)
                || (exit.segment_start_time != entry_.segment_start_time)
                || (exit.has_unsaved_data != entry_.has_unsaved_data)
                || (exit.mode != entry_.mode);
    if (changed) {
        qWarning("[STATE-GUARD] Unacknowledged state mutation in %s: "
                 "durations %zu->%zu, segId '%s'->'%s', unsaved %d->%d, mode %d->%d",
                 method_,
                 entry_.durations_size, exit.durations_size,
                 qPrintable(entry_.segment_id.toString()), qPrintable(exit.segment_id.toString()),
                 entry_.has_unsaved_data, exit.has_unsaved_data,
                 static_cast<int>(entry_.mode), static_cast<int>(exit.mode));
        Q_ASSERT_X(false, method_,
                    "SessionState changed without explicit transition call");
    }
}

void Timer::StateGuard::markTransitioned()
{
    transitioned_ = true;
}

/**
 * Checks structural invariants on the in-memory duration segments.
 *
 * This runs after every public method in debug builds. It catches
 * data-structure corruption early — before it propagates to the DB
 * or produces confusing UI behavior. Violations are logged as warnings
 * (not fatal) so tests surface them as QWARN output.
 *
 * Invariants checked:
 *   1. Segment ordering: startTimes must be non-decreasing.
 *   2. No overlapping segments of the same type on the same day.
 *   3. All segment_ids must be non-empty.
 */
void Timer::checkDurationInvariants() const
{
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
            && !isCrossMidnight(durations[i].startTime, durations[i - 1].startTime)
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
 * either no row (not yet committed) or exactly one finalized row with a
 * matching segment_id. Called after every successful commitSession() in
 * updateDurationsInDB() to catch divergence between memory and storage.
 */
void Timer::checkCrossLayerInvariants() const
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
        }
    }
}

#endif // QT_NO_DEBUG

// ============================================================================
// Timer implementation
// ============================================================================

Timer::Timer(const Settings &settings, SessionStore& db, QObject *parent)
    : QObject(parent), settings_(settings), now_provider_(QDateTime::currentDateTime),
      timer_(), session_(), mode_(Mode::None),
      autopause_pending_resume_(false), is_locked_(false),
      dialog_open_(false), db_(db),
      checkpoint_interval_msec_(settings.getCheckpointIntervalMsec()),
      last_history_load_skipped_(0), last_history_load_repaired_(0),
      startup_recovered_seconds_(0), startup_recovery_notification_needed_(false),
      day_boundary_watcher_(*this)
{
    // Setup checkpoint timer (disabled if interval is 0)
    if (checkpoint_interval_msec_ > 0) {
        checkpointTimer_.setInterval(checkpoint_interval_msec_);
    }
    checkpointTimer_.setSingleShot(false);
    connect(&checkpointTimer_, &QTimer::timeout, this, &Timer::saveCheckpoint);

    // Log when checkpoints are disabled
    if (checkpoint_interval_msec_ == 0) {
        Logger::Log("[CHECKPOINT] Checkpoints disabled (interval = 0)");
    }

    // No persistence side effects in the constructor.
    // Call initializeFromStore() after checkSchemaOnStartup() returns Ready/Created.
}

void Timer::initializeFromStore()
{
    const StartupRecoveryResult recovery = db_.recoverStartupCheckpoints(now_provider_());
    if (recovery.ok) {
        startup_recovered_seconds_ = recovery.recovered_seconds;
        startup_recovery_notification_needed_ = recovery.notify_user;
    }
}

Timer::~Timer()
{
    // T8: Stop and disconnect checkpointTimer_ before acquiring the mutex so
    // that no in-flight slot can fire against a partially-destroyed object.
    checkpointTimer_.stop();
    checkpointTimer_.disconnect();
    QMutexLocker locker(&mutex_);
    stopTimer(now_provider_(), StopReason::Shutdown);
}

/**
 * Retries saving previously unsaved durations at the start of a new session.
 *
 * Three outcomes:
 * - Success: clears unsaved state, returns true (startTimer continues)
 * - FatalError/CallerBug: returns false (startTimer aborts)
 * - TransientError: increments retry counter; returns false if max retries
 *   reached (abort with warning), returns true otherwise (warn-and-continue)
 */
bool Timer::retryUnsavedDurations()
{
    // Use unsaved_durations if present to keep retries idempotent.
    const std::deque<TimeDuration>& retry_source = session_.unsaved_durations.empty() ? session_.durations : session_.unsaved_durations;
    if (!session_.has_unsaved_data || retry_source.empty()) {
        return true;
    }

    Logger::Log("[DB] Retrying save of previously unsaved durations");
    const SessionStoreResult retryResult = appendDurationsChunkToDB(retry_source);
    if (retryResult.okOrDisabled()) {
        // Disabled history is a no-op, not a retry failure.
        session_.clearUnsaved();
        session_.consecutive_retry_failures = 0;
        Logger::Log("[DB] Previously unsaved durations saved successfully");
        return true;
    }
    if (retryResult.category == SessionStoreResult::FatalError
        || retryResult.category == SessionStoreResult::CallerBug) {
        // Already handled by logDbResultOrWarn inside appendDurationsChunkToDB
        if (retryResult.category == SessionStoreResult::FatalError)
            session_.consecutive_retry_failures = 3;
        return false;
    }
    // TransientError
    session_.consecutive_retry_failures++;
    Logger::Log(QString("[DB] CRITICAL: Retry save failed (consecutive=%1) - unsaved data retained")
        .arg(session_.consecutive_retry_failures));
    if (session_.consecutive_retry_failures >= 3) {
        emit userWarning("Repeated save failures (" + QString::number(session_.consecutive_retry_failures) +
                         " consecutive). Please check your storage, then restart the application to retry.");
        return false;
    }
    emit userWarning("Could not save previous session data. It is kept in memory and will be retried.");
    return true;
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
void Timer::startTimer(const QDateTime& now)
{
    // T10: Clear autopause flag at the top of startTimer so both Pause→Activity
    // and None→Activity paths start with a clean state.
    autopause_pending_resume_ = false;

    if (mode_ == Mode::Pause) {
        Logger::Log("[DEBUG] Starting Timer from Pause - D=" + QString::number(session_.durations.size()));
        timer_.restart();

        // T1+T9: Record the completed Pause segment with its own fresh segment_id
        // (current_checkpoint_segment_id was assigned when transitioning to Pause mode).
        // Do NOT reuse or mutate any prior Activity segment_id — that is the
        // root bug T1 fixed. Timeline::normalized() handles merging adjacent
        // same-type segments at DB-sync time.
        addDuration(DurationType::Pause, session_.segment_start_time, now, session_.segment_id);

        mode_ = Mode::Activity;
        session_.beginNewSegment(now);

        // Persist the completed Pause row immediately so it survives a crash.
        // Without this, a crash before the next pauseTimer/stopTimer would lose
        // the Pause entry, making the reloaded timeline show two Activities
        // back-to-back with no gap. The row is written as finalized (is_finalized=1)
        // because the Pause segment is complete the moment the user un-pauses.
        updateDurationsInDB();

        if (checkpoint_interval_msec_ > 0) {
            checkpointTimer_.start(); // Resume periodic checkpoint saving
        }
        day_boundary_watcher_.armScheduledStop(now);
        Logger::Log("[TIMER] > Timer unpaused");
        emit started(true);
        return;
    }
    if (mode_ == Mode::None) {
        Logger::Log("[DEBUG] Starting Timer from Stopped - D=" + QString::number(session_.durations.size()));

        if (!retryUnsavedDurations()) return;
        const bool retry_succeeded = !session_.has_unsaved_data; // has_unsaved_data cleared by clearUnsaved on success; true means retry was needed but failed

        unsigned int boot_time_sec = settings_.getBootTimeSec();
        bool shouldAddBootTime = false;
        if (boot_time_sec > 0) {
            QDate today = now.date();
            bool hasEntriesInMemory = false;
            for (const auto &d : session_.durations) {
                if (d.startTime.date() == today) {
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
        if (retry_succeeded) {
            session_.resetForNewSession();
        }
        if (shouldAddBootTime) {
            QDateTime bootStart = now.addMSecs(-static_cast<qint64>(boot_time_sec) * 1000);
            addDuration(DurationType::Activity, bootStart, now);
        }
        timer_.start();
        mode_ = Mode::Activity;
        session_.beginNewSegment(now);
        if (checkpoint_interval_msec_ > 0) {
            checkpointTimer_.start(); // Start periodic checkpoint saving
        }
        day_boundary_watcher_.armScheduledStop(now);
        Logger::Log("[TIMER] >> Timer started");
        shutdown_called_ = false;
        emit started(false);
        return;
    }
    Logger::Log("[DEBUG] Trying to Start Timer from Mode Activity - D=" + QString::number(session_.durations.size()));
}

void Timer::pauseTimer(const QDateTime& now)
{
    if (mode_ != Mode::Activity) {
        Logger::Log("[DEBUG] Pause ignored; not in Activity");
        return;
    }
    Logger::Log("[DEBUG] Pausing Timer from Activity - D=" + QString::number(session_.durations.size()));
    timer_.restart();
    addDuration(DurationType::Activity, session_.segment_start_time, now, session_.segment_id);

    finalizePauseTransition(now);
    Logger::Log("[TIMER] Timer paused <");
    emit paused();
}

void Timer::finalizePauseTransition(const QDateTime& now)
{
    mode_ = Mode::Pause;
    session_.updateSegmentStartTime(now);
    updateDurationsInDB();
    session_.beginNewSegment(now);
    checkpointTimer_.stop();
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
void Timer::backpauseTimer(const QDateTime& now, const QDateTime& lockTime)
{
    if (mode_ != Mode::Activity) {
        Logger::Log("[DEBUG] Backpause ignored; not in Activity");
        return;
    }
    if (!settings_.isAutopauseEnabled()) {
        Logger::Log("[DEBUG] Autopause disabled; backpause ignored");
        return;
    }
    Logger::Log("[DEBUG] Backpausing Timer from Activity - D=" + QString::number(session_.durations.size()));
    qint64 backpause_msec = settings_.getBackpauseMsec();

    // Bounds check: backpause should be between 1 second and 1 hour
    constexpr qint64 MIN_BACKPAUSE_MS = 1000;      // 1 second minimum
    constexpr qint64 MAX_BACKPAUSE_MS = 3600000;   // 1 hour maximum
    if (backpause_msec < MIN_BACKPAUSE_MS) {
        Logger::Log(QString("[WARNING] Backpause value %1ms below minimum, using %2ms")
            .arg(backpause_msec).arg(MIN_BACKPAUSE_MS));
        backpause_msec = MIN_BACKPAUSE_MS;
    } else if (backpause_msec > MAX_BACKPAUSE_MS) {
        Logger::Log(QString("[WARNING] Backpause value %1ms exceeds maximum, using %2ms")
            .arg(backpause_msec).arg(MAX_BACKPAUSE_MS));
        backpause_msec = MAX_BACKPAUSE_MS;
    }

    timer_.restart();

    // Calculate Activity end time (backpause boundary). Anchor the pause at the
    // real wall-clock lock instant when it is known: the LongOngoingLock event
    // may arrive long after the lock (e.g. after a suspend), so now−backpause
    // would credit the suspended interval as Activity. Fall back to now−backpause
    // only when no lock time was plumbed through (e.g. deferred replay).
    QDateTime activity_end = lockTime.isValid() ? lockTime : now.addMSecs(-backpause_msec);

    // Ensure activity_end is not before segment_start_time
    if (activity_end < session_.segment_start_time) {
        activity_end = session_.segment_start_time;
    }

    // Activity segment: segment_start_time to activity_end
    if (activity_end > session_.segment_start_time) {
        addDuration(DurationType::Activity, session_.segment_start_time, activity_end, session_.segment_id);
    }

    // Pause segment: activity_end to now
    QDateTime pause_start = activity_end;
    addDuration(DurationType::Pause, pause_start, now);

    finalizePauseTransition(now);
    Logger::Log("[TIMER] Timer retroactively paused <");
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
void Timer::stopTimer(const QDateTime& now, StopReason reason)
{
    if (mode_ == Mode::None) {
        Logger::Log("[DEBUG] Stop ignored; already stopped");
        return;
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
    day_boundary_watcher_.cancel();
    autopause_pending_resume_ = false;
    Logger::Log("[TIMER] Timer stopped <<");

    // Persist current session durations only (not entire history). History dialog does replace explicitly.
    // Use update interface to check for existing entries by start time and update them instead of creating duplicates
    if (updateDurationsInDB()) {
        session_.resetForNewSession();
        // Use the same `now` for the shutdown marker to maintain a single
        // consistent timestamp across the entire stop operation.
        db_.setLastCleanShutdownMarker(now);
        Logger::Log("[DB] Session durations updated");
    } else {
        session_.markUnsaved();
        session_.unsaved_durations = session_.durations;
        Logger::Log("[DB] Error updating session durations - data retained for next save attempt");
    }

    emit stopped(reason);
}

void Timer::useTimerViaButton(Button button)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "useTimerViaButton");
    guard.markTransitioned();
#endif
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

void Timer::onStartPausePressed()
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "onStartPausePressed");
    guard.markTransitioned();
#endif
    const QDateTime now = now_provider_();
    if (discardCrossMidnightOngoingAndStop(now)) {
#ifndef QT_NO_DEBUG
        checkDurationInvariants();
#endif
        return;
    }
    switch (mode_) {
        case Mode::Activity: pauseTimer(now); break;
        case Mode::Pause:    startTimer(now); break;
        case Mode::None:     startTimer(now); break;
    }
#ifndef QT_NO_DEBUG
    checkDurationInvariants();
#endif
}

void Timer::onStopPressed()
{
    useTimerViaButton(Button::Stop);
}

/**
 * Handles desktop lock/unlock events from LockStateWatcher.
 *
 * Event handling:
 * - Lock: Saves final checkpoint, suspends further checkpoints via is_locked_
 * - LongOngoingLock: Triggers backpause if timer was in Activity mode
 * - Unlock: Resumes checkpoints, restarts timer if it was auto-paused
 *
 * Note: Lock/Unlock events for checkpoint control are always processed.
 * Autopause behavior (LongOngoingLock handling) respects isAutopauseEnabled().
 */
void Timer::useTimerViaLockEvent(LockEvent event, const QDateTime& lockTime)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "useTimerViaLockEvent");
    guard.markTransitioned();
#endif
    const QDateTime now = now_provider_();
    if (discardCrossMidnightOngoingAndStop(now)) {
        // Already-forced stopped — ignore the lock event for engine
        // purposes. autopause_pending_resume_ is already cleared inside
        // the helper so a subsequent Unlock does not restart.
#ifndef QT_NO_DEBUG
        checkDurationInvariants();
#endif
        return;
    }
    if (event == LockEvent::Lock) {
        is_locked_ = true;
        if (dialog_open_) {
            Logger::Log("[LOCK] suspended during history edit (Lock)");
            return;
        }
        // Save a checkpoint when lock is detected (only if actively tracking time)
        if (mode_ == Mode::Activity) {
            saveCheckpointInternal(now);
            Logger::Log("[LOCK] Desktop locked - checkpoint saved, further checkpoints suspended");
        } else {
            Logger::Log("[LOCK] Desktop locked - no checkpoint (timer not in Activity mode)");
        }
        return;
    }
    if (event == LockEvent::Unlock) {
        is_locked_ = false;
        if (dialog_open_) {
            // Unlock supersedes any pending LongOngoingLock
            pending_lock_event_ = LockEvent::None;
            Logger::Log("[LOCK] suspended during history edit (Unlock)");
            return;
        }
        Logger::Log("[LOCK] Desktop unlocked - checkpoint saving resumed");
    }
    if (!settings_.isAutopauseEnabled()) {
        Logger::Log("[DEBUG] Autopause disabled; lock event ignored");
        return;
    }
    if (event == LockEvent::LongOngoingLock) {
        if (dialog_open_) {
            pending_lock_event_ = LockEvent::LongOngoingLock;
            Logger::Log("[LOCK] suspended during history edit (LongOngoingLock)");
            return;
        }
        autopause_pending_resume_ = (mode_ == Mode::Activity);
        if (autopause_pending_resume_) {
            backpauseTimer(now, lockTime);
            emit modeChanged(PauseCause::LockAutopause);
        }
    } else if (event == LockEvent::Unlock) {
        if (autopause_pending_resume_) {
            startTimer(now);
            emit modeChanged(PauseCause::LockResume);
        }
        autopause_pending_resume_ = false;
    }
#ifndef QT_NO_DEBUG
    checkDurationInvariants();
#endif
}

void Timer::onTick(const QDateTime& now)
{
    QMutexLocker locker(&mutex_);
    day_boundary_watcher_.tick(now);
}

qint64 Timer::getActiveTime() const
{
    // Totals come from the wall-clock timeline (completed + ongoing) so the
    // display stays consistent with persistence across a suspend; the monotonic
    // timer_ freezes during suspend and no longer drives the displayed total.
    return snapshot().activeMsec();
}

qint64 Timer::getPauseTime() const
{
    return snapshot().pauseMsec();
}

Timeline Timer::snapshot() const
{
    QMutexLocker lock(&mutex_);
    return Timeline(session_.durations, getOngoingDuration_locked());
}

Timeline Timer::getCurrentDurations() const
{
    QMutexLocker locker(&mutex_);
    return Timeline(session_.durations, std::nullopt);
}

void Timer::setDurationType(size_t idx, DurationType type)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "setDurationType");
    guard.markTransitioned(); // Intentionally mutates durations
#endif
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
 * No-op (returns true) when checkpoints are not applicable: checkpoint interval
 * disabled, not in Activity mode, the segment is not an Activity segment, the
 * segment has no positive duration / invalid timestamps, or a dialog is open /
 * the session is locked. Disabled history is also treated as success. Returns
 * false only on a real checkpoint write failure (caller bug or DB error).
 */
bool Timer::maybeReanchorCheckpoint(const TimeDuration& seg)
{
    if (checkpoint_interval_msec_ == 0 || mode_ != Mode::Activity || seg.type != DurationType::Activity)
        return true;
    if (seg.duration <= 0 || !seg.startTime.isValid() || !seg.endTime.isValid())
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
        Logger::Log("[CHECKPOINT] replaceCurrentDurations: saveCheckpoint caller bug: " + cpResult.message);
        return false;
    } else if (cpResult.category == SessionStoreResult::FatalError) {
        Logger::Log("[CHECKPOINT] replaceCurrentDurations: Fatal DB error saving checkpoint: " + cpResult.message);
        emit userWarning("Database error while saving checkpoint: " + cpResult.message);
        return false;
    } else {
        Logger::Log("[CHECKPOINT] replaceCurrentDurations: saveCheckpoint failed: " + cpResult.message);
        return false;
    }
}

/**
 * Atomically replaces the in-memory durations and re-anchors checkpoint tracking
 * from an edited Timeline.
 *
 * This is the sole external entry point for overwriting session_.durations from
 * outside Timer (e.g., when HistoryDialog saves edits). It couples the two
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
Timer::EditCommitResult Timer::commitEditedTimeline(const Timeline& edited)
{
    QMutexLocker locker(&mutex_);
#ifndef QT_NO_DEBUG
    StateGuard guard(*this, "commitEditedTimeline");
    guard.markTransitioned(); // Intentionally replaces durations
#endif
    const std::deque<TimeDuration>& newDurations = edited.completed();
    const std::optional<TimeDuration>& ongoing = edited.ongoing();

    session_.durations = newDurations;
    session_.clearUnsaved();

    bool checkpointOk = true;
    if (ongoing.has_value() && mode_ != Mode::None) {
        const TimeDuration& seg = ongoing.value();
        session_.adoptOngoingSegment(seg);

        // Align mode_ with the edited ongoing segment type so the live row and
        // the next checkpoint/stop reflect the user's choice rather than the
        // pre-edit mode. (adoptOngoingSegment intentionally ignores type.)
        const Mode desired = (seg.type == DurationType::Activity) ? Mode::Activity : Mode::Pause;
        if (mode_ != desired) {
            mode_ = desired;
            if (desired == Mode::Activity) {
                checkpointTimer_.start();   // checkpoints run only in Activity
                emit started(true);
            } else {
                checkpointTimer_.stop();
                emit paused();
            }
        }

        checkpointOk = maybeReanchorCheckpoint(seg);
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
        session_.clearSegment();
        checkpointTimer_.stop();
        mode_ = Mode::None;
        emit stopped(StopReason::EditApplied);
    }

#ifndef QT_NO_DEBUG
    checkDurationInvariants();
#endif
    return checkpointOk ? EditCommitResult::success()
                        : EditCommitResult::failure("checkpoint write failed after edit commit");
}

void Timer::replaceCurrentDurations(const std::deque<TimeDuration>& newDurations,
                                    const std::optional<TimeDuration>& ongoing)
{
    commitEditedTimeline(Timeline(newDurations, ongoing));
}

void Timer::commit(const Timeline& edited)
{
    commitEditedTimeline(edited);
}

std::deque<TimeDuration> Timer::getDurationsHistory()
{
    QMutexLocker locker(&mutex_);
    auto loadResult = db_.loadDurations();
    last_history_load_skipped_ = loadResult.skipped;
    last_history_load_repaired_ = loadResult.repaired;
    return loadResult.durations;
}

std::pair<int, int> Timer::getLastHistoryLoadStats() const
{
    QMutexLocker locker(&mutex_);
    return std::make_pair(last_history_load_skipped_, last_history_load_repaired_);
}

std::optional<TimeDuration> Timer::getOngoingDuration() const
{
    QMutexLocker locker(&mutex_);
    return getOngoingDuration_locked();
}

std::optional<TimeDuration> Timer::getOngoingDuration_locked() const
{
    if (mode_ == Mode::None) return std::nullopt;
    if (!session_.segment_start_time.isValid()) return std::nullopt;
    const QDateTime now = now_provider_();
    const DurationType type = (mode_ == Mode::Activity) ? DurationType::Activity : DurationType::Pause;
    if (session_.segment_start_time >= now) {
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

qint64 Timer::getStartupRecoveredSeconds() const
{
    QMutexLocker locker(&mutex_);
    return startup_recovered_seconds_;
}

bool Timer::shouldShowStartupRecoveryNotification() const
{
    QMutexLocker locker(&mutex_);
    return startup_recovery_notification_needed_;
}

bool Timer::canMarkCleanShutdown() const
{
    QMutexLocker locker(&mutex_);
    return mode_ == Mode::None && !session_.has_unsaved_data;
}

Timer::ShutdownResult Timer::shutdown()
{
    QMutexLocker locker(&mutex_);
    shutdown_called_ = true;
    bool stopCalled = false;
    if (mode_ != Mode::None) {
        stopTimer(now_provider_(), StopReason::Shutdown);
        stopCalled = true;
    }
    const bool stopped = (mode_ == Mode::None);
    const bool canCleanMark = stopped && !session_.has_unsaved_data;
    return {stopped, canCleanMark, stopCalled};
}

bool Timer::appendDurationsToDB()
{
    // Disabled history is a no-op, not a failure.
    return appendDurationsChunkToDB(session_.durations).okOrDisabled();
}

void Timer::logDbResultOrWarn(const SessionStoreResult& result, const QString& context)
{
    if (result.category == SessionStoreResult::CallerBug) {
        Logger::Log("[DB] CRITICAL: " + context + " caller bug: " + result.message);
    } else if (result.category == SessionStoreResult::FatalError) {
        Logger::Log("[DB] CRITICAL: " + context + " fatal error: " + result.message);
        emit userWarning("Database error: " + result.message);
    } else if (result.category == SessionStoreResult::TransientError) {
        Logger::Log("[DB] " + context + " transient error: " + result.message);
    }
    // Success: nothing to log/emit
}

SessionStoreResult Timer::appendDurationsChunkToDB(const std::deque<TimeDuration>& durations)
{
    if (durations.empty())
        return SessionStoreResult::success();
    const SessionStoreResult result = db_.commitSession(Timeline(durations, std::nullopt));
    logDbResultOrWarn(result, "commitSession");
    return result;
}

bool Timer::updateDurationsInDB()
{
    if (session_.durations.empty()) {
        session_.clearUnsaved();
        return true;
    }
    const SessionStoreResult result = db_.commitSession(Timeline(session_.durations, std::nullopt));
    logDbResultOrWarn(result, "commitSession");
    // Disabled history is a no-op, not a save failure: treat it as success so
    // we don't retain bogus unsaved data or skip the clean-shutdown marker.
    const bool ok = result.okOrDisabled();
#ifndef QT_NO_DEBUG
    if (ok) {
        checkCrossLayerInvariants();
    }
#endif
    return ok;
}

SessionStoreResult Timer::replaceAll(const Timeline& history, const Timeline& session)
{
    QMutexLocker locker(&mutex_);
    const SessionStoreResult result = db_.replaceAll(history, session);
    if (result.okOrDisabled()) {
        // DB is now authoritative; stale retry cache must not survive past here.
        session_.clearUnsaved();
    }
    return result;
}

EntriesForDateResult Timer::hasEntriesForDate(const QDate& date)
{
    return db_.hasEntriesForDate(date);
}

/**
 * Appends a single same-day segment to session_.durations.
 *
 * DayBoundaryWatcher's scheduled stop and watchdog prevent cross-midnight
 * inputs from reaching here; this discard is a last-line defence.  Zero-
 * and negative-duration inputs (e.g. start == end) are also dropped.
 * Each rejection is logged with the specific reason.
 */
void Timer::addDuration(DurationType type,
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
    const qint64 dur = seg->duration;
    session_.durations.emplace_back(std::move(*seg));
    Logger::Log(QString("[DEBUG] Added duration (%1, %2 ms)")
        .arg(type == DurationType::Activity ? "Activity" : "Pause")
        .arg(dur));
}

bool Timer::isOngoingSegmentCrossMidnight() const
{
    QMutexLocker locker(&mutex_);
    if (mode_ == Mode::None) return false;
    if (!session_.segment_start_time.isValid()) return false;
    return isCrossMidnight(session_.segment_start_time, QDateTime(now_provider_().date(), QTime(0,0,0), Qt::LocalTime));
}

/**
 * Forces the engine into the stopped state when the ongoing segment has
 * crossed midnight, discarding the in-flight segment.
 *
 * Returns true when it fired (telling the caller to abandon any pending
 * transition). Safe to call multiple times; only the first call does real work.
 *
 * Emits stopped(MidnightWatchdog). The GUI is updated via the stopped() signal
 * connection in MainWin.
 */
bool Timer::discardCrossMidnightOngoingAndStop(const QDateTime& now)
{
    if (mode_ == Mode::None) return false;
    if (!session_.segment_start_time.isValid()) return false;
    if (!isCrossMidnight(session_.segment_start_time, now)) return false;

    // Capture the in-flight segment id before teardown so its orphaned
    // (unfinalized) checkpoint row can be deleted below.
    const SegmentId discardedId = session_.segment_id;

    Logger::Log(QString("[MIDNIGHT] Cross-midnight ongoing detected "
                        "(segment_start=%1, now=%2) — discarding segment, forcing stop")
        .arg(session_.segment_start_time.toString(Qt::ISODateWithMs))
        .arg(now.toString(Qt::ISODateWithMs)));

    // Best-effort: persist any already-completed (same-day) segments that
    // accumulated before midnight. Only the ONGOING segment is discarded.
    if (!session_.durations.empty()) {
        if (!updateDurationsInDB()) {
            session_.markUnsaved();
            session_.unsaved_durations = session_.durations;
            Logger::Log("[MIDNIGHT] Could not flush completed segments to DB - retained in unsaved buffer");
        } else {
            session_.resetForNewSession();
        }
    }

    // Reuse stopTimer for teardown. The cross-midnight ongoing segment is
    // discarded by addDuration (TimeDuration::create returns CrossMidnight).
    // durations is already empty (success path) or retained in unsaved buffer
    // (flush-failure path), so the updateDurationsInDB call inside stopTimer is a
    // no-op on success and a harmless retry on flush failure.
    stopTimer(now, StopReason::MidnightWatchdog);

    // Delete the discarded segment's checkpoint row. Without this, a later
    // startup recovery would finalize it (silently re-adding the deliberately
    // discarded time) or the stale policy would drop it (silently losing
    // checkpointed pre-midnight work). Observable side effect + log line.
    if (!discardedId.isEmpty()) {
        const SessionStoreResult r = db_.discardCheckpoint(discardedId);
        logDbResultOrWarn(r, "discardCheckpoint");
    }
    return true;
}

void Timer::saveCheckpoint()
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
    saveCheckpointInternal(now);
}

void Timer::saveCheckpointInternal(const QDateTime& now)
{
    // NOTE: This function assumes the mutex is already held by the caller

    if (checkpoint_interval_msec_ == 0) return;
    if (mode_ != Mode::Activity)        return;
    if (discardCrossMidnightOngoingAndStop(now)) return;

    // Use wall-clock elapsed rather than timer_.elapsed() to stay correct across suspend.
    const qint64 wallClockMs = session_.segment_start_time.msecsTo(now);
    if (wallClockMs <= 0) {
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
    }
}

void Timer::beginExclusiveEdit()
{
    QMutexLocker locker(&mutex_);
    dialog_open_ = true;
    checkpointTimer_.stop();
    Logger::Log("[CHECKPOINT] Checkpoints paused (dialog open)");
}

void Timer::endExclusiveEdit()
{
    QMutexLocker locker(&mutex_);
    dialog_open_ = false;
    Logger::Log("[CHECKPOINT] Checkpoints resumed (dialog closed)");

    // Replay deferred events that were suspended while dialog was open.
    if (pending_midnight_stop_) {
        pending_midnight_stop_ = false;
        pending_lock_event_ = LockEvent::None;
        Logger::Log("[MIDNIGHT] Replaying deferred midnight stop after dialog close");
        stopTimer(now_provider_(), StopReason::MidnightScheduled);
    } else if (pending_lock_event_ == LockEvent::LongOngoingLock) {
        pending_lock_event_ = LockEvent::None;
        Logger::Log("[LOCK] Replaying deferred LongOngoingLock after dialog close");
        useTimerViaLockEvent(LockEvent::LongOngoingLock);
    } else {
        pending_lock_event_ = LockEvent::None;
    }
    // Restart checkpoint timer only after deferred events are replayed, so
    // stopTimer/backpauseTimer can set mode_ before we decide whether to arm.
    if (checkpoint_interval_msec_ > 0 && mode_ == Mode::Activity && !is_locked_) {
        checkpointTimer_.start();
    }
}

