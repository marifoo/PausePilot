/**
 * FakeSessionStore -- spy test double for SessionStore.
 *
 * Records every call made to it (callLog) and stores segments passed to
 * commitSession/replaceAll so tests can inspect what was written. No
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

SessionStoreResult FakeSessionStore::replaceAll(const Timeline& history, const Timeline& session)
{
    callLog.append("replaceAll");
    // Uniqueness is checked across the COMBINED payload (C4/R05-003): a
    // history row and a session row sharing an id is also a CallerBug, not
    // just a duplicate within one bucket.
    std::deque<TimeDuration> combined = history.completed();
    combined.insert(combined.end(), session.completed().begin(), session.completed().end());
    if (hasDuplicateSegmentId(combined)) {
        return SessionStoreResult::callerBug("replaceAll: duplicate segment_id in payload");
    }
    // Disabled is a contractual no-op, exactly like every other failure
    // category -- only Success may mutate fake state (R05-005/R02-003).
    if (replaceDurationsResult.ok()) {
        storedDurations.clear();
        committedSegmentIds.clear();
        // D13e (M1, review): the real store writes history rows with
        // is_finalized = 1 and session rows with is_finalized = 0
        // (sqlitesessionstore.cpp), and loadDurations() there only selects
        // is_finalized = 1 rows -- so a real replaceAll() with a non-empty
        // session Timeline is NOT fully visible to a later loadDurations().
        // This fake has no is_finalized notion and stores/projects both
        // buckets alike. Currently safe: no fake replaceAll() call site
        // reaches this mutation block with a non-empty session Timeline
        // (the one that passes a non-empty session, test_contract_matrix.cpp:757,
        // returns CallerBug above first), so the two buckets never actually
        // diverge in practice.
        for (const auto& d : history.completed()) {
            storedDurations.push_back(d);
            committedSegmentIds.insert(d.segment_id.toString());
        }
        for (const auto& d : session.completed()) {
            storedDurations.push_back(d);
            committedSegmentIds.insert(d.segment_id.toString());
        }
    }
    return replaceDurationsResult;
}

LoadResult FakeSessionStore::loadDurations()
{
    callLog.append("loadDurations");
    // D13e: project storedDurations (what commitSession/replaceAll actually
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
    // HistoryDialog/HistoryEditSession tests) -- replaceAll()'s
    // storedDurations.clear() above deliberately does not touch this preset.
    // Some of those fixtures DO go on to call replaceAll() on the same
    // fakeDb, so a preset row that replaceAll() would have deleted can
    // linger here uncleared; that is safe only because those fixtures never
    // call loadDurations() again after the replaceAll() to observe it.
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
    // Mirror deleting the unfinalized rows: drop any recorded checkpoints for this id.
    savedCheckpoints.erase(
        std::remove_if(savedCheckpoints.begin(), savedCheckpoints.end(),
                       [&](const CheckpointRecord& cp) { return cp.segmentId == segmentId; }),
        savedCheckpoints.end());
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
