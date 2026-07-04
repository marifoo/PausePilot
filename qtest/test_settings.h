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
};

#endif // TEST_SETTINGS_H
