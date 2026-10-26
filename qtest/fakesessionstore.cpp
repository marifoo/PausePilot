/**
 * FakeSessionStore -- spy test double for SessionStore.
 *
 * Records every call made to it (callLog) and stores segments passed to
 * commitSession so tests can inspect what was written. No
 * overlap or reconcile POLICY is duplicated here; that behavior is verified
 * against the real SqliteSessionStore in test_persistence_contract.cpp.
 * D13e: commitSession() DOES mirror the real store's identity mechanics --
 * normalizing (merging adjacent same-type segments) and upserting by
 * segment_id -- since those determine the physical shape storedDurations
 * must have for loadDurations() to be a truthful mirror of committed state.
 *
 * Return values come from the configurable *Result members set by the test.
 */

#include "fakesessionstore.h"
#include <algorithm>

namespace {
// C4 (R05-003): the fake deliberately does not duplicate the concrete
// store's day-validity/overlap SQL policy, but a duplicate segment_id is an
// IDENTITY contract fact, not a policy decision -- both implementations must
// agree that it is CallerBug, not a silently-accepted spy call.
bool hasDuplicateSegmentId(const std::deque<TimeDuration>& rows)
{
    QSet<QString> seen;
    for (const auto& d : rows) {
        const QString id = d.segment_id.toString();
        if (seen.contains(id))
            return true;
        seen.insert(id);
    }
    return false;
}
}

FakeSessionStore::FakeSessionStore()
    : startupRecoveryResult{}
{
}

SessionStoreResult FakeSessionStore::commitSession(const Timeline& session)
{
    callLog.append("commitSession");
    if (hasDuplicateSegmentId(session.completed())) {
        return SessionStoreResult::callerBug("commitSession: duplicate segment_id in payload");
    }
    if (commitSessionResult.ok()) {
        // D13e: mirror SqliteSessionStore::commitSession()'s own pipeline --
        // normalize (merging adjacent same-type segments extends the
        // survivor's endTime and merges away the other segment_id), delete
        // rows for merged-away ids, then upsert the normalized rows by
        // segment_id. Without this, storedDurations would keep raw,
        // unmerged rows that no real database would ever hold, and
        // TimeTracker::checkCrossLayerInvariants() (which compares against
        // the normalized view, matching what the real store persists) would
        // report a false field mismatch or missing row.
        const auto [normed, removedIds] = session.normalizedWithRemovedIds();
        for (const auto& id : removedIds) {
            storedDurations.erase(
                std::remove_if(storedDurations.begin(), storedDurations.end(),
                                [&](const TimeDuration& row) { return row.segment_id.toString() == id; }),
                storedDurations.end());
        }
        for (const auto& d : normed.completed()) {
            committedSegmentIds.insert(d.segment_id.toString());
            auto existing = std::find_if(storedDurations.begin(), storedDurations.end(),
                                          [&](const TimeDuration& row) {
                                              return row.segment_id.toString() == d.segment_id.toString();
                                          });
            if (existing != storedDurations.end()) {
                *existing = d;
            } else {
                storedDurations.push_back(d);
            }
        }
    }
    return commitSessionResult;
}

LoadResult FakeSessionStore::loadDurations()
{
    callLog.append("loadDurations");
    // D13e: project storedDurations (what commitSession actually
    // committed) so the double mirrors real persisted state instead of only
    // ever returning what a test manually preset. A non-Success status is a
    // test simulating a failed/disabled load: durations is cleared here,
    // matching the real store, which always returns empty durations on
    // Disabled and on every error path (sqlitesessionstore.cpp).
    if (loadDurationsResult.status != SessionStoreResult::Success) {
        LoadResult result;
        result.status = loadDurationsResult.status;
        return result;
    }
    // D13e (M2, review): loadDurationsResult.durations models pre-existing
    // rows a read-only fixture seeds before constructing TimeTracker (e.g.
    // HistoryDialog/HistoryEditSession tests).
    // Dedupe on segment_id with storedDurations winning: a preset row
    // sharing an id with a later-committed row must not appear twice here --
    // the real store upserts such a row to one, so two rows would be an
    // impossible state that falsely trips TimeTracker::checkCrossLayerInvariants()'s
    // matchCount > 1 check.
    LoadResult result;
    result.status = SessionStoreResult::Success;
    for (const auto& d : loadDurationsResult.durations) {
        const bool supersededByStored =
            std::any_of(storedDurations.begin(), storedDurations.end(),
                        [&](const TimeDuration& row) {
                            return row.segment_id.toString() == d.segment_id.toString();
                        });
        if (!supersededByStored) {
            result.durations.push_back(d);
        }
    }
    result.durations.insert(result.durations.end(), storedDurations.begin(), storedDurations.end());
    return result;
}

namespace {
// H5 (WP1): formats a TimeDuration exactly as SqliteSessionStore's raw
// columns would read back (see toUtcIso() in sqlitesessionstore.cpp), so
// loadHistoryRows() is a truthful fake of the real store's raw-text contract.
StoredHistoryRow toStoredHistoryRow(const TimeDuration& d, bool finalized)
{
    StoredHistoryRow row;
    row.segmentId = d.segment_id.toString();
    row.type = QString::number(static_cast<int>(d.type));
    row.startUtc = d.startTime.toUTC().toString(Qt::ISODateWithMs);
    row.endUtc = d.endTime.toUTC().toString(Qt::ISODateWithMs);
    row.isFinalized = finalized ? "1" : "0";
    // Parse the raw times to local time (like parseHistoryRow in the real store)
    if (!row.segmentId.isEmpty()) {
        const int typeInt = row.type.toInt();
        if (typeInt == static_cast<int>(DurationType::Activity) ||
            typeInt == static_cast<int>(DurationType::Pause)) {
            const QDateTime startLocal = LocalTime::fromInstant(QDateTime::fromString(row.startUtc, Qt::ISODateWithMs));
            const QDateTime endLocal = LocalTime::fromInstant(QDateTime::fromString(row.endUtc, Qt::ISODateWithMs));
            if (startLocal.isValid() && endLocal.isValid() && startLocal <= endLocal && startLocal.msecsTo(endLocal) > 0) {
                const SegmentId segmentId = SegmentId::fromString(row.segmentId);
                const DurationType type = static_cast<DurationType>(typeInt);
                row.parsed = TimeDuration::fromPersistedRow(type, startLocal, endLocal, segmentId);
            }
        }
    }
    return row;
}
}

HistoryRowsResult FakeSessionStore::loadHistoryRows()
{
    callLog.append("loadHistoryRows");
    HistoryRowsResult result;
    result.status = loadDurationsResult.status;
    if (result.status != SessionStoreResult::Success) {
        return result;
    }

    // Collect all rows that will go into the result, with proper sorting and merging.
    // WP1b: Fake fidelity for loadHistoryRows() must:
    // 1. Sort rows by startUtc text; ties keep insertion order as a stand-in for id
    // 2. Convert parsed times with toLocalTime()
    // 3. Add unfinalized rows:
    //    - historyUnfinalizedRows, plus
    //    - savedCheckpoints projected as the latest record per segmentId with raw isFinalized "0"
    //    - Exclude any id that already exists as a finalized row
    //    - Drop an unfinalized preset whose id matches a finalized row's id

    std::vector<StoredHistoryRow> allRows;

    // Collect finalized rows: preset rows (deduped against storedDurations, storedDurations winning)
    QSet<QString> usedIds;
    for (const auto& d : storedDurations) {
        usedIds.insert(d.segment_id.toString());
        allRows.push_back(toStoredHistoryRow(d, true));
    }
    for (const auto& d : loadDurationsResult.durations) {
        const QString id = d.segment_id.toString();
        if (!usedIds.contains(id)) {
            allRows.push_back(toStoredHistoryRow(d, true));
            usedIds.insert(id);
        }
    }

    // Collect checkpoint rows: latest record per segmentId from savedCheckpoints
    // Project as unfinalized, but only if the id is not already finalized
    QMap<QString, const CheckpointRecord*> latestCheckpoint;
    for (const auto& cp : savedCheckpoints) {
        latestCheckpoint[cp.segmentId.toString()] = &cp;
    }
    for (const auto& cp : latestCheckpoint) {
        const QString id = cp->segmentId.toString();
        if (!usedIds.contains(id)) {
            TimeDuration d = TimeDuration::fromPersistedRow(cp->type, cp->startTime, cp->endTime, cp->segmentId);
            allRows.push_back(toStoredHistoryRow(d, false));
            usedIds.insert(id);
        }
    }

    // Collect unfinalized rows from historyUnfinalizedRows
    for (const auto& d : historyUnfinalizedRows) {
        const QString id = d.segment_id.toString();
        if (!usedIds.contains(id)) {
            allRows.push_back(toStoredHistoryRow(d, false));
            usedIds.insert(id);
        }
    }

    // Sort by startUtc text; ties keep insertion order (stable sort)
    std::stable_sort(allRows.begin(), allRows.end(),
                     [](const StoredHistoryRow& a, const StoredHistoryRow& b) {
                         return a.startUtc < b.startUtc;
                     });

    result.rows = allRows;
    return result;
}

SessionStoreResult FakeSessionStore::saveHistoryDays(const std::vector<StoredHistoryRow>& openSnapshot,
                                                      const std::vector<HistoryDayChange>& days)
{
    callLog.append("saveHistoryDays");
    lastHistorySaveSnapshot = openSnapshot;
    lastHistorySaveDays = days;

    // WP2: the fake enforces only the id-uniqueness CallerBug fact -- it is a
    // spy, not a policy engine. The real day-validity/stale/new-conflict
    // checks are exercised only against SqliteSessionStore.
    QSet<QString> seenIds;
    for (const auto& day : days) {
        for (const auto& id : day.deletedIds) {
            const QString idStr = id.toString();
            if (seenIds.contains(idStr))
                return SessionStoreResult::callerBug("saveHistoryDays: duplicate id in payload");
            seenIds.insert(idStr);
        }
        for (const auto& upsert : day.upserts) {
            const QString idStr = upsert.segment_id.toString();
            if (seenIds.contains(idStr))
                return SessionStoreResult::callerBug("saveHistoryDays: duplicate id in payload");
            seenIds.insert(idStr);
        }
    }

    if (saveHistoryDaysResult.ok()) {
        for (const auto& day : days) {
            for (const auto& id : day.deletedIds) {
                storedDurations.erase(
                    std::remove_if(storedDurations.begin(), storedDurations.end(),
                                    [&](const TimeDuration& row) { return row.segment_id == id; }),
                    storedDurations.end());
            }
            for (const auto& upsert : day.upserts) {
                auto existing = std::find_if(storedDurations.begin(), storedDurations.end(),
                                              [&](const TimeDuration& row) {
                                                  return row.segment_id == upsert.segment_id;
                                              });
                if (existing != storedDurations.end()) {
                    *existing = upsert;
                } else {
                    storedDurations.push_back(upsert);
                }
            }
        }
    }
    return saveHistoryDaysResult;
}

EntriesForDateResult FakeSessionStore::hasEntriesForDate(const QDate& /*date*/)
{
    callLog.append("hasEntriesForDate");
    return entriesForDateResult;
}

SessionStoreResult FakeSessionStore::saveCheckpoint(DurationType type, const QDateTime& startTime,
                                                     const QDateTime& endTime, const SegmentId& segmentId)
{
    callLog.append("saveCheckpoint");
    if (segmentId.isEmpty()) {
        return SessionStoreResult::callerBug("empty segment_id");
    }
    if (saveCheckpointResult.ok()) {
        // Check whether this segment_id already has a checkpoint record.
        // Multiple saves for the same segment_id are expected (it is an upsert/update),
        // so we allow them — we only assert that no *different* finalized row already owns this id.
        bool alreadySaved = false;
        for (const auto& cp : savedCheckpoints) {
            if (cp.segmentId == segmentId) {
                alreadySaved = true;
                break;
            }
        }
        // A segment_id that was finalized via commitSession must not also appear as a
        // checkpoint (that would mean TimeTracker submitted the same id to two separate
        // paths). C4 fix round 3 (F7): match SqliteSessionStore::saveCheckpoint, which
        // classifies this as CallerBug (see test_saveCheckpoint_callerBug_finalized_
        // segmentId in test_contract_matrix.cpp) rather than aborting the process --
        // hasDuplicateSegmentId() exists precisely so both implementations agree on
        // this identity fact.
        if (committedSegmentIds.contains(segmentId.toString())) {
            return SessionStoreResult::callerBug(
                "segment_id already committed via commitSession");
        }
        (void)alreadySaved; // Multiple checkpoint saves for the same id are fine (upsert).
        savedCheckpoints.push_back({type, startTime, endTime, segmentId});
    }
    return saveCheckpointResult;
}

SessionStoreResult FakeSessionStore::discardCheckpoint(const SegmentId& segmentId)
{
    callLog.append("discardCheckpoint");
    if (segmentId.isEmpty()) {
        return SessionStoreResult::callerBug("empty segment_id");
    }
    if (discardCheckpointResult.category != SessionStoreResult::Success) {
        // Disabled or a failure category: a deliberate no-op, not a mutation.
        return discardCheckpointResult;
    }
    // WP1b: Mirror deleting the unfinalized rows: drop any recorded checkpoints
    // for this id, plus remove matching historyUnfinalizedRows entries.
    savedCheckpoints.erase(
        std::remove_if(savedCheckpoints.begin(), savedCheckpoints.end(),
                       [&](const CheckpointRecord& cp) { return cp.segmentId == segmentId; }),
        savedCheckpoints.end());
    historyUnfinalizedRows.erase(
        std::remove_if(historyUnfinalizedRows.begin(), historyUnfinalizedRows.end(),
                       [&](const TimeDuration& row) { return row.segment_id == segmentId; }),
        historyUnfinalizedRows.end());
    return discardCheckpointResult;
}

SchemaStatus FakeSessionStore::checkSchemaOnStartup()
{
    callLog.append("checkSchemaOnStartup");
    return checkSchemaResult;
}

SessionStoreResult FakeSessionStore::flushToDisc()
{
    callLog.append("flushToDisc");
    return flushToDiscResult;
}

SessionStoreResult FakeSessionStore::setLastCleanShutdownMarker(const QDateTime& /*timestamp*/)
{
    callLog.append("setLastCleanShutdownMarker");
    return setMarkerResult;
}

MarkCleanShutdownResult FakeSessionStore::markCleanShutdown()
{
    // D6/M1: marker write, then -- only if it succeeded -- the durability
    // barrier. Mirrors SqliteSessionStore::markCleanShutdown() so tests can
    // inject marker/flush failures independently via setMarkerResult and
    // flushToDiscResult and observe both call order, short-circuiting, and
    // which step (markerWritten) a failure is attributed to.
    const SessionStoreResult markerResult = setLastCleanShutdownMarker(QDateTime());
    if (!markerResult.ok()) {
        return {markerResult, false};
    }
    return {flushToDisc(), true};
}

StartupRecoveryResult FakeSessionStore::recoverStartupCheckpoints(const QDateTime& /*now*/)
{
    callLog.append("recoverStartupCheckpoints");
    return startupRecoveryResult;
}
