#ifndef TEST_DATABASE_H
#define TEST_DATABASE_H

#include <QObject>
#include "testcommon.h"

class DatabaseTest : public QObject
{
    Q_OBJECT

private:
    QString db_path_;
    QString db_backup_path_;
    
    void resetDatabaseFile() const
    {
        if (QFile::exists(db_path_)) {
            QFile::remove(db_path_);
        }
    }

private slots:
    void initTestCase();
    void cleanupTestCase();
    
    // Core database tests
    void test_database_backups_and_retention_and_disable();
    void test_databasemanager_write_failure();
    void test_database_transaction_rollback_on_insert_failure();
    void test_database_transaction_rollback_on_replace_failure();
    void test_database_checkpoint_single_row_per_segment();
    void test_database_checkpoint_missing_segment_reinserts_same_segment_id();
    void test_database_checkpoint_preserves_start_time();
    void test_database_update_by_id_insert_mode();
    void test_database_update_by_id_updates_existing_row_on_start_drift();
    void test_database_update_by_id_different_segments_same_start_both_exist();
    void test_database_update_by_id_empty_deque();
    void test_database_load_negative_duration();
    void test_database_load_start_after_end();
    void test_database_load_invalid_enum_type();
    void test_database_load_duration_mismatch_tolerance();
    void test_database_load_invalid_type_increments_skipped_and_omits_row();
    void test_database_load_200ms_mismatch_increments_repaired_and_uses_computed();
    void test_database_timezone_roundtrip();
    void test_database_millisecond_precision();
    void test_database_schema_validation_fresh_database();
    void test_database_schema_creates_idx_finalized_start();
    void test_database_backup_file_creation();
    void test_database_backup_preserves_contents();
    void test_database_backup_filenames_collision_free();

    // Schema/validation tests
    void test_exactMatching_upsertReplacesById();
    void test_explicitStartTimes_constructorComputesDuration();
    void test_explicitStartTimes_startTimePreservedAfterClean();
    void test_explicitStartTimes_mergeUpdatesAllFields();
    void test_splitPreservesStartTime();
    void test_zeroDurationNotAdded();
    void test_negativeDurationHandled();
    void test_checkpointPreservesStartTimeOnUpdateBySegmentId();
    void test_clockDriftResilience_durationStoredFromElapsed();
    void test_loadDurations_skipsNegativeDurationRows();

    // hasEntriesForDate tri-state result tests (T18)
    void test_hasEntriesForDate_returns_unknown_when_history_disabled();
    void test_hasEntriesForDate_returns_no_on_empty_database();
    void test_hasEntriesForDate_returns_yes_when_entries_exist();
    void test_hasEntriesForDate_utc_positive_offset();
    void test_hasEntriesForDate_utc_negative_offset();
    void test_hasEntriesForDate_utc_regression();

    // Retention cleanup once-per-session tests (T22)
    void test_retention_cleanup_runs_once_across_multiple_opens();
    void test_retention_cleanup_retries_after_failure();

    // Connection name uniqueness (T25)
    void test_connection_names_unique_across_100_instances();

    // Step 6: long-lived connection + WAL guard
    void test_connection_open_after_construction();
    void test_pragma_synchronous_normal_set_after_construction();

    // commitSession and replaceAll contract tests
    void test_T_commitSession_upserts_by_segment_id();
    void test_U_commitSession_orphan_cleanup_is_internal();
    void test_V_replaceAll_wipes_and_rewrites();
    void test_W_backup_created_before_replaceAll_not_commitSession();

    // flushToDisc durable heartbeat (Issue 2)
    void test_flushToDisc_writes_heartbeat_row();
    void test_flushToDisc_idempotent();
    void test_flushToDisc_restores_synchronous_normal();

    // Step 2 (S1): saveDurations(Append) upserts on segment_id collision
    void test_database_saveDurations_append_upserts_existing_segment_id();

    // Step 3 (S2 + T3): atomic overlap-guarded finalisation.
    void test_finalizeIfNoOverlap_succeeds_when_no_overlap();
    void test_finalizeIfNoOverlap_rejects_when_overlapping_finalized_row_exists();
    void test_finalizeIfNoOverlap_leaves_row_unchanged_on_overlap();
    // B1 (R07-002): typed finalize outcome distinguishes policy decisions
    // (OverlapRejected/Missing) from real SQL/transaction failures (Error).
    void test_finalizeIfNoOverlap_reports_missing_when_row_absent();
    void test_finalizeIfNoOverlap_returns_error_on_begin_failure();
    void test_finalizeIfNoOverlap_returns_error_on_probe_failure();
    void test_finalizeIfNoOverlap_returns_error_on_update_failure();
    void test_finalizeIfNoOverlap_returns_error_on_commit_failure();
    void test_reconcileUnfinalizedCheckpoints_sql_error_is_not_reported_as_dropped();
    void test_recoverStartupCheckpoints_marker_intact_on_finalize_sql_error();
    void test_reconcileUnfinalizedCheckpoints_reports_finalized_and_dropped();
    void test_reconcileUnfinalizedCheckpoints_outright_drops_are_deleted();

    // SchemaStatus enum: validation of existing DB shape
    void test_schema_status_ready_on_existing_valid_db();
    void test_schema_status_outdated_legacy_columns();
    void test_schema_status_outdated_missing_column();
    void test_schema_status_outdated_missing_unique_constraint();
    void test_schema_status_ready_recreates_missing_idx_start_utc();
    void test_schema_status_ready_creates_missing_app_settings_and_idx_segment_id();
    void test_schema_status_inaccessible_readonly_file();

    // TimeTracker::initializeFromStore isolation: no side effects before recovery is called
    void test_constructor_only_does_not_consume_marker();
    void test_outdated_schema_does_not_mutate_db();

    // saveCheckpoint demotion guard: must not overwrite finalized rows
    void test_saveCheckpoint_does_not_demote_finalized_row();

    // Item C: validate schema before any additive DDL
    void test_outdated_schema_no_additive_ddl_on_incompatible_db();

    // Item D: marker must survive reconciliation failure
    void test_recoverStartupCheckpoints_marker_intact_on_reconcile_failure();
    // Finding 2: marker must survive orphan load failure
    void test_recoverStartupCheckpoints_marker_intact_on_load_failure();

    // A11: notification compares marker against the newest finalized orphan
    void test_recoverStartupCheckpoints_notifies_when_marker_covers_only_older_orphan();
    // A12: empty segment_id rows are treated as corrupt at load
    void test_loadDurations_skips_empty_segment_id_row();

    // A14: a backward clock jump must not resurrect a future-dated orphan
    void test_recoverStartupCheckpoints_drops_future_dated_orphan();

    // MUST-FIX4 (review packet C3): a cross-day orphan is well-formed by B2's
    // malformed-row definition, so recovery finalization needs its own
    // day-validity check to avoid crediting a cross-day checkpoint.
    void test_recoverStartupCheckpoints_drops_unfinalized_cross_day_orphan();

    // B2 (R08-001..003,005, R05-004): atomic recovery disposition, exact
    // millisecond aggregation, malformed-row cleanup, and idempotency.
    void test_recoverStartupCheckpoints_deletes_malformed_rows_without_public_count();
    void test_recoverStartupCheckpoints_aggregates_exact_milliseconds_no_truncation();
    void test_reconcileUnfinalizedCheckpoints_missing_row_is_internal_error();
    void test_recoverStartupCheckpoints_marker_delete_failure_preserves_marker();
    void test_recoverStartupCheckpoints_discard_intent_clear_failure_preserves_everything();
    void test_recoverStartupCheckpoints_second_startup_is_idempotent();

    // F6: ensureSettingsTable() must validate the shape of a pre-existing app_settings table
    void test_ensureSettingsTable_rebuilds_malformed_table();
    void test_ensureSettingsTable_rebuilds_constraint_malformed_table();
    void test_ensureSettingsTable_leaves_healthy_table_untouched();

    // ========================================================================
    // A4 (R05-001): a failed checkpoint discard must leave a DURABLE intent
    // (a store-owned tombstone) surviving a transient row-delete failure, and
    // startup recovery must honor it -- deleting only a matching unfinalized
    // row, never a finalized one.
    // ========================================================================

    // Ordinary success: the tombstone is recorded then resolved in the same
    // call, alongside the physical row delete.
    void test_A4_discardCheckpoint_success_leaves_no_residual_intent();
    // A row-delete failure after the tombstone commit leaves the intent
    // durable and the (still unfinalized) row untouched.
    void test_A4_discardCheckpoint_row_delete_failure_leaves_durable_intent();
    // Repeating discardCheckpoint() for the same segment is idempotent.
    void test_A4_discardCheckpoint_repeated_application_is_idempotent();
    // Disabled history performs no mutation.
    void test_A4_discardCheckpoint_disabled_history_no_mutation();
    // Real close-and-reopen: a durable intent surviving a row-delete failure
    // is honored by the NEXT recovery -- the unfinalized row is deleted, not
    // finalized/credited, and the tombstone is resolved.
    void test_A4_recovery_after_reopen_deletes_row_matching_durable_intent();
    // A discard intent that (by a reused/corrupt identity) matches a
    // FINALIZED row must never delete it; only the tombstone is resolved.
    void test_A4_recovery_never_deletes_finalized_row_matching_intent();

    // C2 (R07-001): a failed COMMIT must roll back, leaving the connection
    // outside the transaction so later autocommit operations behave normally.
    void test_C2_saveDurations_append_rolls_back_on_commit_failure();
    void test_C2_replaceAll_rolls_back_on_commit_failure();
    void test_C2_saveCheckpoint_insert_rolls_back_on_commit_failure();
    void test_C2_saveCheckpoint_update_rolls_back_on_commit_failure();
    void test_C2_replaceAll_preserves_commit_error_when_rollback_also_fails();
    void test_C2_saveCheckpoint_insert_rolls_back_on_genuine_commit_lock_failure();
    // C2 round-2 fix (R07-001): a commit failure with an EMPTY error text
    // must still surface as a failure to the caller -- not be misread as
    // success because the old contract inferred success from message
    // emptiness.
    void test_C2_replaceAll_fails_on_commit_failure_with_empty_error_text();

    // C3 (R13-001/R14-001): the day-validity/overlap payload guard rejects
    // an invalid payload before mutation at every uTimer write boundary, and
    // ordinary load deletes malformed finalized rows transactionally.
    void test_C3_loadDurations_deletes_all_malformed_row_shapes();
    // Finding 4 (C3 review, Medium): prove commit-or-rollback for the
    // malformed-row delete by injection, matching the C2 standard.
    void test_C3_loadDurations_rolls_back_malformed_delete_on_commit_failure_and_retries();
    void test_C3_commitSession_rejects_cross_day_payload();
    void test_C3_saveCheckpoint_rejects_cross_day_interval();
    void test_C3_replaceAll_rejects_cross_type_overlap();
    void test_C3_replaceAll_rejects_cross_day_payload();
    // Review packet C3 MUST-FIX3: pin the previously-unpinned NonPositive,
    // DuplicateId, and InvalidTimestamp violation branches at the store
    // write boundary (mutation testing showed disabling any of these
    // individually caused zero test failures).
    void test_C3_saveCheckpoint_rejects_nonpositive_interval();
    void test_C3_commitSession_rejects_duplicate_id();
    void test_C3_commitSession_rejects_invalid_timestamp();
};

#endif // TEST_DATABASE_H
