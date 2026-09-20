#ifndef TEST_LOGGER_H
#define TEST_LOGGER_H

#include <QObject>
#include <QTemporaryDir>
#include "testcommon.h"

class Settings;

class LoggerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // Test A: disabled-logging path is silent
    void test_a_disabled_logging_is_silent();
    // Test A1: rotation trims the oldest lines the first time the log file is
    // opened (must run before any other test lets Logger actually open the
    // file -- Logger's file handle is a process-wide singleton, opened once).
    void test_a1_rotation_trims_oldest_lines_on_first_open();
    // Test B: enabled-logging path writes to the log file
    void test_b_enabled_logging_writes();
    // Test C: toggle at runtime is honoured (pre-flip messages absent, post-flip present)
    void test_c_toggle_at_runtime_is_honoured();
    // Test D: Logger::configure() captures logToFile as a value -- Logger
    // must keep working after the Settings object it was read from is gone.
    void test_d_settings_lifetime_survives_settings_destruction();
    // Test E: the first write/flush failure disables file logging for the
    // rest of the process. Must run LAST -- it permanently disables Logger.
    void test_e_forced_write_failure_disables_logging_permanently();

private:
    QTemporaryDir settingsDir_;
    Settings* settingsOff_ = nullptr;
    Settings* settingsOn_  = nullptr;
};

#endif // TEST_LOGGER_H
