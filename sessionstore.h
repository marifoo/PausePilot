/**
 * SessionStore — Abstract interface for time-duration persistence.
 *
 * Extracted from the concrete SqliteSessionStore class so that TimeTracker
 * (and anything else that needs DB access) can depend on an abstraction
 * rather than the SQLite implementation.  This enables:
 *
 *   - FakeSessionStore in tests: records every call, returns
 *     configurable success/failure, stores data in memory (no SQLite).
 *   - Easier reasoning about which DB operations TimeTracker actually needs.
 *
 * Every public method that TimeTracker calls on SqliteSessionStore is
 * represented here as a pure virtual.  Internal helpers (ensureOpen,
 * ensureSchema, createBackup, etc.) remain private implementation details
 * of the concrete class.
 *
 * ── Unified result vocabulary ──────────────────────────────────────────
 *
 * All store methods that can fail use SessionStoreResult::Category to
 * report their outcome.  The categories have consistent meanings across
 * every method:
 *
 *   Success       — operation completed; any returned data is valid.
 *
 *   Disabled      — history storage is disabled (history_days_to_keep == 0).
 *                   The call was a no-op.  This is not an error; callers
 *                   should treat the absence of data as intentional.
 *                   Retrying with the same arguments will always return
 *                   Disabled until history is re-enabled.
 *
 *   TransientError — a recoverable failure (e.g. SQL execution error,
 *                   transaction could not start).  The caller may retry;
 *                   data was not mutated.
 *
 *   FatalError    — unrecoverable failure (e.g. DB connection lost).
 *                   Callers should emit a user warning and refuse new sessions.
 *
 *   CallerBug     — invalid input supplied by the caller (e.g. empty
 *                   segment_id, or a segment_id that already belongs to
 *                   finalized history).  Retrying with the same arguments
 *                   will not help.  Log as critical.
 *
 * Methods that carry additional payload (loadDurations, hasEntriesForDate)
 * embed a status field that uses the same categories.  The per-method
 * contract matrices in test_contract_matrix.cpp document which categories
 * apply to each method.
 */

#ifndef SESSIONSTORE_H
#define SESSIONSTORE_H

#include <QDateTime>
#include <QString>
#include <deque>
#include "types.h"
#include "timeline.h"

/**
 * Typed result returned by SessionStore operations that write to the database.
 *
 * Category semantics — see unified vocabulary in file-level comment above.
 */
struct SessionStoreResult {
    enum Category { Success, Disabled, TransientError, FatalError, CallerBug };
    Category category;
    QString message;
    bool ok() const { return category == Success; }
    /// True when the call did its job OR was a no-op because history storage
    /// is disabled.  Control-flow seams that treat "history off" the same as
    /// success (no unsaved-data retention, no failure warning) should use this
    /// instead of ok(); Disabled is not an error.
    bool okOrDisabled() const { return category == Success || category == Disabled; }
    static SessionStoreResult success()  { return {Success, {}}; }
    static SessionStoreResult disabled() { return {Disabled, {}}; }
    static SessionStoreResult transient(QString msg) { return {TransientError, std::move(msg)}; }
    static SessionStoreResult callerBug(QString msg) { return {CallerBug, std::move(msg)}; }
    static SessionStoreResult fatal(QString msg) { return {FatalError, std::move(msg)}; }
};

/**
 * Result of TimeTracker::drainForShutdown() — the terminal-shutdown attempt to
 * persist all pending commit work, independent of tracker mode.
 *
 *   Complete   — a live segment (if any) was closed and no commit payload
 *                remains only in memory; a clean-shutdown marker may be
 *                written.
 *   Incomplete — a payload remains in memory only; category/message describe
 *                why the persistence attempt failed. No clean marker may be
 *                written.
 */
struct DrainResult {
    enum class Status { Complete, Incomplete };
    Status status = Status::Complete;
    SessionStoreResult::Category category = SessionStoreResult::Success;
    QString message;
    /// True when this drain's own live-segment stop already wrote the
    /// clean-shutdown marker synchronously (no retry was needed). Lets
    /// ShutdownCoordinator avoid a redundant marker write. Never used to
    /// decide clean-marker eligibility -- see clean().
    bool markerAlreadyWritten = false;

    bool clean() const { return status == Status::Complete; }

    static DrainResult complete(bool markerAlreadyWritten = false) {
        return {Status::Complete, SessionStoreResult::Success, {}, markerAlreadyWritten};
    }
    static DrainResult incomplete(SessionStoreResult::Category cat, QString msg) {
        return {Status::Incomplete, cat, std::move(msg), false};
    }
};

/**
 * Result of loadDurations().
 *
 * status carries the same SessionStoreResult::Category vocabulary:
 *   Success       — query ran; durations holds all finalized rows (may be empty).
 *   Disabled      — history storage is disabled; durations is always empty.
 *                   Treat as "no history," not an error.
 *   TransientError / FatalError — DB could not be opened or the query failed;
 *                   durations is empty.  Surface an error to the user.
 *
 * CallerBug is not applicable to loadDurations.
 */
struct LoadResult {
    std::deque<TimeDuration> durations;
    int skipped = 0;
    int repaired = 0;
    SessionStoreResult::Category status = SessionStoreResult::Success;

    size_t size() const { return durations.size(); }
    bool empty() const { return durations.empty(); }
    const TimeDuration& operator[](size_t idx) const { return durations[idx]; }
    std::deque<TimeDuration>::const_iterator begin() const { return durations.begin(); }
    std::deque<TimeDuration>::const_iterator end() const { return durations.end(); }
};

/**
 * Result returned by recoverStartupCheckpoints() (B2/R08-001..003,005,
 * R05-004).
 *
 * The store loads and classifies every unfinalized checkpoint row, then --
 * within one atomic disposition -- deletes malformed rows, applies the
 * too-short (<1 s) / stale (>24 h) / future-dated and overlap policies,
 * finalizes qualifying rows, resolves durable discard intents (A4), and
 * consumes the clean-shutdown marker.  A failure at any stage rolls back the
 * ENTIRE disposition: no row is mutated and the marker (if any) is preserved.
 *
 *   status == Success        — recovery committed (even if zero rows were
 *                               finalized/dropped).  finalized_count,
 *                               policy_dropped_count, and recovered_msec
 *                               describe exactly the rows physically mutated
 *                               by this call.
 *   status == Disabled       — history storage is off; a deliberate no-op.
 *                               All counts are zero and notify_user is false.
 *   status == TransientError
 *   status == FatalError     — a critical store failure (marker read, orphan
 *                               load, or the recovery transaction itself)
 *                               prevented reconciliation.  NO row was
 *                               mutated and the marker (if any) was
 *                               preserved.  message describes the failure.
 *                               Caller must treat all counts as 0 and must
 *                               NOT show a recovery notification.
 *
 * CallerBug is not applicable.
 *
 * policy_dropped_count counts rows deleted for a recovery POLICY reason
 * (too-short, stale, future-dated, overlap-rejected, or a durable A4 discard
 * intent) -- never a repaired-row count. Malformed rows (invalid timestamp/
 * type, empty id, non-positive interval) are also deleted as part of the
 * same atomic disposition, but are store-internal cleanup rather than a
 * policy decision: their count is logged only and is not part of this
 * struct.
 */
struct StartupRecoveryResult {
    SessionStoreResult::Category status = SessionStoreResult::Success;
    QString message;
    qint64 recovered_msec = 0;
    int finalized_count = 0;
    int policy_dropped_count = 0;
    bool notify_user = false;

    bool ok() const { return status == SessionStoreResult::Success; }
    bool okOrDisabled() const { return status == SessionStoreResult::Success || status == SessionStoreResult::Disabled; }

    static StartupRecoveryResult disabled() {
        StartupRecoveryResult r;
        r.status = SessionStoreResult::Disabled;
        return r;
    }
    static StartupRecoveryResult failure(SessionStoreResult::Category cat, QString msg) {
        StartupRecoveryResult r;
        r.status = cat;
        r.message = std::move(msg);
        return r;
    }
};

/**
 * Result of checkSchemaOnStartup().
 *
 *   Ready        — existing DB has the expected schema; safe to use.
 *   Created      — DB file did not exist; a fresh schema was built.
 *   Outdated     — existing DB has wrong columns or missing/non-unique constraints;
 *                  caller must refuse to start and ask the user to remove the file.
 *   Inaccessible — DB file could not be opened (permissions, I/O error);
 *                  caller must refuse to start.
 */
enum class SchemaStatus { Ready, Created, Outdated, Inaccessible };

class SessionStore
{
public:
    virtual ~SessionStore() = default;

    /// @pre All segment_ids in the timeline must be non-empty
    virtual SessionStoreResult commitSession(const Timeline& session) = 0;

    /// Atomically replaces all rows (history finalized + session unfinalized).
    /// Returns Disabled when history storage is off (no-op, not an error).
    /// Returns TransientError or FatalError on DB failure.
    /// CallerBug is not applicable.
    virtual SessionStoreResult replaceAll(const Timeline& history, const Timeline& session) = 0;

    /// Returns all finalized rows sorted chronologically.
    /// result.status is Disabled when history is off, or TransientError/FatalError
    /// on DB failure.  On any non-Success status, result.durations is empty.
    virtual LoadResult loadDurations() = 0;

    /// Returns Yes iff at least one finalised row's start falls on `localDate`
    /// interpreted in the system local time zone.
    /// Returns Unknown when history is disabled or the DB cannot be opened —
    /// callers must treat Unknown as "answer unknowable," not "no entries."
    virtual EntriesForDateResult hasEntriesForDate(const QDate& localDate) = 0;

    /// Writes or extends an unfinalized checkpoint row identified by segmentId.
    /// Invariant: checkpoint writes must never demote finalized history. If segmentId
    /// already belongs to a finalized row, the call returns CallerBug and leaves the
    /// row unchanged. Only unfinalized rows may be created or updated by this method.
    /// Returns Disabled when history storage is off (no-op, not an error).
    /// @pre segmentId must be non-empty
    virtual SessionStoreResult saveCheckpoint(DurationType type, const QDateTime& startTime,
                                              const QDateTime& endTime, const SegmentId& segmentId) = 0;

    /// Deletes the unfinalized checkpoint row(s) for segmentId (is_finalized = 0).
    /// Called when the engine deliberately discards an in-flight segment (e.g. the
    /// cross-midnight watchdog) so the orphaned checkpoint is neither resurrected
    /// by startup recovery nor silently dropped by the stale-orphan policy.
    ///
    /// The DISCARD INTENT itself must be durable across a failure of this call
    /// (R05-001): implementations own recording it (e.g. a store-owned tombstone)
    /// before attempting the physical row delete, and startup recovery must honor
    /// any unresolved intent it finds -- deleting only a matching unfinalized row,
    /// never a finalized one. Returning TransientError/FatalError therefore does
    /// not mean the intent was lost; the caller retains the segment id and may
    /// call this again -- both the tombstone write and the row delete are
    /// idempotent, so repeated calls are safe.
    /// Returns Disabled when history storage is off (no-op, not an error).
    /// Returns TransientError/FatalError on DB failure.
    /// @pre segmentId must be non-empty
    virtual SessionStoreResult discardCheckpoint(const SegmentId& segmentId) = 0;

    virtual SchemaStatus checkSchemaOnStartup() = 0;

    /// Forces pending writes to disc.  Returns Disabled when history is off,
    /// TransientError on failure, Success otherwise.
    /// CallerBug is not applicable.
    virtual SessionStoreResult flushToDisc() = 0;

    /// Writes the clean-shutdown marker timestamp.
    /// Returns Disabled when history is off (no-op, not an error).
    /// Returns TransientError on write failure.
    /// CallerBug is not applicable.
    virtual SessionStoreResult setLastCleanShutdownMarker(const QDateTime& timestamp) = 0;

    /// Recovers unfinalized checkpoint rows left by a previous crash or unclean exit.
    /// Internally consumes the last-clean-shutdown marker, applies too-short/stale/
    /// overlap policies, and finalizes qualifying rows.  Returns domain facts only —
    /// row ids and finalize/drop mechanics are hidden inside the store.
    /// `now` is used for the stale-age check (>24 h) and should be the current time.
    virtual StartupRecoveryResult recoverStartupCheckpoints(const QDateTime& now) = 0;
};

#endif // SESSIONSTORE_H
