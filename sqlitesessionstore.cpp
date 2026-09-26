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
 * - saveCheckpoint(): Lightweight update without backup, used for periodic checkpoints
 * - updateDurationsById(): Smart upsert matching entries by stable segment_id, used
 *   via commitSession() for stopTimer/HistoryDialog
 *
 * Backup strategy:
 * - replaceAll() creates timestamped .backup file + .durations.txt log before writes
 * - saveCheckpoint() skips backup for performance (called every 5 minutes)
 *
 * Configuration:
 * - history_days_to_keep_ = 0 disables database entirely (all methods return early)
 * - Old entries are purged once per startup via checkSchemaOnStartup() →
 *   performRetentionCleanup(), not in the constructor.
 */

#include "sqlitesessionstore.h"
#include "apppaths.h"
#include "dayvalidity.h"
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
// D8 (R06-001): the exact column/constraint body ensureSchema() creates the
// durations table with. Shared verbatim between creation and startup
// validation so validation compares against what creation actually does,
// not a second hand-maintained description of it. SQLite's sqlite3EndTable()
// stores a CREATE statement's sqlite_master.sql text as "CREATE " + type +
// " " + the verbatim submitted text from the table-name token onward -- so
// "IF NOT EXISTS" is never included and the prefix is normalized to exactly
// one space. This follows from reading SQLite's source and was verified
// empirically against SQLite 3.51.2; it is not independently confirmed on
// every SQLite 3.x release. Given that, an existing table can be checked for
// full semantic compatibility -- declared types, PRIMARY KEY/AUTOINCREMENT,
// NOT NULL, DEFAULT, the nonempty-segment_id CHECK, and the UNIQUE(segment_id)
// constraint -- with one exact-text comparison instead of separately
// re-deriving each property from PRAGMA table_info.
//
// WARNING: this string is therefore a persisted wire format, not ordinary
// source text. Every database created since c07ec71 (see below) was created
// with these exact bytes. Reformatting it for readability -- a space after
// each comma, one column per line, reordering columns -- is a semantic no-op
// in SQL but changes the compared text, which would make every such existing
// database Outdated on next startup (main.cpp then refuses to start). Any
// real change to this schema needs an actual migration path, not a literal
// edit here.
//
// Bounded exposure of this check's introduction: a database built from
// 520f2df up to but not including c07ec71 (before the CHECK constraint was
// added) has the right six columns, PK/AUTOINCREMENT, and UNIQUE(segment_id)
// but no CHECK(length(segment_id) > 0) -- Ready before D8, Outdated after,
// with no repair mode per plan v2 section 2. No *tagged* release is affected:
// `git ls-tree -r` finds no sqlite/database file at any of v0.9..v2.0, and
// the database first arrived in abea78e ("Adding Database and full History
// window"), which no release tag contains -- v2.0 predates it. This says
// nothing about untagged builds from main in that window, which could still
// have produced such a database.
const char* kDurationsColumnsSql =
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "segment_id TEXT NOT NULL CHECK(length(segment_id) > 0),"
    "type INTEGER NOT NULL,"
    "start_utc TEXT NOT NULL,"
    "end_utc TEXT NOT NULL,"
    "is_finalized INTEGER NOT NULL DEFAULT 0,"
    "UNIQUE(segment_id)";
// Same idea for idx_start_utc's partial predicate: an incompatible database
// could carry an index of this name whose definition differs (not partial,
// or a different WHERE clause). "CREATE INDEX IF NOT EXISTS" silently no-ops
// on a same-named index regardless of its definition, so a wrong existing
// definition must be detected and rebuilt rather than left in place.
const char* kIdxStartUtcTail =
    "idx_start_utc ON durations(start_utc) WHERE is_finalized = 1";
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

// H5/WP2 helpers for saveHistoryDays(). Pure -- none of these touch `db`.

const StoredHistoryRow* findSnapshotRow(const std::vector<StoredHistoryRow>& snapshot, const QString& id)
{
    auto it = std::find_if(snapshot.begin(), snapshot.end(),
                            [&](const StoredHistoryRow& r) { return r.segmentId == id; });
    return it == snapshot.end() ? nullptr : &(*it);
}

// A row is editable for `day` iff it is finalized, well-formed, and its
// parsed interval lies fully inside the day's UTC span -- the same
// definition saveHistoryDays()'s CallerBug checks and its stale check share.
bool rowEditableInSpan(const StoredHistoryRow& row, const QDateTime& dayStartUtc, const QDateTime& dayEndUtc)
{
    if (!row.finalized() || !row.parsed)
        return false;
    return row.parsed->startTime >= dayStartUtc && row.parsed->endTime <= dayEndUtc;
}

// Every CallerBug fact saveHistoryDays() must reject before any mutation and
// before the Disabled guard (see sessionstore.h's CallerBug/Disabled
// precedence rule, shared with commitSession/replaceAll/saveCheckpoint/
// discardCheckpoint). Returns nullopt when the payload is acceptable.
std::optional<QString> validateHistoryDaysCallerFacts(const std::vector<StoredHistoryRow>& openSnapshot,
                                                       const std::vector<HistoryDayChange>& days)
{
    if (days.empty())
        return QString("saveHistoryDays: days must not be empty");

    QSet<QDate> seenDates;
    QSet<QString> seenIds;

    for (const auto& day : days) {
        if (seenDates.contains(day.date))
            return QString("saveHistoryDays: duplicate date %1").arg(day.date.toString(Qt::ISODate));
        seenDates.insert(day.date);

        if (!day.dayStartUtc.isValid() || !day.dayEndUtc.isValid() ||
            day.dayStartUtc.msecsTo(day.dayEndUtc) <= 0)
            return QString("saveHistoryDays: invalid or non-positive span for %1").arg(day.date.toString(Qt::ISODate));

        for (const auto& id : day.deletedIds) {
            const QString idStr = id.toString();
            if (seenIds.contains(idStr))
                return QString("saveHistoryDays: id %1 used more than once").arg(idStr);
            seenIds.insert(idStr);

            const StoredHistoryRow* row = findSnapshotRow(openSnapshot, idStr);
            if (!row || !rowEditableInSpan(*row, day.dayStartUtc, day.dayEndUtc))
                return QString("saveHistoryDays: deleted id %1 is not editable for its day").arg(idStr);
        }

        for (const auto& upsert : day.upserts) {
            const QString idStr = upsert.segment_id.toString();
            if (seenIds.contains(idStr))
                return QString("saveHistoryDays: id %1 used more than once").arg(idStr);
            seenIds.insert(idStr);

            if (upsert.startTime < day.dayStartUtc || upsert.endTime > day.dayEndUtc)
                return QString("saveHistoryDays: upsert %1 lies outside its day's span").arg(idStr);

            const StoredHistoryRow* row = findSnapshotRow(openSnapshot, idStr);
            if (row && !rowEditableInSpan(*row, day.dayStartUtc, day.dayEndUtc))
                return QString("saveHistoryDays: upsert id %1 is not editable for its day").arg(idStr);
        }

        const DayValidity::Violation violation = DayValidity::validatePayload(day.upserts);
        if (violation != DayValidity::Violation::None)
            return QString("saveHistoryDays: invalid upsert payload (%1)").arg(DayValidity::describe(violation));
    }

    return std::nullopt;
}

// Raw-column fingerprint of one row, for the stale-check multiset compare
// below. \x1f (unit separator) cannot appear in any of these columns (ids,
// integer type text, or ISO-8601 timestamps), so concatenation cannot
// collide two distinct row tuples into one fingerprint.
QString rowFingerprint(const StoredHistoryRow& r)
{
    return r.segmentId + '\x1f' + r.type + '\x1f' + r.startUtc + '\x1f' + r.endUtc + '\x1f' + r.isFinalized;
}

// True iff `a` and `b` hold the same rows as a MULTISET of raw columns,
// order-independent (the stale check in saveHistoryDays()).
bool sameRawRows(const std::vector<StoredHistoryRow>& a, const std::vector<StoredHistoryRow>& b)
{
    QStringList fpA, fpB;
    for (const auto& r : a) fpA << rowFingerprint(r);
    for (const auto& r : b) fpB << rowFingerprint(r);
    fpA.sort();
    fpB.sort();
    return fpA == fpB;
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
        // Apply the connection PRAGMAs so writes on this connection behave
        // correctly even before the schema is confirmed.
        applyConnectionPragmas();
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
    // D8 (R06-004): a transparent reconnect creates a brand-new native
    // connection, which loses every session-level PRAGMA the earlier
    // connection had set (e.g. synchronous=NORMAL reverts to SQLite's
    // default of FULL). Re-apply the centralized connection policy so a
    // reconnect is genuinely transparent to callers. Best-effort: failure
    // is logged inside applyConnectionPragmas() but does not fail the
    // reconnect itself, matching this PRAGMA's pre-existing non-fatal
    // treatment.
    applyConnectionPragmas();
    return true;
}

/**
 * D8 (R06-004): centralizes the connection-local PRAGMAs required for the
 * lifetime of any native SQLite connection this store opens. Called from
 * construction, ensureOpen()'s transparent reconnect, and createBackup()'s
 * reopen, so all three apply (and verify) the same policy instead of each
 * keeping its own copy -- which is how the reconnect path came to silently
 * omit synchronous=NORMAL in the first place.
 */
bool SqliteSessionStore::applyConnectionPragmas()
{
    QSqlQuery setQuery(db);
    if (!setQuery.exec("PRAGMA synchronous=NORMAL")) {
        Logger::Log("[DB] Warning: Failed to set synchronous=NORMAL: " + setQuery.lastError().text());
        return false;
    }
    QSqlQuery verifyQuery(db);
    if (!verifyQuery.exec("PRAGMA synchronous") || !verifyQuery.next()) {
        Logger::Log("[DB] Warning: Failed to verify synchronous mode: " + verifyQuery.lastError().text());
        return false;
    }
    if (verifyQuery.value(0).toInt() != 1) { // 1 == NORMAL
        Logger::Log(QString("[DB] Warning: synchronous mode is %1 after applying NORMAL")
            .arg(verifyQuery.value(0).toInt()));
        return false;
    }
    return true;
}

/**
 * D8 (R06-002): proves the database directory can actually host SQLite's
 * rollback journal, not just that the database file's own permission bits
 * are writable (isWritable() checks above only ever prove the latter). Runs
 * a real write against app_settings -- an INSERT OR REPLACE always dirties a
 * page and forces journal creation, unlike an UPDATE that could match zero
 * rows and dirty nothing -- inside a transaction that is unconditionally
 * rolled back, so success or failure leaves no visible effect. Requires
 * app_settings to already exist; callers run this after schema setup.
 */
bool SqliteSessionStore::verifyWriteCapability()
{
    if (!db.transaction()) {
        Logger::Log("[DB] verifyWriteCapability: failed to begin probe transaction: " + db.lastError().text());
        return false;
    }
    QSqlQuery probe(db);
    probe.prepare("INSERT OR REPLACE INTO app_settings (key, value) VALUES ('__write_probe__', '1')");
    bool ok = probe.exec();
    if (!ok) {
        Logger::Log("[DB] verifyWriteCapability: probe write failed: " + probe.lastError().text());
    }
    if (!db.rollback()) {
        Logger::Log("[DB] verifyWriteCapability: probe rollback failed: " + db.lastError().text());
        ok = false;
    }
    return ok;
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
        QString("CREATE TABLE IF NOT EXISTS durations (%1)").arg(kDurationsColumnsSql)
    );

    if (!schemaOk) {
        Logger::Log("[DB] Error creating table: " + query_new.lastError().text());
        return false;
    }

    if (!ensureSettingsTable()) {
        return false;
    }

    // Non-fatal: a missing discard_intents table only degrades A4's
    // discard-durability guarantee to ordinary (non-durable) TransientError
    // behavior; it does not indicate schema incompatibility.
    if (!ensureDiscardIntentsTable()) {
        Logger::Log("[DB] Warning: Failed to ensure discard_intents table");
    }

    QSqlQuery startUtcIndexQuery(db);
    if (!startUtcIndexQuery.exec(QString("CREATE INDEX IF NOT EXISTS %1").arg(kIdxStartUtcTail))) {
        Logger::Log("[DB] Warning: Failed to create start_utc index: " + startUtcIndexQuery.lastError().text());
    }

    // synchronous=NORMAL: in rollback journal mode this provides good crash
    // safety (only OS crash / power loss can lose data, not an app crash).
    // The shutdown path (flushToDisc) temporarily promotes to FULL for the
    // final durable write before the process exits.
    applyConnectionPragmas();

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
 *   Ready        — existing DB has the expected schema and demonstrated
 *                  write/journal capability; safe to use.
 *   Outdated     — the existing durations table's column names do not match,
 *                  OR (D8/R06-001) its full DDL text differs from the exact
 *                  canonical body ensureSchema() creates -- covering every
 *                  declared type, the id PRIMARY KEY/AUTOINCREMENT identity,
 *                  every NOT NULL/DEFAULT/CHECK constraint, and the
 *                  UNIQUE(segment_id) constraint in one comparison, not just
 *                  the column-name set. Caller must refuse to start.
 *   Inaccessible — the DB file could not be opened or read, OR (D8/R06-003)
 *                  the app_settings table is malformed and its atomic
 *                  DROP+CREATE+COMMIT repair failed (the old table/marker are
 *                  preserved, but startup still refuses rather than reporting
 *                  a stronger state than the marker/recovery subsystem can
 *                  deliver), OR (D8/R06-002) a real write inside a rolled-
 *                  back transaction failed, proving the database directory
 *                  cannot host SQLite's rollback journal even though the
 *                  file's own permission bits are writable.
 *
 * idx_start_utc: if the partial index is absent, or present with the wrong
 * definition (D8/R06-001: a same-named index of any shape silently survives
 * "CREATE INDEX IF NOT EXISTS"), it is dropped/(re)created rather than
 * returning Outdated, because the index can be rebuilt without data loss and
 * its absence/mismatch does not indicate schema incompatibility.
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
        // D8 (R06-002): prove the directory can actually host a rollback
        // journal, not just that ensureSchema()'s CREATE TABLE succeeded.
        // Not literally the first write on this path -- performRetentionCleanup()
        // and pruneOldBackups() above already wrote -- but those failures are
        // non-fatal by design, so this is the first write whose failure is
        // gating: the point where an unwritable directory gets reported.
        if (!verifyWriteCapability()) {
            Logger::Log("[DB] checkSchemaOnStartup: write-capability probe failed on fresh DB");
            return SchemaStatus::Inaccessible;
        }
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

    // D8 (R06-001): the column-name set alone does not prove declared types,
    // PRIMARY KEY/AUTOINCREMENT identity, NOT NULL/DEFAULT/CHECK
    // constraints, or that segment_id's uniqueness is enforced by an inline
    // (non-partial) constraint rather than some other shape. See
    // kDurationsColumnsSql's comment above for exactly how/why SQLite's
    // sqlite_master.sql storage of a CREATE TABLE statement's text makes one
    // exact-text comparison against the same canonical body ensureSchema()
    // creates catch every one of those semantic properties at once, without
    // re-deriving each individually.
    {
        QSqlQuery sqlQuery(db);
        if (!sqlQuery.exec("SELECT sql FROM sqlite_master WHERE type='table' AND name='durations'")) {
            Logger::Log("[DB] checkSchemaOnStartup: sqlite_master lookup failed: " + sqlQuery.lastError().text());
            return SchemaStatus::Inaccessible;
        }
        const QString expectedSql = QString("CREATE TABLE durations (%1)").arg(kDurationsColumnsSql);
        const QString actualSql = sqlQuery.next() ? sqlQuery.value(0).toString() : QString();
        if (actualSql != expectedSql) {
            Logger::Log(QString("[DB] checkSchemaOnStartup: durations table definition mismatch. "
                                "Expected \"%1\", found \"%2\"")
                .arg(expectedSql, actualSql));
            return SchemaStatus::Outdated;
        }
    }

    // D8 review round 2: a separate PRAGMA index_list/index_info pass that
    // rejected a missing/partial UNIQUE(segment_id) used to live here. It was
    // strictly dominated by the full-text comparison above -- that check only
    // ever admits a durations table whose DDL IS the canonical body, which
    // contains "UNIQUE(segment_id)" as a table constraint, guaranteeing a
    // unique, non-partial sqlite_autoindex -- so the block could never
    // actually return Outdated. Mutation testing (deleting it) left the
    // suite green, including test_schema_status_outdated_missing_unique_constraint,
    // which the text check alone already covers. Removed rather than kept as
    // "defense in depth": an unreachable branch is the kind of split-across-
    // several-partial-checks obscurity R06-001 was filed against.

    // D8 (R06-001): idx_start_utc's predicate is part of what later queries
    // (hasEntriesForDate, orphan overlap probing) rely on. "CREATE INDEX IF
    // NOT EXISTS" below silently no-ops on a same-named index regardless of
    // its definition, so a wrong existing definition (missing/different
    // WHERE clause, non-partial) must be dropped here first so it gets
    // rebuilt correctly -- this is a repair, not an incompatibility, so it
    // does not return Outdated.
    {
        QSqlQuery sqlQuery(db);
        if (sqlQuery.exec("SELECT sql FROM sqlite_master WHERE type='index' AND name='idx_start_utc'")
            && sqlQuery.next()) {
            const QString expectedSql = QString("CREATE INDEX %1").arg(kIdxStartUtcTail);
            const QString actualSql = sqlQuery.value(0).toString();
            // Finalize this statement before running DDL on the same
            // connection -- SQLite refuses schema-modifying statements
            // ("database table is locked") while another statement on the
            // same connection still has an open cursor.
            sqlQuery.finish();
            if (actualSql != expectedSql) {
                QSqlQuery dropQuery(db);
                if (!dropQuery.exec("DROP INDEX idx_start_utc")) {
                    Logger::Log("[DB] Warning: Failed to drop mismatched idx_start_utc: " + dropQuery.lastError().text());
                }
            }
        }
    }

    // Additive DDL that is safe to (re)create on any validated existing DB.
    // These objects are absent when upgrading from older app versions that
    // pre-date app_settings.  Running them here (after
    // validation passes) ensures no DDL touches an incompatible DB.
    //
    // D8 (R06-003): unlike the other additive objects, app_settings is not
    // optional metadata -- it holds the clean-shutdown marker that recovery
    // reads before reconciling orphans. Reporting Ready after a failed
    // ensure/repair would advertise a stronger startup state than the
    // marker/recovery subsystem can actually deliver, so a failure here
    // refuses startup instead of continuing.
    if (!ensureSettingsTable()) {
        Logger::Log("[DB] checkSchemaOnStartup: failed to ensure app_settings table on existing DB — refusing to start");
        return SchemaStatus::Inaccessible;
    }

    if (!ensureDiscardIntentsTable()) {
        Logger::Log("[DB] Warning: Failed to ensure discard_intents table on existing DB");
        // Non-fatal: log and continue (see ensureSchema() for rationale).
    }

    // (Re)create idx_start_utc if absent or just dropped above for having a
    // wrong definition. Safe to rebuild without data loss either way.
    {
        QSqlQuery startUtcIndexQuery(db);
        if (!startUtcIndexQuery.exec(QString("CREATE INDEX IF NOT EXISTS %1").arg(kIdxStartUtcTail))) {
            Logger::Log("[DB] Warning: Failed to recreate idx_start_utc: " + startUtcIndexQuery.lastError().text());
            // Non-fatal: log and continue.
        }
    }

    // Retention cleanup and backup pruning run once per startup, after schema is
    // confirmed valid.  Failures are non-fatal (logged, but do not abort startup).
    performRetentionCleanup();
    pruneOldBackups();

    // D8 (R06-002): prove the directory can actually host a rollback
    // journal before declaring the DB Ready for use. Not literally the
    // first write here either -- performRetentionCleanup()/pruneOldBackups()
    // just above already wrote -- but the first one whose failure gates
    // startup instead of being logged and ignored.
    if (!verifyWriteCapability()) {
        Logger::Log("[DB] checkSchemaOnStartup: write-capability probe failed");
        return SchemaStatus::Inaccessible;
    }

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
    const QDateTime threshold = now_provider_().addDays(-static_cast<int>(history_days_to_keep_));
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
    bool keyIsPrimaryKey = false;
    bool valueIsNotNull = false;
    while (colQuery.next()) {
        const QString name = colQuery.value(1).toString(); // column 1 = name
        foundColumns.insert(name);
        if (name == "key") {
            keyIsPrimaryKey = colQuery.value(5).toInt() == 1; // column 5 = pk
        } else if (name == "value") {
            valueIsNotNull = colQuery.value(3).toInt() == 1; // column 3 = notnull
        }
    }

    static const QSet<QString> kExpectedColumns = {"key", "value"};
    if (foundColumns == kExpectedColumns && keyIsPrimaryKey && valueIsNotNull) {
        return true;
    }

    Logger::Log(QString("[DB] app_settings has unexpected columns/constraints {%1} - rebuilding")
        .arg(QStringList(foundColumns.values()).join(", ")));

    // D8 (R06-003): DROP+CREATE must be one atomic operation. The old table
    // (and any clean-shutdown marker row in it) is only ever given up once
    // the whole rebuild -- DROP, CREATE, and COMMIT -- has succeeded; any
    // failure along the way rolls back, leaving the old table and marker
    // exactly as they were rather than losing the table (DROP succeeded,
    // CREATE/COMMIT did not) or the marker (DROP unconditionally discarded
    // it regardless of what followed).
    if (!db.transaction()) {
        Logger::Log("[DB] Error starting app_settings rebuild transaction: " + db.lastError().text());
        return false;
    }

    QSqlQuery dropQuery(db);
#ifndef QT_NO_DEBUG
    if (force_settings_drop_failure_dbg_) {
        force_settings_drop_failure_dbg_ = false;
        Logger::Log("[DB] Error dropping malformed app_settings table: injected failure (test)");
        db.rollback();
        return false;
    }
#endif
    if (!dropQuery.exec("DROP TABLE app_settings")) {
        Logger::Log("[DB] Error dropping malformed app_settings table: " + dropQuery.lastError().text());
        db.rollback();
        return false;
    }

    QSqlQuery recreateQuery(db);
#ifndef QT_NO_DEBUG
    if (force_settings_create_failure_dbg_) {
        force_settings_create_failure_dbg_ = false;
        Logger::Log("[DB] Error re-creating app_settings table: injected failure (test)");
        db.rollback();
        return false;
    }
#endif
    if (!recreateQuery.exec(kCreateSql)) {
        Logger::Log("[DB] Error re-creating app_settings table: " + recreateQuery.lastError().text());
        db.rollback();
        return false;
    }

#ifndef QT_NO_DEBUG
    // D8 review round 2 (finding 7ii): scope the injected commit failure to
    // THIS call site by arming the shared seam right here, rather than
    // requiring the test to arm force_commit_failure_dbg_ itself and hope no
    // earlier commit on the path consumes it first.
    if (force_settings_commit_failure_dbg_) {
        force_settings_commit_failure_dbg_ = false;
        force_commit_failure_dbg_ = true;
    }
#endif
    const CommitOutcome outcome = commitOrRollback("ensureSettingsTable rebuild");
    return outcome.committed;
}

// A4 (R05-001): discard_intents is a durable tombstone table. A row here
// means the engine has deliberately rejected a segment (currently: the
// cross-midnight watchdog/scheduled-stop discard) and its unfinalized
// checkpoint row must never be resurrected by startup recovery, even if the
// physical row delete itself keeps failing. IF NOT EXISTS makes this safe to
// call from both a fresh-DB build and an existing-DB upgrade.
bool SqliteSessionStore::ensureDiscardIntentsTable()
{
    QSqlQuery query(db);
    if (!query.exec(
            "CREATE TABLE IF NOT EXISTS discard_intents ("
            "segment_id TEXT PRIMARY KEY"
            ")")) {
        Logger::Log("[DB] Error creating discard_intents table: " + query.lastError().text());
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
 * - This runs before 'replaceAll' (major save) but not 'saveCheckpoint' (frequent).
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
    // corrupted state; report it as a missing backup and let the caller decide.
    {
        QStorageInfo storage(QFileInfo(db.databaseName()).absoluteDir().absolutePath());
        const qint64 available = storage.bytesAvailable();
#ifndef QT_NO_DEBUG
        const bool lowDisk = force_backup_low_disk_dbg_ || (available >= 0 && available < 100LL * 1024 * 1024);
        force_backup_low_disk_dbg_ = false;
#else
        const bool lowDisk = (available >= 0 && available < 100LL * 1024 * 1024);
#endif
        if (lowDisk) {
            Logger::Log(QString("[DB] Warning: Low disk space (%1 MB). Skipping backup.")
                .arg(available / (1024 * 1024)));
            return BackupResult::BackupFailed;
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
#ifndef QT_NO_DEBUG
    bool success;
    if (force_backup_copy_failure_dbg_) {
        success = false;
        force_backup_copy_failure_dbg_ = false;
    } else {
        success = QFile::copy(db.databaseName(), backupName);
    }
#else
    bool success = QFile::copy(db.databaseName(), backupName);
#endif

    if (!success) {
        Logger::Log("[DB] Error: Failed to create backup of database");
    } else {
        Logger::Log("[DB] Created database backup: " + backupName);
        lastBackupPath_ = backupName;
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
            out << "Duration: " << d.duration() << "ms | ";
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
    // D8 (R06-004): re-apply the centralized connection PRAGMAs since
    // reopening creates a fresh connection that loses all session-level
    // PRAGMAs set earlier.
    if (wasOpen) {
        if (!db.open()) {
            Logger::Log("[DB] CRITICAL: Failed to reopen database after backup: " + db.lastError().text());
            return BackupResult::ReopenFailed;
        }
        applyConnectionPragmas();
    }

    return success ? BackupResult::Success : BackupResult::BackupFailed;
}

/**
 * Persists the completed segments of a session to the database.
 *
 * Normalizes the timeline internally (merging adjacent same-type segments),
 * computes orphaned segment IDs, and delegates to updateDurationsById()
 * with the clean deque and orphan list. Callers never see removedSegmentIds.
 *
 * C3 (R13-001/R14-001): the raw payload is validated as a final guard before
 * any mutation (checked before normalizing, since normalization only merges
 * same-type rows and cannot fix or hide any of these violations).
 * commitSession() only ever carries uTimer's own live-session data, which is
 * always same-day by construction, so cross-day is rejected along with every
 * other violation.
 */
SessionStoreResult SqliteSessionStore::commitSession(const Timeline& session)
{
    // C4 (R05-003): CallerBug is checked before the Disabled guard -- see the
    // precedence rule in sessionstore.h. Validate the raw payload before
    // normalizing: normalization only merges same-type touching/overlapping
    // rows, so it neither introduces nor hides any of these violations, and
    // checking first avoids depending on Timeline::normalized()'s own
    // same-day debug assertion to catch this.
    const DayValidity::Violation violation = DayValidity::validatePayload(session.completed());
    if (violation != DayValidity::Violation::None) {
        Logger::Log("[DB] commitSession: rejecting invalid payload (" + DayValidity::describe(violation) + ")");
        return SessionStoreResult::callerBug("commitSession: invalid payload (" + DayValidity::describe(violation) + ")");
    }
    if (history_days_to_keep_ == 0) {
        return SessionStoreResult::disabled();
    }
    const auto [normed, orphanIds] = session.normalizedWithRemovedIds();
    return updateDurationsById(normed.completed(), orphanIds);
}

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

// C2 (R07-001): see header for the invariant this enforces. Assumes the
// caller already holds an open transaction and has performed its mutation;
// never begins one. There is exactly one commit path and one
// rollback/close path below so an injected test fault falls through the
// same code a real failure would.
SqliteSessionStore::CommitOutcome SqliteSessionStore::commitOrRollback(const QString& logContext)
{
    bool committed;
    QString err;
#ifndef QT_NO_DEBUG
    if (force_commit_failure_dbg_) {
        force_commit_failure_dbg_ = false;
        committed = false;
        err = "injected commit failure (test)";
    } else
#endif
    {
#ifndef QT_NO_DEBUG
        if (force_commit_close_before_dbg_) {
            force_commit_close_before_dbg_ = false;
            db.close();
        }
#endif
        committed = db.commit();
        if (!committed) {
            err = db.lastError().text();
        }
    }
    if (committed) {
        return CommitOutcome{true, QString()};
    }

    Logger::Log("[DB] " + logContext + ": " + err);

    bool rolledBack;
    QString rollbackErr;
#ifndef QT_NO_DEBUG
    if (force_rollback_failure_dbg_) {
        force_rollback_failure_dbg_ = false;
        rolledBack = false;
        rollbackErr = "injected rollback failure (test)";
    } else
#endif
    {
        rolledBack = db.rollback();
        if (!rolledBack) {
            rollbackErr = db.lastError().text();
        }
    }
    if (!rolledBack) {
        Logger::Log("[DB] Warning: rollback after commit failure also failed: " + rollbackErr);
        // The connection cannot be trusted to be outside the transaction
        // anymore -- close it so the next ensureOpen() reconnects clean
        // rather than leaving every subsequent db.transaction() failing
        // forever (R07-001).
        closeConnection();
    }
    return CommitOutcome{false, err};
}

SessionStoreResult SqliteSessionStore::replaceAll(const Timeline& history, const Timeline& session)
{
    QMutexLocker locker(&db_mutex_);

    const std::deque<TimeDuration>& historyDurations = history.completed();
    const std::deque<TimeDuration>& currentSessionDurations = session.completed();

    std::deque<TimeDuration> allDurations;
    allDurations.insert(allDurations.end(), historyDurations.begin(), historyDurations.end());
    allDurations.insert(allDurations.end(), currentSessionDurations.begin(), currentSessionDurations.end());

    // C3 (R13-001/R14-001): reject the entire payload before any mutation, the
    // same uniform rule as every other write boundary. HistoryEditSession
    // never carries a legacy cross-midnight or pre-existing cross-type-overlap
    // row this far -- it drops both from the save payload before calling
    // replaceAll() (see historyeditsession.cpp's buildSavePayload()) -- so
    // this guard should never actually trigger from the History path; it
    // remains the final backstop for any other caller.
    // C4 (R05-003): checked before the Disabled guard below -- see the
    // precedence rule in sessionstore.h.
    const DayValidity::Violation violation = DayValidity::validatePayload(allDurations);
    if (violation != DayValidity::Violation::None) {
        Logger::Log("[DB] replaceAll: rejecting invalid payload (" + DayValidity::describe(violation) + ")");
        return SessionStoreResult::callerBug("replaceAll: invalid payload (" + DayValidity::describe(violation) + ")");
    }

    if (history_days_to_keep_ == 0) {
        return SessionStoreResult::disabled();
    }

    if (!ensureOpen()) {
        // C4 (R07-003): ensureOpen() failure is TransientError consistently
        // everywhere it is checked -- see updateDurationsById for the rationale.
        Logger::Log("[DB] Could not open DB to replace durations");
        return SessionStoreResult::transient("DB connection could not be opened");
    }

    const BackupResult backup = createBackup(allDurations, TransactionMode::Replace);
    if (backup == BackupResult::ReopenFailed) {
        // Not part of the C4/R07-003 ensureOpen() consistency table: this is
        // db.open() failing to reopen a connection createBackup() itself just
        // closed for the copy window, not the initial ensureOpen() health
        // check above -- a mid-operation loss right after a working
        // connection, which stays FatalError.
        Logger::Log("[DB] CRITICAL: DB connection lost after backup - aborting replace");
        return SessionStoreResult::fatal("DB connection lost after backup");
    }
    const bool backupMissing = (backup != BackupResult::Success);
    if (backupMissing) {
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

    const CommitOutcome commitResult = commitOrRollback("Error committing replace transaction");
    if (!commitResult.committed) {
        return SessionStoreResult::transient("Failed to commit replace transaction: " + commitResult.errorMessage);
    }

#ifndef QT_NO_DEBUG
    checkSegmentIdUniqueness();
#endif

    SessionStoreResult result = SessionStoreResult::success();
    result.backupMissing = backupMissing;
    return result;
}

/**
 * Shared row-validity parse (H5): applies the same rules loadDurations() has
 * always used to a row's raw text columns. Returns the parsed TimeDuration
 * (local time) iff the row is well-formed; nullopt for any of: empty
 * segment_id, out-of-range type, unparseable timestamp, start > end, or a
 * non-positive computed interval. Does not log or delete -- that remains
 * each caller's own policy.
 */
std::optional<TimeDuration> SqliteSessionStore::parseHistoryRow(const QString& segmentIdStr, const QString& typeStr,
                                                                 const QString& startUtcStr, const QString& endUtcStr)
{
    if (segmentIdStr.isEmpty()) {
        return std::nullopt;
    }

    const int typeInt = typeStr.toInt();
    if (typeInt != static_cast<int>(DurationType::Activity) &&
        typeInt != static_cast<int>(DurationType::Pause)) {
        return std::nullopt;
    }
    const DurationType type = static_cast<DurationType>(typeInt);

    const QDateTime startDateTime = QDateTime::fromString(startUtcStr, Qt::ISODateWithMs).toLocalTime();
    const QDateTime endDateTime   = QDateTime::fromString(endUtcStr, Qt::ISODateWithMs).toLocalTime();
    if (!startDateTime.isValid() || !endDateTime.isValid()) {
        return std::nullopt;
    }

    if (startDateTime > endDateTime) {
        return std::nullopt;
    }
    if (startDateTime.msecsTo(endDateTime) <= 0) {
        return std::nullopt;
    }

    const SegmentId segmentId = SegmentId::fromString(segmentIdStr);
    return TimeDuration::fromPersistedRow(type, startDateTime, endDateTime, segmentId);
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
        // C4 (R07-003): ensureOpen() failure is TransientError consistently
        // everywhere it is checked -- see updateDurationsById for the rationale.
        Logger::Log("[DB] Could not open DB to load Durations");
        result.status = SessionStoreResult::TransientError;
        return result;
    }

    // Start read transaction for consistent snapshot
    if (!db.transaction()) {
        Logger::Log("[DB] Error starting read transaction: " + db.lastError().text());
        result.status = SessionStoreResult::TransientError;
        return result;
    }

    // Load all durations ordered by start_utc. `id` is carried only so a
    // malformed row can be deleted below (C3); it is never returned to the caller.
    QSqlQuery query(db);
    query.prepare("SELECT id, segment_id, type, start_utc, end_utc FROM durations WHERE is_finalized = 1 ORDER BY start_utc");
    if (!query.exec()) {
        db.rollback();
        Logger::Log("[DB] Error executing load query: " + query.lastError().text());
        result.status = SessionStoreResult::TransientError;
        return result;
    }

    // C3 (R13-001/R14-001, extends B2's recovery-time half to ordinary load):
    // ids of rows skipped below for a genuinely malformed reason (invalid
    // timestamp/type, empty id, non-positive interval). These are deleted
    // transactionally after the read transaction below so malformed rows
    // never escape the store to be silently purged later by an unrelated edit.
    std::vector<long long> malformedIds;
    int skippedCount = 0;

    // Parse each row and reconstruct TimeDuration objects. Validity rules are
    // shared with loadHistoryRows() via parseHistoryRow() (H5); this method
    // additionally logs the malformed row's raw values below and queues it
    // for deletion -- loadHistoryRows() does neither.
    while (query.next()) {
        const long long rowId = query.value(0).toLongLong();
        const QString segmentIdStr = query.value(1).toString();
        const QString typeStr = query.value(2).toString();
        const QString startUtcStr = query.value(3).toString();
        const QString endUtcStr = query.value(4).toString();

        const std::optional<TimeDuration> parsed = parseHistoryRow(segmentIdStr, typeStr, startUtcStr, endUtcStr);
        if (!parsed) {
            // An empty segment_id, an out-of-range type, an unparseable
            // timestamp, start > end, or a non-positive interval are all
            // corrupt-row conditions (C3) -- logged together here since
            // parseHistoryRow() no longer distinguishes which one fired.
            Logger::Log(QString("[DB] Warning: Skipped malformed row (id=%1, segment_id='%2', type='%3', start='%4', end='%5')")
                .arg(rowId).arg(segmentIdStr, typeStr, startUtcStr, endUtcStr));
            skippedCount++;
            malformedIds.push_back(rowId);
            continue;
        }

        // Cross-midnight rows are accepted via fromPersistedRow() so that
        // createPages() can bucket them onto both the start-date and end-date pages.
        result.durations.push_back(*parsed);
    }

    // Rollback is idiomatic for read-only transactions: no writes were made,
    // so there is nothing to commit. Both commit() and rollback() release the
    // transaction lock, but rollback() signals the intent more clearly.
    db.rollback();

    // C3: delete every malformed row found above in one transaction, logged
    // with a single count. Best-effort: a failure here is logged and left for
    // the next load to retry; it does not change the LoadResult already built
    // from valid rows.
    if (!malformedIds.empty()) {
        if (db.transaction()) {
            QSqlQuery deleteQuery(db);
            deleteQuery.prepare("DELETE FROM durations WHERE id = :id");
            bool deleteOk = true;
            for (long long id : malformedIds) {
                deleteQuery.bindValue(":id", static_cast<qlonglong>(id));
                if (!deleteQuery.exec()) {
                    Logger::Log("[DB] loadDurations: error deleting malformed row: " + deleteQuery.lastError().text());
                    deleteOk = false;
                    break;
                }
            }
            if (deleteOk) {
                // C4 review (finding 4): route through the same commit-or-
                // rollback helper every other write boundary uses, so a
                // genuine commit failure here is provably rolled back
                // (connection restored to autocommit-ready) rather than
                // left half-applied, and the C2 test injection seam
                // (setForceCommitFailure_dbg()) covers this path too.
                const CommitOutcome commitResult = commitOrRollback(
                    "loadDurations: failed to delete malformed rows; will retry on next load");
                if (commitResult.committed) {
                    Logger::Log(QString("[DB] loadDurations: deleted %1 malformed row(s)").arg(malformedIds.size()));
                }
            } else {
                db.rollback();
                Logger::Log("[DB] loadDurations: failed to delete malformed rows; will retry on next load");
            }
        } else {
            Logger::Log("[DB] loadDurations: could not start transaction to delete malformed rows");
        }
    }

    if (skippedCount > 0) {
        Logger::Log(QString("[DB] loadDurations reconciliation summary: skipped=%1").arg(skippedCount));
    }

    return result;
}

/**
 * Read-only raw query (H5): returns every row in the durations table exactly
 * as stored, sorted chronologically. Unlike loadDurations(), never filters by
 * is_finalized and never deletes or repairs a malformed row -- this exists so
 * a caller (History's date-scoped editor) can see the true persisted state,
 * including rows loadDurations() would otherwise silently skip or purge.
 */
HistoryRowsResult SqliteSessionStore::loadHistoryRows()
{
    QMutexLocker locker(&db_mutex_);

    HistoryRowsResult result;

    if (history_days_to_keep_ == 0) {
        result.status = SessionStoreResult::Disabled;
        return result;
    }

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to load history rows");
        result.status = SessionStoreResult::TransientError;
        return result;
    }

    // Read transaction for a consistent snapshot, matching loadDurations().
    if (!db.transaction()) {
        Logger::Log("[DB] Error starting read transaction: " + db.lastError().text());
        result.status = SessionStoreResult::TransientError;
        return result;
    }

    QSqlQuery query(db);
    query.prepare("SELECT segment_id, type, start_utc, end_utc, is_finalized FROM durations ORDER BY start_utc, id");
    if (!query.exec()) {
        db.rollback();
        Logger::Log("[DB] Error executing loadHistoryRows query: " + query.lastError().text());
        result.status = SessionStoreResult::TransientError;
        return result;
    }

    while (query.next()) {
        StoredHistoryRow row;
        row.segmentId = query.value(0).toString();
        row.type = query.value(1).toString();
        row.startUtc = query.value(2).toString();
        row.endUtc = query.value(3).toString();
        row.isFinalized = query.value(4).toString();
        row.parsed = parseHistoryRow(row.segmentId, row.type, row.startUtc, row.endUtc);
        result.rows.push_back(std::move(row));
    }

    // Rollback is idiomatic for a read-only transaction: nothing was written.
    db.rollback();

    return result;
}

/**
 * H5/WP2: re-reads every row whose [start_utc, end_utc) overlaps
 * [dayStartUtc, dayEndUtc), using the same text predicate hasEntriesForDate()
 * uses. Assumes the caller already holds an open transaction (does not begin
 * or end one); returns nullopt on a SQL failure, leaving rollback and the
 * TransientError mapping to the caller.
 */
std::optional<std::vector<StoredHistoryRow>> SqliteSessionStore::selectRowsForSpan(const QDateTime& dayStartUtc, const QDateTime& dayEndUtc)
{
    QSqlQuery query(db);
    query.prepare("SELECT segment_id, type, start_utc, end_utc, is_finalized FROM durations "
                  "WHERE start_utc < :dayEnd AND end_utc > :dayStart ORDER BY start_utc, segment_id");
    query.bindValue(":dayStart", toUtcIso(dayStartUtc));
    query.bindValue(":dayEnd", toUtcIso(dayEndUtc));
    if (!query.exec()) {
        Logger::Log("[DB] saveHistoryDays: error re-reading day span: " + query.lastError().text());
        return std::nullopt;
    }

    std::vector<StoredHistoryRow> rows;
    while (query.next()) {
        StoredHistoryRow row;
        row.segmentId = query.value(0).toString();
        row.type = query.value(1).toString();
        row.startUtc = query.value(2).toString();
        row.endUtc = query.value(3).toString();
        row.isFinalized = query.value(4).toString();
        row.parsed = parseHistoryRow(row.segmentId, row.type, row.startUtc, row.endUtc);
        rows.push_back(std::move(row));
    }
    return rows;
}

/**
 * H5/WP2: scoped, date-limited History save. See sessionstore.h for the full
 * contract; summary of the flow, all inside one transaction covering every
 * day in `days`:
 *
 *   1. CallerBug input-shape checks (validateHistoryDaysCallerFacts) --
 *      before any mutation, before the Disabled guard.
 *   2. Disabled / ensureOpen() / BEGIN early exits.
 *   3. Per day: re-read the day, compare against the matching openSnapshot
 *      rows as a multiset of raw columns -- any difference is Conflict.
 *   4. Per day: any new/changed row overlapping a retained row of the other
 *      type is CallerBug (U3, dayvalidity.h's hasNewCrossTypeOverlap()).
 *   5. Apply every day's deletes and upserts.
 *   6. Commit via commitOrRollback(); checkSegmentIdUniqueness() in debug.
 *
 * Steps 3-4 run for every day before step 5 touches anything, so a Conflict
 * or CallerBug found on a later day still leaves every earlier day's data
 * unmutated.
 */
SessionStoreResult SqliteSessionStore::saveHistoryDays(const std::vector<StoredHistoryRow>& openSnapshot,
                                                        const std::vector<HistoryDayChange>& days)
{
    QMutexLocker locker(&db_mutex_);

    const std::optional<QString> callerBug = validateHistoryDaysCallerFacts(openSnapshot, days);
    if (callerBug) {
        Logger::Log("[DB] saveHistoryDays: rejecting invalid payload (" + *callerBug + ")");
        return SessionStoreResult::callerBug(*callerBug);
    }

    if (history_days_to_keep_ == 0) {
        return SessionStoreResult::disabled();
    }

    if (!ensureOpen()) {
        Logger::Log("[DB] Could not open DB to save history days");
        return SessionStoreResult::transient("DB connection could not be opened");
    }

    if (!db.transaction()) {
        Logger::Log("[DB] Error starting transaction for saveHistoryDays: " + db.lastError().text());
        return SessionStoreResult::transient("Failed to start transaction: " + db.lastError().text());
    }

    // M1: every early-exit rollback below must also close the connection if
    // the rollback itself fails -- otherwise the connection is left inside an
    // open transaction (holding any earlier day's already-applied UPDATE/
    // DELETE/INSERT) and every subsequent db.transaction() on it fails until
    // restart. Mirrors commitOrRollback()'s own close-on-failed-rollback
    // policy; also honours force_rollback_failure_dbg_ so a bare rollback
    // here can be fault-injected the same way commitOrRollback()'s can.
    auto rollbackOrClose = [this]() {
#ifndef QT_NO_DEBUG
        if (force_rollback_failure_dbg_) {
            force_rollback_failure_dbg_ = false;
            closeConnection();
            return;
        }
#endif
        if (!db.rollback()) {
            closeConnection();
        }
    };

    // ---- Stale check + new-conflict check, per day, before any mutation ----
    for (const auto& day : days) {
        const QString dayStartText = toUtcIso(day.dayStartUtc);
        const QString dayEndText = toUtcIso(day.dayEndUtc);

        const std::optional<std::vector<StoredHistoryRow>> reread = selectRowsForSpan(day.dayStartUtc, day.dayEndUtc);
        if (!reread) {
            rollbackOrClose();
            return SessionStoreResult::transient("Failed to re-read day for staleness check");
        }

        // The same text predicate applied to the snapshot in C++ (the
        // canonical-text assumption documented in sessionstore.h).
        std::vector<StoredHistoryRow> snapshotSubset;
        for (const auto& row : openSnapshot) {
            if (row.startUtc < dayEndText && row.endUtc > dayStartText)
                snapshotSubset.push_back(row);
        }

        if (!sameRawRows(*reread, snapshotSubset)) {
            rollbackOrClose();
            Logger::Log("[DB] saveHistoryDays: stale data detected for " + day.date.toString(Qt::ISODate));
            return SessionStoreResult::conflict("History changed since it was opened");
        }

        QSet<QString> touchedIds;
        for (const auto& id : day.deletedIds)
            touchedIds.insert(id.toString());
        for (const auto& upsert : day.upserts)
            touchedIds.insert(upsert.segment_id.toString());

        std::deque<TimeDuration> retained;
        for (const auto& row : *reread) {
            if (row.parsed && !touchedIds.contains(row.segmentId))
                retained.push_back(*row.parsed);
        }

        if (DayValidity::hasNewCrossTypeOverlap(day.upserts, retained)) {
            rollbackOrClose();
            return SessionStoreResult::callerBug("saveHistoryDays: new row overlaps a retained row of the other type");
        }
    }

    // ---- Apply: deletes, then updates/inserts, for every day ----
    QSqlQuery deleteQuery(db);
    deleteQuery.prepare("DELETE FROM durations WHERE segment_id = :id AND is_finalized = 1");
    QSqlQuery updateQuery(db);
    updateQuery.prepare("UPDATE durations SET type = :type, start_utc = :start_utc, end_utc = :end_utc "
                         "WHERE segment_id = :id AND is_finalized = 1");
    QSqlQuery insertQuery(db);
    insertQuery.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                         "VALUES (:id, :type, :start_utc, :end_utc, 1)");

    for (const auto& day : days) {
        for (const auto& id : day.deletedIds) {
            deleteQuery.bindValue(":id", id.toString());
            if (!deleteQuery.exec()) {
                rollbackOrClose();
                Logger::Log("[DB] saveHistoryDays: error deleting row: " + deleteQuery.lastError().text());
                return SessionStoreResult::transient("Failed to delete row: " + deleteQuery.lastError().text());
            }
            if (deleteQuery.numRowsAffected() != 1) {
                rollbackOrClose();
                Logger::Log("[DB] saveHistoryDays: delete affected an unexpected row count");
                return SessionStoreResult::conflict("History changed since it was opened");
            }
        }

        for (const auto& upsert : day.upserts) {
            const QString idStr = upsert.segment_id.toString();
            const QString startUtcStr = toUtcIso(upsert.startTime);
            const QString endUtcStr = toUtcIso(upsert.endTime);
            const bool isUpdate = findSnapshotRow(openSnapshot, idStr) != nullptr;

            if (isUpdate) {
                updateQuery.bindValue(":id", idStr);
                updateQuery.bindValue(":type", static_cast<int>(upsert.type));
                updateQuery.bindValue(":start_utc", startUtcStr);
                updateQuery.bindValue(":end_utc", endUtcStr);
                if (!updateQuery.exec()) {
                    rollbackOrClose();
                    Logger::Log("[DB] saveHistoryDays: error updating row: " + updateQuery.lastError().text());
                    return SessionStoreResult::transient("Failed to update row: " + updateQuery.lastError().text());
                }
                if (updateQuery.numRowsAffected() != 1) {
                    rollbackOrClose();
                    Logger::Log("[DB] saveHistoryDays: update affected an unexpected row count");
                    return SessionStoreResult::conflict("History changed since it was opened");
                }
            } else {
                insertQuery.bindValue(":id", idStr);
                insertQuery.bindValue(":type", static_cast<int>(upsert.type));
                insertQuery.bindValue(":start_utc", startUtcStr);
                insertQuery.bindValue(":end_utc", endUtcStr);
                if (!insertQuery.exec()) {
                    rollbackOrClose();
                    Logger::Log("[DB] saveHistoryDays: error inserting row: " + insertQuery.lastError().text());
                    return SessionStoreResult::transient("Failed to insert row: " + insertQuery.lastError().text());
                }
            }
        }
    }

    const CommitOutcome commitResult = commitOrRollback("Error committing saveHistoryDays transaction");
    if (!commitResult.committed) {
        return SessionStoreResult::transient("Failed to commit saveHistoryDays transaction: " + commitResult.errorMessage);
    }

#ifndef QT_NO_DEBUG
    checkSegmentIdUniqueness();
#endif

    return SessionStoreResult::success();
}

/**
 * Checks whether finalized entries overlap a given local date.
 *
 * Returns a tri-state result:
 * - Yes:     At least one finalized entry overlaps the date's [00:00, 24:00)
 *            local half-open span (DayValidity::overlapsLocalDate).
 * - No:      The DB was queried successfully and no entry overlaps.
 * - Unknown: The DB could not be opened (history disabled or open failure).
 *            Callers should treat Unknown conservatively — e.g. do NOT assume
 *            "no entries exist" when the answer is actually unknowable.
 *
 * D1 (R08-004): this matches on overlap, not merely on whether the row
 * *started* that day. Every row a current uTimer write path can produce is
 * same-day, so overlap and start-date agree for all conforming data; they
 * diverge only for a legacy cross-midnight row already sitting in someone's
 * database (out of contract, but loadable -- see C3). The sole caller,
 * boot-credit eligibility, must never double-count time a legacy row already
 * covers, so overlap is the conservative and correct semantics here.
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
    const QDateTime utcStart = DayValidity::localDayStart(date).toUTC();
    const QDateTime utcEnd   = DayValidity::localDayEnd(date).toUTC();

    // Check if any finalized entry overlaps that UTC range.
    QSqlQuery query(db);
    query.prepare(
        "SELECT 1 FROM durations"
        " WHERE is_finalized = 1"
        "   AND start_utc < :utcEndTs"
        "   AND end_utc   > :utcStartTs"
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
 * Unlike replaceAll(), this method:
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

    // C4 fix round 2 (MEDIUM-6): checked before the Disabled guard below --
    // see the precedence rule in sessionstore.h (CallerBug wins over
    // Disabled), matching commitSession/replaceAll. A checkpoint is always
    // uTimer's own live segment, so it must be strictly same-day like every
    // other write boundary (C3/R13-001/R14-001).
    const std::deque<TimeDuration> checkpointRow{
        TimeDuration::fromPersistedRow(type, startTime, endTime, segmentId)};
    const DayValidity::Violation violation = DayValidity::validatePayload(checkpointRow);
    if (violation != DayValidity::Violation::None) {
        Logger::Log("[DB] saveCheckpoint: rejecting invalid interval (" + DayValidity::describe(violation) + ")");
        return SessionStoreResult::callerBug("saveCheckpoint: invalid interval (" + DayValidity::describe(violation) + ")");
    }

    if (history_days_to_keep_ == 0) {
        return SessionStoreResult::disabled();
    }

    if (!ensureOpen()) {
        // C4 (R07-003): ensureOpen() failure is TransientError consistently
        // everywhere it is checked -- see updateDurationsById for the rationale.
        Logger::Log("[DB] Could not open DB to save checkpoint");
        return SessionStoreResult::transient("DB connection could not be opened");
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

    QString commitErrorMsg;
    if (updateQuery.numRowsAffected() > 0) {
        const CommitOutcome commitResult = commitOrRollback("Error committing checkpoint update");
        success = commitResult.committed;
        commitErrorMsg = commitResult.errorMessage;
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

        const CommitOutcome commitResult = commitOrRollback("Error committing checkpoint insert");
        success = commitResult.committed;
        commitErrorMsg = commitResult.errorMessage;
    }

#ifndef QT_NO_DEBUG
    if (success) {
        checkSegmentIdUniqueness();
    }
#endif

    return success
        ? SessionStoreResult::success()
        : SessionStoreResult::transient("Checkpoint transaction commit failed: " + commitErrorMsg);
}

/**
 * Discards a deliberately-rejected in-flight segment's checkpoint (A4/R05-001).
 *
 * Durability: the discard INTENT is recorded in discard_intents as its own
 * committed statement BEFORE the physical row delete is attempted. Both
 * steps are individually auto-committed (deliberately not wrapped in one
 * transaction) so that a failure of the second step leaves the first step's
 * tombstone durably in place -- recovery can then finish the job even if
 * this call, and every retry of it before the process ends, keeps failing.
 * Both the tombstone write (INSERT OR IGNORE) and the row delete (DELETE ...
 * WHERE, a no-op if already gone) are idempotent, so repeated calls for the
 * same segment_id are always safe.
 */
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
        // C4 (R07-003): ensureOpen() failure is TransientError consistently
        // everywhere it is checked -- see updateDurationsById for the rationale.
        Logger::Log("[DB] Could not open DB to discard checkpoint");
        return SessionStoreResult::transient("DB connection could not be opened");
    }

    // Defensive: guarantee the table exists even if this is somehow called
    // before checkSchemaOnStartup() has run its additive DDL (e.g. an
    // existing pre-A4 DB in a test that seeds/exercises this call directly).
    if (!ensureDiscardIntentsTable()) {
        return SessionStoreResult::transient("Failed to prepare discard-intent storage");
    }

    QSqlQuery intentQuery(db);
    intentQuery.prepare("INSERT OR IGNORE INTO discard_intents (segment_id) VALUES (:id)");
    intentQuery.bindValue(":id", segmentId.toString());
    if (!intentQuery.exec()) {
        Logger::Log("[DB] Error recording discard intent: " + intentQuery.lastError().text());
        return SessionStoreResult::transient("Failed to record discard intent: " + intentQuery.lastError().text());
    }

#ifndef QT_NO_DEBUG
    if (force_discard_row_delete_failure_dbg_) {
        Logger::Log("[DB] discardCheckpoint: injected row-delete failure (test) — intent remains durable");
        return SessionStoreResult::transient("Injected discard-delete failure (test)");
    }
#endif

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

    // The row is confirmed gone (or was already gone) -- resolve the intent.
    // Best-effort: a failure here is harmless (recovery re-resolves a
    // leftover tombstone as a safe no-op), so it does not fail the call.
    QSqlQuery clearQuery(db);
    clearQuery.prepare("DELETE FROM discard_intents WHERE segment_id = :id");
    clearQuery.bindValue(":id", segmentId.toString());
    if (!clearQuery.exec()) {
        Logger::Log("[DB] Warning: failed to clear discard intent: " + clearQuery.lastError().text());
    }

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
        // C4 (R07-003): ensureOpen() failure is TransientError everywhere it
        // is checked (see saveCheckpoint/discardCheckpoint/replaceAll/
        // loadDurations) -- ensureOpen() itself is designed to transparently
        // reopen a lost connection, and this exact scenario (e.g. a
        // temporarily read-only file) is retried successfully by TimeTracker
        // without a restart once the underlying cause is fixed.
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

    // C2 (R07-001, D13e): route through commitOrRollback() like every other
    // commit site -- a bare db.rollback() here does not check whether the
    // rollback itself failed, which would leave the connection stuck inside
    // an open transaction with no closeConnection() recovery.
    const CommitOutcome commitResult = commitOrRollback("Error committing update transaction");
    if (!commitResult.committed) {
        return SessionStoreResult::transient("Failed to commit update transaction: " + commitResult.errorMessage);
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
        // C4 (R07-003): see updateDurationsById -- ensureOpen() failure is
        // classified TransientError consistently everywhere it is checked.
        Logger::Log("[DB] flushToDisc: could not open DB");
        return SessionStoreResult::transient("DB connection could not be opened");
    }

    QSqlQuery pragma(db);
    // Low-5: every restore below is best-effort -- flushToDisc()'s own
    // result never depends on it succeeding -- but a silently-failed
    // restore leaves the connection at FULL indefinitely, so it must at
    // least be logged rather than discarded.
    auto restoreSynchronousNormal = [&pragma]() {
        if (!pragma.exec("PRAGMA synchronous=NORMAL")) {
            Logger::Log("[DB] flushToDisc: warning: failed to restore synchronous=NORMAL: " + pragma.lastError().text());
        }
    };
    if (!pragma.exec("PRAGMA synchronous=FULL")) {
        Logger::Log("[DB] flushToDisc: set synchronous=FULL failed: " + pragma.lastError().text());
        return SessionStoreResult::transient("Failed to set synchronous=FULL: " + pragma.lastError().text());
    }

    if (!db.transaction()) {
        Logger::Log("[DB] flushToDisc: could not begin transaction");
        restoreSynchronousNormal();
        return SessionStoreResult::transient("Failed to start flush transaction: " + db.lastError().text());
    }

    const QString ts = now_provider_().toUTC().toString(Qt::ISODateWithMs);
    QSqlQuery query(db);
    query.prepare("INSERT OR REPLACE INTO app_settings (key, value) VALUES ('last_flush_utc', :ts)");
    query.bindValue(":ts", ts);

    if (!query.exec()) {
        Logger::Log("[DB] flushToDisc: write failed: " + query.lastError().text());
        db.rollback();
        restoreSynchronousNormal();
        return SessionStoreResult::transient("Flush write failed: " + query.lastError().text());
    }
    if (!db.commit()) {
        Logger::Log("[DB] flushToDisc: commit failed: " + db.lastError().text());
        db.rollback();
        restoreSynchronousNormal();
        return SessionStoreResult::transient("Flush commit failed: " + db.lastError().text());
    }

    Logger::Log("[DB] flushToDisc: durable heartbeat committed");
    restoreSynchronousNormal();
    return SessionStoreResult::success();
}

std::optional<SqliteSessionStore::OrphanLoadResult> SqliteSessionStore::loadUnfinalizedCheckpoints()
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return OrphanLoadResult{};
    }

    if (!ensureOpen()) {
        return std::nullopt;
    }

    OrphanLoadResult result;

    QSqlQuery query(db);
    query.prepare("SELECT id, segment_id, type, start_utc, end_utc FROM durations WHERE is_finalized = 0 ORDER BY id ASC");
    if (!query.exec()) {
        Logger::Log("[DB] Error loading orphan checkpoints: " + query.lastError().text());
        return std::nullopt;
    }

    while (query.next()) {
        const long long rowId = query.value(0).toLongLong();

        // An empty segment_id is a corrupt row (SegmentId::fromString asserts
        // non-empty in debug). B2 (R08-003): malformed rows are explicit
        // delete work owned by the store, not a silent omission -- record the
        // id so recoverStartupCheckpoints() can delete it transactionally.
        const QString segmentIdStr = query.value(1).toString();
        if (segmentIdStr.isEmpty()) {
            Logger::Log("[DB] loadUnfinalizedCheckpoints: malformed row (empty segment_id), id=" + QString::number(rowId));
            result.malformedIds.push_back(rowId);
            continue;
        }
        const int typeInt = query.value(2).toInt();
        if (typeInt != static_cast<int>(DurationType::Activity) &&
            typeInt != static_cast<int>(DurationType::Pause)) {
            Logger::Log(QString("[DB] loadUnfinalizedCheckpoints: malformed row (invalid type %1), id=%2").arg(typeInt).arg(rowId));
            result.malformedIds.push_back(rowId);
            continue;
        }

        const QDateTime startTime = QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs).toLocalTime();
        const QDateTime endTime   = QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs).toLocalTime();
        if (!startTime.isValid() || !endTime.isValid()) {
            Logger::Log(QString("[DB] loadUnfinalizedCheckpoints: malformed row (invalid timestamp(s)), id=%1 start=%2 end=%3")
                .arg(rowId).arg(query.value(3).toString()).arg(query.value(4).toString()));
            result.malformedIds.push_back(rowId);
            continue;
        }
        if (startTime >= endTime) {
            Logger::Log(QString("[DB] loadUnfinalizedCheckpoints: malformed row (start >= end), id=%1 start=%2 end=%3")
                .arg(rowId).arg(startTime.toString(Qt::ISODateWithMs)).arg(endTime.toString(Qt::ISODateWithMs)));
            result.malformedIds.push_back(rowId);
            continue;
        }

        OrphanCheckpoint checkpoint;
        checkpoint.id = rowId;
        checkpoint.segment_id = SegmentId::fromString(query.value(1).toString());
        checkpoint.type = static_cast<DurationType>(typeInt);
        checkpoint.duration = startTime.msecsTo(endTime);
        checkpoint.startTime = startTime;
        checkpoint.endTime = endTime;
        result.valid.push_back(checkpoint);
    }

    return result;
}

/**
 * Pure probe-and-finalize logic (R07-002/B1's overlap policy), extracted so
 * B2's recovery transaction can run it as one step among several sharing a
 * single outer transaction. Assumes the caller already holds an open
 * transaction; never begins, commits, or rolls back.
 */
SqliteSessionStore::FinalizeResult SqliteSessionStore::finalizeOrOverlapWithinOpenTransaction(qint64 rowId, const QDateTime& startUtc, const QDateTime& endUtc)
{
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
        const QString err = probe.lastError().text();
        Logger::Log("[DB] Error probing for orphan overlap: " + err);
        return {FinalizeOutcome::Error, err};
    }

    if (probe.next()) {
        // Overlap detected: leave the row unchanged here (the caller decides
        // what to do with an OverlapRejected outcome -- B2's recovery
        // transaction deletes it).
        return {FinalizeOutcome::OverlapRejected, QString()};
    }

    QSqlQuery updateQuery(db);
    updateQuery.prepare("UPDATE durations SET is_finalized = 1 WHERE id = :id AND is_finalized = 0");
    updateQuery.bindValue(":id", static_cast<qlonglong>(rowId));
    if (!updateQuery.exec()) {
        const QString err = updateQuery.lastError().text();
        Logger::Log("[DB] Error finalising orphan row: " + err);
        return {FinalizeOutcome::Error, err};
    }

    if (updateQuery.numRowsAffected() == 0) {
        // Row missing or already finalised.
        return {FinalizeOutcome::Missing, QString()};
    }

    return {FinalizeOutcome::Finalized, QString()};
}

/**
 * B2: applies the entire row-mutation half of startup recovery's disposition
 * -- malformed-row deletion, policy drops (too-short/stale/future/A4
 * discard-forced), and finalize-or-delete-on-overlap -- as steps sharing ONE
 * transaction that the CALLER already opened (recoverStartupCheckpoints()).
 * This method itself never begins, commits, or rolls back; on the first
 * failure it sets ok=false and returns immediately so the caller can roll
 * back the entire disposition (R08-002/R07-002: a real failure, an overlap
 * rejection, and a genuine drop must never be conflated, and a partial
 * mutation must never survive a reported failure).
 *
 * A Missing outcome (the row vanished between load and finalize) cannot
 * legitimately happen in this single-process flow -- db_mutex_ has been held
 * by the caller since the row was loaded -- so it is treated the same as a
 * real SQL error rather than a silent policy no-op (resolves B1's deferred
 * Low).
 */
SqliteSessionStore::ReconcileResult SqliteSessionStore::reconcileUnfinalizedCheckpoints(const std::vector<OrphanCheckpoint>& orphansToFinalize,
                                                                                       const std::vector<long long>& policyDropIds,
                                                                                       const std::vector<long long>& malformedIds)
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

    if (orphansToFinalize.empty() && policyDropIds.empty() && malformedIds.empty()) {
        return result;
    }

    QSqlQuery dropQuery(db);
    dropQuery.prepare("DELETE FROM durations WHERE id = :id AND is_finalized = 0");

    // Malformed rows: logged-only count (StartupRecoveryResult does not
    // expose it -- this is store-internal cleanup, not a recovery policy
    // decision).
    for (long long id : malformedIds) {
        dropQuery.bindValue(":id", static_cast<qlonglong>(id));
        if (!dropQuery.exec()) {
            Logger::Log("[DB] Error deleting malformed orphan row: " + dropQuery.lastError().text());
            result.ok = false;
            return result;
        }
    }
    result.malformed_deleted = static_cast<int>(malformedIds.size());

    // Policy drops supplied by the caller (too-short/stale/future/A4 discard-forced).
    for (long long id : policyDropIds) {
        dropQuery.bindValue(":id", static_cast<qlonglong>(id));
        if (!dropQuery.exec()) {
            Logger::Log("[DB] Error dropping orphan row: " + dropQuery.lastError().text());
            result.ok = false;
            return result;
        }
    }
    result.policy_dropped = policyDropIds;

    for (const auto& orphan : orphansToFinalize) {
        const QDateTime startUtc = orphan.startTime.toUTC();
        const QDateTime endUtc   = orphan.endTime.toUTC();
        const FinalizeResult finalizeResult = finalizeOrOverlapWithinOpenTransaction(orphan.id, startUtc, endUtc);
        switch (finalizeResult.outcome) {
        case FinalizeOutcome::Finalized:
            result.finalized.push_back(orphan.id);
            break;
        case FinalizeOutcome::OverlapRejected:
            // R08-002: an overlap-rejected orphan is now physically deleted
            // (transactionally, as part of this same disposition) instead of
            // being left is_finalized=0 to recur on every future launch.
            dropQuery.bindValue(":id", static_cast<qlonglong>(orphan.id));
            if (!dropQuery.exec()) {
                Logger::Log("[DB] Error deleting overlap-rejected orphan row: " + dropQuery.lastError().text());
                result.ok = false;
                return result;
            }
            result.policy_dropped.push_back(orphan.id);
            break;
        case FinalizeOutcome::Missing:
            Logger::Log(QString("[DB] recoverStartupCheckpoints: internal error -- orphan row %1 vanished during recovery").arg(orphan.id));
            result.ok = false;
            return result;
        case FinalizeOutcome::Error:
            // A real SQL/transaction failure is not a policy decision: it must
            // not be silently reported as a dropped row. Mark reconciliation
            // unsuccessful and stop -- the caller rolls back the ENTIRE
            // disposition, including drops already applied above.
            Logger::Log(QString("[DB] finalizeOrOverlap failed for orphan row %1: ").arg(orphan.id) + finalizeResult.errorMessage);
            result.ok = false;
            return result;
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
        // C4 (R07-003): see updateDurationsById -- ensureOpen() failure is
        // classified TransientError consistently everywhere it is checked.
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

/**
 * D6 (R05-002/R15-004): the single durable shutdown operation. Writes the
 * clean-shutdown marker timestamped via now_provider_ (the same injected
 * seam flushToDisc()'s heartbeat write already uses), then -- only if that
 * write succeeded -- executes the FULL durability barrier so the marker's
 * own commit is covered by the fsync rather than left outside it (R15-004).
 * Each step takes db_mutex_ independently; this method holds it for
 * neither, so there is no nested-lock risk.
 *
 * M1: markerWritten distinguishes which step a failure came from -- false
 * for a marker-write failure (the record was never committed), true for a
 * barrier failure (the marker WAS committed; only the covering fsync
 * failed). Callers must not treat these the same way.
 */
MarkCleanShutdownResult SqliteSessionStore::markCleanShutdown()
{
    const SessionStoreResult markerResult = setLastCleanShutdownMarker(now_provider_());
    if (!markerResult.ok()) {
        // Disabled or a failure: do not attempt the barrier. Either way the
        // marker write itself did not durably complete.
        return {markerResult, false};
    }
    return {flushToDisc(), true};
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

/**
 * Deletes the clean-shutdown marker row (R08-001). Assumes the caller
 * already holds an open transaction; does not begin, commit, or roll back --
 * marker consumption is folded into recoverStartupCheckpoints()' single
 * atomic disposition so a later failure in that same transaction correctly
 * un-deletes it too.
 */
bool SqliteSessionStore::deleteMarkerRow()
{
    QSqlQuery deleteQuery(db);
    deleteQuery.prepare("DELETE FROM app_settings WHERE key = :key");
    deleteQuery.bindValue(":key", QString::fromLatin1(kLastCleanShutdownKey));
    return deleteQuery.exec();
}

/**
 * Loads all durable discard-intent segment ids (A4/R05-001).
 *
 * Returns nullopt only on a real query failure -- an empty (but successfully
 * queried) table returns an empty vector, not nullopt.
 */
std::optional<std::vector<SegmentId>> SqliteSessionStore::loadDiscardIntents()
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return std::vector<SegmentId>{};
    }

    if (!ensureOpen()) {
        return std::nullopt;
    }

    if (!ensureDiscardIntentsTable()) {
        return std::nullopt;
    }

    QSqlQuery q(db);
    if (!q.exec("SELECT segment_id FROM discard_intents")) {
        Logger::Log("[DB] Error loading discard intents: " + q.lastError().text());
        return std::nullopt;
    }

    std::vector<SegmentId> ids;
    while (q.next()) {
        const QString s = q.value(0).toString();
        if (!s.isEmpty()) {
            ids.push_back(SegmentId::fromString(s));
        }
    }
    return ids;
}

/**
 * Startup checkpoint recovery (B2/R08-001..003,005, R05-004, R05-001).
 *
 * Loads and classifies every unfinalized checkpoint row and every durable
 * discard intent (A4) BEFORE any externally visible mutation, then applies
 * malformed-row deletion, policy drops (too-short <1s / stale >24h /
 * future-dated / A4 discard-forced), overlap-guarded finalization (deleting
 * overlap-rejected rows instead of leaving them to recur every launch), and
 * clean-marker consumption all within ONE transaction. A failure at any
 * mutation stage rolls back that entire transaction, so no partial
 * disposition can ever be reported as either success or (falsely) as a
 * "dropped" row -- and the marker is preserved.
 *
 * All internal mechanics (row ids, marker timestamps) stay inside this
 * method. TimeTracker receives only domain facts via StartupRecoveryResult,
 * whose counts describe exactly what this transaction committed.
 */
StartupRecoveryResult SqliteSessionStore::recoverStartupCheckpoints(const QDateTime& now)
{
    QMutexLocker locker(&db_mutex_);

    if (history_days_to_keep_ == 0) {
        return StartupRecoveryResult::disabled();
    }

    // ---- Phase 1: read-only load and classification (no mutation yet) ----

    // Read the marker first (without deleting it). If the read fails, do not
    // reconcile — DB state is unknown and finalising rows could conflict with
    // in-progress data. The marker is only deleted inside the recovery
    // transaction below, so a failure here never leaves the store mutated.
    const std::optional<MarkerResult> markerResult = readLastCleanShutdownMarker();
    if (markerResult.has_value() && markerResult->status == MarkerResult::Status::Error) {
        Logger::Log("[DB] recoverStartupCheckpoints: marker read failed — skipping orphan reconciliation");
        return StartupRecoveryResult::failure(SessionStoreResult::TransientError, "Failed to read clean-shutdown marker");
    }

    const std::optional<OrphanLoadResult> orphansOpt = loadUnfinalizedCheckpoints();
    if (!orphansOpt.has_value()) {
        Logger::Log("[DB] recoverStartupCheckpoints: orphan load failed — marker preserved, no reconciliation");
        return StartupRecoveryResult::failure(SessionStoreResult::TransientError, "Failed to load unfinalized checkpoints");
    }
    const std::deque<OrphanCheckpoint>& orphans = orphansOpt->valid;
    const std::vector<long long>& malformedIds = orphansOpt->malformedIds;

    // A4 (R05-001): a durable discard intent wins over ordinary orphan
    // policy. It may claim only a matching UNFINALIZED row (never a
    // finalized one -- a reused/corrupt identity must not delete real
    // history); either way the tombstone is resolved below once its fate
    // is settled, so it cannot linger as unresolvable work.
    const std::optional<std::vector<SegmentId>> intentsOpt = loadDiscardIntents();
    if (!intentsOpt.has_value()) {
        Logger::Log("[DB] recoverStartupCheckpoints: discard-intent load failed — marker preserved, no reconciliation");
        return StartupRecoveryResult::failure(SessionStoreResult::TransientError, "Failed to load discard intents");
    }
    std::vector<SegmentId> unmatchedIntents = *intentsOpt;

    // Partition orphans whose segment_id has a durable discard intent: these
    // are force-dropped (outright, bypassing overlap/age policy) regardless
    // of duration/age, and never contribute to recovered_msec or notification.
    std::vector<long long> policyDropIds;
    std::vector<SegmentId> resolvedIntents;
    std::deque<OrphanCheckpoint> remainingOrphans;
    for (const auto& orphan : orphans) {
        auto it = std::find(unmatchedIntents.begin(), unmatchedIntents.end(), orphan.segment_id);
        if (it != unmatchedIntents.end()) {
            policyDropIds.push_back(orphan.id);
            resolvedIntents.push_back(orphan.segment_id);
            unmatchedIntents.erase(it);
        } else {
            remainingOrphans.push_back(orphan);
        }
    }
    // M4: snapshot the discard-forced drops before the policy loop below
    // appends to the SAME vector. Measured here, not from resolvedIntents:
    // the insert() below adds unmatched intents that delete no row.
    const int discardForcedDrops = static_cast<int>(policyDropIds.size());

    // Any intent left unmatched named no unfinalized row -- either already
    // deleted, or (a reused/corrupt identity) it now names a finalized row.
    // Either way there is nothing left to delete; just resolve the tombstone.
    resolvedIntents.insert(resolvedIntents.end(), unmatchedIntents.begin(), unmatchedIntents.end());

    std::vector<OrphanCheckpoint> toFinalize;
    for (const auto& orphan : remainingOrphans) {
        const bool tooShort = orphan.duration < kMinRecoverableOrphanDurationMs;
        const bool stale = orphan.endTime.isValid() && (orphan.endTime.msecsTo(now) > kOrphanStaleAgeMs);
        // A backward clock jump since the checkpoint was written can leave the
        // orphan's endTime after "now" (msecsTo(now) negative, passing the
        // stale check above). Drop future-dated orphans instead of resurrecting them.
        const bool futureDated = orphan.endTime.isValid() && (now.msecsTo(orphan.endTime) > kOrphanFutureSlackMs);
        // MUST-FIX4 (review packet C3): a cross-day orphan is well-formed by
        // B2's malformed-row definition (not empty id/invalid type/invalid
        // timestamp/start>=end), so it would otherwise reach finalizeOrOverlap
        // with no day-validity check at all. Policy-drop it exactly like the
        // other out-of-contract shapes above rather than finalizing it into
        // history.
        const bool crossDay = isCrossMidnight(orphan.startTime, orphan.endTime);

        if (tooShort || stale || futureDated || crossDay) {
            policyDropIds.push_back(orphan.id);
        } else {
            toFinalize.push_back(orphan);
        }
    }

    // ---- Phase 2: one atomic transaction for the entire disposition ----

    const bool hasMarkerToConsume = markerResult.has_value() && markerResult->status == MarkerResult::Status::Found;
    const bool hasWorkToDo = !malformedIds.empty() || !policyDropIds.empty() || !toFinalize.empty()
        || !resolvedIntents.empty() || hasMarkerToConsume;

    if (!hasWorkToDo) {
        return StartupRecoveryResult{};
    }

    if (!ensureOpen()) {
        // C4 (R07-003): ensureOpen() failure is TransientError consistently
        // everywhere it is checked -- see updateDurationsById for the rationale.
        Logger::Log("[DB] recoverStartupCheckpoints: could not open DB for recovery transaction");
        return StartupRecoveryResult::failure(SessionStoreResult::TransientError, "DB connection could not be opened");
    }

    if (!db.transaction()) {
        const QString err = db.lastError().text();
        Logger::Log("[DB] recoverStartupCheckpoints: could not start recovery transaction: " + err);
        return StartupRecoveryResult::failure(SessionStoreResult::TransientError, "Failed to start recovery transaction: " + err);
    }

    ReconcileResult reconcile = reconcileUnfinalizedCheckpoints(toFinalize, policyDropIds, malformedIds);
    if (!reconcile.ok) {
        Logger::Log("[DB] recoverStartupCheckpoints: reconciliation failed — rolling back entire disposition");
        db.rollback();
        return StartupRecoveryResult::failure(SessionStoreResult::TransientError, "Startup recovery reconciliation failed");
    }

    // Resolve discard intents within the same transaction.
    if (!resolvedIntents.empty()) {
        QSqlQuery clearQuery(db);
        clearQuery.prepare("DELETE FROM discard_intents WHERE segment_id = :id");
        for (const auto& id : resolvedIntents) {
            clearQuery.bindValue(":id", id.toString());
            if (!clearQuery.exec()) {
                Logger::Log("[DB] recoverStartupCheckpoints: failed to clear discard intent: " + clearQuery.lastError().text());
                db.rollback();
                return StartupRecoveryResult::failure(SessionStoreResult::TransientError, "Failed to resolve discard intent");
            }
        }
    }

    // Consume the marker within the same transaction (R08-001): its deletion
    // now shares fate with every row mutation above instead of being a
    // fire-and-forget call whose result was ignored.
    if (hasMarkerToConsume && !deleteMarkerRow()) {
        Logger::Log("[DB] recoverStartupCheckpoints: failed to consume clean-shutdown marker: " + db.lastError().text());
        db.rollback();
        return StartupRecoveryResult::failure(SessionStoreResult::TransientError, "Failed to consume clean-shutdown marker");
    }

    if (!db.commit()) {
        const QString err = db.lastError().text();
        Logger::Log("[DB] recoverStartupCheckpoints: commit failed: " + err);
        db.rollback();
        return StartupRecoveryResult::failure(SessionStoreResult::TransientError, "Failed to commit recovery transaction: " + err);
    }

    // ---- Committed: build the truthful public result from what was mutated ----

    StartupRecoveryResult result;
    result.finalized_count = static_cast<int>(reconcile.finalized.size());
    result.policy_dropped_count = static_cast<int>(reconcile.policy_dropped.size());
    result.discard_dropped_count = discardForcedDrops;

    // Accumulate exact milliseconds only for rows the store actually
    // finalized (R08-005: no per-row truncation, no seconds conversion).
    for (const auto& orphan : toFinalize) {
        if (std::find(reconcile.finalized.begin(), reconcile.finalized.end(), orphan.id)
                != reconcile.finalized.end()) {
            result.recovered_msec += orphan.duration;
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

    Logger::Log(QString("[DB] Startup checkpoint recovery: finalized=%1, policy_dropped=%2 (discard_forced=%3), malformed_deleted=%4, recovered_msec=%5")
        .arg(reconcile.finalized.size())
        .arg(reconcile.policy_dropped.size())
        .arg(discardForcedDrops)
        .arg(reconcile.malformed_deleted)
        .arg(result.recovered_msec));

    return result;
}
