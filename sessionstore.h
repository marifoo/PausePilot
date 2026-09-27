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
 * ensureSchema, etc.) remain private implementation details
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
 *   CallerBug     — invalid input supplied by the caller (e.g. empty
 *                   segment_id, or a segment_id that already belongs to
 *                   finalized history).  Retrying with the same arguments
 *                   will not help.  Log as critical.
 *
 *   Conflict      — the stored data no longer matches the caller's snapshot
 *                   of it (a concurrent change since the snapshot was taken).
 *                   Only saveHistoryDays() (H5/WP2) returns this category;
 *                   the caller must re-read fresh data before retrying, not
 *                   simply resubmit the same payload.
 *
 * Every write boundary's ensureOpen()/open-failure path returns
 * TransientError (see the ensureOpen()-consistency table in
 * test_contract_matrix.cpp). WP9 (C11) removed replaceAll() and its backup,
 * the last production producer of an unrecoverable failure, and WP10 (C2)
 * removed the now-unused FatalError category itself.
 *
 * Precedence (C4/R05-003): where both a CallerBug and a Disabled outcome
 * would apply to the same call (e.g. a duplicate/empty id submitted while
 * history is disabled), CallerBug wins. A caller bug is a programming error
 * that must surface regardless of whether history persistence happens to be
 * switched off -- checking Disabled first would mask it. commitSession,
 * saveCheckpoint, and discardCheckpoint all check their identity/validity
 * guard before the Disabled guard.
 *
 * Methods that carry additional payload (loadDurations, hasEntriesForDate)
 * embed a status field that uses the same categories.  The per-method
 * contract matrices in test_contract_matrix.cpp document which categories
 * apply to each method.
 *
 * ── Input facts for commit/checkpoint (C4) ─────────────────────
 *
 * commitSession takes Timeline-wrapped rows: segment_id emptiness
 * is structurally impossible (TimeDuration always mints one), so the only
 * caller-identity fact it validates at runtime is uniqueness -- no two
 * rows in the payload may share a segment_id.
 * saveCheckpoint/discardCheckpoint take a bare SegmentId parameter instead,
 * so both explicitly reject an empty one as CallerBug.
 *
 * Every write boundary additionally validates timestamp validity, a
 * positive `[start, end)` interval, and same-local-day shape (see
 * dayvalidity.h); commitSession also rejects an Activity/Pause
 * overlap across the payload. Callers may submit rows in any order: the
 * store rejects an Activity/Pause overlap and merges same-type overlaps via
 * normalization, rather than requiring the caller to pre-sort or
 * pre-deduplicate. FakeSessionStore enforces the same identity
 * facts (empty/duplicate segment_id) as the concrete store, but -- being a
 * spy, not a policy engine -- does not duplicate the day-validity/overlap
 * checks; those are exercised only against the real SqliteSessionStore (see
 * test_persistence_contract.cpp).
 */

#ifndef SESSIONSTORE_H
#define SESSIONSTORE_H

#include <QDateTime>
#include <QString>
#include <deque>
#include <optional>
#include <vector>
#include "types.h"
#include "timeline.h"

/**
 * Typed result returned by SessionStore operations that write to the database.
 *
 * Category semantics — see unified vocabulary in file-level comment above.
 */
struct SessionStoreResult {
    enum Category { Success, Disabled, TransientError, CallerBug, Conflict };
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
    static SessionStoreResult conflict(QString msg) { return {Conflict, std::move(msg)}; }
};

/**
 * Result of markCleanShutdown() (D6/R05-002/R15-004/M1).
 *
 * markCleanShutdown() is two internal steps -- marker write, then (only if
 * that succeeded) the FULL durability barrier -- and callers must be able to
 * tell WHICH step failed: a marker-write failure means the clean-shutdown
 * record itself was never committed (session data is unaffected), while a
 * barrier failure happens only after the marker WAS successfully committed,
 * so it is a durability-of-everything-including-the-marker failure, not a
 * "record not written" failure. Presenting these the same way to the user is
 * false in the barrier case (M1).
 */
struct MarkCleanShutdownResult {
    SessionStoreResult result;
    /// True once the internal marker-write step itself returned Success
    /// (i.e. the FULL barrier was reached at all). False if the marker
    /// write failed or was Disabled -- in both of those cases the barrier
    /// is never attempted, so this is also false then.
    bool markerWritten = false;

    bool ok() const { return result.ok(); }
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
    /// D6/Medium-3/M1 (R05-002): true only when drainForShutdown() itself
    /// reported Complete (session data fully committed, nothing pending)
    /// AND this result was downgraded to Incomplete specifically because
    /// markCleanShutdown()'s MARKER-WRITE step failed (its
    /// MarkCleanShutdownResult::markerWritten was false) -- the
    /// clean-shutdown record itself was never committed, but session data
    /// is NOT at risk. Deliberately false for a barrier-step failure (the
    /// marker WAS committed there; only the covering fsync failed) -- that
    /// is a genuine durability failure, not "record not written", and must
    /// not be presented the same reassuring way. Callers (MainWin) must not
    /// present this flag's true case the same way as a genuinely unsaved
    /// session.
    bool markerOnlyFailure = false;

    bool clean() const { return status == Status::Complete; }

    static DrainResult complete() {
        return {Status::Complete, SessionStoreResult::Success, {}, false};
    }
    static DrainResult incomplete(SessionStoreResult::Category cat, QString msg) {
        return {Status::Incomplete, cat, std::move(msg), false};
    }
    /// A clean drain whose own commit/discard work is fully resolved, but
    /// whose clean-shutdown MARKER WRITE failed (not the barrier -- see
    /// markerOnlyFailure above).
    static DrainResult markerFailed(SessionStoreResult::Category cat, QString msg) {
        return {Status::Incomplete, cat, std::move(msg), true};
    }
};

/**
 * Result of loadDurations().
 *
 * status carries the same SessionStoreResult::Category vocabulary:
 *   Success       — query ran; durations holds all finalized rows (may be empty).
 *   Disabled      — history storage is disabled; durations is always empty.
 *                   Treat as "no history," not an error.
 *   TransientError — DB could not be opened or the query failed; durations
 *                   is empty.  Surface an error to the user.
 *
 * CallerBug is not applicable to loadDurations: its ensureOpen() failure
 * path is consolidated onto TransientError.
 */
struct LoadResult {
    std::deque<TimeDuration> durations;
    SessionStoreResult::Category status = SessionStoreResult::Success;

    size_t size() const { return durations.size(); }
    bool empty() const { return durations.empty(); }
    const TimeDuration& operator[](size_t idx) const { return durations[idx]; }
    std::deque<TimeDuration>::const_iterator begin() const { return durations.begin(); }
    std::deque<TimeDuration>::const_iterator end() const { return durations.end(); }
};

/**
 * One `durations` row exactly as stored, for loadHistoryRows() (H5).
 *
 * Every column is carried as raw text (query.value(i).toString()), never
 * type-converted or validated -- this struct exists precisely so a caller
 * can see a row that loadDurations() would silently skip or delete.
 * `parsed` is set iff the row is well-formed by the same rules
 * loadDurations() applies (non-empty id, valid type, valid timestamps,
 * positive interval); its times are converted to local time. A row with
 * `parsed == std::nullopt` is malformed and must be treated read-only by the
 * caller -- loadHistoryRows() never deletes or repairs anything.
 */
struct StoredHistoryRow {
    QString segmentId, type, startUtc, endUtc, isFinalized;
    bool finalized() const { return isFinalized == "1"; }
    std::optional<TimeDuration> parsed;
};

/**
 * Result of loadHistoryRows() (H5).
 *
 * status carries the same SessionStoreResult::Category vocabulary:
 *   Success        — query ran; rows holds every row in the table (finalized,
 *                    unfinalized, multi-boundary, and malformed alike).
 *   Disabled       — history storage is disabled; rows is always empty.
 *   TransientError — DB could not be opened or the query failed; rows is empty.
 *
 * Unlike loadDurations(), this is a pure read: it never deletes malformed
 * rows and never filters by is_finalized. CallerBug is not applicable (no
 * parameters, no write).
 */
struct HistoryRowsResult {
    SessionStoreResult::Category status = SessionStoreResult::Success;
    std::vector<StoredHistoryRow> rows;
};

/**
 * One local date's worth of History edits, for saveHistoryDays() (H5/WP2).
 *
 * dayStartUtc/dayEndUtc are the half-open UTC span of `date`
 * (DayValidity::localDayStart/localDayEnd(date).toUTC()), captured once when
 * History was opened.
 *
 * deletedIds are editable open-time rows removed outright (merge losers); an
 * empty set is valid (H5 permits replacing every row on a date with
 * nothing). upserts are the changed rows: an id already present in the
 * caller's snapshot keeps its identity (UPDATE), any other id is a split
 * half or a brand-new row (INSERT).
 */
struct HistoryDayChange {
    QDate date;
    QDateTime dayStartUtc, dayEndUtc;
    std::vector<SegmentId> deletedIds;
    std::deque<TimeDuration> upserts;
};

/**
 * Result returned by recoverStartupCheckpoints() (B2/R08-001..003,005,
 * R05-004).
 *
 * The store loads and classifies every unfinalized checkpoint row, then --
 * within one atomic disposition -- deletes malformed rows, applies the
 * expired-beyond-History-retention (using this call's own `now`) / cross-day
 * / overlap policies, finalizes qualifying rows, resolves durable discard
 * intents (A4), and consumes the clean-shutdown marker.  A failure at any stage rolls back the
 * ENTIRE disposition: no row is mutated and the marker (if any) is preserved.
 *
 *   status == Success        — recovery committed (even if zero rows were
 *                               finalized/dropped).  finalized_count,
 *                               policy_dropped_count, and recovered_msec
 *                               describe exactly the rows physically mutated
 *                               by this call.
 *   status == Disabled       — history storage is off; a deliberate no-op.
 *                               All counts are zero and notify_user is false.
 *   status == TransientError — a store failure (marker read, orphan load, the
 *                               dedicated ensureOpen() guard, or the recovery
 *                               transaction itself) prevented reconciliation.
 *                               NO row was mutated and the marker (if any)
 *                               was preserved.  message describes the
 *                               failure.  Caller must treat all counts as 0
 *                               and must NOT show a recovery notification.
 *
 * CallerBug is not applicable: every ensureOpen()/open-failure path this
 * method can take is consolidated onto TransientError.
 *
 * policy_dropped_count counts rows deleted for a recovery POLICY reason
 * (expired beyond History retention using recovery's own `now`, cross-day,
 * overlap-rejected, or a durable A4 discard intent) -- never a repaired-row
 * count. Malformed rows (invalid timestamp/
 * type, empty id, non-positive interval) are also deleted as part of the
 * same atomic disposition, but are store-internal cleanup rather than a
 * policy decision: their count is logged only and is not part of this
 * struct.
 *
 * discard_dropped_count is the SUBSET of policy_dropped_count that was
 * force-dropped by a durable A4 discard intent, i.e. work the user
 * explicitly discarded. policy_dropped_count - discard_dropped_count is
 * therefore the number of rows recovery deleted on its own policy
 * (expired-beyond-retention/cross-day/overlap-rejected) -- the only drops a
 * caller should surface to the user.
 */
struct StartupRecoveryResult {
    SessionStoreResult::Category status = SessionStoreResult::Success;
    QString message;
    qint64 recovered_msec = 0;
    int finalized_count = 0;
    int policy_dropped_count = 0;
    int discard_dropped_count = 0;
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

/// One entry per SessionStore virtual operation (C4/R02-003). Exists only so
/// test_contract_matrix.cpp can static_assert its enumerator count against
/// Count -- this only catches a CHANGE in the number of operations (an
/// addition or removal), not an added virtual whose count happens to match;
/// it is a reminder tripwire, not a structural guarantee.
enum class Operation {
    CommitSession, LoadDurations, HasEntriesForDate, SaveCheckpoint,
    DiscardCheckpoint, CheckSchemaOnStartup, FlushToDisc,
    RecoverStartupCheckpoints, MarkCleanShutdown, LoadHistoryRows, SaveHistoryDays, Count
};

class SessionStore
{
public:
    virtual ~SessionStore() = default;

    /// @pre All segment_ids in the timeline must be non-empty
    /// Returns CallerBug (C3/C4) if the payload fails the day-validity/overlap
    /// guard: invalid timestamp, non-positive interval, cross-day interval,
    /// duplicate segment_id, or an Activity/Pause overlap. Rejected before
    /// any mutation. This guard does not separately check for an empty
    /// segment_id: TimeDuration's private constructor mints a fresh id
    /// whenever it is given an empty one (see types.h), so no TimeDuration a
    /// Timeline can carry ever has an empty id -- the precondition above is
    /// structural, not a runtime check. Duplicate ids ARE a runtime check
    /// (C4/R05-003): both the concrete store and FakeSessionStore reject them
    /// as CallerBug before any mutation, so an invalid identity can never be
    /// silently written by either implementation.
    virtual SessionStoreResult commitSession(const Timeline& session) = 0;

    /// Returns all finalized rows sorted chronologically.
    /// result.status is Disabled when history is off, or TransientError on DB
    /// failure. On any non-Success
    /// status, result.durations is empty.
    virtual LoadResult loadDurations() = 0;

    /// Read-only raw query (H5): returns EVERY row in the durations table --
    /// finalized, unfinalized, multi-boundary, and malformed alike -- exactly
    /// as stored, sorted chronologically. Never deletes or repairs a row
    /// (unlike loadDurations(), which deletes malformed rows as a side
    /// effect); callers must never infer a row's absence from this result
    /// meaning it was never persisted -- loadHistoryRows itself never removes
    /// rows. Returns Disabled when history storage is off (rows
    /// empty) or TransientError on DB open/query failure (rows empty).
    virtual HistoryRowsResult loadHistoryRows() = 0;

    /// Atomically applies a scoped, date-limited History edit (H5/WP2), one
    /// transaction covering every day in `days`. `openSnapshot` is the
    /// caller's own loadHistoryRows() snapshot taken when History was
    /// opened -- callers never assemble it ad hoc; TimeTracker supplies it
    /// from its own open-time context.
    ///
    /// CallerBug (checked before any mutation and before the Disabled guard
    /// below -- same precedence as every other write boundary, see the
    /// vocabulary comment above):
    ///   - `days` is empty, two entries share the same date, or a day's span
    ///     is invalid or non-positive;
    ///   - a segment_id is used more than once across every day's deletes
    ///     and upserts combined (this also catches an id both deleted and
    ///     upserted);
    ///   - a deleted id, or an upsert id that also appears in `openSnapshot`,
    ///     is not editable for its day: not finalized, malformed, or its
    ///     interval does not lie fully inside that day's span;
    ///   - an upsert fails DayValidity::validatePayload() or lies outside
    ///     its day's span.
    /// Returns Disabled when history storage is off (no-op). Returns
    /// TransientError if the connection or the transaction cannot be opened.
    ///
    /// Inside the transaction, per day:
    ///   - the day is re-read with the same text predicate hasEntriesForDate()
    ///     uses (`start_utc < dayEnd AND end_utc > dayStart`, both rendered
    ///     as canonical UTC-ISO text) and compared against the matching rows
    ///     of `openSnapshot` (selected with the same text predicate in C++)
    ///     as a multiset of raw columns; any difference rolls back the whole
    ///     transaction and returns Conflict;
    ///   - a new or changed row that overlaps a retained row of the other
    ///     type (the untouched, well-formed re-read rows minus this day's
    ///     deleted/upserted ids) rolls back and returns CallerBug (U3), via
    ///     the shared helper in dayvalidity.h.
    /// Then, for every day: each delete and each update (an upsert id
    /// already present in `openSnapshot`) is matched by
    /// `segment_id AND is_finalized = 1` and must affect exactly one row --
    /// otherwise rollback and Conflict. Every other upsert id is inserted; a
    /// SQL failure there, including a UNIQUE clash, rolls back and returns
    /// TransientError.
    ///
    /// Never touches a row outside the selected day spans, and never
    /// touches an unfinalized row. No backup and no export.
    ///
    /// Canonical-text assumption: every row this app writes stores start_utc/
    /// end_utc as toUtcIso() text (UTC, ISODateWithMs, trailing 'Z'), so text
    /// order equals time order and the text predicate above is equivalent to
    /// the timestamp comparison it stands in for. A non-canonical row (hand-
    /// edited or otherwise malformed) is judged by the same predicate on
    /// both the re-read and the snapshot side, so its inclusion is at least
    /// consistent and a change to it is still detected; the only accepted
    /// gap is such a row whose actual *time* overlaps a day but whose *text*
    /// does not, which falls outside both the stale check and the
    /// new-conflict check. uTimer itself never writes such a row.
    virtual SessionStoreResult saveHistoryDays(const std::vector<StoredHistoryRow>& openSnapshot,
                                               const std::vector<HistoryDayChange>& days) = 0;

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
    /// Returns CallerBug (C3) if [startTime, endTime) is invalid, non-positive,
    /// or crosses local midnight -- rejected before any mutation.
    /// @pre segmentId must be non-empty -- unlike commitSession,
    /// segmentId here is a bare SegmentId, not wrapped in a TimeDuration, so
    /// emptiness is not structurally impossible; both implementations check
    /// it explicitly and return CallerBug (not Disabled or a store failure).
    virtual SessionStoreResult saveCheckpoint(DurationType type, const QDateTime& startTime,
                                              const QDateTime& endTime, const SegmentId& segmentId) = 0;

    /// Deletes the unfinalized checkpoint row(s) for segmentId (is_finalized = 0).
    /// Called when the engine deliberately discards an in-flight segment (e.g. the
    /// cross-midnight watchdog) so the orphaned checkpoint is neither resurrected
    /// by startup recovery nor silently dropped by its retention-expiry policy.
    ///
    /// The DISCARD INTENT itself must be durable across a failure of this call
    /// (R05-001): implementations own recording it (e.g. a store-owned tombstone)
    /// before attempting the physical row delete, and startup recovery must honor
    /// any unresolved intent it finds -- deleting only a matching unfinalized row,
    /// never a finalized one. Returning TransientError therefore does
    /// not mean the intent was lost; the caller retains the segment id and may
    /// call this again -- both the tombstone write and the row delete are
    /// idempotent, so repeated calls are safe.
    /// Returns Disabled when history storage is off (no-op, not an error).
    /// Returns TransientError on DB failure.
    /// @pre segmentId must be non-empty
    virtual SessionStoreResult discardCheckpoint(const SegmentId& segmentId) = 0;

    virtual SchemaStatus checkSchemaOnStartup() = 0;

    /// Forces pending writes to disc.  Returns Disabled when history is off,
    /// TransientError on failure, Success otherwise.
    /// CallerBug is not applicable.
    virtual SessionStoreResult flushToDisc() = 0;

    /// D6 (R05-002/R15-004): the single durable shutdown operation. The sole
    /// owner of "durably mark clean" -- ShutdownCoordinator is the only
    /// caller, and only once TimeTracker::drainForShutdown() has reported a
    /// clean drain (all commit/discard work resolved). Internally:
    ///   1. writes the clean-shutdown marker, timestamped via the store's own
    ///      injected now-provider seam (the same seam D2 gave
    ///      flushToDisc()'s heartbeat write -- see the D6 packet's deviation
    ///      note), not a timestamp supplied by the caller;
    ///   2. only if that write succeeded, executes the FULL durability
    ///      barrier (temporarily promotes to PRAGMA synchronous=FULL, writes
    ///      and flushes, then restores NORMAL) so the marker's own commit is
    ///      covered by the fsync, not left outside it.
    /// Returns Disabled when history is off (no-op, not an error) or
    /// TransientError if either step fails; the barrier is never attempted
    /// after a failed or Disabled marker write, and success requires both
    /// steps to complete. The returned markerWritten (M1) tells the caller
    /// WHICH step a failure came from: false for a marker-write failure
    /// (the record itself was never committed), true for a barrier failure
    /// (the marker WAS committed; only the covering fsync failed) -- callers
    /// must not treat these two failures the same way.
    virtual MarkCleanShutdownResult markCleanShutdown() = 0;

    /// Recovers unfinalized checkpoint rows left by a previous crash or unclean exit.
    /// Internally consumes the last-clean-shutdown marker, applies the retention-
    /// cutoff/cross-day/overlap policies, and finalizes qualifying rows.  Returns
    /// domain facts only — row ids and finalize/drop mechanics are hidden inside
    /// the store.
    /// `now` is used to compute the retention cutoff (now.addDays(-history_days_to_keep_),
    /// same semantics as performRetentionCleanup()'s threshold) and should be the
    /// current time.
    virtual StartupRecoveryResult recoverStartupCheckpoints(const QDateTime& now) = 0;
};

#endif // SESSIONSTORE_H
