#ifndef TEST_SETTINGS_H
#define TEST_SETTINGS_H

#include <QObject>
#include "testcommon.h"

class SettingsTest : public QObject
{
    Q_OBJECT

private slots:
    void test_settings_defaults_and_bounds();
    // 9.4: unknown keys survive a load/save round-trip; sync failure is reported
    void test_settings_unknown_key_survives_roundtrip();
    void test_settings_sync_failure_reported();
    // A14: boot_time_sec_ is bounded so a garbage/huge value cannot silently
    // void the boot credit (see timer.cpp cross-midnight day-boundary check).
    void test_settings_boot_time_sec_bound();
    // D12-1: a malformed (present but unparseable) key falls back to the
    // documented default instead of silently becoming 0/false.
    void test_settings_malformed_int_uses_default();
    void test_settings_malformed_bool_uses_default();
    // C12c: the removed checkpoint-interval key must be deleted from the ini
    // for every legacy value (including the old "0 = off" sentinel and a
    // malformed value), while other known keys and an unknown key survive.
    void test_settings_obsolete_checkpoint_key_removed();
    // M1: the static pre-pass reader must agree with the constructor for
    // every debug_log_to_file value, since Logger::configure() is now called
    // from it before Settings is constructed.
    void test_settings_readLogToFile_matches_constructor();
    // D12-3: setAutopauseState()/setPinToTopState() persist first and only
    // update in-memory state on success; on a write failure they report the
    // failure and retain the prior value.
    void test_settings_setAutopauseState_success_updates_value();
    void test_settings_setAutopauseState_write_failure_retains_prior_value();
    void test_settings_setPinToTopState_write_failure_retains_prior_value();
};

#endif // TEST_SETTINGS_H
