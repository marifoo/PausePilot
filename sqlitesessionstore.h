#ifndef SQLITESESSIONSTORE_H
#define SQLITESESSIONSTORE_H

#include <QObject>
#include <QSqlDatabase>
#include <QDateTime>
#include <QMutex>
#include <deque>
#include <optional>
#include <vector>
#include <functional>
#include "types.h"
#include "settings.h"
#include "sessionstore.h"

// createBackup uses TransactionMode as an implementation detail.
// The enum is not part of SessionStore's public interface; it lives here
// so it can be removed from types.h.
enum class TransactionMode { Append, Replace };

// Three-way result returned by createBackup().
//   Success      — file copied and DB reopened.
//   BackupFailed — file copy failed; DB connection state is unchanged (caller
//                  may proceed without a backup — data is not at risk yet).
//   ReopenFailed — DB could not be reopened after the close-copy step; caller
//                  must abort the pending write — the connection is dead.
enum class BackupResult { Success, BackupFailed, ReopenFailed };

// Connection lifecycle: the database is opened in the constructor
// (ensureSchema creates tables, indexes, and sets PRAGMA once) and
// closed in the destructor.  Retention cleanup and backup pruning run once per
// startup via checkSchemaOnStartup() — safe to call more than once (idempotent),
// but intended to be called once.  All public methods call ensureOpen() as a
// lightweight health check; if the connection is unexpectedly lost, ensureOpen()
// reopens it transparently.

class SqliteSessionStore : public QObject, public SessionStore
{
    Q_OBJECT
public:
    explicit SqliteSessionStore(const Settings& settings, QObject *parent = nullptr);
    ~SqliteSessionStore();

    SessionStoreResult commitSession(const Timeline& session) override;
    SessionStoreResult replaceAll(const Timeline& history, const Timeline& session) override;
    LoadResult loadDurations() override;
    EntriesForDateResult hasEntriesForDate(const QDate& date) override;
    SessionStoreResult saveCheckpoint(DurationType type, const QDateTime& startTime, const QDateTime& endTime, const SegmentId& segmentId) override;
    SessionStoreResult discardCheckpoint(const SegmentId& segmentId) override;
    SchemaStatus checkSchemaOnStartup() override;
    SessionStoreResult flushToDisc() override;
    MarkCleanShutdownResult markCleanShutdown() override;
    StartupRecoveryResult recoverStartupCheckpoints(const QDateTime& now) override;

private:
    // Private types used by startup recovery helpers.
    struct OrphanCheckpoint {
        long long id = -1;
        SegmentId segment_id;
        DurationType type = DurationType::Activity;
        qint64 duration = 0;
        QDateTime startTime;
        QDateTime endTime;
    };
    // B2 (R08-003): loadUnfinalizedCheckpoints() classifies EVERY unfinalized
    // row before any mutation. `valid` are candidates for the normal
    // policy/finalize path; `malformedIds` (invalid timestamp/type, empty id,
    // non-positive interval) are explicit delete work owned by the store --
    // never a silent omission.
    struct OrphanLoadResult {
        std::deque<OrphanCheckpoint> valid;
        std::vector<long long> malformedIds;
    };
    // R07-002 (B1): typed outcome for finalizeIfNoOverlap() so a deliberate
    // policy decision (overlap rejection, missing row) can never be reported
    // as -- or silently substitute for -- a real transaction/SQL failure.
    enum class FinalizeOutcome { Finalized, OverlapRejected, Missing, Error };
    struct FinalizeResult {
        FinalizeOutcome outcome = FinalizeOutcome::Error;
        QString errorMessage;  // populated only when outcome == Error
    };
    // B2: policy_dropped counts every row deleted for a recovery POLICY
    // reason -- outright (too-short/stale/future/A4 discard-forced) drops
    // supplied by the caller AND overlap-rejected rows discovered while
    // finalizing (R08-002: these are now physically deleted here, not left
    // unfinalized to recur every launch). malformed_deleted is a logged-only
    // count, matching StartupRecoveryResult's public/internal split.
    struct ReconcileResult {
        std::vector<long long> finalized;
        std::vector<long long> policy_dropped;
        int malformed_deleted = 0;
        bool ok = true;
    };
    struct MarkerResult {
        enum class Status { Found, NotFound, Error };
        QDateTime timestamp;  // valid only when status == Found
        Status status = Status::NotFound;
    };
    // C2 (R07-001 round 2): commitOrRollback()'s result. `committed` is the
    // single source of truth callers must branch on -- db.commit() can
    // return false without ever calling setLastError(), so an empty
    // errorMessage must never be read as success. errorMessage is populated
    // only when committed is false.
    struct CommitOutcome {
        bool committed = false;
        QString errorMessage;
    };

    QSqlDatabase db;
    /// D2/MEDIUM-7: the store's own now-provider seam, matching
    /// TimeTracker's now_provider_ pattern -- used wherever a persisted
    /// decision reads the clock directly (retention cutoff, flush
    /// timestamp), so tests can inject a controllable clock instead of the
    /// real one. Defaults to the real UTC clock.
    std::function<QDateTime()> now_provider_ = QDateTime::currentDateTimeUtc;
    uint history_days_to_keep_;
    bool db_was_fresh_;  // true if the DB file did not exist when the connection was first opened
    bool schema_creation_failed_ = false;
#ifndef QT_NO_DEBUG
    bool force_reconcile_failure_dbg_ = false;
    // A4 (R05-001) test seam: makes discardCheckpoint()'s physical row-delete
    // step fail after the tombstone write has already committed, so tests can
    // prove the discard intent survives that failure durably.
    bool force_discard_row_delete_failure_dbg_ = false;
    // C2 (R07-001) test seam: makes the next commitOrRollback() call treat
    // COMMIT as having failed, without touching the real connection, so
    // tests can prove the resulting rollback restores the connection to a
    // healthy, autocommit-ready state. Only consumed if control reaches
    // commitOrRollback(); if the caller's own operation fails earlier (e.g.
    // an empty id, an UPDATE/INSERT error), the flag stays armed and trips
    // the NEXT commit on this store instance instead. Self-resetting once
    // consumed.
    bool force_commit_failure_dbg_ = false;
    // C2 (R07-001) test seam: combined with force_commit_failure_dbg_, makes
    // the rollback that follows the injected commit failure ALSO genuinely
    // fail -- without calling db.rollback() or closing the connection itself,
    // so the real transaction is left genuinely open and it is
    // commitOrRollback()'s own cleanup (not this seam) that must close it.
    // Same "only consumed if reached" caveat as above: it is checked only
    // after a commit failure, so if armed while no commit fails it stays
    // armed for the next one.
    bool force_rollback_failure_dbg_ = false;
    // C2 (R07-001 round 2) test seam: closes the real connection immediately
    // before commitOrRollback() calls the real db.commit(), reproducing the
    // "commit after close" failure mode observed empirically on Qt 5.15.18:
    // QSqlDatabase::commit()/QSQLiteDriver::commitTransaction() bail on an
    // unopened driver and return false WITHOUT ever calling setLastError(),
    // so db.lastError().text() is genuinely empty -- unlike
    // force_commit_failure_dbg_, which fakes a non-empty message. Proves
    // callers must branch on CommitOutcome::committed, not on
    // errorMessage.isEmpty(). Self-resetting once consumed.
    bool force_commit_close_before_dbg_ = false;
    // D8 (R06-003) test seams: make ensureSettingsTable()'s rebuild fail at
    // the DROP or CREATE step (independent of commitOrRollback's own COMMIT
    // seam above), so tests can prove the old table/marker survive each
    // possible failure point of the rebuild. Self-resetting once consumed.
    // D11 scope: these two, like every other force_*_dbg_ seam above, belong
    // to the debug-probe surface D11 strips once production callers no
    // longer need direct SQL/state seeding.
    bool force_settings_drop_failure_dbg_ = false;
    bool force_settings_create_failure_dbg_ = false;
    // D8 review round 2 (finding 7ii): a rebuild-scoped alternative to
    // arming the shared force_commit_failure_dbg_ directly. That flag is
    // consumed by whichever commitOrRollback() call is reached FIRST on the
    // store instance; arming it before checkSchemaOnStartup() happens to
    // land on ensureSettingsTable()'s rebuild today only because nothing
    // else commits earlier on that path. This seam is consumed only at
    // ensureSettingsTable()'s own commit site (it arms
    // force_commit_failure_dbg_ there, immediately before that call), so the
    // test stays correct even if an earlier commit is ever added to the
    // path. D11 scope, same as the two seams above.
    bool force_settings_commit_failure_dbg_ = false;
#endif

    // Guards all public entry points so that no two operations can interleave.
    // This is particularly important for createBackup(), which closes and
    // reopens the database file: the mutex prevents any concurrent call from
    // hitting a closed connection during that window.  QRecursiveMutex is used
    // because public methods may call each other (e.g. replaceAll →
    // createBackup) and we must not deadlock on re-entry.
    mutable QRecursiveMutex db_mutex_;

    // D11 (D6 debt/R09-002): no longer part of the SessionStore interface --
    // only markCleanShutdown() may call this, and that rule is now enforced
    // structurally (private, non-virtual) rather than by comment alone.
    // Writes the clean-shutdown marker timestamp. Returns Disabled when
    // history is off (no-op, not an error). Returns TransientError on write
    // failure. CallerBug is not applicable.
    SessionStoreResult setLastCleanShutdownMarker(const QDateTime& timestamp);

    bool ensureOpen();
    bool ensureSchema();
    void closeConnection();
    // D8 (R06-004): applies every connection-local PRAGMA the store requires
    // for the lifetime of a native SQLite connection (currently just
    // synchronous=NORMAL) and reads it back to verify it took effect. Shared
    // by construction, ensureOpen()'s transparent reconnect, and
    // createBackup()'s reopen, so a reconnect can no longer silently drift
    // from the configured policy. Failures are logged and non-fatal,
    // matching this PRAGMA's pre-existing best-effort treatment.
    bool applyConnectionPragmas();
    // D8 (R06-002): proves the database directory can host SQLite's rollback
    // journal, not just that the file's own permission bits are writable.
    // Runs a real write (guaranteed to dirty a page, unlike an UPDATE that
    // may match zero rows) inside a transaction that is always rolled back.
    bool verifyWriteCapability();
    bool ensureSettingsTable();
    // A4 (R05-001): the discard_intents table is a durable tombstone recording
    // that the engine has deliberately rejected a segment. Additive DDL, safe
    // to (re)create on any DB (mirrors idx_segment_id/idx_start_utc).
    bool ensureDiscardIntentsTable();
    // Loads all durable discard-intent segment ids. nullopt on a real DB error
    // (distinct from "table empty").
    std::optional<std::vector<SegmentId>> loadDiscardIntents();
    BackupResult createBackup(const std::deque<TimeDuration>& durations, TransactionMode mode);
    // Path of the most recent successful createBackup() copy; empty if none
    // has run yet. Set for test observability (lastBackupPath_dbg() below)
    // so a test can assert against the exact file createBackup() wrote
    // instead of scanning the data directory.
    QString lastBackupPath_;
    void performRetentionCleanup();
    void pruneOldBackups(int keepCount = 5);
    bool insertRows(QSqlQuery& query, const std::deque<TimeDuration>& rows);
    // D11 (R07-005/R09-002): no longer part of the public surface -- its only
    // caller is commitSession(). Kept private; every test call site now goes
    // through commitSession()/replaceAll() or a scoped raw-SQL fixture.
    SessionStoreResult updateDurationsById(const std::deque<TimeDuration>& durations,
                                           const std::vector<QString>& removedSegmentIds = {});
    // C2 (R07-001): commits the currently open transaction, rolling back on
    // any failure -- including the injected debug faults above -- so a
    // failed COMMIT never leaves the long-lived connection inside its
    // transaction. If the rollback itself also fails, closes the connection
    // so the next ensureOpen() reconnects clean instead of every later
    // db.transaction() failing forever. Returns committed=true on success;
    // otherwise committed=false and errorMessage carries the primary
    // commit-failure message, captured before rollback (mirroring
    // finalizeIfNoOverlap's pattern) so a rollback failure is logged
    // separately instead of overwriting the caller-visible error. Callers
    // MUST branch on `committed`, not on errorMessage.isEmpty(): db.commit()
    // can return false without calling setLastError() (e.g. commit after the
    // connection was closed), which would otherwise read as success.
    // logContext names the caller for the log line, matching each site's
    // pre-existing message text.
    CommitOutcome commitOrRollback(const QString& logContext);

    // Startup recovery helpers (implementation details of recoverStartupCheckpoints).
    // B2: loads AND classifies every unfinalized row before any externally
    // visible mutation (R08-003) -- malformed rows are never a silent omission.
    std::optional<OrphanLoadResult> loadUnfinalizedCheckpoints();
    // Self-transacting single-row finalize (BEGIN/COMMIT/ROLLBACK owned here).
    // Kept for direct/isolated use (finalizeIfNoOverlap_dbg); B2's atomic
    // recovery transaction instead calls finalizeOrOverlapWithinOpenTransaction
    // directly so the whole disposition shares one transaction.
    FinalizeResult finalizeIfNoOverlap(qint64 rowId, const QDateTime& startUtc, const QDateTime& endUtc);
    // Pure probe-and-finalize logic (no BEGIN/COMMIT/ROLLBACK of its own);
    // assumes the caller already holds an open transaction.
    FinalizeResult finalizeOrOverlapWithinOpenTransaction(qint64 rowId, const QDateTime& startUtc, const QDateTime& endUtc);
    // B2: assumes the caller already holds an open transaction (recoverStartupCheckpoints
    // folds this into its single atomic disposition). Deletes malformedIds and
    // policyDropIds outright, then finalizes each orphansToFinalize candidate --
    // deleting it instead if overlap-rejected (R08-002). A Missing outcome (row
    // vanished since load, impossible in this single-process flow) is treated as
    // an internal error, same as a real SQL failure: ok=false, stop immediately.
    ReconcileResult reconcileUnfinalizedCheckpoints(const std::vector<OrphanCheckpoint>& orphansToFinalize,
                                                    const std::vector<long long>& policyDropIds,
                                                    const std::vector<long long>& malformedIds);
    std::optional<MarkerResult> readLastCleanShutdownMarker();
    // Assumes the caller already holds an open transaction; does not commit
    // or roll back. Replaces the old self-transacting deleteLastCleanShutdownMarker()
    // now that marker consumption is folded into recoverStartupCheckpoints'
    // single atomic transaction (R08-001).
    bool deleteMarkerRow();

#ifndef QT_NO_DEBUG
    /// Debug-build verification: checks that no segment_id appears more than
    /// once in the durations table.  Called after every successful write
    /// operation.  Violations are logged via qWarning (not fatal).
    /// The database must be open when this is called.
    void checkSegmentIdUniqueness();

public:
    // ---- Debug-build test probes ----
    // Expose internal DB connection management for test seeding via raw SQL.
    // NOT compiled in release builds.

    /// Opens (or reopens) the database connection. Returns true on success.
    /// Exposed for tests that need to seed the DB via rawDb_dbg() directly.
    bool ensureOpen_dbg() { return ensureOpen(); }

    /// Closes the database connection immediately.
    /// Call after raw SQL seeding to release the connection for the next open.
    void closeConnection_dbg() { closeConnection(); }

    /// Returns the raw QSqlDatabase handle for test-only SQL seeding.
    /// The connection must be open (call ensureOpen_dbg() first).
    QSqlDatabase& rawDb_dbg() { return db; }

    /// Exposes private types and methods for white-box tests of recovery internals.
    using OrphanCheckpoint_dbg = OrphanCheckpoint;
    using ReconcileResult_dbg  = ReconcileResult;
    using FinalizeOutcome_dbg  = FinalizeOutcome;
    using FinalizeResult_dbg   = FinalizeResult;
    std::deque<OrphanCheckpoint> loadUnfinalizedCheckpoints_dbg() {
        const auto r = loadUnfinalizedCheckpoints();
        return r.has_value() ? r->valid : std::deque<OrphanCheckpoint>{};
    }
    /// Malformed-row ids (invalid timestamp/type, empty id, non-positive
    /// interval) found by the SAME load that loadUnfinalizedCheckpoints_dbg()
    /// just exposed. Re-runs the load; only for tests that need both halves.
    std::vector<long long> loadUnfinalizedCheckpointsMalformedIds_dbg() {
        const auto r = loadUnfinalizedCheckpoints();
        return r.has_value() ? r->malformedIds : std::vector<long long>{};
    }
    FinalizeResult finalizeIfNoOverlap_dbg(qint64 rowId, const QDateTime& s, const QDateTime& e) {
        return finalizeIfNoOverlap(rowId, s, e);
    }
    /// Wraps reconcileUnfinalizedCheckpoints() (which itself assumes an
    /// already-open transaction, matching how recoverStartupCheckpoints()
    /// uses it) in its own BEGIN/COMMIT-or-ROLLBACK so isolated tests can
    /// call it standalone, exactly mirroring production usage.
    ReconcileResult reconcileUnfinalizedCheckpoints_dbg(
        const std::vector<OrphanCheckpoint>& toFinalize,
        const std::vector<long long>& policyDropIds,
        const std::vector<long long>& malformedIds = {})
    {
        if (!ensureOpen() || !db.transaction()) {
            ReconcileResult r;
            r.ok = false;
            return r;
        }
        ReconcileResult result = reconcileUnfinalizedCheckpoints(toFinalize, policyDropIds, malformedIds);
        if (result.ok) {
            if (!db.commit()) {
                db.rollback();
                result.ok = false;
            }
        } else {
            db.rollback();
        }
        return result;
    }
    QString dbConnectionName_dbg() const { return db.connectionName(); }

    /// The path createBackup() last copied to on success; empty if no backup
    /// has been written yet. Lets a test identify the exact backup file
    /// instead of scanning the data directory for one.
    QString lastBackupPath_dbg() const { return lastBackupPath_; }

    /// Exposes the private marker-write step directly for tests that need to
    /// exercise ITS OWN Success/Disabled/TransientError contract, without
    /// going through the full markCleanShutdown() durability barrier.
    /// Production code must reach this only via markCleanShutdown() (D11).
    SessionStoreResult setLastCleanShutdownMarker_dbg(const QDateTime& timestamp) {
        return setLastCleanShutdownMarker(timestamp);
    }

    /// D2/MEDIUM-7: overrides now_provider_ for deterministic tests of
    /// retention cutoff / flush timestamp without waiting on real time.
    void setNowProvider_dbg(std::function<QDateTime()> provider) { now_provider_ = std::move(provider); }

    /// Forces reconcileUnfinalizedCheckpoints to return ok=false, for testing
    /// that recoverStartupCheckpoints does not mutate the marker on failure.
    void setForceReconcileFailure_dbg(bool v) { force_reconcile_failure_dbg_ = v; }

    /// Forces discardCheckpoint()'s row-delete step to fail, for testing that
    /// the discard intent (tombstone) survives durably despite the failure.
    void setForceDiscardRowDeleteFailure_dbg(bool v) { force_discard_row_delete_failure_dbg_ = v; }

    /// Forces the next commitOrRollback() call to treat COMMIT as failed and
    /// roll back, without touching the real connection. Only consumed once
    /// control reaches commitOrRollback() -- arm it immediately before a
    /// call expected to reach commit, or it stays armed and affects a later
    /// one instead. Self-resetting once consumed.
    void setForceCommitFailure_dbg(bool v) { force_commit_failure_dbg_ = v; }

    /// Combined with setForceCommitFailure_dbg(true): makes the rollback
    /// that follows also genuinely fail, without calling db.rollback() or
    /// closing the connection itself -- the real transaction is left open
    /// for commitOrRollback()'s own cleanup to close. Only consumed once a
    /// commit failure is reached; same arming caveat as above.
    void setForceRollbackFailure_dbg(bool v) { force_rollback_failure_dbg_ = v; }

    /// Forces the next commitOrRollback() call to close the real connection
    /// immediately before calling the real db.commit(), so that call
    /// genuinely fails with an EMPTY db.lastError().text() -- reproducing
    /// the "commit after close" failure mode that force_commit_failure_dbg_
    /// (which fakes a non-empty message) cannot. Only consumed once control
    /// reaches commitOrRollback(); same arming caveat as the seams above.
    void setForceCommitCloseBefore_dbg(bool v) { force_commit_close_before_dbg_ = v; }

    /// Forces the next ensureSettingsTable() rebuild's DROP TABLE step to
    /// fail, without touching the real connection. Self-resetting once
    /// consumed (only if a rebuild is actually reached). D11 scope: part of
    /// the debug-probe surface to strip once production callers no longer
    /// need direct SQL/state seeding.
    void setForceSettingsDropFailure_dbg(bool v) { force_settings_drop_failure_dbg_ = v; }

    /// Forces the next ensureSettingsTable() rebuild's CREATE TABLE step to
    /// fail (DROP succeeds), without touching the real connection.
    /// Self-resetting once consumed. D11 scope, same as the setter above.
    void setForceSettingsCreateFailure_dbg(bool v) { force_settings_create_failure_dbg_ = v; }

    /// Forces ensureSettingsTable()'s own rebuild COMMIT to fail, scoped to
    /// that call site specifically (unlike setForceCommitFailure_dbg(),
    /// which is consumed by whichever commitOrRollback() call is reached
    /// first). Self-resetting once consumed (only if a rebuild is actually
    /// reached). D11 scope, same as the two setters above.
    void setForceSettingsCommitFailure_dbg(bool v) { force_settings_commit_failure_dbg_ = v; }
#endif // QT_NO_DEBUG

};

#endif // SQLITESESSIONSTORE_H
