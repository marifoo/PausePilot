/**
 * SqliteSessionStore - SQLite persistence for time duration data.
 *
 * Connection pattern:
 * - Long-lived: Database opened in the constructor, closed in the destructor.
 * - ensureOpen() is called at the start of every public method as a health
 *   check; if the connection was closed (e.g. by createBackup's copy window),
 *   it will be reopened transparently.
 *
 * Thread safety:
 * - A QRecursiveMutex (db_mutex_) guards every public entry point, preventing
 *   concurrent operations from interleaving.  This is essential because
 *   createBackup() temporarily closes and reopens the database connection;
 *   without the mutex, a concurrent call could hit a closed connection.
 *   QRecursiveMutex (rather than QMutex) is used because public methods
 *   call private helpers that are also called from other public methods.
 *
 * Save methods:
 * - saveDurations(): Full save with backup creation, used for stopTimer/HistoryDialog
 * - saveCheckpoint(): Lightweight update without backup, used for periodic checkpoints
 * - updateDurationsById(): Smart upsert matching entries by stable segment_id
 *
 * Backup strategy:
 * - saveDurations() creates timestamped .backup file + .durations.txt log before writes
 * - saveCheckpoint() skips backup for performance (called every 5 minutes)
 *
 * Configuration:
 * - history_days_to_keep_ = 0 disables database entirely (all methods return early)
 * - Old entries are purged once per startup via checkSchemaOnStartup() →
 *   performRetentionCleanup(), not in the constructor.
 */

#include "sqlitesessionstore.h"
#include "apppaths.h"
#include <QMutexLocker>
#include <QSet>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTextStream>
#include <algorithm>
#include <atomic>
#include "logger.h"

namespace {
const char* kLastCleanShutdownKey = "last_clean_shutdown";
constexpr qint64 kMinRecoverableOrphanDurationMs = 1000;
constexpr qint64 kOrphanStaleAgeMs = 24LL * 60LL * 60LL * 1000LL;
// A backward clock jump can make an orphan's endTime land after "now" at
// recovery time (endTime.msecsTo(now) negative, defeating the stale check
// below). A small slack tolerates normal timestamp/tick granularity without
// misclassifying genuinely-fresh orphans; anything further in the future is
// a clock-jump artifact and must be dropped, not resurrected.
constexpr qint64 kOrphanFutureSlackMs = 60LL * 1000LL;

static QString toUtcIso(const QDateTime& dt)
{
    return dt.toUTC().toString(Qt::ISODateWithMs);
}

// Monotonically increasing counter used to mint unique SQLite connection names.
// Using a counter instead of the object address (reinterpret_cast<quintptr>(this))
// avoids name collisions when a SqliteSessionStore is destroyed and a new one is
// allocated at the same address — which is perfectly legal and surprisingly common
// in loops or test harnesses.
std::atomic<uint64_t> s_connection_seq{0};
}

SqliteSessionStore::SqliteSessionStore(const Settings& settings, QObject *parent)
    : QObject(parent), history_days_to_keep_(std::max(settings.getHistoryDays(), 0)),
      db_was_fresh_(false)
{
    // Mint a unique connection name using a monotonic counter.
    // See s_connection_seq declaration for rationale (avoids address-reuse collisions).
    QString connectionName = QString("uTimer_connection_%1").arg(s_connection_seq.fetch_add(1, std::memory_order_relaxed));
    db = QSqlDatabase::addDatabase("QSQLITE", connectionName);

    db.setDatabaseName(AppPaths::databaseFile());

    if (history_days_to_keep_ == 0) {
        Logger::Log("[DB] History days to keep is set to 0, database will not be used.");
        return;
    }

    // Record whether this is a fresh DB before opening.
    db_was_fresh_ = !QFile::exists(db.databaseName());

    // Open the connection.
    if (!db.open()) {
        Logger::Log("[DB] Error opening database: " + db.lastError().text());
        return;
    }
    // Reject read-only files immediately.  Without this check, db.open() can
    // succeed on a read-only SQLite file (opening in read-only mode), leaving
    // the connection open but unable to write — the same guard ensureOpen()
    // applies on reconnect.
    if (!QFileInfo(db.databaseName()).isWritable()) {
        Logger::Log("[DB] Warning: Database file is not writable.");
        db.close();
        return;
    }

    if (db_was_fresh_) {
        // Fresh DB: build the full schema now.  checkSchemaOnStartup() will
        // see db_was_fresh_ == true and skip validation, returning Created.
        if (!ensureSchema()) {
            schema_creation_failed_ = true;
            db.close();
        }
    } else {
        // Existing DB: no DDL until checkSchemaOnStartup() validates the schema.
        // Set PRAGMA synchronous=NORMAL so writes on this connection behave
        // correctly even before the schema is confirmed.
        QSqlQuery syncQuery(db);
        if (!syncQuery.exec("PRAGMA synchronous=NORMAL")) {
            Logger::Log("[DB] Warning: Failed to set synchronous=NORMAL: " + syncQuery.lastError().text());
        }
    }
}

SqliteSessionStore::~SqliteSessionStore()
{
    closeConnection();
    
    QString connName = db.connectionName();
    db = QSqlDatabase();
    
    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase::removeDatabase(connName);
    }
}

/**
 * Lightweight connection health check.
 *
 * Returns true immediately if the connection is already open.  If the
 * connection was lost unexpectedly (e.g. after an OS-level file error),
 * reopens it and returns true.  Schema migrations, indexes, and PRAGMA are NOT
 * re-applied here — those run via ensureSchema() (called from the constructor
 * and idempotently from checkSchemaOnStartup()).
 *
 * After reopening, verifies the file is writable at the OS level.  SQLite can
 * open a read-only file in read-only mode (db.open() returns true), but all
 * subsequent write calls would fail with SQLITE_READONLY on the same connection.
 * Closing and returning false here forces the caller to treat it as a DB failure
 * and try again once write access is restored.  Note: isWritable() checks
 * permission bits only — it cannot detect immutable-bit or SELinux-denied writes.
 */
bool SqliteSessionStore::ensureOpen()
{
    if (history_days_to_keep_ == 0) {
        return false;
    }
    if (db.isOpen()) {
        return true;
    }
    if (!db.open()) {
        Logger::Log("[DB] Error reopening database: " + db.lastError().text());
        return false;
    }
    if (!QFileInfo(db.databaseName()).isWritable()) {
        db.close();
        return false;
    }
    return true;
}

/**
 * Idempotent schema setup: creates the durations and app_settings tables (if
 * needed), creates indexes, and sets PRAGMA synchronous=NORMAL.  All DDL uses
 * IF NOT EXISTS so repeated calls are safe.
 *
 * Journal mode note: this app uses SQLite's default rollback journal mode, NOT
 * WAL.  WAL's concurrent-readers advantage is irrelevant for a single-process
 * app.  Rollback journal keeps the database as a single file (no -wal/-shm
 * sidecars), which simplifies close-copy-reopen backups and portability.
 *
 * Returns false on DDL failures (caller closes the connection).
 * Non-fatal failures (index creation, PRAGMA) are logged and allowed through —
 * the DB is still usable for reads and writes.
 */
bool SqliteSessionStore::ensureSchema()
{
    QSqlQuery query_new(db);
    bool schemaOk = query_new.exec(
        "CREATE TABLE IF NOT EXISTS durations ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "segment_id TEXT NOT NULL CHECK(length(segment_id) > 0),"
        "type INTEGER NOT NULL,"
        "start_utc TEXT NOT NULL,"
        "end_utc TEXT NOT NULL,"
        "is_finalized INTEGER NOT NULL DEFAULT 0,"
        "UNIQUE(segment_id)"
        ")"
    );

    if (!schemaOk) {
        Logger::Log("[DB] Error creating table: " + query_new.lastError().text());
        return false;
    }

    if (!ensureSettingsTable()) {
        return false;
    }

    QSqlQuery segmentIndexQuery(db);
    if (!segmentIndexQuery.exec("CREATE INDEX IF NOT EXISTS idx_segment_id ON durations(segment_id)")) {
        Logger::Log("[DB] Warning: Failed to create segment_id index: " + segmentIndexQuery.lastError().text());
    }

    QSqlQuery startUtcIndexQuery(db);
    if (!startUtcIndexQuery.exec("CREATE INDEX IF NOT EXISTS idx_start_utc ON durations(start_utc) WHERE is_finalized = 1")) {
        Logger::Log("[DB] Warning: Failed to create start_utc index: " + startUtcIndexQuery.lastError().text());
    }

    // PRAGMA synchronous=NORMAL: in rollback journal mode this provides good
    // crash safety (only OS crash / power loss can lose data, not an app crash).
    // The shutdown path (flushToDisc) temporarily promotes to FULL for the
    // final durable write before the process exits.
    QSqlQuery syncQuery(db);
    if (!syncQuery.exec("PRAGMA synchronous=NORMAL")) {
        Logger::Log("[DB] Warning: Failed to set synchronous=NORMAL: " + syncQuery.lastError().text());
    }

    return true;
}

void SqliteSessionStore::closeConnection()
{
    if (db.isOpen()) {
        db.close();
    }
}

// ============================================================================
// Debug-build database invariant checking
// ============================================================================

#ifndef QT_NO_DEBUG

/**
 * Verifies that no segment_id appears more than once in the durations table.
 *
 * Called after every successful write operation in debug builds. The
 * UNIQUE(segment_id) constraint should prevent duplicates at the schema
 * level, but this check catches bugs where a write path accidentally
 * inserts a row without going through the constraint (e.g. using a
 * different table name, or a migration path that bypasses the constraint).
 *
 * The database must be open when this is called.  Violations are logged
 * via qWarning (not fatal) so they surface in test output.
 */
void SqliteSessionStore::checkSegmentIdUniqueness()
{
    if (!db.isOpen()) {
        return;
    }

    QSqlQuery query(db);
    if (!query.exec("SELECT segment_id, COUNT(*) as cnt FROM durations GROUP BY segment_id HAVING COUNT(*) > 1")) {
        qWarning("[INVARIANT-DB] Failed to run segment_id uniqueness check: %s",
                 qPrintable(query.lastError().text()));
        return;
    }

    while (query.next()) {
        QString segId = query.value(0).toString();
        int count = query.value(1).toInt();
        qWarning("[INVARIANT-DB] Duplicate segment_id detected: '%s' appears %d times",
                 qPrintable(segId), count);
    }
}

#endif // QT_NO_DEBUG

/**
 * Schema validation + startup housekeeping.  Call once at application startup,
 * before any recovery or data access.
 *
 * Returns:
 *   Created      — DB file did not previously exist; fresh schema was built.
 *   Ready        — existing DB has the expected schema; safe to use.
 *   Outdated     — column set or UNIQUE(segment_id) constraint does not match;
 *                  caller must refuse to start.
 *   Inaccessible — DB file could not be opened.
 *
 * idx_start_utc: if the partial index is absent it is (re)created rather than
 * returning Outdated, because the index can be rebuilt without data loss and
 * its absence does not indicate schema incompatibility.
 */
SchemaStatus SqliteSessionStore::checkSchemaOnStartup()
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return SchemaStatus::Ready;
    }

    if (db_was_fresh_ && schema_creation_failed_) {
        Logger::Log("[DB] checkSchemaOnStartup: fresh-DB schema creation failed earlier — refusing to start");
        return SchemaStatus::Inaccessible;
    }

    if (!ensureOpen()) {
        return SchemaStatus::Inaccessible;
    }

    if (db_was_fresh_) {
        // Fresh DB: ensureSchema() already ran in the constructor.
        performRetentionCleanup();
        pruneOldBackups();
        return SchemaStatus::Created;
    }

    // Validate the durations table columns using PRAGMA table_info.
    // The expected set must match what ensureSchema() creates.
    {
        static const QSet<QString> kExpectedColumns = {
            "id", "segment_id", "type", "start_utc", "end_utc", "is_finalized"
        };

        QSqlQuery colQuery(db);
        if (!colQuery.exec("PRAGMA table_info(durations)")) {
            Logger::Log("[DB] checkSchemaOnStartup: PRAGMA table_info failed: " + colQuery.lastError().text());
            return SchemaStatus::Inaccessible;
        }

        QSet<QString> foundColumns;
        while (colQuery.next()) {
            foundColumns.insert(colQuery.value(1).toString()); // column 1 = name
        }

        if (foundColumns != kExpectedColumns) {
            Logger::Log(QString("[DB] checkSchemaOnStartup: column mismatch. "
                                "Expected {%1}, found {%2}")
                .arg(QStringList(kExpectedColumns.values()).join(", "))
                .arg(QStringList(foundColumns.values()).join(", ")));
            return SchemaStatus::Outdated;
        }
    }

    // Validate UNIQUE(segment_id) constraint via PRAGMA index_list + index_info.
    {
        QSqlQuery idxList(db);
        if (!idxList.exec("PRAGMA index_list(durations)")) {
            Logger::Log("[DB] checkSchemaOnStartup: PRAGMA index_list failed: " + idxList.lastError().text());
            return SchemaStatus::Inaccessible;
        }

        // Collect all unique indexes and check their covered columns.
        bool foundUniqueSegmentId = false;
        while (idxList.next()) {
            // Columns: seq, name, unique, origin, partial
            const QString idxName   = idxList.value(1).toString();
            const int isUnique      = idxList.value(2).toInt();
            if (!isUnique) {
                continue;
            }
            // Check if this unique index covers exactly segment_id.
            QSqlQuery idxInfo(db);
            idxInfo.prepare("PRAGMA index_info(" + idxName + ")");
            if (!idxInfo.exec()) {
                continue;
            }
            QStringList coveredCols;
            while (idxInfo.next()) {
                coveredCols.append(idxInfo.value(2).toString()); // column 2 = name
            }
            if (coveredCols.size() == 1 && coveredCols[0] == "segment_id") {
                foundUniqueSegmentId = true;
                break;
            }
        }

        if (!foundUniqueSegmentId) {
            Logger::Log("[DB] checkSchemaOnStartup: missing UNIQUE(segment_id) constraint");
            return SchemaStatus::Outdated;
        }
    }

    // Additive DDL that is safe to (re)create on any validated existing DB.
    // These objects are absent when upgrading from older app versions that
    // pre-date app_settings / idx_segment_id.  Running them here (after
    // validation passes) ensures no DDL touches an incompatible DB.
    if (!ensureSettingsTable()) {
        Logger::Log("[DB] Warning: Failed to ensure app_settings table on existing DB");
        // Non-fatal: log and continue; the durations table is the critical one.
    }

    {
        QSqlQuery segmentIndexQuery(db);
        if (!segmentIndexQuery.exec("CREATE INDEX IF NOT EXISTS idx_segment_id ON durations(segment_id)")) {
            Logger::Log("[DB] Warning: Failed to create segment_id index: " + segmentIndexQuery.lastError().text());
        }
    }

    // Recreate idx_start_utc if absent (the partial index is safe to rebuild;
    // its absence does not indicate schema incompatibility).
    {
        QSqlQuery startUtcIndexQuery(db);
        if (!startUtcIndexQuery.exec("CREATE INDEX IF NOT EXISTS idx_start_utc ON durations(start_utc) WHERE is_finalized = 1")) {
            Logger::Log("[DB] Warning: Failed to recreate idx_start_utc: " + startUtcIndexQuery.lastError().text());
            // Non-fatal: log and continue.
        }
    }

    // Retention cleanup and backup pruning run once per startup, after schema is
    // confirmed valid.  Failures are non-fatal (logged, but do not abort startup).
    performRetentionCleanup();
    pruneOldBackups();

    return SchemaStatus::Ready;
}

/**
 * Deletes entries older than history_days_to_keep_ from the durations table.
 * Called once per startup from checkSchemaOnStartup() after schema validation.
 * Non-fatal: failures are logged but do not propagate to the caller.
 */
void SqliteSessionStore::performRetentionCleanup()
{
    if (!db.transaction()) {
        Logger::Log("[DB] Error starting retention cleanup transaction: " + db.lastError().text());
        return;
    }
    const QDateTime threshold = QDateTime::currentDateTimeUtc().addDays(-static_cast<int>(history_days_to_keep_));
    QSqlQuery query(db);
    query.prepare("DELETE FROM durations WHERE end_utc < :threshold");
    query.bindValue(":threshold", toUtcIso(threshold));
    if (!query.exec()) {
        db.rollback();
        Logger::Log("[DB] Error clearing old durations: " + query.lastError().text());
        return;
    }
    if (!db.commit()) {
        Logger::Log("[DB] Error committing retention cleanup: " + db.lastError().text());
        db.rollback();
    }
}

/**
 * Prunes old backup files in the database directory, keeping only the
 * `keepCount` most recent of each file type (.backup and .durations.txt).
 * Files are named with ISO timestamps so alphabetical sort = chronological.
 * Called once per startup from checkSchemaOnStartup(). Non-fatal.
 */
void SqliteSessionStore::pruneOldBackups(int keepCount)
{
    QDir dir(QFileInfo(db.databaseName()).absoluteDir());
    const QString base = QFileInfo(db.databaseName()).fileName();

    for (const QString& pattern : {base + ".*.backup", base + ".*.durations.txt"}) {
        QStringList files = dir.entryList(
            QStringList() << pattern, QDir::Files, QDir::Name);
        while (files.size() > keepCount) {
            const QString name = files.takeFirst(); // oldest (alphabetical = chronological)
            if (!QFile::remove(dir.filePath(name))) {
                Logger::Log("[DB] Warning: Failed to remove old backup file: " + name);
            }
        }
    }
}

// CREATE ... IF NOT EXISTS no-ops on a pre-existing table of any shape, so
// validate the columns and rebuild on mismatch. app_settings only holds the
// clean-shutdown marker; dropping a malformed table costs at most one
// spurious recovery notification, whereas keeping it silently disables crash
// recovery (marker reads/writes fail at runtime).
bool SqliteSessionStore::ensureSettingsTable()
{
    static const char* kCreateSql =
        "CREATE TABLE IF NOT EXISTS app_settings ("
        "key TEXT PRIMARY KEY,"
        "value TEXT NOT NULL"
        ")";

    QSqlQuery query(db);
    if (!query.exec(kCreateSql)) {
        Logger::Log("[DB] Error creating app_settings table: " + query.lastError().text());
        return false;
    }

    QSqlQuery colQuery(db);
    if (!colQuery.exec("PRAGMA table_info(app_settings)")) {
        Logger::Log("[DB] Error validating app_settings table: " + colQuery.lastError().text());
        return false;
    }

    QSet<QString> foundColumns;
    while (colQuery.next()) {
        foundColumns.insert(colQuery.value(1).toString()); // column 1 = name
    }

    static const QSet<QString> kExpectedColumns = {"key", "value"};
    if (foundColumns == kExpectedColumns) {
        return true;
    }

    Logger::Log(QString("[DB] app_settings has unexpected columns {%1} - rebuilding")
        .arg(QStringList(foundColumns.values()).join(", ")));

    QSqlQuery dropQuery(db);
    if (!dropQuery.exec("DROP TABLE app_settings")) {
        Logger::Log("[DB] Error dropping malformed app_settings table: " + dropQuery.lastError().text());
        return false;
    }

    QSqlQuery recreateQuery(db);
    if (!recreateQuery.exec(kCreateSql)) {
        Logger::Log("[DB] Error re-creating app_settings table: " + recreateQuery.lastError().text());
        return false;
    }

    return true;
}

/**
 * Creates a safety copy of the database before critical write operations.
 *
 * Why:
 * SQLite databases can occasionally become corrupted during write operations,
 * especially if the system crashes (power loss) during a transaction.
 * Since this app tracks user history, data loss is unacceptable.
 *
 * Strategy:
 * - Copies the .sqlite file to a timestamped .backup file.
 * - Writes a human-readable text dump (.durations.txt) for verification.
 * - This runs before 'saveDurations' (major save) but not 'saveCheckpoint' (frequent).
 *
 * Concurrency:
 * - This method closes and reopens the database to ensure a clean file copy.
 *   The caller (a public method) must already hold db_mutex_, which prevents
 *   any other SqliteSessionStore operation from seeing the closed connection.
 *   QRecursiveMutex allows re-entrant calls within the same thread.
 *
 * The current close-copy-reopen approach is correct for rollback-journal mode
 * (no -wal/-shm sidecars).  If WAL mode is ever enabled, switch to SQLite's
 * online backup API (sqlite3_backup_*) so the source DB stays open during the copy.
 */
BackupResult SqliteSessionStore::createBackup(const std::deque<TimeDuration>& durations, TransactionMode mode)
{
    if (!QFile::exists(db.databaseName())) {
        return BackupResult::Success;
    }

    // Skip backup if disk space is low (< 100 MB).  Proceeding with a full-table
    // write on a nearly-full volume risks leaving both the DB and the backup in a
    // corrupted state; warn once and continue without a backup instead.
    {
        QStorageInfo storage(QFileInfo(db.databaseName()).absoluteDir().absolutePath());
        const qint64 available = storage.bytesAvailable();
        if (available >= 0 && available < 100LL * 1024 * 1024) {
            Logger::Log(QString("[DB] Warning: Low disk space (%1 MB). Skipping backup.")
                .arg(available / (1024 * 1024)));
            return BackupResult::Success;
        }
    }

    // Close database before copying
    bool wasOpen = db.isOpen();
    if (wasOpen) {
        db.close();
    }

    // Generate backup filename with millisecond-resolution timestamp.
    // Add a numeric suffix if the target already exists (guards against two saves
    // within the same millisecond producing the same name).
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-ddTHH-mm-ss-zzz");
    QString base = QString("%1.%2").arg(db.databaseName()).arg(timestamp);
    QString backupName = base + ".backup";
    for (int n = 1; QFile::exists(backupName); ++n)
        backupName = QString("%1-%2.backup").arg(base).arg(n);
    QString durationsFileName = backupName;
    durationsFileName.chop(static_cast<int>(qstrlen(".backup")));
    durationsFileName += ".durations.txt";

    // Copy the database file
    bool success = QFile::copy(db.databaseName(), backupName);

    if (!success) {
        Logger::Log("[DB] Error: Failed to create backup of database");
    } else {
        Logger::Log("[DB] Created database backup: " + backupName);
    }

    // Write durations to text file
    QFile durationsFile(durationsFileName);
    if (durationsFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&durationsFile);
        out << "Transaction Mode: " << (mode == TransactionMode::Replace ? "Replace" : "Append") << "\n";
        out << "Total Durations: " << durations.size() << "\n";
        out << "Timestamp: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
        out << "----------------------------------------\n";

        for (const auto& d : durations) {
            out << "Type: " << (d.type == DurationType::Activity ? "Activity" : "Pause") << " | ";
            out << "Duration: " << d.duration << "ms | ";
            out << "Start Date: " << d.startTime.toUTC().date().toString(Qt::ISODate) << " | ";
            out << "Start Time: " << d.startTime.toUTC().time().toString("HH:mm:ss.zzz") << " | ";
            out << "End Date: " << d.endTime.toUTC().date().toString(Qt::ISODate) << " | ";
            out << "End Time: " << d.endTime.toUTC().time().toString("HH:mm:ss.zzz") << "\n";
        }

        durationsFile.close();
        Logger::Log("[DB] Created durations log: " + durationsFileName);
    } else {
        Logger::Log("[DB] Warning: Could not create durations log file");
    }

    // Reopen database if it was open before.
    // Re-apply PRAGMA synchronous=NORMAL since reopening creates a fresh connection
    // that loses all session-level PRAGMAs set in ensureSchema().
    if (wasOpen) {
        if (!db.open()) {
            Logger::Log("[DB] CRITICAL: Failed to reopen database after backup: " + db.lastError().text());
            return BackupResult::ReopenFailed;
        }
        QSqlQuery pragmaQuery(db);
        pragmaQuery.exec("PRAGMA synchronous=NORMAL");
    }

    return success ? BackupResult::Success : BackupResult::BackupFailed;
}

/**
 * Persists the completed segments of a session to the database.
 *
 * Normalizes the timeline internally (merging adjacent same-type segments),
 * computes orphaned segment IDs, and delegates to updateDurationsById()
 * with the clean deque and orphan list. Callers never see removedSegmentIds.
 */
SessionStoreResult SqliteSessionStore::commitSession(const Timeline& session)
{
    if (history_days_to_keep_ == 0) {
        return SessionStoreResult::disabled();
    }
    const auto [normed, orphanIds] = session.normalizedWithRemovedIds();
    return updateDurationsById(normed.completed(), orphanIds);
}

/**
 * Persists a batch of duration entries to the database.
 *
 * Modes:
 * - Append: Adds new entries to the end (standard usage).
 * - Replace: Deletes ALL existing entries and writes the new set (used by HistoryDialog).
 *
 * Safety:
 * - Wraps the operation in a SQL transaction.
 * - Creates a file-level backup before starting.
 * - Rolls back the transaction on any error.
 */
bool SqliteSessionStore::insertRows(QSqlQuery& query, const std::deque<TimeDuration>& rows)
{
    for (const auto& d : rows) {
        query.bindValue(":segment_id", d.segment_id.toString());
        query.bindValue(":type", static_cast<int>(d.type));
        query.bindValue(":start_utc", toUtcIso(d.startTime));
        query.bindValue(":end_utc", toUtcIso(d.endTime));

        if (!query.exec()) {
            db.rollback();
            Logger::Log("[DB] Error inserting duration: " + query.lastError().text());
            return false;
        }
    }
    return true;
}

bool SqliteSessionStore::saveDurations(const std::deque<TimeDuration>& durations, TransactionMode mode,
                                     const std::vector<QString>& removedSegmentIds)
{
    QMutexLocker locker(&db_mutex_);

    // If history storage is disabled, treat as success (no-op)
    if (history_days_to_keep_ == 0) {
        return true;
    }

    // Validate connection first (ensureOpen before createBackup so we don't
    // create a backup for a DB we can't open).
    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to save Durations");
        return false;
    }

    const BackupResult backup = createBackup(durations, mode);
    if (backup == BackupResult::ReopenFailed) {
        Logger::Log("[DB] CRITICAL: DB connection lost after backup - aborting save");
        return false;
    }
    if (backup != BackupResult::Success) {
        const QString modeStr = (mode == TransactionMode::Replace) ? "REPLACE" : "APPEND";
        Logger::Log("[DB] Warning: Backup failed before " + modeStr + " operation - proceeding without backup");
    }

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting transaction for Saving: " + db.lastError().text());
        return false;
    }

    // Replace mode: clear existing entries before inserting new ones
    if (mode == TransactionMode::Replace) {
        QSqlQuery clearQuery(db);
        if (!clearQuery.exec("DELETE FROM durations")) {
            db.rollback();
            Logger::Log("[DB] Error clearing durations table: " + clearQuery.lastError().text());
            return false;
        }
    }

    // Delete orphaned segment_ids that were merged away by cleanDurations.
    // This must happen inside the same transaction as the INSERT for atomicity.
    if (!removedSegmentIds.empty()) {
        QSqlQuery deleteOrphanQuery(db);
        deleteOrphanQuery.prepare("DELETE FROM durations WHERE segment_id = :segment_id");
        for (const auto& orphanId : removedSegmentIds) {
            deleteOrphanQuery.bindValue(":segment_id", orphanId);
            if (!deleteOrphanQuery.exec()) {
                db.rollback();
                Logger::Log("[DB] Error deleting orphaned segment_id: " + deleteOrphanQuery.lastError().text());
                return false;
            }
        }
    }

    // Prepare upsert statement for batch insertion (storing times in UTC).
    // ON CONFLICT(segment_id) preserves the autoincrement id (unlike INSERT OR REPLACE),
    // which the reconciliation path relies on via OrphanCheckpoint.id, and avoids aborting
    // the surrounding transaction when an Append re-encounters an existing segment_id.
    QSqlQuery query(db);
    query.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                  "VALUES (:segment_id, :type, :start_utc, :end_utc, 1) "
                  "ON CONFLICT(segment_id) DO UPDATE SET "
                  "    type         = excluded.type, "
                  "    start_utc    = excluded.start_utc, "
                  "    end_utc      = excluded.end_utc, "
                  "    is_finalized = excluded.is_finalized");

    if (!insertRows(query, durations))
        return false;

    bool commitSuccessful = db.commit();
    if (!commitSuccessful) {
        Logger::Log("[DB] Error committing save transaction: " + db.lastError().text());
    }

#ifndef QT_NO_DEBUG
    if (commitSuccessful) {
        checkSegmentIdUniqueness();
    }
#endif

    return commitSuccessful;
}

SessionStoreResult SqliteSessionStore::replaceAll(const Timeline& history, const Timeline& session)
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return SessionStoreResult::disabled();
    }

    const std::deque<TimeDuration>& historyDurations = history.completed();
    const std::deque<TimeDuration>& currentSessionDurations = session.completed();

    std::deque<TimeDuration> allDurations;
    allDurations.insert(allDurations.end(), historyDurations.begin(), historyDurations.end());
    allDurations.insert(allDurations.end(), currentSessionDurations.begin(), currentSessionDurations.end());

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to replace durations");
        return SessionStoreResult::fatal("DB connection could not be opened");
    }

    const BackupResult backup = createBackup(allDurations, TransactionMode::Replace);
    if (backup == BackupResult::ReopenFailed) {
        Logger::Log("[DB] CRITICAL: DB connection lost after backup - aborting replace");
        return SessionStoreResult::fatal("DB connection lost after backup");
    }
    if (backup != BackupResult::Success) {
        Logger::Log("[DB] Warning: Backup failed before REPLACE operation - proceeding without backup");
    }

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting transaction for replace: " + db.lastError().text());
        return SessionStoreResult::transient("Failed to start replace transaction: " + db.lastError().text());
    }

    QSqlQuery clearQuery(db);
    if (!clearQuery.exec("DELETE FROM durations")) {
        db.rollback();
        Logger::Log("[DB] Error clearing durations table: " + clearQuery.lastError().text());
        return SessionStoreResult::transient("Failed to clear durations: " + clearQuery.lastError().text());
    }

    QSqlQuery finalizedInsert(db);
    finalizedInsert.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, :type, :start_utc, :end_utc, 1)"
    );
    if (!insertRows(finalizedInsert, historyDurations))
        return SessionStoreResult::transient("Failed to insert history rows");

    QSqlQuery unfinalizedInsert(db);
    unfinalizedInsert.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)"
    );
    if (!insertRows(unfinalizedInsert, currentSessionDurations))
        return SessionStoreResult::transient("Failed to insert session rows");

    if (!db.commit()) {
        Logger::Log("[DB] Error committing replace transaction: " + db.lastError().text());
        return SessionStoreResult::transient("Failed to commit replace transaction: " + db.lastError().text());
    }

#ifndef QT_NO_DEBUG
    checkSegmentIdUniqueness();
#endif

    return SessionStoreResult::success();
}

/**
 * Retrieves the entire history of durations from the database.
 *
 * - Returns entries sorted by Start Date/Time (chronological).
 * - Validates each row:
 *   - Type must be a valid enum value (Activity or Pause).
 *   - Both start_utc and end_utc must parse as valid UTC timestamps.
 *   - start must be <= end.
 *   - Computed duration (start.msecsTo(end)) must be positive.
 *   - Corrupted/Invalid rows are skipped and logged.
 * - Timestamps are stored in UTC and converted to local time on load.
 */
LoadResult SqliteSessionStore::loadDurations()
{
    QMutexLocker locker(&db_mutex_);

    LoadResult result;

    if (history_days_to_keep_ == 0) {
        result.status = SessionStoreResult::Disabled;
        return result;
    }

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to load Durations");
        result.status = SessionStoreResult::FatalError;
        return result;
    }

    // Start read transaction for consistent snapshot
    if (!db.transaction()) {
        Logger::Log("[DB] Error starting read transaction: " + db.lastError().text());
        result.status = SessionStoreResult::TransientError;
        return result;
    }

    // Load all durations ordered by start_utc
    QSqlQuery query(db);
    query.prepare("SELECT segment_id, type, start_utc, end_utc FROM durations WHERE is_finalized = 1 ORDER BY start_utc");
    if (!query.exec()) {
        db.rollback();
        Logger::Log("[DB] Error executing load query: " + query.lastError().text());
        result.status = SessionStoreResult::TransientError;
        return result;
    }

    // Parse each row and reconstruct TimeDuration objects
    while (query.next()) {
        // An empty segment_id is a corrupt row (SegmentId::fromString asserts
        // non-empty in debug; the empty id would match every empty-id row on
        // upsert in release). Treat it as a skipped corrupt row.
        const QString segmentIdStr = query.value(0).toString();
        if (segmentIdStr.isEmpty()) {
            Logger::Log("[DB] Warning: Skipped row with empty segment_id");
            result.skipped++;
            continue;
        }
        const SegmentId segmentId = SegmentId::fromString(segmentIdStr);
        const int typeInt = query.value(1).toInt();
        const QDateTime startDateTime = QDateTime::fromString(query.value(2).toString(), Qt::ISODateWithMs).toLocalTime();
        const QDateTime endDateTime   = QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs).toLocalTime();

        // Validate type enum range
        if (typeInt != static_cast<int>(DurationType::Activity) &&
            typeInt != static_cast<int>(DurationType::Pause)) {
            Logger::Log(QString("[DB] Warning: Invalid type value %1, skipping entry").arg(typeInt));
            result.skipped++;
            continue;
        }

        DurationType type = static_cast<DurationType>(typeInt);

        // Validate timestamps
        if (!startDateTime.isValid() || !endDateTime.isValid()) {
            Logger::Log(QString("[DB] Warning: Skipped entry with invalid UTC timestamp(s) - start: %1, end: %2")
                .arg(query.value(2).toString()).arg(query.value(3).toString()));
            result.skipped++;
            continue;
        }

        // Validate start <= end
        if (startDateTime > endDateTime) {
            Logger::Log(QString("[DB] Warning: Skipped entry with start > end - Start: %1, End: %2")
                .arg(startDateTime.toString(Qt::ISODate)).arg(endDateTime.toString(Qt::ISODate)));
            result.skipped++;
            continue;
        }

        // Create TimeDuration — cross-midnight rows are accepted via fromPersistedRow() so
        // that createPages() can bucket them onto both the start-date and end-date pages.
        if (startDateTime.msecsTo(endDateTime) <= 0) {
            Logger::Log(QString("[DB] Dropped zero/negative-duration row at load: %1 → %2")
                .arg(startDateTime.toString(Qt::ISODateWithMs))
                .arg(endDateTime.toString(Qt::ISODateWithMs)));
            result.skipped++;
            continue;
        }
        result.durations.emplace_back(TimeDuration::fromPersistedRow(type, startDateTime, endDateTime, segmentId));
    }

    // Rollback is idiomatic for read-only transactions: no writes were made,
    // so there is nothing to commit. Both commit() and rollback() release the
    // transaction lock, but rollback() signals the intent more clearly.
    db.rollback();

    if (result.skipped > 0) {
        Logger::Log(QString("[DB] loadDurations reconciliation summary: skipped=%1").arg(result.skipped));
    }

    return result;
}

/**
 * Checks whether finalized entries exist for a given date.
 *
 * Returns a tri-state result:
 * - Yes:     At least one finalized entry exists for the date.
 * - No:      The DB was queried successfully and zero entries were found.
 * - Unknown: The DB could not be opened (history disabled or open failure).
 *            Callers should treat Unknown conservatively — e.g. do NOT assume
 *            "no entries exist" when the answer is actually unknowable.
 */
EntriesForDateResult SqliteSessionStore::hasEntriesForDate(const QDate& date)
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return EntriesForDateResult::Unknown;
    }

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to check entries for date");
        return EntriesForDateResult::Unknown;
    }

    // Compute the UTC half-open range covering the caller's local day.
    const QDateTime localStart(date, QTime(0, 0, 0), Qt::LocalTime);
    const QDateTime localEnd = localStart.addDays(1);
    const QDateTime utcStart = localStart.toUTC();
    const QDateTime utcEnd   = localEnd.toUTC();

    // Check if any finalized entry started within that UTC range.
    QSqlQuery query(db);
    query.prepare(
        "SELECT 1 FROM durations"
        " WHERE is_finalized = 1"
        "   AND start_utc >= :utcStartTs"
        "   AND start_utc <  :utcEndTs"
        " LIMIT 1"
    );
    query.bindValue(":utcStartTs", utcStart.toString(Qt::ISODateWithMs));
    query.bindValue(":utcEndTs",   utcEnd.toString(Qt::ISODateWithMs));

    EntriesForDateResult result = EntriesForDateResult::Unknown;
    if (query.exec()) {
        result = query.next() ? EntriesForDateResult::Yes : EntriesForDateResult::No;
    } else {
        Logger::Log("[DB] Error checking entries for date: " + query.lastError().text());
    }

    return result;
}

/**
 * Saves or updates a checkpoint entry for crash recovery.
 *
 * Unlike saveDurations(), this method:
 * - Does NOT create backups (for performance - called every 5 minutes)
 * - Uses segment_id to update existing row instead of inserting new one
 * - Stores the actual segment_start_time_ (preserves original start, only updates end/duration)
 *
 * The caller (TimeTracker) rotates session_.segment_id on mode changes,
 * causing the next checkpoint to create a new row for the new segment.
 */
SessionStoreResult SqliteSessionStore::saveCheckpoint(DurationType type, const QDateTime& startTime, const QDateTime& endTime, const SegmentId& segmentId)
{
    QMutexLocker locker(&db_mutex_);

    if (segmentId.isEmpty()) {
        Logger::Log("[DB] saveCheckpoint: empty segment_id — caller bug");
        return SessionStoreResult::callerBug("saveCheckpoint called with empty segment_id");
    }

    if (history_days_to_keep_ == 0) {
        return SessionStoreResult::disabled();
    }

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to save checkpoint");
        return SessionStoreResult::fatal("DB connection could not be opened");
    }

    const QString startUtcStr = toUtcIso(startTime);
    const QString endUtcStr   = toUtcIso(endTime);

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting transaction for checkpoint: " + db.lastError().text());
        return SessionStoreResult::transient("Failed to start checkpoint transaction: " + db.lastError().text());
    }

    bool success = false;
    QSqlQuery updateQuery(db);
    updateQuery.prepare(
        "UPDATE durations SET end_utc = :end_utc, is_finalized = 0 "
        "WHERE segment_id = :segment_id AND is_finalized = 0"
    );
    updateQuery.bindValue(":end_utc", endUtcStr);
    updateQuery.bindValue(":segment_id", segmentId.toString());

    if (!updateQuery.exec()) {
        db.rollback();
        Logger::Log("[DB] Error updating checkpoint by segment_id: " + updateQuery.lastError().text());
        return SessionStoreResult::transient("Failed to update checkpoint: " + updateQuery.lastError().text());
    }

    if (updateQuery.numRowsAffected() > 0) {
        success = db.commit();
        if (!success) {
            Logger::Log("[DB] Error committing checkpoint update: " + db.lastError().text());
        }
    } else {
        QSqlQuery insertQuery(db);
        insertQuery.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:segment_id, :type, :start_utc, :end_utc, 0)"
        );
        insertQuery.bindValue(":segment_id", segmentId.toString());
        insertQuery.bindValue(":type", static_cast<int>(type));
        insertQuery.bindValue(":start_utc", startUtcStr);
        insertQuery.bindValue(":end_utc", endUtcStr);

        if (!insertQuery.exec()) {
            db.rollback();
            // Primary result code 19 = SQLITE_CONSTRAINT. Assumes extended result
            // codes stay disabled on this connection (they are never enabled here);
            // with them on, a UNIQUE violation would report 2067 instead.
            // UNIQUE(segment_id) is the only constraint that can fire on this INSERT
            // path (NOT NULL columns are all bound and the empty-segment_id case is
            // rejected at function entry), so 19 here means a finalized row already
            // owns this segment_id. Demoting it would corrupt history — report a
            // caller bug instead.
            static const QString kSqliteConstraint = "19";
            if (insertQuery.lastError().nativeErrorCode() == kSqliteConstraint) {
                Logger::Log("[DB] saveCheckpoint: segment_id belongs to a finalized row — caller bug: "
                            + segmentId.toString());
                return SessionStoreResult::callerBug(
                    "saveCheckpoint: segment_id " + segmentId.toString()
                    + " already belongs to a finalized row");
            }
            Logger::Log("[DB] Error inserting checkpoint by segment_id: " + insertQuery.lastError().text());
            return SessionStoreResult::transient("Failed to insert checkpoint: " + insertQuery.lastError().text());
        }

        success = db.commit();
        if (!success) {
            Logger::Log("[DB] Error committing checkpoint insert: " + db.lastError().text());
        }
    }

#ifndef QT_NO_DEBUG
    if (success) {
        checkSegmentIdUniqueness();
    }
#endif

    return success
        ? SessionStoreResult::success()
        : SessionStoreResult::transient("Checkpoint transaction commit failed: " + db.lastError().text());
}

SessionStoreResult SqliteSessionStore::discardCheckpoint(const SegmentId& segmentId)
{
    QMutexLocker locker(&db_mutex_);

    if (segmentId.isEmpty()) {
        Logger::Log("[DB] discardCheckpoint: empty segment_id — caller bug");
        return SessionStoreResult::callerBug("discardCheckpoint called with empty segment_id");
    }

    if (history_days_to_keep_ == 0) {
        return SessionStoreResult::disabled();
    }

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to discard checkpoint");
        return SessionStoreResult::fatal("DB connection could not be opened");
    }

    // Only unfinalized rows may be removed here; finalized history is never touched.
    QSqlQuery query(db);
    query.prepare("DELETE FROM durations WHERE segment_id = :segment_id AND is_finalized = 0");
    query.bindValue(":segment_id", segmentId.toString());
    if (!query.exec()) {
        Logger::Log("[DB] Error discarding checkpoint: " + query.lastError().text());
        return SessionStoreResult::transient("Failed to discard checkpoint: " + query.lastError().text());
    }

    Logger::Log(QString("[DB] discardCheckpoint: removed %1 unfinalized row(s) for segment_id %2")
        .arg(query.numRowsAffected()).arg(segmentId.toString()));
    return SessionStoreResult::success();
}

/**
 * Smart upsert (update or insert) for a batch of durations using segment identity.
 *
 * Use Case:
 * - When the timer stops or pauses, we update existing segment rows by segment_id.
 * - If a segment row is missing, it is inserted with the same segment_id.
 */
SessionStoreResult SqliteSessionStore::updateDurationsById(const std::deque<TimeDuration>& durations,
                                                           const std::vector<QString>& removedSegmentIds)
{
    QMutexLocker locker(&db_mutex_);

    if (durations.empty()) {
        return SessionStoreResult::success();
    }

    if (history_days_to_keep_ == 0) {
        return SessionStoreResult::disabled();
    }

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to update durations");
        return SessionStoreResult::transient("DB connection could not be opened");
    }

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting transaction for updating durations: " + db.lastError().text());
        return SessionStoreResult::transient("Failed to start transaction: " + db.lastError().text());
    }

    // Delete orphaned segment_ids that were merged away by cleanDurations.
    // This must happen inside the same transaction as the UPDATE/INSERT for atomicity.
    if (!removedSegmentIds.empty()) {
        QSqlQuery deleteOrphanQuery(db);
        deleteOrphanQuery.prepare("DELETE FROM durations WHERE segment_id = :segment_id");
        for (const auto& orphanId : removedSegmentIds) {
            deleteOrphanQuery.bindValue(":segment_id", orphanId);
            if (!deleteOrphanQuery.exec()) {
                db.rollback();
                const QString errMsg = deleteOrphanQuery.lastError().text();
                Logger::Log("[DB] Error deleting orphaned segment_id: " + errMsg);
                return SessionStoreResult::transient("Failed to delete orphan: " + errMsg);
            }
        }
    }

    QSqlQuery updateQuery(db);
    updateQuery.prepare(
        "UPDATE durations SET type = :type, start_utc = :start_utc, end_utc = :end_utc, is_finalized = 1 "
        "WHERE segment_id = :segment_id"
    );

    QSqlQuery insertQuery(db);
    insertQuery.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, :type, :start_utc, :end_utc, 1)"
    );

    int count = 0;

    for (const auto& d : durations) {
        const QString startUtcStr = toUtcIso(d.startTime);
        const QString endUtcStr   = toUtcIso(d.endTime);

        updateQuery.bindValue(":segment_id", d.segment_id.toString());
        updateQuery.bindValue(":type", static_cast<int>(d.type));
        updateQuery.bindValue(":start_utc", startUtcStr);
        updateQuery.bindValue(":end_utc", endUtcStr);

        if (!updateQuery.exec()) {
            db.rollback();
            const QString errMsg = updateQuery.lastError().text();
            Logger::Log("[DB] Error updating duration by segment_id: " + errMsg);
            return SessionStoreResult::transient("Failed to update duration: " + errMsg);
        }

        if (updateQuery.numRowsAffected() == 0) {
            insertQuery.bindValue(":segment_id", d.segment_id.toString());
            insertQuery.bindValue(":type", static_cast<int>(d.type));
            insertQuery.bindValue(":start_utc", startUtcStr);
            insertQuery.bindValue(":end_utc", endUtcStr);

            if (!insertQuery.exec()) {
                db.rollback();
                const QString errMsg = insertQuery.lastError().text();
                Logger::Log("[DB] Error inserting duration by segment_id: " + errMsg);
                return SessionStoreResult::transient("Failed to insert duration: " + errMsg);
            }
        }
        count++;
    }

    if (!db.commit()) {
        const QString errMsg = db.lastError().text();
        Logger::Log("[DB] Error committing update transaction: " + errMsg);
        db.rollback();
        return SessionStoreResult::transient("Failed to commit update transaction: " + errMsg);
    }

    Logger::Log(QString("[DB] Upserted %1 durations").arg(count));

#ifndef QT_NO_DEBUG
    checkSegmentIdUniqueness();
#endif

    return SessionStoreResult::success();
}

/**
 * Forces any pending database writes to be physically written to disk.
 *
 * This is critical during Windows shutdown to ensure data is persisted
 * before the process is terminated. SQLite may buffer writes even after
 * commit() returns — this method ensures they're flushed by temporarily
 * promoting to PRAGMA synchronous=FULL and issuing a dummy write to
 * trigger the fsync.
 *
 * Durability guarantee depends on journal mode:
 * This app uses rollback journal mode (the SQLite default), NOT WAL.  In
 * rollback journal mode, promoting synchronous to FULL is sufficient to
 * guarantee durability — the journal file is fully flushed before the
 * transaction is considered committed.
 *
 * If WAL mode is ever enabled (e.g. for concurrent reader performance),
 * this method must also call:
 *   PRAGMA wal_checkpoint(RESTART)
 * after the commit, to flush the WAL file into the main database file.
 * Without the checkpoint, the most recent writes live only in the WAL
 * and a power failure could leave them unrecoverable from the main file.
 */
SessionStoreResult SqliteSessionStore::flushToDisc()
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return SessionStoreResult::disabled();
    }

    if (!ensureOpen()) {
        Logger::Log("[DB] flushToDisc: could not open DB");
        return SessionStoreResult::transient("DB connection could not be opened");
    }

    QSqlQuery pragma(db);
    if (!pragma.exec("PRAGMA synchronous=FULL")) {
        Logger::Log("[DB] flushToDisc: set synchronous=FULL failed: " + pragma.lastError().text());
        return SessionStoreResult::transient("Failed to set synchronous=FULL: " + pragma.lastError().text());
    }

    if (!db.transaction()) {
        Logger::Log("[DB] flushToDisc: could not begin transaction");
        pragma.exec("PRAGMA synchronous=NORMAL");
        return SessionStoreResult::transient("Failed to start flush transaction: " + db.lastError().text());
    }

    const QString ts = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery query(db);
    query.prepare("INSERT OR REPLACE INTO app_settings (key, value) VALUES ('last_flush_utc', :ts)");
    query.bindValue(":ts", ts);

    if (!query.exec()) {
        Logger::Log("[DB] flushToDisc: write failed: " + query.lastError().text());
        db.rollback();
        pragma.exec("PRAGMA synchronous=NORMAL");
        return SessionStoreResult::transient("Flush write failed: " + query.lastError().text());
    }
    if (!db.commit()) {
        Logger::Log("[DB] flushToDisc: commit failed: " + db.lastError().text());
        db.rollback();
        pragma.exec("PRAGMA synchronous=NORMAL");
        return SessionStoreResult::transient("Flush commit failed: " + db.lastError().text());
    }

    Logger::Log("[DB] flushToDisc: durable heartbeat committed");
    pragma.exec("PRAGMA synchronous=NORMAL");
    return SessionStoreResult::success();
}

std::optional<std::deque<SqliteSessionStore::OrphanCheckpoint>> SqliteSessionStore::loadUnfinalizedCheckpoints()
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return std::deque<OrphanCheckpoint>{};
    }

    if (!ensureOpen()) {
        return std::nullopt;
    }

    std::deque<OrphanCheckpoint> orphans;

    QSqlQuery query(db);
    query.prepare("SELECT id, segment_id, type, start_utc, end_utc FROM durations WHERE is_finalized = 0 ORDER BY id ASC");
    if (!query.exec()) {
        Logger::Log("[DB] Error loading orphan checkpoints: " + query.lastError().text());
        return std::nullopt;
    }

    while (query.next()) {
        // An empty segment_id is a corrupt row (SegmentId::fromString asserts
        // non-empty in debug). Skip it rather than assert-aborting.
        const QString segmentIdStr = query.value(1).toString();
        if (segmentIdStr.isEmpty()) {
            Logger::Log("[DB] loadUnfinalizedCheckpoints: skipped row with empty segment_id");
            continue;
        }
        const int typeInt = query.value(2).toInt();
        if (typeInt != static_cast<int>(DurationType::Activity) &&
            typeInt != static_cast<int>(DurationType::Pause)) {
            continue;
        }

        const QDateTime startTime = QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs).toLocalTime();
        const QDateTime endTime   = QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs).toLocalTime();
        if (!startTime.isValid() || !endTime.isValid()) {
            Logger::Log(QString("[DB] loadUnfinalizedCheckpoints: skipped row with invalid timestamp(s): start=%1 end=%2")
                .arg(query.value(3).toString()).arg(query.value(4).toString()));
            continue;
        }
        if (startTime >= endTime) {
            Logger::Log(QString("[DB] loadUnfinalizedCheckpoints: skipped row with start >= end: start=%1 end=%2")
                .arg(startTime.toString(Qt::ISODateWithMs)).arg(endTime.toString(Qt::ISODateWithMs)));
            continue;
        }

        OrphanCheckpoint checkpoint;
        checkpoint.id = query.value(0).toLongLong();
        checkpoint.segment_id = SegmentId::fromString(query.value(1).toString());
        checkpoint.type = static_cast<DurationType>(typeInt);
        checkpoint.duration = startTime.msecsTo(endTime);
        checkpoint.startTime = startTime;
        checkpoint.endTime = endTime;
        orphans.push_back(checkpoint);
    }

    return orphans;
}

bool SqliteSessionStore::finalizeIfNoOverlap(qint64 rowId, const QDateTime& startUtc, const QDateTime& endUtc)
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return false;
    }

    if (!ensureOpen()) {
        return false;
    }

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting finalizeIfNoOverlap transaction: " + db.lastError().text());
        return false;
    }

    // Probe for any *other* finalised row that overlaps [startUtc, endUtc).
    // Two intervals [a, b) and [c, d) overlap iff a < d AND c < b.
    // ISO-8601 strings compare lexicographically in chronological order.
    // The idx_start_utc partial index covers the LHS of the predicate.
    const QString startTs = startUtc.toString(Qt::ISODateWithMs);
    const QString endTs   = endUtc.toString(Qt::ISODateWithMs);

    QSqlQuery probe(db);
    probe.prepare(
        "SELECT 1 FROM durations"
        " WHERE is_finalized = 1"
        "   AND id != :rowId"
        "   AND start_utc < :endTs"
        "   AND end_utc   > :startTs"
        " LIMIT 1"
    );
    probe.bindValue(":rowId", static_cast<qlonglong>(rowId));
    probe.bindValue(":endTs", endTs);
    probe.bindValue(":startTs", startTs);

    if (!probe.exec()) {
        Logger::Log("[DB] Error probing for orphan overlap: " + probe.lastError().text());
        db.rollback();
        return false;
    }

    if (probe.next()) {
        // Overlap detected: leave the row unchanged.
        db.rollback();
        return false;
    }

    QSqlQuery updateQuery(db);
    updateQuery.prepare("UPDATE durations SET is_finalized = 1 WHERE id = :id AND is_finalized = 0");
    updateQuery.bindValue(":id", static_cast<qlonglong>(rowId));
    if (!updateQuery.exec()) {
        Logger::Log("[DB] Error finalising orphan row: " + updateQuery.lastError().text());
        db.rollback();
        return false;
    }

    const bool updated = updateQuery.numRowsAffected() > 0;
    if (!updated) {
        // Row missing or already finalised — nothing to commit, but treat as a
        // no-op rather than a failure.
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        Logger::Log("[DB] Error committing finalizeIfNoOverlap: " + db.lastError().text());
        db.rollback();
        return false;
    }

    return true;
}

SqliteSessionStore::ReconcileResult SqliteSessionStore::reconcileUnfinalizedCheckpoints(const std::vector<OrphanCheckpoint>& orphansToFinalize,
                                                                                       const std::vector<long long>& outrightDropIds)
{
    QMutexLocker locker(&db_mutex_);

    ReconcileResult result;

#ifndef QT_NO_DEBUG
    if (force_reconcile_failure_dbg_) {
        result.ok = false;
        return result;
    }
#endif

    if (history_days_to_keep_ == 0) {
        result.ok = false;
        return result;
    }

    if (orphansToFinalize.empty() && outrightDropIds.empty()) {
        return result;
    }

    if (!ensureOpen()) {
        result.ok = false;
        return result;
    }

    // Outright drops happen first in their own transaction so they survive even
    // if a subsequent finalize call fails.  Finalise calls are then issued one
    // by one via finalizeIfNoOverlap, each in its own atomic transaction.
    if (!outrightDropIds.empty()) {
        if (!db.transaction()) {
            Logger::Log("[DB] Error starting orphan drop transaction: " + db.lastError().text());
            result.ok = false;
            return result;
        }

        QSqlQuery dropQuery(db);
        dropQuery.prepare("DELETE FROM durations WHERE id = :id AND is_finalized = 0");
        for (long long id : outrightDropIds) {
            dropQuery.bindValue(":id", static_cast<qlonglong>(id));
            if (!dropQuery.exec()) {
                Logger::Log("[DB] Error dropping orphan row: " + dropQuery.lastError().text());
                db.rollback();
                result.ok = false;
                return result;
            }
        }

        if (!db.commit()) {
            Logger::Log("[DB] Error committing orphan drop transaction: " + db.lastError().text());
            db.rollback();
            result.ok = false;
            return result;
        }
    }

    for (const auto& orphan : orphansToFinalize) {
        const QDateTime startUtc = orphan.startTime.toUTC();
        const QDateTime endUtc   = orphan.endTime.toUTC();
        if (finalizeIfNoOverlap(orphan.id, startUtc, endUtc)) {
            result.finalized.push_back(orphan.id);
        } else {
            result.dropped.push_back(orphan.id);
        }
    }

    return result;
}

SessionStoreResult SqliteSessionStore::setLastCleanShutdownMarker(const QDateTime& timestamp)
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return SessionStoreResult::disabled();
    }

    if (!ensureOpen()) {
        return SessionStoreResult::transient("DB connection could not be opened");
    }

    if (!db.transaction()) {
        return SessionStoreResult::transient("Failed to start marker transaction: " + db.lastError().text());
    }

    const QString markerValue = toUtcIso(timestamp);
    QSqlQuery query(db);
    query.prepare("INSERT OR REPLACE INTO app_settings (key, value) VALUES (:key, :value)");
    query.bindValue(":key", QString::fromLatin1(kLastCleanShutdownKey));
    query.bindValue(":value", markerValue);

    if (!query.exec()) {
        db.rollback();
        return SessionStoreResult::transient("Marker write failed: " + query.lastError().text());
    }

    if (!db.commit()) {
        db.rollback();
        return SessionStoreResult::transient("Marker commit failed: " + db.lastError().text());
    }

    return SessionStoreResult::success();
}

std::optional<SqliteSessionStore::MarkerResult> SqliteSessionStore::readLastCleanShutdownMarker()
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return std::nullopt;
    }

    if (!ensureOpen()) {
        return MarkerResult { {}, MarkerResult::Status::Error };
    }

    std::optional<QDateTime> parsedTs;
    QSqlQuery readQuery(db);
    readQuery.prepare("SELECT value FROM app_settings WHERE key = :key");
    readQuery.bindValue(":key", QString::fromLatin1(kLastCleanShutdownKey));
    if (!readQuery.exec()) {
        return MarkerResult { {}, MarkerResult::Status::Error };
    }
    if (readQuery.next()) {
        const QString value = readQuery.value(0).toString();
        QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
        if (!parsed.isValid()) {
            parsed = QDateTime::fromString(value, Qt::ISODate);
        }
        if (parsed.isValid()) {
            parsedTs = parsed.toLocalTime();
        }
    }

    if (parsedTs.has_value()) {
        return MarkerResult { *parsedTs, MarkerResult::Status::Found };
    }
    return MarkerResult { {}, MarkerResult::Status::NotFound };
}

bool SqliteSessionStore::deleteLastCleanShutdownMarker()
{
    QMutexLocker locker(&db_mutex_);

    if (!ensureOpen()) {
        return false;
    }

    if (!db.transaction()) {
        return false;
    }

    QSqlQuery deleteQuery(db);
    deleteQuery.prepare("DELETE FROM app_settings WHERE key = :key");
    deleteQuery.bindValue(":key", QString::fromLatin1(kLastCleanShutdownKey));
    if (!deleteQuery.exec()) {
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        db.rollback();
        return false;
    }

    return true;
}

/**
 * Startup checkpoint recovery.
 *
 * Consumes the last-clean-shutdown marker, loads unfinalized checkpoint rows,
 * applies the too-short (<1 s) / stale (>24 h) / overlap policies, finalizes
 * qualifying rows, and decides whether the user needs to be notified.
 *
 * All internal mechanics (row ids, finalize/drop lists, marker timestamps) stay
 * inside this method.  TimeTracker receives only domain facts via StartupRecoveryResult.
 */
StartupRecoveryResult SqliteSessionStore::recoverStartupCheckpoints(const QDateTime& now)
{
    StartupRecoveryResult result;

    // Read the marker first (without deleting it).  If the read fails, do not
    // reconcile — DB state is unknown and finalising rows could conflict with
    // in-progress data.  The marker is only deleted after reconciliation succeeds
    // so that ok==false never leaves the store in a mutated state.
    const std::optional<MarkerResult> markerResult = readLastCleanShutdownMarker();
    if (markerResult.has_value() && markerResult->status == MarkerResult::Status::Error) {
        Logger::Log("[DB] recoverStartupCheckpoints: marker read failed — skipping orphan reconciliation");
        result.ok = false;
        return result;
    }
    // When markerResult is nullopt (history disabled), loadUnfinalizedCheckpoints()
    // returns an empty deque (disabled is a clean no-op), so reconciliation is a safe no-op.

    const std::optional<std::deque<OrphanCheckpoint>> orphansOpt = loadUnfinalizedCheckpoints();
    if (!orphansOpt.has_value()) {
        Logger::Log("[DB] recoverStartupCheckpoints: orphan load failed — marker preserved, no reconciliation");
        result.ok = false;
        return result;
    }
    const std::deque<OrphanCheckpoint>& orphans = *orphansOpt;
    if (orphans.empty()) {
        // No orphans: delete the marker (clean state) and return.
        if (markerResult.has_value()) {
            deleteLastCleanShutdownMarker();
        }
        return result;
    }

    std::vector<OrphanCheckpoint> toFinalize;
    std::vector<long long> dropIds;

    for (const auto& orphan : orphans) {
        const bool tooShort = orphan.duration < kMinRecoverableOrphanDurationMs;
        const bool stale = orphan.endTime.isValid() && (orphan.endTime.msecsTo(now) > kOrphanStaleAgeMs);
        // A backward clock jump since the checkpoint was written can leave the
        // orphan's endTime after "now" (msecsTo(now) negative, passing the
        // stale check above). Drop future-dated orphans instead of resurrecting them.
        const bool futureDated = orphan.endTime.isValid() && (now.msecsTo(orphan.endTime) > kOrphanFutureSlackMs);

        if (tooShort || stale || futureDated) {
            dropIds.push_back(orphan.id);
        } else {
            toFinalize.push_back(orphan);
        }
    }

    ReconcileResult reconcile = reconcileUnfinalizedCheckpoints(toFinalize, dropIds);
    if (!reconcile.ok) {
        Logger::Log("[DB] recoverStartupCheckpoints: reconciliation transaction failed");
        result.ok = false;
        return result;
    }

    // Reconciliation succeeded: now it is safe to consume the marker.
    if (markerResult.has_value()) {
        deleteLastCleanShutdownMarker();
    }

    result.finalized_count = static_cast<int>(reconcile.finalized.size());
    result.dropped_count   = static_cast<int>(dropIds.size()) + static_cast<int>(reconcile.dropped.size());

    // Count seconds only for rows the store actually finalized.
    for (const auto& orphan : toFinalize) {
        if (std::find(reconcile.finalized.begin(), reconcile.finalized.end(), orphan.id)
                != reconcile.finalized.end()) {
            result.recovered_seconds += orphan.duration / 1000;
        }
    }

    // Decide whether to notify the user.
    // If a valid clean-shutdown marker exists and its timestamp covers (is >= )
    // the newest finalized orphan's end time, the shutdown was clean — no notification.
    if (!reconcile.finalized.empty()) {
        bool showNotification = true;

        const bool hasValidMarker = markerResult.has_value()
            && markerResult->status == MarkerResult::Status::Found
            && markerResult->timestamp.isValid();
        if (hasValidMarker) {
            const QDateTime markerUtc = markerResult->timestamp.toUTC();
            QDateTime newestFinalizedOrphanEndUtc;
            for (const auto& orphan : orphans) {
                if (std::find(reconcile.finalized.begin(), reconcile.finalized.end(), orphan.id)
                        == reconcile.finalized.end()) {
                    continue;
                }
                const QDateTime orphanEndUtc = orphan.endTime.toUTC();
                if (!newestFinalizedOrphanEndUtc.isValid() || orphanEndUtc > newestFinalizedOrphanEndUtc) {
                    newestFinalizedOrphanEndUtc = orphanEndUtc;
                }
            }
            // Suppress only when the marker covers the NEWEST finalized orphan;
            // notify iff any finalized orphan ends after the marker.
            if (newestFinalizedOrphanEndUtc.isValid() && markerUtc >= newestFinalizedOrphanEndUtc) {
                showNotification = false;
            }
        }

        result.notify_user = showNotification;
    }

    Logger::Log(QString("[DB] Startup checkpoint recovery: finalized=%1, dropped=%2, overlap_rejected=%3, recovered_seconds=%4")
        .arg(reconcile.finalized.size())
        .arg(dropIds.size())
        .arg(reconcile.dropped.size())
        .arg(result.recovered_seconds));

    return result;
}
