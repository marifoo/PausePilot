#ifndef SQLITESESSIONSTORE_H
#define SQLITESESSIONSTORE_H

#include <QObject>
#include <QSqlDatabase>
#include <QDateTime>
#include <QMutex>
#include <deque>
#include <optional>
#include <vector>
#include "types.h"
#include "settings.h"
#include "sessionstore.h"

// saveDurations/createBackup use TransactionMode as an implementation detail.
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
    SessionStoreResult setLastCleanShutdownMarker(const QDateTime& timestamp) override;
    StartupRecoveryResult recoverStartupCheckpoints(const QDateTime& now) override;

    // Non-virtual helpers kept for test seeding and internal use.
    // No longer part of SessionStore — callers that previously used the
    // interface to call these should use commitSession instead.
    bool saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode,
                       const std::vector<QString>& removedSegmentIds = {});
    SessionStoreResult updateDurationsById(const std::deque<TimeDuration>& durations,
                                           const std::vector<QString>& removedSegmentIds = {});

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
    struct ReconcileResult {
        std::vector<long long> finalized;
        std::vector<long long> dropped;
        bool ok = true;
    };
    struct MarkerResult {
        enum class Status { Found, NotFound, Error };
        QDateTime timestamp;  // valid only when status == Found
        Status status = Status::NotFound;
    };

    QSqlDatabase db;
    uint history_days_to_keep_;
    bool db_was_fresh_;  // true if the DB file did not exist when the connection was first opened
    bool schema_creation_failed_ = false;
#ifndef QT_NO_DEBUG
    bool force_reconcile_failure_dbg_ = false;
    // A4 (R05-001) test seam: makes discardCheckpoint()'s physical row-delete
    // step fail after the tombstone write has already committed, so tests can
    // prove the discard intent survives that failure durably.
    bool force_discard_row_delete_failure_dbg_ = false;
#endif

    // Guards all public entry points so that no two operations can interleave.
    // This is particularly important for createBackup(), which closes and
    // reopens the database file: the mutex prevents any concurrent call from
    // hitting a closed connection during that window.  QRecursiveMutex is used
    // because public methods may call each other (e.g. saveDurations →
    // createBackup) and we must not deadlock on re-entry.
    mutable QRecursiveMutex db_mutex_;

    bool ensureOpen();
    bool ensureSchema();
    void closeConnection();
    bool ensureSettingsTable();
    // A4 (R05-001): the discard_intents table is a durable tombstone recording
    // that the engine has deliberately rejected a segment. Additive DDL, safe
    // to (re)create on any DB (mirrors idx_segment_id/idx_start_utc).
    bool ensureDiscardIntentsTable();
    // Loads all durable discard-intent segment ids. nullopt on a real DB error
    // (distinct from "table empty").
    std::optional<std::vector<SegmentId>> loadDiscardIntents();
    // Best-effort: clears resolved discard intents. Failure is logged only --
    // a leftover tombstone is safely re-resolved (no-op) on the next recovery.
    void clearDiscardIntents(const std::vector<SegmentId>& ids);
    BackupResult createBackup(const std::deque<TimeDuration>& durations, TransactionMode mode);
    void performRetentionCleanup();
    void pruneOldBackups(int keepCount = 5);
    bool insertRows(QSqlQuery& query, const std::deque<TimeDuration>& rows);

    // Startup recovery helpers (implementation details of recoverStartupCheckpoints).
    std::optional<std::deque<OrphanCheckpoint>> loadUnfinalizedCheckpoints();
    bool finalizeIfNoOverlap(qint64 rowId, const QDateTime& startUtc, const QDateTime& endUtc);
    ReconcileResult reconcileUnfinalizedCheckpoints(const std::vector<OrphanCheckpoint>& orphansToFinalize,
                                                    const std::vector<long long>& outrightDropIds);
    std::optional<MarkerResult> readLastCleanShutdownMarker();
    bool deleteLastCleanShutdownMarker();

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
    std::deque<OrphanCheckpoint> loadUnfinalizedCheckpoints_dbg() { return loadUnfinalizedCheckpoints().value_or(std::deque<OrphanCheckpoint>{}); }
    bool finalizeIfNoOverlap_dbg(qint64 rowId, const QDateTime& s, const QDateTime& e) {
        return finalizeIfNoOverlap(rowId, s, e);
    }
    ReconcileResult reconcileUnfinalizedCheckpoints_dbg(
        const std::vector<OrphanCheckpoint>& toFinalize,
        const std::vector<long long>& outrightDropIds)
    {
        return reconcileUnfinalizedCheckpoints(toFinalize, outrightDropIds);
    }
    QString dbConnectionName_dbg() const { return db.connectionName(); }

    /// Forces reconcileUnfinalizedCheckpoints to return ok=false, for testing
    /// that recoverStartupCheckpoints does not mutate the marker on failure.
    void setForceReconcileFailure_dbg(bool v) { force_reconcile_failure_dbg_ = v; }

    /// Forces discardCheckpoint()'s row-delete step to fail, for testing that
    /// the discard intent (tombstone) survives durably despite the failure.
    void setForceDiscardRowDeleteFailure_dbg(bool v) { force_discard_row_delete_failure_dbg_ = v; }
#endif // QT_NO_DEBUG

};

#endif // SQLITESESSIONSTORE_H
